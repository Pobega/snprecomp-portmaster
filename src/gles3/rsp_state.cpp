#include "rsp_state.h"
#include "rdram_access.h"
#include <cmath>
#include <cstring>
#include <algorithm>

namespace snp::gles3 {

namespace {
    constexpr float N64_WIDTH = 320.0f;
    constexpr float N64_HEIGHT = 240.0f;
    constexpr float N64_HALF_WIDTH = 160.0f;
    constexpr float SUBPIXEL_SCALE = 4.0f;
    constexpr float UV_FIXED_SCALE = 32.0f;
    constexpr float DSDX_1CYCLE_SCALE = 1024.0f;
    constexpr float DSDX_COPY_SCALE = 4096.0f;
    constexpr uint32_t F3DEX2_LIGHT_STRIDE = 24u;
    constexpr uint32_t F3DEX2_FIRST_LIGHT_OFFSET = 48u;
    constexpr float ALPHA_THRESHOLD_CUTOUT = 0.12f;
    constexpr float ALPHA_THRESHOLD_COMPARE = 0.0039f;
    constexpr float PI_F = 3.14159265f;

    float compute_tile_shift_scale(uint8_t shift) {
        if (shift == 0) {
            return 1.0f;
        }
        if (shift <= 10) {
            return 1.0f / static_cast<float>(1u << shift);
        }
        return static_cast<float>(1u << (16u - shift));
    }

    Matrix4 make_ortho_n64_screen() {
        Matrix4 ortho{};
        ortho.m[0][0] = 2.0f / N64_WIDTH;
        ortho.m[1][1] = -2.0f / N64_HEIGHT;
        ortho.m[2][2] = 1.0f;
        ortho.m[3][0] = -1.0f;
        ortho.m[3][1] = 1.0f;
        ortho.m[3][3] = 1.0f;
        return ortho;
    }
}

Matrix4 Matrix4::identity() {
    Matrix4 r{};
    for (int i = 0; i < 4; ++i) {
        r.m[i][i] = 1.0f;
    }
    return r;
}

Matrix4 Matrix4::multiply(const Matrix4& a, const Matrix4& b) {
    Matrix4 r{};
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            for (int k = 0; k < 4; ++k) {
                r.m[i][j] += a.m[i][k] * b.m[k][j];
            }
        }
    }
    return r;
}

RspState::RspState(TmemTextureCache& tmem_cache, CombinerShaderCache& shader_cache, FboManager& fbo_manager)
    : tmem_cache_(tmem_cache), shader_cache_(shader_cache), fbo_manager_(fbo_manager) {
    reset();

    for (size_t i = 0; i < PASS_RING_COUNT; ++i) {
        gl::glGenVertexArrays(1, &pass_ring_[i].vao);
        gl::glGenBuffers(1, &pass_ring_[i].vbo);
        gl::glGenBuffers(1, &pass_ring_[i].ibo);

        gl::glBindVertexArray(pass_ring_[i].vao);
        gl::glBindBuffer(GL_ARRAY_BUFFER, pass_ring_[i].vbo);
        gl::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, pass_ring_[i].ibo);

        gl::glEnableVertexAttribArray(0);
        gl::glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(BatchVertex), reinterpret_cast<void*>(0));

        gl::glEnableVertexAttribArray(1);
        gl::glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(BatchVertex), reinterpret_cast<void*>(4 * sizeof(float)));

        gl::glEnableVertexAttribArray(2);
        gl::glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(BatchVertex), reinterpret_cast<void*>(8 * sizeof(float)));

        gl::glBindVertexArray(0);
        gl::glBindBuffer(GL_ARRAY_BUFFER, 0);
        gl::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    }
}

RspState::~RspState() {
    for (size_t i = 0; i < PASS_RING_COUNT; ++i) {
        if (pass_ring_[i].vao != 0 && gl::glDeleteVertexArrays) gl::glDeleteVertexArrays(1, &pass_ring_[i].vao);
        if (pass_ring_[i].vbo != 0 && gl::glDeleteBuffers) gl::glDeleteBuffers(1, &pass_ring_[i].vbo);
        if (pass_ring_[i].ibo != 0 && gl::glDeleteBuffers) gl::glDeleteBuffers(1, &pass_ring_[i].ibo);
    }
}

void RspState::reset() {
    submit_render_pass();
    pass_vertices_.clear();
    pass_indices_.clear();
    pass_draws_.clear();
    batch_vertices_.clear();
    batch_indices_.clear();
    batch_count_ = 0;
    tri_count_ = 0;
    modelview_top_ = 0;
    modelview_stack_[0] = Matrix4::identity();
    projection_matrix_ = Matrix4::identity();
    std::memset(vtx_buffer_, 0, sizeof(vtx_buffer_));
    std::memset(segments_, 0, sizeof(segments_));
    geometry_mode_ = 0;
    other_mode_h_ = 0;
    other_mode_l_ = 0;
    combine_w0_ = 0;
    combine_w1_ = 0;
    active_tile_ = 0;
    tex_enabled_ = false;
    tex_scale_s_ = 1.0f;
    tex_scale_t_ = 1.0f;
    prim_lod_frac_ = 0.0f;
    num_dir_lights_ = 1;
    current_batch_tex_ = 0;
    current_tex_scale_s_ = 1.0f;
    current_tex_scale_t_ = 1.0f;
    current_tex_offset_s_ = 0.0f;
    current_tex_offset_t_ = 0.0f;
    rect_align_origin_ = G_EX_ORIGIN_NONE;
    rect_align_offset_x_ = 0;
    rect_align_offset_y_ = 0;
    while (!scissor_stack_.empty()) scissor_stack_.pop();
    current_scissor_ = ScissorRect{ 0, 0, 320, 240 };
}

void RspState::set_window_size(int win_w, int win_h, bool expand_aspect) {
    win_w_ = win_w;
    win_h_ = win_h;
    expand_aspect_ = expand_aspect;
    if (expand_aspect && win_h > 0) {
        widescreen_scale_ = (static_cast<float>(win_w) / static_cast<float>(win_h)) / (4.0f / 3.0f);
    } else {
        widescreen_scale_ = 1.0f;
    }
}

