#pragma once

#include <cstdint>
#include <vector>
#include <stack>
#include "gl_loader.h"
#include "n64_gbi.h"
#include "tmem_texture_cache.h"
#include "combiner_shader_cache.h"
#include "fbo_manager.h"

namespace snp::gles3 {

struct Matrix4 {
    float m[4][4];

    static Matrix4 identity();
    static Matrix4 multiply(const Matrix4& a, const Matrix4& b);
};

struct Light {
    float r = 0.0f, g = 0.0f, b = 0.0f;
    float dir_x = 0.0f, dir_y = 1.0f, dir_z = 0.0f;
};

struct RspVertex {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float u = 0.0f, v = 0.0f;
    float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
};

struct BatchVertex {
    float x, y, z, w;
    float r, g, b, a;
    float u, v;
};

struct ScissorRect {
    int x = 0;
    int y = 0;
    int width = 320;
    int height = 240;
};

class RspState {
public:
    static constexpr size_t MODELVIEW_STACK_DEPTH = 32;
    static constexpr size_t VERTEX_CACHE_SIZE = 32;
    static constexpr size_t MAX_DIR_LIGHTS = 8;

    RspState(TmemTextureCache& tmem_cache, CombinerShaderCache& shader_cache, FboManager& fbo_manager);
    ~RspState();

    void reset();
    void set_window_size(int win_w, int win_h, bool expand_aspect);

    // Matrix commands
    void load_matrix(const uint8_t* rdram, uint32_t addr, uint32_t params);
    void pop_matrix(uint32_t count);

    // Geometry, texture & RDP mode commands
    void set_geometry_mode(uint32_t clear_bits, uint32_t set_bits);
    void set_texture(uint32_t tile, bool enabled, uint16_t scale_s, uint16_t scale_t);
    void set_other_mode_h(uint32_t shift, uint32_t length, uint32_t data);
    void set_other_mode_l(uint32_t shift, uint32_t length, uint32_t data);

    // Colors
    void set_prim_color(uint32_t w0, uint32_t color);
    void set_env_color(uint32_t color);
    void set_fog_color(uint32_t color);
    void set_blend_color(uint32_t color);
    void set_fill_color(uint32_t color);
    void set_combine(uint32_t w0, uint32_t w1);

    // Lights & Viewport & Fog
    void move_mem(const uint8_t* rdram, uint8_t type, uint8_t offset, uint32_t addr);
    void move_word(uint8_t index, uint16_t offset, uint32_t value);

    // Vertices & Drawing
    void load_vertices(const uint8_t* rdram, uint32_t addr, uint32_t numv, uint32_t v0);
    void modify_vertex(uint32_t vtx, uint32_t where, uint32_t val);
    void draw_tri1(uint32_t v0, uint32_t v1, uint32_t v2);
    void draw_tri2(uint32_t v0, uint32_t v1, uint32_t v2, uint32_t v3, uint32_t v4, uint32_t v5);
    void draw_quad(uint32_t v0, uint32_t v1, uint32_t v2, uint32_t v3);
    void draw_tex_rect(uint32_t tile, uint32_t ulx, uint32_t uly, uint32_t lrx, uint32_t lry,
                       uint32_t uls, uint32_t ult, uint32_t dsdx, uint32_t dtdy, bool flip, const uint8_t* rdram);
    void draw_fill_rect(uint32_t ulx, uint32_t uly, uint32_t lrx, uint32_t lry);

    // Scissor & Rect Alignment
    void set_scissor(uint32_t ulx, uint32_t uly, uint32_t lrx, uint32_t lry);
    void ex_set_scissor(uint32_t ulx, uint32_t uly, uint32_t lrx, uint32_t lry);
    void ex_push_scissor();
    void ex_pop_scissor();
    void ex_set_rect_align(uint32_t origin, int32_t offset_x, int32_t offset_y);
    void ex_set_rect_align(uint32_t origin_x, uint32_t origin_y, int32_t offset_x, int32_t offset_y);
    float get_widescreen_scale() const { return widescreen_scale_; }
    float get_tallscreen_scale() const { return tallscreen_scale_; }

    void flush_batch();
    void submit_render_pass();

    struct RspStats {
        uint32_t passes = 0;
        uint32_t batches = 0;
        uint32_t triangles = 0;
        double total_submit_ms = 0.0;
    };
    RspStats get_and_reset_stats() {
        RspStats s{ pass_count_, batch_count_, tri_count_, total_submit_ms_ };
        pass_count_ = 0;
        batch_count_ = 0;
        tri_count_ = 0;
        total_submit_ms_ = 0.0;
        return s;
    }

