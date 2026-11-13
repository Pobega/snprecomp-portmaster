#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cassert>
#include <vector>
#include <cmath>

#include "gles3_renderer_context.h"
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
    // 1) Offscreen G_SETCIMG at 0x0011B760 cleared red via G_FILLRECT (G_CYC_FILL)
    // =========================================================================
    uint32_t dl1_addr = 0x00001000u;
    uint32_t pc1 = dl1_addr;

    // G_SETCIMG: RGBA 16-bit at 0x0011B760, width 320
    emit_cmd(rdram.data(), pc1, (snp::gles3::G_SETCIMG << 24) | (snp::gles3::G_IM_FMT_RGBA << 21) | (snp::gles3::G_IM_SIZ_16b << 19) | (320 - 1), 0x0011B760u);
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

    // CI4 8x8 Texture Data (32 bytes) at 0x00004100: filled with index 1 (0x11)
    for (uint32_t i = 0; i < 32; ++i) {
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
    emit_cmd(rdram.data(), pc2, (snp::gles3::G_LOADBLOCK << 24) | (0u << 12) | 0u, (0u << 24) | (63u << 12) | 0u);
    emit_cmd(rdram.data(), pc2, (snp::gles3::G_SETTILESIZE << 24) | (0u << 12) | 0u, (0u << 24) | ((7 * 4) << 12) | (7 * 4));

    // Combiner: G_CC_DECALRGBA ((0 - 0)*0 + TEXEL0) -> d0 = 1 (G_CCMUX_TEXEL0), ad0 = 1 (G_ACMUX_TEXEL0)
    emit_cmd(rdram.data(), pc2, (snp::gles3::G_SETCOMBINE << 24) | (0u << 20) | (31u << 15) | (0u << 12) | (7u << 9) | (0u << 5) | (31u << 0),
                               (0u << 28) | (0u << 24) | (0u << 21) | (7u << 18) | (1u << 15) | (0u << 12) | (1u << 9) | (1u << 6) | (0u << 3) | (1u << 0));

    // Draw CI4 quad in bottom-right: (220, 160) to (280, 220)
    emit_cmd(rdram.data(), pc2, (snp::gles3::G_TEXRECT << 24) | ((280 * 4) << 12) | (220 * 4), ((0u) << 24) | ((220 * 4) << 12) | (160 * 4));
    emit_cmd(rdram.data(), pc2, (snp::gles3::G_RDPHALF_1 << 24), 0u);
    emit_cmd(rdram.data(), pc2, (snp::gles3::G_RDPHALF_2 << 24), (1024u << 16) | 1024u);

    // Sample 0x0011B760 FBO back onto a top-left strip via G_SETTIMG + G_TEXRECT
    emit_cmd(rdram.data(), pc2, (snp::gles3::G_SETTIMG << 24) | (snp::gles3::G_IM_FMT_RGBA << 21) | (snp::gles3::G_IM_SIZ_16b << 19) | (320 - 1), 0x0011B760u + 50 * 320 * 2);
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

    renderer->shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();

    std::printf("[Test] ALL GLES3 RENDERER TESTS PASSED SUCCESSFULLY!\n");
    return 0;
}