Matrix4 RspState::read_matrix(const uint8_t* rdram, uint32_t phys_addr) const {
    Matrix4 r{};
    for (int i = 0; i < 16; ++i) {
        uint32_t hi_word = read_rdram_u32(rdram, phys_addr + (i / 2) * 4);
        int16_t hi = static_cast<int16_t>(hi_word >> ((i & 1) ? 0 : 16));
        uint32_t lo_word = read_rdram_u32(rdram, phys_addr + 32 + (i / 2) * 4);
        uint16_t lo = static_cast<uint16_t>(lo_word >> ((i & 1) ? 0 : 16));
        int32_t fixed = static_cast<int32_t>((static_cast<uint32_t>(hi) << 16) | lo);
        r.m[i / 4][i % 4] = static_cast<float>(fixed) / 65536.0f;
    }
    return r;
}

void RspState::load_matrix(const uint8_t* rdram, uint32_t addr, uint32_t params) {
    flush_batch();
    Matrix4 m = read_matrix(rdram, resolve_segment_address(addr, segments_));

    if (params & G_MTX_PROJECTION) {
        projection_matrix_ = (params & G_MTX_LOAD) ? m : Matrix4::multiply(m, projection_matrix_);
    } else {
        if ((params & G_MTX_PUSH) && (modelview_top_ + 1 < static_cast<int>(MODELVIEW_STACK_DEPTH))) {
            modelview_stack_[modelview_top_ + 1] = modelview_stack_[modelview_top_];
            modelview_top_++;
        }
        modelview_stack_[modelview_top_] = (params & G_MTX_LOAD) ? m : Matrix4::multiply(m, modelview_stack_[modelview_top_]);
    }
}

void RspState::pop_matrix(uint32_t count) {
    flush_batch();
    uint32_t num = std::max(1u, count / 64u);
    modelview_top_ = std::max(0, modelview_top_ - static_cast<int>(num));
}

void RspState::set_geometry_mode(uint32_t clear_bits, uint32_t set_bits) {
    flush_batch();
    geometry_mode_ = (geometry_mode_ & ~clear_bits) | set_bits;
}

void RspState::set_texture(uint32_t tile, bool enabled, uint16_t scale_s, uint16_t scale_t) {
    flush_batch();
    active_tile_ = tile & 0x07u;
    tex_enabled_ = enabled;
    tex_scale_s_ = (scale_s == 0xFFFFu) ? 1.0f : (static_cast<float>(scale_s) / 65536.0f);
    tex_scale_t_ = (scale_t == 0xFFFFu) ? 1.0f : (static_cast<float>(scale_t) / 65536.0f);
}

void RspState::set_other_mode_h(uint32_t shift, uint32_t length, uint32_t data) {
    flush_batch();
    uint32_t len = length + 1u;
    uint32_t bit_pos = 32u - shift - len;
    uint32_t mask = (len >= 32u) ? 0xFFFFFFFFu : (((1u << len) - 1u) << bit_pos);
    other_mode_h_ = (other_mode_h_ & ~mask) | (data & mask);
}

void RspState::set_other_mode_l(uint32_t shift, uint32_t length, uint32_t data) {
    flush_batch();
    uint32_t len = length + 1u;
    uint32_t bit_pos = 32u - shift - len;
    uint32_t mask = (len >= 32u) ? 0xFFFFFFFFu : (((1u << len) - 1u) << bit_pos);
    other_mode_l_ = (other_mode_l_ & ~mask) | (data & mask);
}

void RspState::set_prim_color(uint32_t w0, uint32_t color) {
    flush_batch();
    prim_lod_frac_ = (w0 & 0xFFu) / 255.0f;
    prim_color_[0] = ((color >> 24) & 0xFF) / 255.0f;
    prim_color_[1] = ((color >> 16) & 0xFF) / 255.0f;
    prim_color_[2] = ((color >> 8) & 0xFF) / 255.0f;
    prim_color_[3] = (color & 0xFF) / 255.0f;
}

void RspState::set_env_color(uint32_t color) {
    flush_batch();
    env_color_[0] = ((color >> 24) & 0xFF) / 255.0f;
    env_color_[1] = ((color >> 16) & 0xFF) / 255.0f;
    env_color_[2] = ((color >> 8) & 0xFF) / 255.0f;
    env_color_[3] = (color & 0xFF) / 255.0f;
}

void RspState::set_fog_color(uint32_t color) {
    flush_batch();
    fog_color_[0] = ((color >> 24) & 0xFF) / 255.0f;
    fog_color_[1] = ((color >> 16) & 0xFF) / 255.0f;
    fog_color_[2] = ((color >> 8) & 0xFF) / 255.0f;
    fog_color_[3] = (color & 0xFF) / 255.0f;
}

void RspState::set_blend_color(uint32_t color) {
    flush_batch();
    blend_color_[0] = ((color >> 24) & 0xFF) / 255.0f;
    blend_color_[1] = ((color >> 16) & 0xFF) / 255.0f;
    blend_color_[2] = ((color >> 8) & 0xFF) / 255.0f;
    blend_color_[3] = (color & 0xFF) / 255.0f;
}

void RspState::set_fill_color(uint32_t color) {
    fill_color_ = color;
}

void RspState::set_combine(uint32_t w0, uint32_t w1) {
    flush_batch();
    combine_w0_ = w0;
    combine_w1_ = w1;
}

