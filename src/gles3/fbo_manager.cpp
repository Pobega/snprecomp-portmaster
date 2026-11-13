#include "fbo_manager.h"
#include "rdram_access.h"
#include <algorithm>
#include <cstdio>
#include <vector>

namespace snp::gles3 {

namespace {
    constexpr uint64_t FNV1A_OFFSET_BASIS = 0xcbf29ce484222325ULL;
    constexpr uint64_t FNV1A_PRIME = 0x100000001b3ULL;
    constexpr uint32_t N64_SCREEN_WIDTH = 320;
    constexpr uint32_t N64_SCREEN_HEIGHT = 240;
    constexpr uint32_t N64_BUFFER_BYTES = N64_SCREEN_WIDTH * N64_SCREEN_HEIGHT * 2;
}

FboManager::FboManager() = default;

FboManager::~FboManager() {
    reset();
}

void FboManager::reset() {
    for (auto& pair : fbo_map_) {
        FboEntry& entry = pair.second;
        if (entry.depth_rb != 0 && gl::glDeleteRenderbuffers) {
            gl::glDeleteRenderbuffers(1, &entry.depth_rb);
        }
        if (entry.color_tex != 0 && gl::glDeleteTextures) {
            gl::glDeleteTextures(1, &entry.color_tex);
        }
        if (entry.fbo_id != 0 && gl::glDeleteFramebuffers) {
            gl::glDeleteFramebuffers(1, &entry.fbo_id);
        }
    }
    fbo_map_.clear();
    current_fbo_addr_ = 0;
    last_rendered_fbo_addr_ = 0;
    pending_depth_clear_ = false;
}

void FboManager::clear_bound_fbo_depth() {
    gl::glDisable(GL_SCISSOR_TEST);
    gl::glDepthMask(GL_TRUE);
    gl::glClearDepthf(1.0f);
    gl::glClear(GL_DEPTH_BUFFER_BIT);
}

void FboManager::request_depth_clear() {
    pending_depth_clear_ = true;
    FboEntry* active = get_current_fbo();
    if (active && active->fbo_id != 0) {
        gl::glBindFramebuffer(GL_FRAMEBUFFER, active->fbo_id);
        clear_bound_fbo_depth();
    }
}

uint64_t FboManager::compute_rdram_checksum(const uint8_t* rdram, uint32_t addr, size_t size) const {
    if (!rdram) return 0;
    uint64_t hash = FNV1A_OFFSET_BASIS;
    // Sample step across the buffer for speed
    size_t step = std::max<size_t>(1, size / 256);
    for (size_t i = 0; i < size; i += step) {
        hash = (hash ^ read_rdram_u8(rdram, addr + static_cast<uint32_t>(i))) * FNV1A_PRIME;
    }
    return hash;
}

FboEntry& FboManager::get_or_create_fbo(uint32_t cimg_phys_addr, int target_w, int target_h) {
    auto it = fbo_map_.find(cimg_phys_addr);
    if (it != fbo_map_.end()) {
        if (it->second.width == target_w && it->second.height == target_h) {
            return it->second;
        }
        // Size changed: delete and recreate
        if (it->second.depth_rb != 0) gl::glDeleteRenderbuffers(1, &it->second.depth_rb);
        if (it->second.color_tex != 0) gl::glDeleteTextures(1, &it->second.color_tex);
        if (it->second.fbo_id != 0) gl::glDeleteFramebuffers(1, &it->second.fbo_id);
        fbo_map_.erase(it);
    }

    FboEntry entry{};
    entry.cimg_addr = cimg_phys_addr;
    entry.width = target_w;
    entry.height = target_h;

    gl::glGenTextures(1, &entry.color_tex);
    gl::glBindTexture(GL_TEXTURE_2D, entry.color_tex);
    gl::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, target_w, target_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    gl::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    gl::glGenRenderbuffers(1, &entry.depth_rb);
    gl::glBindRenderbuffer(GL_RENDERBUFFER, entry.depth_rb);
    gl::glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, target_w, target_h);

    gl::glGenFramebuffers(1, &entry.fbo_id);
    gl::glBindFramebuffer(GL_FRAMEBUFFER, entry.fbo_id);
    gl::glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, entry.color_tex, 0);
    gl::glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, entry.depth_rb);

    GLenum status = gl::glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        std::fprintf(stderr, "[GLES3] Incomplete framebuffer status: 0x%04X\n", status);
    } else {
        gl::glDisable(GL_SCISSOR_TEST);
        gl::glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        gl::glDepthMask(GL_TRUE);
        gl::glClearDepthf(1.0f);
        gl::glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }

    auto inserted = fbo_map_.emplace(cimg_phys_addr, entry);
    return inserted.first->second;
}

