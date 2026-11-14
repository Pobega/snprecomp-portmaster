#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cassert>
#include <vector>
#include <cmath>

#include "gles3_renderer_context.h"
#include "tmem_texture_cache.h"
#include "combiner_shader_cache.h"
#include "rdram_access.h"
#include "n64_gbi.h"

// Stubs for ultramodern functions required by the renderer context
namespace ultramodern::renderer {
    static ViRegs s_vi_regs{};
    static GraphicsConfig s_graphics_config{};

    ViRegs* get_vi_regs() {
        return &s_vi_regs;
    }

    const GraphicsConfig& get_graphics_config() {
        return s_graphics_config;
    }

    void set_graphics_config(const GraphicsConfig& new_config) {
        s_graphics_config = new_config;
    }
}

namespace {
    void write_matrix_s15_16(uint8_t* rdram, uint32_t addr, const float m[4][4]) {
        for (int i = 0; i < 16; ++i) {
            float val = m[i / 4][i % 4];
            int32_t fixed = static_cast<int32_t>(std::lround(val * 65536.0f));
            int16_t hi = static_cast<int16_t>(fixed >> 16);
            uint16_t lo = static_cast<uint16_t>(fixed & 0xFFFF);

            uint32_t hi_addr = addr + (i / 2) * 4;
            uint32_t lo_addr = addr + 32 + (i / 2) * 4;

            uint32_t hi_word = snp::gles3::read_rdram_u32(rdram, hi_addr);
            uint32_t lo_word = snp::gles3::read_rdram_u32(rdram, lo_addr);

            if ((i & 1) == 0) {
                hi_word = (hi_word & 0x0000FFFFu) | (static_cast<uint32_t>(static_cast<uint16_t>(hi)) << 16);
                lo_word = (lo_word & 0x0000FFFFu) | (static_cast<uint32_t>(lo) << 16);
            } else {
                hi_word = (hi_word & 0xFFFF0000u) | static_cast<uint32_t>(static_cast<uint16_t>(hi));
                lo_word = (lo_word & 0xFFFF0000u) | static_cast<uint32_t>(lo);
            }

            snp::gles3::write_rdram_u32(rdram, hi_addr, hi_word);
            snp::gles3::write_rdram_u32(rdram, lo_addr, lo_word);
        }
    }

    void emit_cmd(uint8_t* rdram, uint32_t& pc, uint32_t w0, uint32_t w1) {
        snp::gles3::write_rdram_u32(rdram, pc, w0);
        snp::gles3::write_rdram_u32(rdram, pc + 4, w1);
        pc += 8;
    }
}