void RspState::move_mem(const uint8_t* rdram, uint8_t type, uint8_t offset, uint32_t addr) {
    uint32_t phys_addr = resolve_segment_address(addr, segments_);
    if (type == G_MV_LIGHT) {
        uint32_t byte_offset = static_cast<uint32_t>(offset) * 8u;
        if (byte_offset < F3DEX2_FIRST_LIGHT_OFFSET) {
            return;
        }
        uint32_t slot = (byte_offset - F3DEX2_FIRST_LIGHT_OFFSET) / F3DEX2_LIGHT_STRIDE;
        uint32_t c = read_rdram_u32(rdram, phys_addr);
        float lr = ((c >> 24) & 0xFF) / 255.0f;
        float lg = ((c >> 16) & 0xFF) / 255.0f;
        float lb = ((c >> 8) & 0xFF) / 255.0f;

        if (slot < num_dir_lights_ && slot < MAX_DIR_LIGHTS) {
            dir_lights_[slot].r = lr;
            dir_lights_[slot].g = lg;
            dir_lights_[slot].b = lb;
            int8_t dx = read_rdram_s8(rdram, phys_addr + 8);
            int8_t dy = read_rdram_s8(rdram, phys_addr + 9);
            int8_t dz = read_rdram_s8(rdram, phys_addr + 10);
            float len = std::sqrt(static_cast<float>(dx * dx + dy * dy + dz * dz));
            if (len > 0.0001f) {
                dir_lights_[slot].dir_x = dx / len;
                dir_lights_[slot].dir_y = dy / len;
                dir_lights_[slot].dir_z = dz / len;
            }
        } else if (slot == num_dir_lights_) {
            ambient_light_.r = lr;
            ambient_light_.g = lg;
            ambient_light_.b = lb;
        }
    }
}

void RspState::move_word(uint8_t index, uint16_t offset, uint32_t value) {
    if (index == G_MW_SEGMENT) {
        uint32_t seg_idx = (offset >> 2) & 0x0Fu;
        segments_[seg_idx] = value & 0x00FFFFFFu;
    } else if (index == G_MW_NUMLIGHT) {
        num_dir_lights_ = std::min<uint32_t>(static_cast<uint32_t>(MAX_DIR_LIGHTS), value / F3DEX2_LIGHT_STRIDE);
    } else if (index == G_MW_FOG) {
        flush_batch();
        fog_mult_ = static_cast<float>(static_cast<int16_t>(value >> 16));
        fog_offset_ = static_cast<float>(static_cast<int16_t>(value & 0xFFFF));
    }
}

void RspState::apply_lighting(RspVertex& v, float nx, float ny, float nz) const {
    const Matrix4& mv = modelview_stack_[modelview_top_];
    float wx = nx * mv.m[0][0] + ny * mv.m[1][0] + nz * mv.m[2][0];
    float wy = nx * mv.m[0][1] + ny * mv.m[1][1] + nz * mv.m[2][1];
    float wz = nx * mv.m[0][2] + ny * mv.m[1][2] + nz * mv.m[2][2];
    float wlen = std::sqrt(wx * wx + wy * wy + wz * wz);
    if (wlen > 0.0001f) {
        wx /= wlen;
        wy /= wlen;
        wz /= wlen;
    }

    float lr = ambient_light_.r;
    float lg = ambient_light_.g;
    float lb = ambient_light_.b;

    uint32_t count = std::min<uint32_t>(num_dir_lights_, static_cast<uint32_t>(MAX_DIR_LIGHTS));
    for (uint32_t i = 0; i < count; ++i) {
        float dot = std::max(0.0f, wx * dir_lights_[i].dir_x + wy * dir_lights_[i].dir_y + wz * dir_lights_[i].dir_z);
        lr += dot * dir_lights_[i].r;
        lg += dot * dir_lights_[i].g;
        lb += dot * dir_lights_[i].b;
    }

    v.r = std::clamp(lr, 0.0f, 1.0f);
    v.g = std::clamp(lg, 0.0f, 1.0f);
    v.b = std::clamp(lb, 0.0f, 1.0f);
}

void RspState::load_vertices(const uint8_t* rdram, uint32_t addr, uint32_t numv, uint32_t v0) {
    uint32_t phys_addr = resolve_segment_address(addr, segments_);
    float scale_u = tex_enabled_ ? tex_scale_s_ : 1.0f;
    float scale_v = tex_enabled_ ? tex_scale_t_ : 1.0f;

    for (uint32_t i = 0; i < numv && (v0 + i) < VERTEX_CACHE_SIZE; ++i) {
        uint32_t v_addr = phys_addr + i * 16;
        RspVertex& v = vtx_buffer_[v0 + i];
        v.x = static_cast<float>(read_rdram_s16(rdram, v_addr + 0));
        v.y = static_cast<float>(read_rdram_s16(rdram, v_addr + 2));
        v.z = static_cast<float>(read_rdram_s16(rdram, v_addr + 4));

        float raw_s = static_cast<float>(read_rdram_s16(rdram, v_addr + 8));
        float raw_t = static_cast<float>(read_rdram_s16(rdram, v_addr + 10));
        v.u = (raw_s * scale_u) / UV_FIXED_SCALE;
        v.v = (raw_t * scale_v) / UV_FIXED_SCALE;

        uint32_t color_word = read_rdram_u32(rdram, v_addr + 12);
        if (geometry_mode_ & G_LIGHTING) {
            float nx = static_cast<float>(static_cast<int8_t>((color_word >> 24) & 0xFF)) / 127.0f;
            float ny = static_cast<float>(static_cast<int8_t>((color_word >> 16) & 0xFF)) / 127.0f;
            float nz = static_cast<float>(static_cast<int8_t>((color_word >> 8) & 0xFF)) / 127.0f;
            v.a = (color_word & 0xFF) / 255.0f;
            apply_lighting(v, nx, ny, nz);

            if (geometry_mode_ & G_TEXTURE_GEN) {
                const Matrix4& mv = modelview_stack_[modelview_top_];
                float vx = nx * mv.m[0][0] + ny * mv.m[1][0] + nz * mv.m[2][0];
                float vy = nx * mv.m[0][1] + ny * mv.m[1][1] + nz * mv.m[2][1];
                float vz = nx * mv.m[0][2] + ny * mv.m[1][2] + nz * mv.m[2][2];
                float vlen = std::sqrt(vx * vx + vy * vy + vz * vz);
                if (vlen > 0.0001f) {
                    vx /= vlen;
                    vy /= vlen;
                }
                if (geometry_mode_ & G_TEXTURE_GEN_LINEAR) {
                    v.u = (std::acos(std::clamp(vx, -1.0f, 1.0f)) / PI_F) * 1024.0f * scale_u;
                    v.v = (std::acos(std::clamp(vy, -1.0f, 1.0f)) / PI_F) * 1024.0f * scale_v;
                } else {
                    v.u = 0.5f * (vx + 1.0f) * 1024.0f * scale_u;
                    v.v = 0.5f * (vy + 1.0f) * 1024.0f * scale_v;
                }
            }
        } else {
            v.r = ((color_word >> 24) & 0xFF) / 255.0f;
            v.g = ((color_word >> 16) & 0xFF) / 255.0f;
            v.b = ((color_word >> 8) & 0xFF) / 255.0f;
            v.a = (color_word & 0xFF) / 255.0f;
        }
    }
}