void FboManager::bind_fbo(uint32_t cimg_phys_addr, int target_w, int target_h, const uint8_t* rdram) {
    FboEntry& entry = get_or_create_fbo(cimg_phys_addr, target_w, target_h);
    gl::glBindFramebuffer(GL_FRAMEBUFFER, entry.fbo_id);
    gl::glViewport(0, 0, entry.width, entry.height);
    if (pending_depth_clear_) {
        clear_bound_fbo_depth();
        pending_depth_clear_ = false;
    }
    current_fbo_addr_ = cimg_phys_addr;
    last_rendered_fbo_addr_ = cimg_phys_addr;
    entry.rendered = true;
    entry.rdram_checksum = compute_rdram_checksum(rdram, cimg_phys_addr, N64_BUFFER_BYTES);
}

FboEntry* FboManager::get_current_fbo() {
    auto it = fbo_map_.find(current_fbo_addr_);
    if (it != fbo_map_.end()) {
        return &it->second;
    }
    return nullptr;
}

GLuint FboManager::find_feedback_texture(uint32_t timg_phys_addr, float& out_v_offset, float& out_v_scale, const uint8_t* rdram) {
    for (const auto& pair : fbo_map_) {
        const FboEntry& entry = pair.second;
        if (!entry.rendered) continue;

        if (timg_phys_addr >= entry.cimg_addr && timg_phys_addr < entry.cimg_addr + N64_BUFFER_BYTES) {
            uint64_t current_checksum = compute_rdram_checksum(rdram, entry.cimg_addr, N64_BUFFER_BYTES);
            // If RDRAM was modified by CPU, don't use stale GPU texture
            if (current_checksum != entry.rdram_checksum && entry.rdram_checksum != 0) {
                return 0;
            }
            uint32_t byte_offset = timg_phys_addr - entry.cimg_addr;
            uint32_t row = byte_offset / (N64_SCREEN_WIDTH * 2);
            out_v_offset = static_cast<float>(row) / static_cast<float>(N64_SCREEN_HEIGHT);
            out_v_scale = 1.0f;
            return entry.color_tex;
        }
    }
    return 0;
}

uint32_t FboManager::resolve_present_fbo_addr(uint32_t vi_origin_phys) const {
    constexpr uint32_t MAX_VI_ROW_DRIFT = N64_SCREEN_WIDTH * 16 * 2;
    for (const auto& pair : fbo_map_) {
        uint32_t fbo_addr = pair.first;
        if ((vi_origin_phys >= fbo_addr && vi_origin_phys <= fbo_addr + MAX_VI_ROW_DRIFT) ||
            (fbo_addr >= vi_origin_phys && fbo_addr <= vi_origin_phys + MAX_VI_ROW_DRIFT)) {
            return pair.second.cimg_addr;
        }
    }

    auto it = fbo_map_.find(last_rendered_fbo_addr_);
    if (it != fbo_map_.end()) {
        return it->second.cimg_addr;
    }
    if (!fbo_map_.empty()) {
        return fbo_map_.begin()->second.cimg_addr;
    }
    return 0;
}

void FboManager::present_vi_origin(uint32_t vi_origin_phys, int win_w, int win_h, bool expand_aspect) {
    const FboEntry* entry = nullptr;

    // Search for matching FBO within a few row offsets of VI origin
    constexpr uint32_t MAX_VI_ROW_DRIFT = N64_SCREEN_WIDTH * 16 * 2;
    for (const auto& pair : fbo_map_) {
        uint32_t fbo_addr = pair.first;
        if ((vi_origin_phys >= fbo_addr && vi_origin_phys <= fbo_addr + MAX_VI_ROW_DRIFT) ||
            (fbo_addr >= vi_origin_phys && fbo_addr <= vi_origin_phys + MAX_VI_ROW_DRIFT)) {
            entry = &pair.second;
            break;
        }
    }

    if (!entry) {
        auto it = fbo_map_.find(last_rendered_fbo_addr_);
        if (it != fbo_map_.end()) {
            entry = &it->second;
        } else if (!fbo_map_.empty()) {
            entry = &fbo_map_.begin()->second;
        }
    }

    if (!entry || entry->fbo_id == 0) {
        return;
    }

    gl::glDisable(GL_SCISSOR_TEST);
    gl::glBindFramebuffer(GL_READ_FRAMEBUFFER, entry->fbo_id);
    gl::glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

    int dst_x = 0;
    int dst_y = 0;
    int dst_w = win_w;
    int dst_h = win_h;

    if (!expand_aspect) {
        float target_ratio = 4.0f / 3.0f;
        float win_ratio = static_cast<float>(win_w) / static_cast<float>(win_h);
        if (win_ratio > target_ratio) {
            dst_h = win_h;
            dst_w = static_cast<int>(win_h * target_ratio);
            dst_x = (win_w - dst_w) / 2;
            dst_y = 0;
        } else {
            dst_w = win_w;
            dst_h = static_cast<int>(win_w / target_ratio);
            dst_x = 0;
            dst_y = (win_h - dst_h) / 2;
        }
        gl::glViewport(0, 0, win_w, win_h);
        gl::glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        gl::glClear(GL_COLOR_BUFFER_BIT);
    }

    gl::glBlitFramebuffer(0, 0, entry->width, entry->height,
                          dst_x, dst_y, dst_x + dst_w, dst_y + dst_h,
                          GL_COLOR_BUFFER_BIT, GL_LINEAR);
    gl::glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

} // namespace snp::gles3