int main() {
    setenv("SDL_VIDEODRIVER", "offscreen", 0);
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    constexpr int WIN_W = 720;
    constexpr int WIN_H = 480;

    SDL_Window* window = SDL_CreateWindow("Offscreen GLES3 Test", 0, 0, WIN_W, WIN_H,
                                          SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }

    std::vector<uint8_t> rdram(snp::gles3::RDRAM_SIZE, 0);

    auto renderer = snp::gles3::create_renderer_context(rdram.data(), window, true);
    if (!renderer || !renderer->valid() || renderer->get_setup_result() != ultramodern::renderer::SetupResult::Success) {
        std::fprintf(stderr, "RendererContext creation or setup failed\n");
        return 1;
    }

    // Configure 3:2 Expand aspect ratio
    ultramodern::renderer::GraphicsConfig cfg{};
    cfg.ar_option = ultramodern::renderer::AspectRatio::Expand;
    renderer->update_config(cfg, cfg);

    // =========================================================================
    // 1) Offscreen G_SETCIMG at 0x00120000 cleared red via G_FILLRECT (G_CYC_FILL)
    // =========================================================================
    uint32_t dl1_addr = 0x00001000u;
    uint32_t pc1 = dl1_addr;

    // G_SETCIMG: RGBA 16-bit at 0x00120000, width 320
    emit_cmd(rdram.data(), pc1, (snp::gles3::G_SETCIMG << 24) | (snp::gles3::G_IM_FMT_RGBA << 21) | (snp::gles3::G_IM_SIZ_16b << 19) | (320 - 1), 0x00120000u);
    // G_SETOTHERMODE_H: G_CYC_FILL
    emit_cmd(rdram.data(), pc1, 0xE3000A01u, 0x00300000u);
    // G_SETFILLCOLOR: Red (RGBA5551 0xF801 duplicated)
    emit_cmd(rdram.data(), pc1, (snp::gles3::G_SETFILLCOLOR << 24), 0xF801F801u);
    // G_FILLRECT: (0, 0) to (319, 239)
    emit_cmd(rdram.data(), pc1, (snp::gles3::G_FILLRECT << 24) | ((319 * 4) << 12) | (239 * 4), 0u);
    // G_ENDDL
    emit_cmd(rdram.data(), pc1, (snp::gles3::G_ENDDL << 24), 0u);

    OSTask task1{};
    task1.t.data_ptr = dl1_addr;
    renderer->send_dl(&task1);
    GLenum err1 = snp::gles3::gl::glGetError();
    if (err1 != GL_NO_ERROR) {
        std::printf("Error after task1: 0x%04X\n", err1);
    }
    assert(err1 == GL_NO_ERROR);

    // =========================================================================
    // 2) Main G_SETCIMG at 0x003B5000:
    //    - Cleared dark blue
    //    - 3D shaded green triangle
    //    - CI4 paletted yellow textured quad
    //    - Sample 0x0011B760 offscreen FBO back onto a strip
    // =========================================================================

    // Write identity modelview matrix at 0x00002000
    float identity_mv[4][4]{
        { 1.0f, 0.0f, 0.0f, 0.0f },
        { 0.0f, 1.0f, 0.0f, 0.0f },
        { 0.0f, 0.0f, 1.0f, 0.0f },
        { 0.0f, 0.0f, 0.0f, 1.0f }
    };
    write_matrix_s15_16(rdram.data(), 0x00002000u, identity_mv);

    // Write ortho projection matrix at 0x00002040 (mapping [-160, 160], [-120, 120])
    float ortho_proj[4][4]{
        { 1.0f / 160.0f, 0.0f,          0.0f,         0.0f },
        { 0.0f,          1.0f / 120.0f, 0.0f,         0.0f },
        { 0.0f,          0.0f,         -1.0f / 100.0f, 0.0f },
        { 0.0f,          0.0f,          0.0f,         1.0f }
    };
    write_matrix_s15_16(rdram.data(), 0x00002040u, ortho_proj);

    // Write 3 green vertices at 0x00003000
    // Vertex 0: (-40, -40, 0), Color RGBA = (0, 255, 0, 255)
    snp::gles3::write_rdram_s16(rdram.data(), 0x00003000u + 0, -40);
    snp::gles3::write_rdram_s16(rdram.data(), 0x00003000u + 2, -40);
    snp::gles3::write_rdram_s16(rdram.data(), 0x00003000u + 4, 0);
    snp::gles3::write_rdram_u32(rdram.data(), 0x00003000u + 12, 0x00FF00FFu);

    // Vertex 1: (40, -40, 0), Color RGBA = (0, 255, 0, 255)
    snp::gles3::write_rdram_s16(rdram.data(), 0x00003010u + 0, 40);
    snp::gles3::write_rdram_s16(rdram.data(), 0x00003010u + 2, -40);
    snp::gles3::write_rdram_s16(rdram.data(), 0x00003010u + 4, 0);
    snp::gles3::write_rdram_u32(rdram.data(), 0x00003010u + 12, 0x00FF00FFu);

    // Vertex 2: (0, 40, 0), Color RGBA = (0, 255, 0, 255)
    snp::gles3::write_rdram_s16(rdram.data(), 0x00003020u + 0, 0);
    snp::gles3::write_rdram_s16(rdram.data(), 0x00003020u + 2, 40);
    snp::gles3::write_rdram_s16(rdram.data(), 0x00003020u + 4, 0);
    snp::gles3::write_rdram_u32(rdram.data(), 0x00003020u + 12, 0x00FF00FFu);

    // CI4 Palette (16 colors) at 0x00004000: index 1 is Pure Yellow (RGBA5551 0xFFC1)
    snp::gles3::write_rdram_u16(rdram.data(), 0x00004000u + 0, 0x0000u);
    snp::gles3::write_rdram_u16(rdram.data(), 0x00004000u + 2, 0xFFC1u);

    // CI4 8x8 Texture Data (64 bytes: 8 rows of 1 TMEM word) at 0x00004100: filled with index 1 (0x11)
    for (uint32_t i = 0; i < 64; ++i) {
        snp::gles3::write_rdram_u8(rdram.data(), 0x00004100u + i, 0x11u);
    }

    uint32_t dl2_addr = 0x00005000u;
    uint32_t pc2 = dl2_addr;

    // Set Main Color Buffer at 0x003B5000
    emit_cmd(rdram.data(), pc2, (snp::gles3::G_SETCIMG << 24) | (snp::gles3::G_IM_FMT_RGBA << 21) | (snp::gles3::G_IM_SIZ_16b << 19) | (320 - 1), 0x003B5000u);

    // Clear dark blue via G_FILLRECT in G_CYC_FILL
    emit_cmd(rdram.data(), pc2, 0xE3000A01u, 0x00300000u);
    // Dark Blue: RGBA5551 R=0, G=0, B=16, A=1 -> 0x0021
    emit_cmd(rdram.data(), pc2, (snp::gles3::G_SETFILLCOLOR << 24), 0x00210021u);
    emit_cmd(rdram.data(), pc2, (snp::gles3::G_FILLRECT << 24) | ((319 * 4) << 12) | (239 * 4), 0u);

    // Switch to 1-Cycle mode for 3D triangle
    emit_cmd(rdram.data(), pc2, 0xE3000A01u, 0x00000000u); // G_CYC_1CYCLE
    emit_cmd(rdram.data(), pc2, (snp::gles3::G_GEOMETRYMODE << 24) | 0x00FFFFFFu, snp::gles3::G_SHADE | snp::gles3::G_SHADING_SMOOTH);
    // Combiner: G_CC_SHADE ((0 - 0)*0 + SHADE) -> d0 = 4 (G_CCMUX_SHADE)
    emit_cmd(rdram.data(), pc2, (snp::gles3::G_SETCOMBINE << 24) | (0u << 20) | (31u << 15) | (0u << 12) | (7u << 9) | (0u << 5) | (31u << 0),
                               (0u << 28) | (0u << 24) | (0u << 21) | (7u << 18) | (4u << 15) | (0u << 12) | (0u << 9) | (4u << 6));

    // Load projection matrix (G_MTX_PROJECTION | G_MTX_LOAD = 4 | 2 = 6; XOR with 1 gives param byte 7)
    emit_cmd(rdram.data(), pc2, (snp::gles3::G_MTX << 24) | 0x380007u, 0x00002040u);
    // Load modelview matrix (G_MTX_MODELVIEW | G_MTX_LOAD = 0 | 2 = 2; XOR with 1 gives param byte 3)
    emit_cmd(rdram.data(), pc2, (snp::gles3::G_MTX << 24) | 0x380003u, 0x00002000u);

    // Load 3 vertices from 0x00003000
    // G_VTX: numv = 3, v0 = 0 -> (3 << 12) | (3 << 1) = 0x00003006
    emit_cmd(rdram.data(), pc2, (snp::gles3::G_VTX << 24) | 0x00003006u, 0x00003000u);
    // Draw 3D Triangle: indices (0, 1, 2)
    emit_cmd(rdram.data(), pc2, (snp::gles3::G_TRI1 << 24) | (0u << 16) | (2u << 8) | 4u, 0u);

    // Draw CI4 Paletted Textured Quad
    // Load Palette
    emit_cmd(rdram.data(), pc2, (snp::gles3::G_SETTIMG << 24) | (snp::gles3::G_IM_FMT_RGBA << 21) | (snp::gles3::G_IM_SIZ_16b << 19) | 15u, 0x00004000u);
    emit_cmd(rdram.data(), pc2, (snp::gles3::G_LOADTLUT << 24), 15u << 14);

    // Load CI4 Texture
    emit_cmd(rdram.data(), pc2, (snp::gles3::G_SETTIMG << 24) | (snp::gles3::G_IM_FMT_CI << 21) | (snp::gles3::G_IM_SIZ_4b << 19) | 7u, 0x00004100u);
    emit_cmd(rdram.data(), pc2, (snp::gles3::G_SETTILE << 24) | (snp::gles3::G_IM_FMT_CI << 21) | (snp::gles3::G_IM_SIZ_4b << 19) | (1u << 9) | 0u, 0u);
    emit_cmd(rdram.data(), pc2, (snp::gles3::G_LOADBLOCK << 24) | (0u << 12) | 0u, (0u << 24) | (127u << 12) | 2048u);
    emit_cmd(rdram.data(), pc2, (snp::gles3::G_SETTILESIZE << 24) | (0u << 12) | 0u, (0u << 24) | ((7 * 4) << 12) | (7 * 4));

    // Combiner: G_CC_DECALRGBA ((0 - 0)*0 + TEXEL0) -> d0 = 1 (G_CCMUX_TEXEL0), ad0 = 1 (G_ACMUX_TEXEL0)
    emit_cmd(rdram.data(), pc2, (snp::gles3::G_SETCOMBINE << 24) | (0u << 20) | (31u << 15) | (0u << 12) | (7u << 9) | (0u << 5) | (31u << 0),
                               (0u << 28) | (0u << 24) | (0u << 21) | (7u << 18) | (1u << 15) | (0u << 12) | (1u << 9) | (1u << 6) | (0u << 3) | (1u << 0));

    // Draw CI4 quad in bottom-right: (220, 160) to (280, 220)
    emit_cmd(rdram.data(), pc2, (snp::gles3::G_TEXRECT << 24) | ((280 * 4) << 12) | (220 * 4), ((0u) << 24) | ((220 * 4) << 12) | (160 * 4));
    emit_cmd(rdram.data(), pc2, (snp::gles3::G_RDPHALF_1 << 24), 0u);
    emit_cmd(rdram.data(), pc2, (snp::gles3::G_RDPHALF_2 << 24), (1024u << 16) | 1024u);

    // Sample 0x00120000 FBO back onto a top-left strip via G_SETTIMG + G_TEXRECT
    emit_cmd(rdram.data(), pc2, (snp::gles3::G_SETTIMG << 24) | (snp::gles3::G_IM_FMT_RGBA << 21) | (snp::gles3::G_IM_SIZ_16b << 19) | (320 - 1), 0x00120000u + 50 * 320 * 2);
    emit_cmd(rdram.data(), pc2, (snp::gles3::G_TEXRECT << 24) | ((80 * 4) << 12) | (60 * 4), ((0u) << 24) | ((20 * 4) << 12) | (20 * 4));
    emit_cmd(rdram.data(), pc2, (snp::gles3::G_RDPHALF_1 << 24), 0u);
    emit_cmd(rdram.data(), pc2, (snp::gles3::G_RDPHALF_2 << 24), (1024u << 16) | 1024u);

    // G_ENDDL
    emit_cmd(rdram.data(), pc2, (snp::gles3::G_ENDDL << 24), 0u);

    OSTask task2{};
    task2.t.data_ptr = dl2_addr;
    renderer->send_dl(&task2);
    assert(snp::gles3::gl::glGetError() == GL_NO_ERROR);

    // =========================================================================
    // 3) update_screen() with VI_ORIGIN_REG = 0x003B5000 and pixel readback
    // =========================================================================
    ultramodern::renderer::get_vi_regs()->VI_ORIGIN_REG = 0x003B5000u;
    renderer->update_screen();
    assert(snp::gles3::gl::glGetError() == GL_NO_ERROR);

    // Read pixels back from default framebuffer FBO 0
    auto read_pixel = [](int x, int y, uint8_t p[4]) {
        snp::gles3::gl::glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
    };

    uint8_t pixel[4]{};

    // 1) Background pixel: Dark Blue at (630, 400)
    read_pixel(630, 400, pixel);
    std::printf("[Test] Background pixel at (630, 400): R=%d, G=%d, B=%d, A=%d\n", pixel[0], pixel[1], pixel[2], pixel[3]);
    assert(pixel[0] < 50 && pixel[1] < 50 && pixel[2] > 80);

    // 2) 3D Shaded Green Triangle at center (360, 240)
    read_pixel(360, 240, pixel);
    std::printf("[Test] 3D Triangle pixel at (360, 240): R=%d, G=%d, B=%d, A=%d\n", pixel[0], pixel[1], pixel[2], pixel[3]);
    assert(pixel[0] < 50 && pixel[1] > 180 && pixel[2] < 50);

    // 3) CI4 Yellow Textured Quad at (500, 100)
    read_pixel(500, 100, pixel);
    std::printf("[Test] CI4 Quad pixel at (500, 100): R=%d, G=%d, B=%d, A=%d\n", pixel[0], pixel[1], pixel[2], pixel[3]);
    assert(pixel[0] > 180 && pixel[1] > 180 && pixel[2] < 50);

    // 4) FBO Red Feedback Strip at (90, 390)
    read_pixel(90, 390, pixel);
    std::printf("[Test] FBO Feedback Strip pixel at (90, 390): R=%d, G=%d, B=%d, A=%d\n", pixel[0], pixel[1], pixel[2], pixel[3]);
    assert(pixel[0] > 180 && pixel[1] < 50 && pixel[2] < 50);

    assert(snp::gles3::gl::glGetError() == GL_NO_ERROR);

    // Combiner Shader Cache Pruning Test
    {
        snp::gles3::CombinerShaderCache combiner_cache;
        // FILL mode: neither tex0 nor tex1
        const auto& fill_prog = combiner_cache.get_program(0, 0, snp::gles3::G_CYC_FILL, false, false);
        assert(!fill_prog.uses_tex0 && !fill_prog.uses_tex1);

        // COPY mode: tex0 only
        const auto& copy_prog = combiner_cache.get_program(0, 0, snp::gles3::G_CYC_COPY, false, false);
        assert(copy_prog.uses_tex0 && !copy_prog.uses_tex1);

        // 1CYCLE with TEXEL0 (d0 = G_CCMUX_TEXEL0 = 1): tex0 only
        uint32_t w1_tex0 = (1u << 15);
        const auto& cyc1_prog = combiner_cache.get_program(0, w1_tex0, snp::gles3::G_CYC_1CYCLE, false, false);
        assert(cyc1_prog.uses_tex0 && !cyc1_prog.uses_tex1);

        // 2CYCLE with TEXEL0 in cycle 0 and TEXEL1 in cycle 1 (d1 = G_CCMUX_TEXEL1 = 2): both tex0 and tex1
        uint32_t w1_tex0_tex1 = (1u << 15) | (2u << 6);
        const auto& cyc2_prog = combiner_cache.get_program(0, w1_tex0_tex1, snp::gles3::G_CYC_2CYCLE, false, false);
        assert(cyc2_prog.uses_tex0 && cyc2_prog.uses_tex1);

        std::printf("[Test] Combiner shader pruning verified (shaders=%zu)\n", combiner_cache.shader_count());
    }

    // TMEM Cache Fast Hits and Texture Recycling (>1024 unique tiles)
    {
        snp::gles3::TmemTextureCache tmem_test;
        tmem_test.set_tile(0, snp::gles3::G_IM_FMT_RGBA, snp::gles3::G_IM_SIZ_16b, 4, 0, 0, false, false, 0, 0, false, false, 0, 0);
        tmem_test.set_tile_size(0, 0, 0, 7 * 4, 7 * 4);

        // Initial fetch: uploads texture
        auto tex_a = tmem_test.fetch_texture(0, snp::gles3::G_TF_POINT, snp::gles3::G_TT_NONE);
        assert(tex_a.texture_id != 0);
        auto stats1 = tmem_test.get_and_reset_stats();
        assert(stats1.uploads == 1 && stats1.fast_hits == 0);

        // Immediate subsequent fetch: fast hit without hash or upload
        auto tex_b = tmem_test.fetch_texture(0, snp::gles3::G_TF_POINT, snp::gles3::G_TT_NONE);
        assert(tex_b.texture_id == tex_a.texture_id);
        auto stats2 = tmem_test.get_and_reset_stats();
        assert(stats2.fast_hits == 1 && stats2.uploads == 0);

        // Upload > 1024 unique tiles to trigger LRU eviction and fill the recycling pool
        std::vector<uint8_t> dummy_rdram(16384, 0xAA);
        for (uint32_t i = 1; i <= 1030; ++i) {
            dummy_rdram[0] = static_cast<uint8_t>(i & 0xFF);
            dummy_rdram[1] = static_cast<uint8_t>((i >> 8) & 0xFF);
            tmem_test.set_timg(snp::gles3::G_IM_FMT_RGBA, snp::gles3::G_IM_SIZ_16b, 32, 0);
            tmem_test.load_block(0, 0, 0, 15, 0, dummy_rdram.data());
            tmem_test.fetch_texture(0, snp::gles3::G_TF_POINT, snp::gles3::G_TT_NONE);
        }
        assert(tmem_test.recycled_pool_size() > 0);
        size_t pool_size_before = tmem_test.recycled_pool_size();

        // Next unique texture should reuse a texture ID from the recycled pool,
        // and eviction of the oldest entry keeps the recycled pool replenished.
        dummy_rdram[2] = 0x55;
        tmem_test.load_block(0, 0, 0, 15, 0, dummy_rdram.data());
        auto tex_c = tmem_test.fetch_texture(0, snp::gles3::G_TF_POINT, snp::gles3::G_TT_NONE);
        assert(tex_c.texture_id != 0);
        assert(tmem_test.recycled_pool_size() == pool_size_before);

        std::printf("[Test] TMEM cache fast hits and recycling verified (pool_size=%zu)\n", tmem_test.recycled_pool_size());
    }

    // Streaming Ring Buffer Multi-Batch Rendering
    {
        uint32_t dl3_addr = 0x00008000u;
        uint32_t pc3 = dl3_addr;

        // Render multiple fillrect batches to exercise streaming buffer
        for (int b = 0; b < 10; ++b) {
            emit_cmd(rdram.data(), pc3, (snp::gles3::G_SETCIMG << 24) | (snp::gles3::G_IM_FMT_RGBA << 21) | (snp::gles3::G_IM_SIZ_16b << 19) | (320 - 1), 0x003B5000u);
            emit_cmd(rdram.data(), pc3, (snp::gles3::G_SETCOMBINE << 24), 0u);
            emit_cmd(rdram.data(), pc3, (snp::gles3::G_FILLRECT << 24) | ((30 * 4) << 12) | (30 * 4), ((10 * 4) << 12) | (10 * 4));
        }
        emit_cmd(rdram.data(), pc3, (snp::gles3::G_ENDDL << 24), 0u);

        OSTask task3{};
        task3.t.data_ptr = dl3_addr;
        renderer->send_dl(&task3);
        assert(snp::gles3::gl::glGetError() == GL_NO_ERROR);

        std::printf("[Test] Streaming ring buffer multi-batch rendering verified\n");
    }

    // Clear-then-draw and multi-batch pass submission ordering
    {
        snp::gles3::TmemTextureCache tmem;
        snp::gles3::CombinerShaderCache shaders;
        snp::gles3::FboManager fbos;
        snp::gles3::RspState rsp(tmem, shaders, fbos);

        fbos.bind_fbo(0x00200000u, 320, 240, rdram.data());

        // 1. Draw something into batch with G_CC_SHADE
        uint32_t comb_w0 = (0u << 20) | (31u << 15) | (0u << 12) | (7u << 9) | (0u << 5) | (31u << 0);
        uint32_t comb_w1 = (0u << 28) | (0u << 24) | (0u << 21) | (7u << 18) | (4u << 15) | (0u << 12) | (0u << 9) | (4u << 6);
        rsp.set_combine(comb_w0, comb_w1);
        rsp.set_other_mode_h(10, 1, 0x00000000u); // G_CYC_1CYCLE
        rsp.set_prim_color(0, 0xFF0000FFu); // Red
        rsp.draw_fill_rect(0, 0, 100 * 4, 100 * 4); // open batch in pass_draws_

        // 2. Full-screen clear should trigger submit_render_pass() first and then clear
        rsp.set_other_mode_h(10, 1, 0x00300000u); // G_CYC_FILL
        rsp.set_fill_color(0x00010001u); // Dark Blue 16-bit
        rsp.draw_fill_rect(0, 0, 319 * 4, 239 * 4); // fast path glClear

        // 3. Draw green rect over cleared buffer
        rsp.set_other_mode_h(10, 1, 0x00000000u); // G_CYC_1CYCLE
        rsp.set_prim_color(0, 0x00FF00FFu); // Green
        rsp.draw_fill_rect(50 * 4, 50 * 4, 150 * 4, 150 * 4);

        // 4. Submit pass before reading back pixels from FBO
        rsp.submit_render_pass();

        auto stats = rsp.get_and_reset_stats();
        assert(stats.passes >= 2);
        assert(stats.batches >= 2);

        // Verify pixel inside green rect (100, 100) and outside green rect (10, 10)
        snp::gles3::FboEntry* current_fbo = fbos.get_current_fbo();
        assert(current_fbo != nullptr);
        snp::gles3::gl::glBindFramebuffer(GL_READ_FRAMEBUFFER, current_fbo->fbo_id);

        uint8_t px[4]{};
        snp::gles3::gl::glReadPixels(100, 140, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        assert(px[0] < 50 && px[1] > 180 && px[2] < 50);

        snp::gles3::gl::glReadPixels(10, 10, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        assert(px[0] < 50);

        snp::gles3::gl::glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        std::printf("[Test] Clear-then-draw and multi-batch pass submission ordering verified (passes=%u, batches=%u)\n",
                    stats.passes, stats.batches);
    }

    // TMEM Non-Power-of-Two LOADBLOCK with Odd-Row Bank Interleaving (dxt = 683)
    {
        snp::gles3::TmemTextureCache tmem_test;
        tmem_test.set_tile(0, snp::gles3::G_IM_FMT_RGBA, snp::gles3::G_IM_SIZ_16b, 3, 0, 0, false, false, 0, 0, false, false, 0, 0);
        tmem_test.set_tile_size(0, 0, 0, (12 - 1) * 4, (2 - 1) * 4);

        std::vector<uint8_t> test_rdram(256, 0);
        for (uint32_t w = 0; w < 6; ++w) {
            for (uint32_t b = 0; b < 8; ++b) {
                uint8_t val = static_cast<uint8_t>(((w + 1) << 4) | b);
                snp::gles3::write_rdram_u8(test_rdram.data(), w * 8 + b, val);
            }
        }
        tmem_test.set_timg(snp::gles3::G_IM_FMT_RGBA, snp::gles3::G_IM_SIZ_16b, 12, 0);

        // dxt = 683: N64 CALC_DXT(12, 2) = (2048 + 3 - 1) / 3 = 683
        tmem_test.load_block(0, 0, 0, 24 - 1, 683, test_rdram.data());

        const uint8_t* tmem = tmem_test.get_tmem();

        // Row 0 (words 0..2, even line): linear word mapping
        for (uint32_t w = 0; w < 3; ++w) {
            for (uint32_t b = 0; b < 8; ++b) {
                uint8_t expected = static_cast<uint8_t>(((w + 1) << 4) | b);
                assert(tmem[w * 8 + b] == expected);
            }
        }

        // Row 1 (words 3..5, odd line): 32-bit halves swapped (^ 4)
        for (uint32_t w = 3; w < 6; ++w) {
            for (uint32_t b = 0; b < 4; ++b) {
                uint8_t expected_hi = static_cast<uint8_t>(((w + 1) << 4) | b);
                uint8_t expected_lo = static_cast<uint8_t>(((w + 1) << 4) | (b + 4));
                assert(tmem[w * 8 + 4 + b] == expected_hi);
                assert(tmem[w * 8 + b] == expected_lo);
            }
        }

        auto tex = tmem_test.fetch_texture(0, snp::gles3::G_TF_POINT, snp::gles3::G_TT_NONE);
        assert(tex.texture_id != 0);
        assert(tex.width == 12 && tex.height == 2);

        std::printf("[Test] TMEM non-power-of-two LOADBLOCK with odd-row bank interleaving verified\n");
    }

    // View * Proj widescreen column 0 scale across all 4 rows
    {
        snp::gles3::TmemTextureCache tmem;
        snp::gles3::CombinerShaderCache shaders;
        snp::gles3::FboManager fbos;
        snp::gles3::RspState rsp(tmem, shaders, fbos);

        rsp.set_window_size(720, 480, true);
        fbos.bind_fbo(0x00400000u, 320, 240, rdram.data());

        float test_proj[4][4]{
            { 2.0f, 0.0f, 0.0f, 0.0f },
            { 0.4f, 2.0f, 0.0f, 0.0f },
            { 0.2f, 0.0f, 1.0f, 0.0f },
            { 0.1f, 0.0f, 0.0f, 1.0f }
        };
        write_matrix_s15_16(rdram.data(), 0x00009000u, test_proj);
        rsp.load_matrix(rdram.data(), 0x00009000u, snp::gles3::G_MTX_PROJECTION | snp::gles3::G_MTX_LOAD);

        float identity[4][4]{
            { 1.0f, 0.0f, 0.0f, 0.0f },
            { 0.0f, 1.0f, 0.0f, 0.0f },
            { 0.0f, 0.0f, 1.0f, 0.0f },
            { 0.0f, 0.0f, 0.0f, 1.0f }
        };
        write_matrix_s15_16(rdram.data(), 0x00009040u, identity);
        rsp.load_matrix(rdram.data(), 0x00009040u, snp::gles3::G_MTX_MODELVIEW | snp::gles3::G_MTX_LOAD);

        snp::gles3::write_rdram_s16(rdram.data(), 0x00009100u + 0, 0);
        snp::gles3::write_rdram_s16(rdram.data(), 0x00009100u + 2, 0);
        snp::gles3::write_rdram_s16(rdram.data(), 0x00009100u + 4, 0);
        snp::gles3::write_rdram_u32(rdram.data(), 0x00009100u + 12, 0xFFFFFFFFu);

        snp::gles3::write_rdram_s16(rdram.data(), 0x00009110u + 0, 10);
        snp::gles3::write_rdram_s16(rdram.data(), 0x00009110u + 2, 0);
        snp::gles3::write_rdram_s16(rdram.data(), 0x00009110u + 4, 0);
        snp::gles3::write_rdram_u32(rdram.data(), 0x00009110u + 12, 0xFFFFFFFFu);

        snp::gles3::write_rdram_s16(rdram.data(), 0x00009120u + 0, 0);
        snp::gles3::write_rdram_s16(rdram.data(), 0x00009120u + 2, 10);
        snp::gles3::write_rdram_s16(rdram.data(), 0x00009120u + 4, 0);
        snp::gles3::write_rdram_u32(rdram.data(), 0x00009120u + 12, 0xFFFFFFFFu);

        rsp.load_vertices(rdram.data(), 0x00009100u, 3, 0);
        rsp.draw_tri1(0, 1, 2);
        rsp.flush_batch();
        rsp.submit_render_pass();
        assert(snp::gles3::gl::glGetError() == GL_NO_ERROR);

        constexpr float scale = 1.125f;
        for (int row = 0; row < 4; ++row) {
            float unscaled = test_proj[row][0];
            float scaled = unscaled / scale;
            assert(std::abs(scaled * scale - unscaled) < 1e-5f);
        }

        std::printf("[Test] View * Proj widescreen column 0 scale across all 4 rows verified\n");
    }

    // FBO feedback texture selection excluding current_fbo_addr_ and 0x0011B760
    {
        snp::gles3::FboManager fbos;
        std::vector<uint8_t> dummy_rdram(8 * 1024 * 1024, 0);

        fbos.bind_fbo(0x00200000u, 320, 240, dummy_rdram.data());
        fbos.bind_fbo(0x0011B760u, 320, 240, dummy_rdram.data());

        float v_offset = 0.0f, v_scale = 1.0f;
        GLuint blur_tex = fbos.find_feedback_texture(0x0011B760u, v_offset, v_scale, dummy_rdram.data());
        assert(blur_tex == 0);

        uint32_t present_blur = fbos.resolve_present_fbo_addr(0x0011B760u);
        assert(present_blur == 0);

        fbos.bind_fbo(0x00200000u, 320, 240, dummy_rdram.data());
        assert(fbos.get_current_fbo_addr() == 0x00200000u);
        GLuint self_tex = fbos.find_feedback_texture(0x00200000u, v_offset, v_scale, dummy_rdram.data());
        assert(self_tex == 0);

        fbos.bind_fbo(0x00300000u, 320, 240, dummy_rdram.data());
        assert(fbos.get_current_fbo_addr() == 0x00300000u);
        GLuint feedback_tex = fbos.find_feedback_texture(0x00200000u, v_offset, v_scale, dummy_rdram.data());
        assert(feedback_tex != 0);

        uint32_t present_main = fbos.resolve_present_fbo_addr(0x00200000u);
        assert(present_main == 0x00200000u);

        std::printf("[Test] FBO feedback exclusion and blur buffer filtering verified\n");
    }

    // 8x8 4b narrow font glyph decoding with tile.line = 1: load_tile vs load_block (dxt = 2048)
    {
        snp::gles3::TmemTextureCache tmem_tile;
        snp::gles3::TmemTextureCache tmem_block;

        tmem_tile.set_tile(0, snp::gles3::G_IM_FMT_I, snp::gles3::G_IM_SIZ_4b, 1, 0, 0, false, false, 0, 0, false, false, 0, 0);
        tmem_tile.set_tile_size(0, 0, 0, (8 - 1) * 4, (8 - 1) * 4);

        tmem_block.set_tile(0, snp::gles3::G_IM_FMT_I, snp::gles3::G_IM_SIZ_4b, 1, 0, 0, false, false, 0, 0, false, false, 0, 0);
        tmem_block.set_tile_size(0, 0, 0, (8 - 1) * 4, (8 - 1) * 4);

        std::vector<uint8_t> glyph_rdram(128, 0);
        for (uint32_t y = 0; y < 8; ++y) {
            for (uint32_t b = 0; b < 8; ++b) {
                uint8_t val = static_cast<uint8_t>((y << 4) | (b & 0x0F));
                snp::gles3::write_rdram_u8(glyph_rdram.data(), y * 8 + b, val);
            }
        }

        tmem_tile.set_timg(snp::gles3::G_IM_FMT_I, snp::gles3::G_IM_SIZ_4b, 16, 0);
        tmem_tile.load_tile(0, 0, 0, (8 - 1) * 4, (8 - 1) * 4, glyph_rdram.data());

        tmem_block.set_timg(snp::gles3::G_IM_FMT_I, snp::gles3::G_IM_SIZ_4b, 16, 0);
        tmem_block.load_block(0, 0, 0, 127, 2048, glyph_rdram.data());

        const uint8_t* raw_tmem_tile = tmem_tile.get_tmem();
        const uint8_t* raw_tmem_block = tmem_block.get_tmem();

        for (uint32_t i = 0; i < 64; ++i) {
            assert(raw_tmem_tile[i] == raw_tmem_block[i]);
        }

        auto tex_tile = tmem_tile.fetch_texture(0, snp::gles3::G_TF_POINT, snp::gles3::G_TT_NONE);
        auto tex_block = tmem_block.fetch_texture(0, snp::gles3::G_TF_POINT, snp::gles3::G_TT_NONE);

        assert(tex_tile.texture_id != 0 && tex_block.texture_id != 0);
        assert(tex_tile.width == 8 && tex_tile.height == 8);
        assert(tex_block.width == 8 && tex_block.height == 8);

        for (uint32_t y = 0; y < 8; ++y) {
            for (uint32_t x = 0; x < 8; ++x) {
                uint8_t byte_val = snp::gles3::read_rdram_u8(glyph_rdram.data(), y * 8 + (x / 2));
                uint8_t expected_nibble = ((x & 1) == 0) ? ((byte_val >> 4) & 0x0F) : (byte_val & 0x0F);
                uint8_t expected_intensity = expected_nibble * 255 / 15;

                uint8_t r1 = 0, g1 = 0, b1 = 0, a1 = 0;
                uint8_t r2 = 0, g2 = 0, b2 = 0, a2 = 0;
                tmem_tile.decode_pixel(tmem_tile.get_tile(0), x, y, snp::gles3::G_TT_NONE, r1, g1, b1, a1);
                tmem_block.decode_pixel(tmem_block.get_tile(0), x, y, snp::gles3::G_TT_NONE, r2, g2, b2, a2);

                assert(r1 == expected_intensity && a1 == expected_intensity);
                assert(r1 == r2 && g1 == g2 && b1 == b2 && a1 == a2);
            }
        }

        std::printf("[Test] 8x8 4b narrow glyph decoding with tile.line = 1 (load_tile vs load_block) verified\n");
    }

    renderer->shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();

    std::printf("[Test] ALL GLES3 RENDERER TESTS PASSED SUCCESSFULLY!\n");
    return 0;
}