void RspState::modify_vertex(uint32_t vtx, uint32_t where, uint32_t val) {
    if (vtx >= VERTEX_CACHE_SIZE) return;
    RspVertex& v = vtx_buffer_[vtx];
    if (where == 0x10) {
        v.r = ((val >> 24) & 0xFF) / 255.0f;
        v.g = ((val >> 16) & 0xFF) / 255.0f;
        v.b = ((val >> 8) & 0xFF) / 255.0f;
        v.a = (val & 0xFF) / 255.0f;
    } else if (where == 0x14) {
        v.u = static_cast<float>(static_cast<int16_t>(val >> 16)) / 32.0f;
        v.v = static_cast<float>(static_cast<int16_t>(val & 0xFFFF)) / 32.0f;
    } else if (where == 0x18) {
        v.x = static_cast<float>(static_cast<int16_t>(val >> 16)) / 4.0f;
        v.y = static_cast<float>(static_cast<int16_t>(val & 0xFFFF)) / 4.0f;
    } else if (where == 0x1C) {
        v.z = static_cast<float>(static_cast<int16_t>(val >> 16)) / 4.0f;
    }
}

void RspState::ensure_batch_state(GLuint tex_id, float tex_scale_s, float tex_scale_t, float tex_offset_s, float tex_offset_t) {
    if (current_batch_tex_ != tex_id ||
        current_tex_scale_s_ != tex_scale_s ||
        current_tex_scale_t_ != tex_scale_t ||
        current_tex_offset_s_ != tex_offset_s ||
        current_tex_offset_t_ != tex_offset_t) {
        flush_batch();
        current_batch_tex_ = tex_id;
        current_tex_scale_s_ = tex_scale_s;
        current_tex_scale_t_ = tex_scale_t;
        current_tex_offset_s_ = tex_offset_s;
        current_tex_offset_t_ = tex_offset_t;
    }
}

float RspState::compute_rect_shift_x() const {
    if (!expand_aspect_ || widescreen_scale_ <= 1.0f) {
        return 0.0f;
    }
    float offset_px = static_cast<float>(rect_align_offset_x_) / SUBPIXEL_SCALE;
    if (rect_align_origin_ == G_EX_ORIGIN_LEFT) {
        return -N64_HALF_WIDTH * (widescreen_scale_ - 1.0f) + offset_px;
    }
    if (rect_align_origin_ == G_EX_ORIGIN_RIGHT) {
        return N64_WIDTH + N64_HALF_WIDTH * (widescreen_scale_ - 1.0f) + offset_px;
    }
    return offset_px;
}

void RspState::apply_gl_scissor_for_rect(const FboEntry& fbo, const ScissorRect& scissor) const {
    float scale_y = static_cast<float>(fbo.height) / N64_HEIGHT;
    int sy = static_cast<int>((N64_HEIGHT - static_cast<float>(scissor.y + scissor.height)) * scale_y);
    int sh = static_cast<int>(static_cast<float>(scissor.height) * scale_y);

    int sx = 0;
    int sx_right = fbo.width;
    if (expand_aspect_ && widescreen_scale_ > 1.0f) {
        auto map_x = [&](int n64_x) -> int {
            float norm = ((static_cast<float>(n64_x) - N64_HALF_WIDTH) / (N64_WIDTH * widescreen_scale_)) + 0.5f;
            return static_cast<int>(std::lround(norm * static_cast<float>(fbo.width)));
        };
        sx = (scissor.x <= 0) ? 0 : map_x(scissor.x);
        int right_n64 = scissor.x + scissor.width;
        sx_right = (right_n64 >= static_cast<int>(N64_WIDTH)) ? fbo.width : map_x(right_n64);
    } else {
        float scale_x = static_cast<float>(fbo.width) / N64_WIDTH;
        sx = static_cast<int>(static_cast<float>(scissor.x) * scale_x);
        sx_right = static_cast<int>(static_cast<float>(scissor.x + scissor.width) * scale_x);
    }

    int sw = std::max(1, sx_right - sx);
    gl::glEnable(GL_SCISSOR_TEST);
    gl::glScissor(std::max(0, sx), std::max(0, sy), sw, std::max(1, sh));
}

void RspState::apply_gl_scissor(const FboEntry& fbo) const {
    apply_gl_scissor_for_rect(fbo, current_scissor_);
}

