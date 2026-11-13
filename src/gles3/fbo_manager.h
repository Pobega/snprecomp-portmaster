#pragma once

#include <cstdint>
#include <unordered_map>
#include "gl_loader.h"

namespace snp::gles3 {

struct FboEntry {
    uint32_t cimg_addr = 0;
    int width = 0;
    int height = 0;
    GLuint fbo_id = 0;
    GLuint color_tex = 0;
    GLuint depth_rb = 0;
    uint64_t rdram_checksum = 0;
    bool rendered = false;
};

class FboManager {
public:
    FboManager();
    ~FboManager();

    void reset();

    FboEntry& get_or_create_fbo(uint32_t cimg_phys_addr, int target_w, int target_h);
    void bind_fbo(uint32_t cimg_phys_addr, int target_w, int target_h, const uint8_t* rdram);
    void request_depth_clear();

    GLuint find_feedback_texture(uint32_t timg_phys_addr, float& out_v_offset, float& out_v_scale, const uint8_t* rdram);
    void present_vi_origin(uint32_t vi_origin_phys, int win_w, int win_h, bool expand_aspect);

    uint32_t get_current_fbo_addr() const { return current_fbo_addr_; }
    FboEntry* get_current_fbo();

private:
    uint64_t compute_rdram_checksum(const uint8_t* rdram, uint32_t addr, size_t size) const;
    void clear_bound_fbo_depth();

    std::unordered_map<uint32_t, FboEntry> fbo_map_;
    uint32_t current_fbo_addr_ = 0;
    uint32_t last_rendered_fbo_addr_ = 0;
    bool pending_depth_clear_ = false;
};

} // namespace snp::gles3