    uint32_t get_segment(uint32_t seg_index) const { return segments_[seg_index & 0x0F]; }
    const uint32_t* get_segments() const { return segments_; }
    void set_segment(uint32_t seg_index, uint32_t addr) { segments_[seg_index & 0x0F] = addr; }

private:
    static constexpr size_t MAX_PASS_VERTICES = 32768;
    static constexpr size_t MAX_PASS_DRAWS = 512;
    static constexpr size_t PASS_RING_COUNT = 8;

    struct PassBufferSlot {
        GLuint vao = 0;
        GLuint vbo = 0;
        GLuint ibo = 0;
    };

    struct PassDrawCmd {
        CombinerProgram prog;
        Matrix4 proj{};
        Matrix4 mv{};
        float tex_scale_s = 1.0f;
        float tex_scale_t = 1.0f;
        float tex_offset_s = 0.0f;
        float tex_offset_t = 0.0f;
        float fog_mult = 0.0f;
        float fog_offset = 0.0f;
        float prim_color[4]{ 1.0f, 1.0f, 1.0f, 1.0f };
        float prim_lod_frac = 0.0f;
        float env_color[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
        float fog_color[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
        float blend_color[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
        float alpha_threshold = 0.5f;
        GLuint tex0_id = 0;
        GLuint tex1_id = 0;
        uint32_t geometry_mode = 0;
        uint32_t other_mode_h = 0;
        uint32_t other_mode_l = 0;
        ScissorRect scissor;
        uint32_t first_index = 0;
        uint32_t index_count = 0;
    };

    Matrix4 read_matrix(const uint8_t* rdram, uint32_t phys_addr) const;
    void apply_lighting(RspVertex& v, float nx, float ny, float nz) const;
    float compute_rect_shift_x() const;
    float compute_rect_shift_y() const;
    void apply_gl_scissor(const FboEntry& fbo) const;
    void apply_gl_scissor_for_rect(const FboEntry& fbo, const ScissorRect& scissor) const;
    void ensure_batch_state(GLuint tex_id, float tex_scale_s, float tex_scale_t, float tex_offset_s, float tex_offset_t);
    void submit_render_pass_internal();

    TmemTextureCache& tmem_cache_;
    CombinerShaderCache& shader_cache_;
    FboManager& fbo_manager_;

    int win_w_ = 720;
    int win_h_ = 480;
    bool expand_aspect_ = false;
    float widescreen_scale_ = 1.0f;
    float tallscreen_scale_ = 1.0f;

    Matrix4 modelview_stack_[MODELVIEW_STACK_DEPTH];
    int modelview_top_ = 0;
    Matrix4 projection_matrix_;

    RspVertex vtx_buffer_[VERTEX_CACHE_SIZE];
    uint32_t segments_[16]{};

    uint32_t geometry_mode_ = 0;
    uint32_t other_mode_h_ = 0;
    uint32_t other_mode_l_ = 0;
    uint32_t combine_w0_ = 0;
    uint32_t combine_w1_ = 0;

    uint32_t active_tile_ = 0;
    bool tex_enabled_ = false;
    float tex_scale_s_ = 1.0f;
    float tex_scale_t_ = 1.0f;

    float prim_color_[4]{ 1.0f, 1.0f, 1.0f, 1.0f };
    float prim_lod_frac_ = 0.0f;
    float env_color_[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
    float fog_color_[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
    float blend_color_[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
    uint32_t fill_color_ = 0;

    float fog_mult_ = 0.0f;
    float fog_offset_ = 0.0f;

    uint32_t num_dir_lights_ = 1;
    Light ambient_light_;
    Light dir_lights_[MAX_DIR_LIGHTS];

    ScissorRect current_scissor_{ 0, 0, 320, 240 };
    std::stack<ScissorRect> scissor_stack_;

    uint32_t rect_align_origin_x_ = G_EX_ORIGIN_NONE;
    uint32_t rect_align_origin_y_ = G_EX_ORIGIN_NONE;
    int32_t rect_align_offset_x_ = 0;
    int32_t rect_align_offset_y_ = 0;

    // 8-slot idle pass ring and command recording
    PassBufferSlot pass_ring_[PASS_RING_COUNT];
    size_t ring_cursor_ = 0;

    std::vector<BatchVertex> batch_vertices_;
    std::vector<uint16_t> batch_indices_;
    GLuint current_batch_tex_ = 0;
    float current_tex_scale_s_ = 1.0f;
    float current_tex_scale_t_ = 1.0f;
    float current_tex_offset_s_ = 0.0f;
    float current_tex_offset_t_ = 0.0f;

    std::vector<BatchVertex> pass_vertices_;
    std::vector<uint16_t> pass_indices_;
    std::vector<PassDrawCmd> pass_draws_;

    uint32_t pass_count_ = 0;
    uint32_t batch_count_ = 0;
    uint32_t tri_count_ = 0;
    double total_submit_ms_ = 0.0;
};

} // namespace snp::gles3