void RspState::flush_batch() {
    if (batch_indices_.empty()) return;

    uint32_t cycle_type = (other_mode_h_ >> 20) & 3u;
    bool is_copy_or_fill = (cycle_type == G_CYC_COPY || cycle_type == G_CYC_FILL);
    bool fog_en = !is_copy_or_fill && ((geometry_mode_ & G_FOG) != 0);
    bool alpha_compare = (other_mode_l_ & 3u) != 0;
    bool cutout_alpha = !is_copy_or_fill && ((other_mode_l_ & CVG_X_ALPHA) != 0);
    bool alpha_test = (cycle_type != G_CYC_FILL) && (alpha_compare || cutout_alpha);

    const CombinerProgram& prog = shader_cache_.get_program(combine_w0_, combine_w1_, cycle_type, fog_en, alpha_test);
    if (prog.program_id == 0) {
        batch_vertices_.clear();
        batch_indices_.clear();
        return;
    }

    GLuint tex0_id = 0;
    if (prog.uses_tex0 && (tex_enabled_ || current_batch_tex_ != 0)) {
        tex0_id = current_batch_tex_;
    }

    GLuint tex1_id = 0;
    if (prog.uses_tex1 && tex_enabled_) {
        uint32_t tile1 = (active_tile_ + 1) & 7u;
        CachedTexture tex1 = tmem_cache_.fetch_texture(tile1, (other_mode_h_ >> 12) & 3u, (other_mode_h_ >> 14) & 3u);
        tex1_id = tex1.texture_id;
    }

    if (!pass_draws_.empty() &&
        (pass_vertices_.size() + batch_vertices_.size() > MAX_PASS_VERTICES ||
         pass_draws_.size() >= MAX_PASS_DRAWS)) {
        submit_render_pass_internal();
    }

    Matrix4 proj = projection_matrix_;
    if (expand_aspect_ && widescreen_scale_ > 1.0f) {
        proj.m[0][0] /= widescreen_scale_;
        proj.m[3][0] /= widescreen_scale_;
    }

    float alpha_threshold = cutout_alpha ? ALPHA_THRESHOLD_CUTOUT : ALPHA_THRESHOLD_COMPARE;

    uint32_t first_index = static_cast<uint32_t>(pass_indices_.size());
    uint16_t base_vtx = static_cast<uint16_t>(pass_vertices_.size());
    pass_vertices_.insert(pass_vertices_.end(), batch_vertices_.begin(), batch_vertices_.end());
    pass_indices_.reserve(pass_indices_.size() + batch_indices_.size());
    for (uint16_t idx : batch_indices_) {
        pass_indices_.push_back(base_vtx + idx);
    }
    uint32_t index_count = static_cast<uint32_t>(batch_indices_.size());

    PassDrawCmd cmd;
    cmd.prog = prog;
    cmd.proj = proj;
    cmd.mv = modelview_stack_[modelview_top_];
    cmd.tex_scale_s = current_tex_scale_s_;
    cmd.tex_scale_t = current_tex_scale_t_;
    cmd.tex_offset_s = current_tex_offset_s_;
    cmd.tex_offset_t = current_tex_offset_t_;
    cmd.fog_mult = fog_mult_;
    cmd.fog_offset = fog_offset_;
    std::memcpy(cmd.prim_color, prim_color_, sizeof(prim_color_));
    cmd.prim_lod_frac = prim_lod_frac_;
    std::memcpy(cmd.env_color, env_color_, sizeof(env_color_));
    std::memcpy(cmd.fog_color, fog_color_, sizeof(fog_color_));
    std::memcpy(cmd.blend_color, blend_color_, sizeof(blend_color_));
    cmd.alpha_threshold = alpha_threshold;
    cmd.tex0_id = tex0_id;
    cmd.tex1_id = tex1_id;
    cmd.geometry_mode = geometry_mode_;
    cmd.other_mode_h = other_mode_h_;
    cmd.other_mode_l = other_mode_l_;
    cmd.scissor = current_scissor_;
    cmd.first_index = first_index;
    cmd.index_count = index_count;

    pass_draws_.push_back(cmd);

    batch_count_++;
    tri_count_ += static_cast<uint32_t>(index_count / 3);

    batch_vertices_.clear();
    batch_indices_.clear();
}

void RspState::submit_render_pass() {
    flush_batch();
    submit_render_pass_internal();
}

void RspState::submit_render_pass_internal() {
    if (pass_draws_.empty()) return;

    uint64_t start_pc = SDL_GetPerformanceCounter();

    PassBufferSlot& slot = pass_ring_[ring_cursor_];
    ring_cursor_ = (ring_cursor_ + 1) % PASS_RING_COUNT;

    gl::glBindVertexArray(slot.vao);
    gl::glBindBuffer(GL_ARRAY_BUFFER, slot.vbo);
    gl::glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(pass_vertices_.size() * sizeof(BatchVertex)),
                     pass_vertices_.data(), GL_STREAM_DRAW);

    gl::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, slot.ibo);
    gl::glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(pass_indices_.size() * sizeof(uint16_t)),
                     pass_indices_.data(), GL_STREAM_DRAW);

    GLuint last_prog = 0;
    Matrix4 last_proj{};
    Matrix4 last_mv{};
    GLuint last_tex0 = 0xFFFFFFFFu;
    GLuint last_tex1 = 0xFFFFFFFFu;
    ScissorRect last_scissor{ -1, -1, -1, -1 };
    std::memset(&last_proj, 0xFF, sizeof(last_proj));
    std::memset(&last_mv, 0xFF, sizeof(last_mv));

    for (const auto& cmd : pass_draws_) {
        if (cmd.prog.program_id != last_prog) {
            gl::glUseProgram(cmd.prog.program_id);
            last_prog = cmd.prog.program_id;
            std::memset(&last_proj, 0xFF, sizeof(last_proj));
            std::memset(&last_mv, 0xFF, sizeof(last_mv));
            last_tex0 = 0xFFFFFFFFu;
            last_tex1 = 0xFFFFFFFFu;
        }

        if (std::memcmp(&cmd.proj, &last_proj, sizeof(Matrix4)) != 0) {
            if (cmd.prog.uProjection >= 0) {
                gl::glUniform4fv(cmd.prog.uProjection, 4, &cmd.proj.m[0][0]);
            }
            last_proj = cmd.proj;
        }

        if (std::memcmp(&cmd.mv, &last_mv, sizeof(Matrix4)) != 0) {
            if (cmd.prog.uModelView >= 0) {
                gl::glUniform4fv(cmd.prog.uModelView, 4, &cmd.mv.m[0][0]);
            }
            last_mv = cmd.mv;
        }

        if (cmd.prog.uTexScale >= 0) gl::glUniform2f(cmd.prog.uTexScale, cmd.tex_scale_s, cmd.tex_scale_t);
        if (cmd.prog.uTexOffset >= 0) gl::glUniform2f(cmd.prog.uTexOffset, cmd.tex_offset_s, cmd.tex_offset_t);
        if (cmd.prog.uFogMult >= 0) gl::glUniform1f(cmd.prog.uFogMult, cmd.fog_mult);
        if (cmd.prog.uFogOffset >= 0) gl::glUniform1f(cmd.prog.uFogOffset, cmd.fog_offset);
        if (cmd.prog.uPrimColor >= 0) gl::glUniform4fv(cmd.prog.uPrimColor, 1, cmd.prim_color);
        if (cmd.prog.uPrimLodFrac >= 0) gl::glUniform1f(cmd.prog.uPrimLodFrac, cmd.prim_lod_frac);
        if (cmd.prog.uEnvColor >= 0) gl::glUniform4fv(cmd.prog.uEnvColor, 1, cmd.env_color);
        if (cmd.prog.uFogColor >= 0) gl::glUniform4fv(cmd.prog.uFogColor, 1, cmd.fog_color);
        if (cmd.prog.uBlendColor >= 0) gl::glUniform4fv(cmd.prog.uBlendColor, 1, cmd.blend_color);
        if (cmd.prog.uAlphaThreshold >= 0) gl::glUniform1f(cmd.prog.uAlphaThreshold, cmd.alpha_threshold);

        if (cmd.prog.uses_tex0 && cmd.tex0_id != 0) {
            if (cmd.tex0_id != last_tex0) {
                gl::glActiveTexture(GL_TEXTURE0);
                gl::glBindTexture(GL_TEXTURE_2D, cmd.tex0_id);
                last_tex0 = cmd.tex0_id;
            }
            if (cmd.prog.uTexture0 >= 0) gl::glUniform1i(cmd.prog.uTexture0, 0);
        }

        if (cmd.prog.uses_tex1 && cmd.tex1_id != 0) {
            if (cmd.tex1_id != last_tex1) {
                gl::glActiveTexture(GL_TEXTURE1);
                gl::glBindTexture(GL_TEXTURE_2D, cmd.tex1_id);
                last_tex1 = cmd.tex1_id;
            }
            if (cmd.prog.uTexture1 >= 0) gl::glUniform1i(cmd.prog.uTexture1, 1);
            gl::glActiveTexture(GL_TEXTURE0);
        }

        uint32_t cycle_type = (cmd.other_mode_h >> 20) & 3u;
        bool is_copy_or_fill = (cycle_type == G_CYC_COPY || cycle_type == G_CYC_FILL);

        if ((cmd.geometry_mode & G_ZBUFFER) && !is_copy_or_fill) {
            gl::glEnable(GL_DEPTH_TEST);
            gl::glDepthFunc((cmd.other_mode_l & Z_CMP) ? GL_LEQUAL : GL_ALWAYS);
            gl::glDepthMask((cmd.other_mode_l & Z_UPD) ? GL_TRUE : GL_FALSE);
            if (cmd.other_mode_l & ZMODE_DEC) {
                gl::glEnable(GL_POLYGON_OFFSET_FILL);
                gl::glPolygonOffset(-1.0f, -1.0f);
            } else {
                gl::glDisable(GL_POLYGON_OFFSET_FILL);
            }
        } else {
            gl::glDisable(GL_DEPTH_TEST);
            gl::glDepthMask(GL_FALSE);
            gl::glDisable(GL_POLYGON_OFFSET_FILL);
        }

        if (cmd.geometry_mode & (G_CULL_FRONT | G_CULL_BACK)) {
            gl::glEnable(GL_CULL_FACE);
            if ((cmd.geometry_mode & (G_CULL_FRONT | G_CULL_BACK)) == (G_CULL_FRONT | G_CULL_BACK)) {
                gl::glCullFace(GL_FRONT_AND_BACK);
            } else if (cmd.geometry_mode & G_CULL_FRONT) {
                gl::glCullFace(GL_FRONT);
            } else {
                gl::glCullFace(GL_BACK);
            }
        } else {
            gl::glDisable(GL_CULL_FACE);
        }

        bool blend_requested = !is_copy_or_fill && ((cmd.other_mode_l & (FORCE_BL | ZMODE_XLU)) != 0);
        if (blend_requested) {
            uint32_t blend_cycle = (cycle_type == G_CYC_2CYCLE) ? 1u : 0u;
            uint32_t m_src = (cmd.other_mode_l >> (22u - 2u * blend_cycle)) & 3u;
            uint32_t b_src = (cmd.other_mode_l >> (18u - 2u * blend_cycle)) & 3u;
            if (m_src == 1u) {
                gl::glEnable(GL_BLEND);
                GLenum dst_factor = (b_src == 2u) ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA;
                gl::glBlendFunc(GL_SRC_ALPHA, dst_factor);
            } else {
                gl::glDisable(GL_BLEND);
            }
        } else {
            gl::glDisable(GL_BLEND);
        }

        if (cmd.scissor.x != last_scissor.x || cmd.scissor.y != last_scissor.y ||
            cmd.scissor.width != last_scissor.width || cmd.scissor.height != last_scissor.height) {
            FboEntry* current_fbo = fbo_manager_.get_current_fbo();
            if (current_fbo) {
                apply_gl_scissor_for_rect(*current_fbo, cmd.scissor);
            }
            last_scissor = cmd.scissor;
        }

        gl::glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(cmd.index_count), GL_UNSIGNED_SHORT,
                           reinterpret_cast<const void*>(static_cast<uintptr_t>(cmd.first_index * sizeof(uint16_t))));
    }

    gl::glBindVertexArray(0);
    pass_vertices_.clear();
    pass_indices_.clear();
    pass_draws_.clear();
    pass_count_++;

    uint64_t end_pc = SDL_GetPerformanceCounter();
    double submit_ms = static_cast<double>(end_pc - start_pc) * 1000.0 / static_cast<double>(SDL_GetPerformanceFrequency());
    total_submit_ms_ += submit_ms;
}

void RspState::draw_tri1(uint32_t v0, uint32_t v1, uint32_t v2) {
    if (v0 >= VERTEX_CACHE_SIZE || v1 >= VERTEX_CACHE_SIZE || v2 >= VERTEX_CACHE_SIZE) return;

    GLuint tex_id = 0;
    float scale_u = 1.0f;
    float scale_v = 1.0f;
    float offset_u = 0.0f;
    float offset_v = 0.0f;

    if (tex_enabled_) {
        const TileDescriptor& tile_desc = tmem_cache_.get_tile(active_tile_);
        CachedTexture tex = tmem_cache_.fetch_texture(active_tile_, (other_mode_h_ >> 12) & 3u, (other_mode_h_ >> 14) & 3u);
        tex_id = tex.texture_id;
        float inv_w = 1.0f / static_cast<float>(tex.width);
        float inv_h = 1.0f / static_cast<float>(tex.height);
        scale_u = compute_tile_shift_scale(tile_desc.shift_s) * inv_w;
        scale_v = compute_tile_shift_scale(tile_desc.shift_t) * inv_h;
        offset_u = -(static_cast<float>(tile_desc.sl) / SUBPIXEL_SCALE) * inv_w;
        offset_v = -(static_cast<float>(tile_desc.tl) / SUBPIXEL_SCALE) * inv_h;
    }

    ensure_batch_state(tex_id, scale_u, scale_v, offset_u, offset_v);

    uint16_t base_idx = static_cast<uint16_t>(batch_vertices_.size());
    const uint32_t idxs[3] = { v0, v1, v2 };
    for (int i = 0; i < 3; ++i) {
        const RspVertex& v = vtx_buffer_[idxs[i]];
        batch_vertices_.push_back({ v.x, v.y, v.z, 1.0f, v.r, v.g, v.b, v.a, v.u, v.v });
    }
    batch_indices_.push_back(base_idx + 0);
    batch_indices_.push_back(base_idx + 1);
    batch_indices_.push_back(base_idx + 2);
}

void RspState::draw_tri2(uint32_t v0, uint32_t v1, uint32_t v2, uint32_t v3, uint32_t v4, uint32_t v5) {
    draw_tri1(v0, v1, v2);
    draw_tri1(v3, v4, v5);
}

void RspState::draw_quad(uint32_t v0, uint32_t v1, uint32_t v2, uint32_t v3) {
    draw_tri1(v0, v1, v2);
    draw_tri1(v0, v2, v3);
}

void RspState::draw_tex_rect(uint32_t tile, uint32_t ulx, uint32_t uly, uint32_t lrx, uint32_t lry,
                             uint32_t uls, uint32_t ult, uint32_t dsdx, uint32_t dtdy, bool flip, const uint8_t* rdram) {
    flush_batch();

    uint32_t cycle_type = (other_mode_h_ >> 20) & 3u;
    bool is_copy = (cycle_type == G_CYC_COPY);
    float inclusive_pad = is_copy ? 1.0f : 0.0f;

    float x0 = static_cast<float>(ulx) / SUBPIXEL_SCALE;
    float y0 = static_cast<float>(uly) / SUBPIXEL_SCALE;
    float x1 = (static_cast<float>(lrx) / SUBPIXEL_SCALE) + inclusive_pad;
    float y1 = (static_cast<float>(lry) / SUBPIXEL_SCALE) + inclusive_pad;

    float shift_x = compute_rect_shift_x();
    x0 += shift_x;
    x1 += shift_x;

    float dsdx_divisor = is_copy ? DSDX_COPY_SCALE : DSDX_1CYCLE_SCALE;
    float s0 = static_cast<float>(static_cast<int16_t>(uls)) / UV_FIXED_SCALE;
    float t0 = static_cast<float>(static_cast<int16_t>(ult)) / UV_FIXED_SCALE;
    float s1 = s0 + (x1 - x0) * (static_cast<float>(static_cast<int16_t>(dsdx)) / dsdx_divisor);
    float t1 = t0 + (y1 - y0) * (static_cast<float>(static_cast<int16_t>(dtdy)) / DSDX_1CYCLE_SCALE);

    float feedback_v_offset = 0.0f;
    float feedback_v_scale = 1.0f;
    GLuint tex_id = fbo_manager_.find_feedback_texture(tmem_cache_.get_timg().phys_addr, feedback_v_offset, feedback_v_scale, rdram);
    float norm_u_scale = 1.0f;
    float norm_v_scale = 1.0f;
    float offset_u = 0.0f;
    float offset_v = 0.0f;

    if (tex_id == 0) {
        uint32_t tile_idx = tile & 0x07u;
        const TileDescriptor& tile_desc = tmem_cache_.get_tile(tile_idx);
        CachedTexture tex = tmem_cache_.fetch_texture(tile_idx, (other_mode_h_ >> 12) & 3u, (other_mode_h_ >> 14) & 3u);
        tex_id = tex.texture_id;
        float inv_w = 1.0f / static_cast<float>(tex.width);
        float inv_h = 1.0f / static_cast<float>(tex.height);
        norm_u_scale = compute_tile_shift_scale(tile_desc.shift_s) * inv_w;
        norm_v_scale = compute_tile_shift_scale(tile_desc.shift_t) * inv_h;
        offset_u = -(static_cast<float>(tile_desc.sl) / SUBPIXEL_SCALE) * inv_w;
        offset_v = -(static_cast<float>(tile_desc.tl) / SUBPIXEL_SCALE) * inv_h;
    } else {
        norm_u_scale = 1.0f / N64_WIDTH;
        norm_v_scale = -1.0f / N64_HEIGHT;
        offset_v = 1.0f - feedback_v_offset;
    }

    Matrix4 old_proj = projection_matrix_;
    Matrix4 old_mv = modelview_stack_[modelview_top_];
    uint32_t old_geom = geometry_mode_;

    projection_matrix_ = make_ortho_n64_screen();
    modelview_stack_[modelview_top_] = Matrix4::identity();
    geometry_mode_ &= ~(G_ZBUFFER | G_CULL_FRONT | G_CULL_BACK | G_FOG);

    ensure_batch_state(tex_id, norm_u_scale, norm_v_scale, offset_u, offset_v);

    uint16_t base_idx = static_cast<uint16_t>(batch_vertices_.size());
    if (!flip) {
        batch_vertices_.push_back({ x0, y0, 0.0f, 1.0f, prim_color_[0], prim_color_[1], prim_color_[2], prim_color_[3], s0, t0 });
        batch_vertices_.push_back({ x1, y0, 0.0f, 1.0f, prim_color_[0], prim_color_[1], prim_color_[2], prim_color_[3], s1, t0 });
        batch_vertices_.push_back({ x1, y1, 0.0f, 1.0f, prim_color_[0], prim_color_[1], prim_color_[2], prim_color_[3], s1, t1 });
        batch_vertices_.push_back({ x0, y1, 0.0f, 1.0f, prim_color_[0], prim_color_[1], prim_color_[2], prim_color_[3], s0, t1 });
    } else {
        batch_vertices_.push_back({ x0, y0, 0.0f, 1.0f, prim_color_[0], prim_color_[1], prim_color_[2], prim_color_[3], s0, t0 });
        batch_vertices_.push_back({ x1, y0, 0.0f, 1.0f, prim_color_[0], prim_color_[1], prim_color_[2], prim_color_[3], s0, t1 });
        batch_vertices_.push_back({ x1, y1, 0.0f, 1.0f, prim_color_[0], prim_color_[1], prim_color_[2], prim_color_[3], s1, t1 });
        batch_vertices_.push_back({ x0, y1, 0.0f, 1.0f, prim_color_[0], prim_color_[1], prim_color_[2], prim_color_[3], s1, t0 });
    }

    batch_indices_.push_back(base_idx + 0);
    batch_indices_.push_back(base_idx + 1);
    batch_indices_.push_back(base_idx + 2);
    batch_indices_.push_back(base_idx + 0);
    batch_indices_.push_back(base_idx + 2);
    batch_indices_.push_back(base_idx + 3);

    flush_batch();
    projection_matrix_ = old_proj;
    modelview_stack_[modelview_top_] = old_mv;
    geometry_mode_ = old_geom;
}

void RspState::draw_fill_rect(uint32_t ulx, uint32_t uly, uint32_t lrx, uint32_t lry) {
    flush_batch();

    uint32_t cycle_type = (other_mode_h_ >> 20) & 3u;
    bool is_inclusive = (cycle_type == G_CYC_FILL || cycle_type == G_CYC_COPY);
    float inclusive_pad = is_inclusive ? 1.0f : 0.0f;

    float x0 = static_cast<float>(ulx) / SUBPIXEL_SCALE;
    float y0 = static_cast<float>(uly) / SUBPIXEL_SCALE;
    float x1 = (static_cast<float>(lrx) / SUBPIXEL_SCALE) + inclusive_pad;
    float y1 = (static_cast<float>(lry) / SUBPIXEL_SCALE) + inclusive_pad;

    float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;

    if (cycle_type == G_CYC_FILL) {
        uint16_t c16 = static_cast<uint16_t>(fill_color_ & 0xFFFFu);
        r = ((c16 >> 11) & 0x1Fu) / 31.0f;
        g = ((c16 >> 6) & 0x1Fu) / 31.0f;
        b = ((c16 >> 1) & 0x1Fu) / 31.0f;
        a = (c16 & 1u) ? 1.0f : 0.0f;

        if (rect_align_origin_ == G_EX_ORIGIN_NONE && rect_align_offset_x_ == 0 &&
            ulx == 0 && uly == 0 && lrx >= 319 * 4 && lry >= 239 * 4) {
            submit_render_pass();
            FboEntry* current_fbo = fbo_manager_.get_current_fbo();
            if (current_fbo) {
                apply_gl_scissor(*current_fbo);
            }
            gl::glClearColor(r, g, b, a);
            gl::glClear(GL_COLOR_BUFFER_BIT);
            return;
        }
    } else {
        r = prim_color_[0];
        g = prim_color_[1];
        b = prim_color_[2];
        a = prim_color_[3];
    }

    float shift_x = compute_rect_shift_x();
    x0 += shift_x;
    x1 += shift_x;

    Matrix4 old_proj = projection_matrix_;
    Matrix4 old_mv = modelview_stack_[modelview_top_];
    uint32_t old_geom = geometry_mode_;

    projection_matrix_ = make_ortho_n64_screen();
    modelview_stack_[modelview_top_] = Matrix4::identity();
    geometry_mode_ &= ~(G_ZBUFFER | G_CULL_FRONT | G_CULL_BACK | G_FOG);

    ensure_batch_state(0, 1.0f, 1.0f, 0.0f, 0.0f);

    uint16_t base_idx = static_cast<uint16_t>(batch_vertices_.size());
    batch_vertices_.push_back({ x0, y0, 0.0f, 1.0f, r, g, b, a, 0.0f, 0.0f });
    batch_vertices_.push_back({ x1, y0, 0.0f, 1.0f, r, g, b, a, 1.0f, 0.0f });
    batch_vertices_.push_back({ x1, y1, 0.0f, 1.0f, r, g, b, a, 1.0f, 1.0f });
    batch_vertices_.push_back({ x0, y1, 0.0f, 1.0f, r, g, b, a, 0.0f, 1.0f });

    batch_indices_.push_back(base_idx + 0);
    batch_indices_.push_back(base_idx + 1);
    batch_indices_.push_back(base_idx + 2);
    batch_indices_.push_back(base_idx + 0);
    batch_indices_.push_back(base_idx + 2);
    batch_indices_.push_back(base_idx + 3);

    flush_batch();
    projection_matrix_ = old_proj;
    modelview_stack_[modelview_top_] = old_mv;
    geometry_mode_ = old_geom;
}

void RspState::set_scissor(uint32_t ulx, uint32_t uly, uint32_t lrx, uint32_t lry) {
    flush_batch();
    current_scissor_.x = ulx / 4;
    current_scissor_.y = uly / 4;
    current_scissor_.width = std::max(1, static_cast<int>(lrx / 4 - current_scissor_.x));
    current_scissor_.height = std::max(1, static_cast<int>(lry / 4 - current_scissor_.y));
}

void RspState::ex_set_scissor(uint32_t ulx, uint32_t uly, uint32_t lrx, uint32_t lry) {
    flush_batch();
    current_scissor_.x = ulx / 4;
    current_scissor_.y = uly / 4;
    current_scissor_.width = std::max(1, static_cast<int>(lrx / 4 - current_scissor_.x));
    current_scissor_.height = std::max(1, static_cast<int>(lry / 4 - current_scissor_.y));
}

void RspState::ex_push_scissor() {
    scissor_stack_.push(current_scissor_);
}

void RspState::ex_pop_scissor() {
    if (!scissor_stack_.empty()) {
        flush_batch();
        current_scissor_ = scissor_stack_.top();
        scissor_stack_.pop();
    }
}

void RspState::ex_set_rect_align(uint32_t origin, int32_t offset_x, int32_t offset_y) {
    rect_align_origin_ = origin;
    rect_align_offset_x_ = offset_x;
    rect_align_offset_y_ = offset_y;
}

} // namespace snp::gles3
