#include "gles3_renderer_context.h"
#include "rdram_access.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>
#include <atomic>

namespace snp::gles3 {

namespace {
OverlayRenderCallback s_overlay_render_callback = nullptr;
OverlayActiveCallback s_overlay_active_callback = nullptr;
std::atomic<int> s_target_swap_interval{ 1 };
} // namespace

void set_overlay_render_callback(OverlayRenderCallback callback) {
    s_overlay_render_callback = callback;
}

void set_overlay_active_callback(OverlayActiveCallback callback) {
    s_overlay_active_callback = callback;
}

void set_vsync_enabled(bool enabled) {
    s_target_swap_interval.store(enabled ? 1 : 0);
}

Gles3RendererContext::Gles3RendererContext(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode)
    : rdram_(rdram), window_handle_(window_handle), developer_mode_(developer_mode) {
    setup_result = ultramodern::renderer::SetupResult::GraphicsAPINotFound;
    chosen_api = ultramodern::renderer::GraphicsApi::Auto;

    const char* dump_env = std::getenv("SNP_DUMP_FRAMES");
    if (dump_env) {
        std::string s(dump_env);
        size_t start = 0;
        while (start < s.size()) {
            size_t comma = s.find(',', start);
            if (comma == std::string::npos) comma = s.size();
            std::string token = s.substr(start, comma - start);
            if (!token.empty()) {
                dump_frames_.push_back(std::stoull(token));
            }
            start = comma + 1;
        }
    }
    const char* quit_env = std::getenv("SNP_AUTO_QUIT_DLS");
    if (quit_env) {
        auto_quit_dls_ = std::stoll(quit_env);
    }
    const char* dump_single = std::getenv("SNP_DUMP_SCREEN_PPM");
    if (dump_single) {
        try {
            uint64_t f = std::stoull(dump_single);
            dump_frames_.push_back(f > 0 ? f : 1);
        } catch (...) {
            dump_frames_.push_back(1);
        }
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

    gl_ctx_ = SDL_GL_CreateContext(window_handle_);
    if (!gl_ctx_) {
        std::fprintf(stderr, "[GLES3] SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        return;
    }

    if (SDL_GL_MakeCurrent(window_handle_, gl_ctx_) != 0) {
        std::fprintf(stderr, "[GLES3] SDL_GL_MakeCurrent failed: %s\n", SDL_GetError());
        return;
    }

    if (!init_gl_loader()) {
        std::fprintf(stderr, "[GLES3] Failed to load OpenGL ES 3.0 functions\n");
        return;
    }

    const char* gl_vendor = reinterpret_cast<const char*>(gl::glGetString(GL_VENDOR));
    const char* gl_renderer = reinterpret_cast<const char*>(gl::glGetString(GL_RENDERER));
    const char* gl_version = reinterpret_cast<const char*>(gl::glGetString(GL_VERSION));
    std::fprintf(stderr, "[GLES3] GL_VENDOR=%s | GL_RENDERER=%s | GL_VERSION=%s\n",
                 gl_vendor ? gl_vendor : "Unknown",
                 gl_renderer ? gl_renderer : "Unknown",
                 gl_version ? gl_version : "Unknown");

    SDL_GetWindowSize(window_handle_, &win_w_, &win_h_);
    if (win_w_ <= 0) win_w_ = 720;
    if (win_h_ <= 0) win_h_ = 480;

    render_w_ = win_w_;
    render_h_ = win_h_;

    tmem_cache_ = std::make_unique<TmemTextureCache>();
    shader_cache_ = std::make_unique<CombinerShaderCache>();
    fbo_manager_ = std::make_unique<FboManager>();
    rsp_state_ = std::make_unique<RspState>(*tmem_cache_, *shader_cache_, *fbo_manager_);

    rsp_state_->set_window_size(win_w_, win_h_, expand_aspect_);

    setup_result = ultramodern::renderer::SetupResult::Success;
}

Gles3RendererContext::~Gles3RendererContext() {
    shutdown();
}

bool Gles3RendererContext::valid() {
    return gl_ctx_ != nullptr;
}

bool Gles3RendererContext::update_config(const ultramodern::renderer::GraphicsConfig& old_config,
                                         const ultramodern::renderer::GraphicsConfig& new_config) {
    (void)old_config;
    expand_aspect_ = (new_config.ar_option == ultramodern::renderer::AspectRatio::Expand);
    SDL_GetWindowSize(window_handle_, &win_w_, &win_h_);
    render_w_ = win_w_;
    render_h_ = win_h_;
    if (rsp_state_) {
        rsp_state_->set_window_size(win_w_, win_h_, expand_aspect_);
    }
    return true;
}

void Gles3RendererContext::enable_instant_present() {
    SDL_GL_SetSwapInterval(0);
}

void Gles3RendererContext::send_dummy_workload(uint32_t fb_address) {
    (void)fb_address;
    if (dl_count_ == 0) {
        uint32_t now = SDL_GetTicks();
        if (now - last_dummy_ticks_ >= 33) {
            frame_dirty_ = true;
            last_dummy_ticks_ = now;
        }
    }
}

void Gles3RendererContext::shutdown() {
    if (gl_ctx_) {
        rsp_state_.reset();
        fbo_manager_.reset();
        shader_cache_.reset();
        tmem_cache_.reset();
        SDL_GL_DeleteContext(gl_ctx_);
        gl_ctx_ = nullptr;
    }
}

uint32_t Gles3RendererContext::get_display_framerate() const {
    return 60;
}

float Gles3RendererContext::get_resolution_scale() const {
    return static_cast<float>(render_h_) / 240.0f;
}

void Gles3RendererContext::send_dl(const OSTask* task) {
    if (!task || !rdram_) return;
    dl_count_++;
    frame_dirty_ = true;
    uint32_t dl_phys = physical_rdram_offset(static_cast<uint32_t>(task->t.data_ptr));
    uint64_t start_pc = SDL_GetPerformanceCounter();
    execute_display_list(dl_phys);
    uint64_t end_pc = SDL_GetPerformanceCounter();
    double dl_ms = static_cast<double>(end_pc - start_pc) * 1000.0 / static_cast<double>(SDL_GetPerformanceFrequency());
    total_dl_ms_ += dl_ms;
    if (dl_ms > max_dl_ms_) max_dl_ms_ = dl_ms;
    dls_in_second_++;
}

void Gles3RendererContext::execute_display_list(uint32_t start_phys_addr) {
    constexpr size_t MAX_DL_STACK = 18;
    std::vector<uint32_t> dl_stack;
    dl_stack.reserve(MAX_DL_STACK);

    uint32_t pc = start_phys_addr;
    bool running = true;

    while (running && pc < RDRAM_SIZE) {
        uint32_t w0 = read_rdram_u32(rdram_, pc);
        uint32_t w1 = read_rdram_u32(rdram_, pc + 4);
        pc += 8;

        uint8_t opcode = static_cast<uint8_t>(w0 >> 24);

        switch (opcode) {
            case G_NOOP:
                break;

            case G_VTX: {
                uint32_t numv = (w0 >> 12) & 0xFFu;
                uint32_t end = (w0 >> 1) & 0x7Fu;
                uint32_t v0 = (end >= numv) ? (end - numv) : 0;
                rsp_state_->load_vertices(rdram_, w1, numv, v0);
                break;
            }

            case G_MODIFYVTX: {
                uint32_t where = (w0 >> 16) & 0xFFu;
                uint32_t vtx = (w0 >> 1) & 0x7Fu;
                rsp_state_->modify_vertex(vtx, where, w1);
                break;
            }

            case G_TRI1: {
                uint32_t v0 = (w0 >> 17) & 0x7Fu;
                uint32_t v1 = (w0 >> 9) & 0x7Fu;
                uint32_t v2 = (w0 >> 1) & 0x7Fu;
                rsp_state_->draw_tri1(v0, v1, v2);
                break;
            }

            case G_TRI2: {
                uint32_t v0 = (w0 >> 17) & 0x7Fu;
                uint32_t v1 = (w0 >> 9) & 0x7Fu;
                uint32_t v2 = (w0 >> 1) & 0x7Fu;
                uint32_t v3 = (w1 >> 17) & 0x7Fu;
                uint32_t v4 = (w1 >> 9) & 0x7Fu;
                uint32_t v5 = (w1 >> 1) & 0x7Fu;
                rsp_state_->draw_tri2(v0, v1, v2, v3, v4, v5);
                break;
            }

            case G_QUAD: {
                uint32_t v0 = (w0 >> 17) & 0x7Fu;
                uint32_t v1 = (w0 >> 9) & 0x7Fu;
                uint32_t v2 = (w0 >> 1) & 0x7Fu;
                uint32_t v3 = (w1 >> 1) & 0x7Fu;
                rsp_state_->draw_quad(v0, v1, v2, v3);
                break;
            }

            case G_TEXTURE: {
                uint32_t tile = (w0 >> 8) & 0x07u;
                bool enabled = (w0 & 0x7Fu) != 0;
                uint16_t scale_s = static_cast<uint16_t>((w1 >> 16) & 0xFFFFu);
                uint16_t scale_t = static_cast<uint16_t>(w1 & 0xFFFFu);
                rsp_state_->set_texture(tile, enabled, scale_s, scale_t);
                break;
            }

            case G_POPMTX: {
                rsp_state_->pop_matrix(w1);
                break;
            }

            case G_GEOMETRYMODE: {
                uint32_t clear_bits = ~w0 & 0x00FFFFFFu;
                uint32_t set_bits = w1 & 0x00FFFFFFu;
                rsp_state_->set_geometry_mode(clear_bits, set_bits);
                break;
            }

            case G_MTX: {
                uint32_t params = (w0 & 0xFFu) ^ 1u;
                rsp_state_->load_matrix(rdram_, w1, params);
                break;
            }

            case G_MOVEWORD: {
                uint8_t index = static_cast<uint8_t>(w0 >> 16);
                uint16_t offset = static_cast<uint16_t>(w0 & 0xFFFFu);
                rsp_state_->move_word(index, offset, w1);
                break;
            }

            case G_MOVEMEM: {
                uint8_t type = static_cast<uint8_t>(w0 & 0xFFu);
                uint8_t offset = static_cast<uint8_t>((w0 >> 8) & 0xFFu);
                rsp_state_->move_mem(rdram_, type, offset, w1);
                break;
            }

            case G_DL: {
                uint32_t target = resolve_segment_address(w1, rsp_state_->get_segments());
                bool branch = ((w0 >> 16) & 0xFFu) != 0;
                if (branch) {
                    pc = target;
                } else {
                    if (dl_stack.size() < MAX_DL_STACK) {
                        dl_stack.push_back(pc);
                        pc = target;
                    }
                }
                break;
            }

            case G_ENDDL: {
                if (dl_stack.empty()) {
                    running = false;
                } else {
                    pc = dl_stack.back();
                    dl_stack.pop_back();
                }
                break;
            }

            case G_RDPHALF_1: {
                rdp_half_1_ = w1;
                break;
            }

            case G_RDPSETOTHERMODE: {
                rsp_state_->set_other_mode_h(0, 23, w0 & 0x00FFFFFFu);
                rsp_state_->set_other_mode_l(0, 31, w1);
                break;
            }

            case G_SETOTHERMODE_L: {
                uint32_t shift = (w0 >> 8) & 0xFFu;
                uint32_t length = w0 & 0xFFu;
                rsp_state_->set_other_mode_l(shift, length, w1);
                break;
            }

            case G_SETOTHERMODE_H: {
                uint32_t shift = (w0 >> 8) & 0xFFu;
                uint32_t length = w0 & 0xFFu;
                rsp_state_->set_other_mode_h(shift, length, w1);
                break;
            }

            case G_TEXRECT:
            case G_TEXRECTFLIP: {
                uint32_t lrx = (w0 >> 12) & 0xFFFu;
                uint32_t lry = w0 & 0xFFFu;
                uint32_t tile = (w1 >> 24) & 0x07u;
                uint32_t ulx = (w1 >> 12) & 0xFFFu;
                uint32_t uly = w1 & 0xFFFu;

                uint32_t h1 = read_rdram_u32(rdram_, pc + 4);
                uint32_t h2 = read_rdram_u32(rdram_, pc + 12);
                pc += 16;

                uint32_t s = (h1 >> 16) & 0xFFFFu;
                uint32_t t = h1 & 0xFFFFu;
                uint32_t dsdx = (h2 >> 16) & 0xFFFFu;
                uint32_t dtdy = h2 & 0xFFFFu;

                if (!cimg_targets_zbuffer_) {
                    rsp_state_->draw_tex_rect(tile, ulx, uly, lrx, lry, s, t, dsdx, dtdy, (opcode == G_TEXRECTFLIP), rdram_);
                }
                break;
            }

            case G_SETSCISSOR: {
                uint32_t ulx = (w0 >> 12) & 0xFFFu;
                uint32_t uly = w0 & 0xFFFu;
                uint32_t lrx = (w1 >> 12) & 0xFFFu;
                uint32_t lry = w1 & 0xFFFu;
                rsp_state_->set_scissor(ulx, uly, lrx, lry);
                break;
            }

            case G_LOADTLUT: {
                rsp_state_->flush_batch();
                uint32_t tile = (w1 >> 24) & 0x07u;
                uint32_t count = (w1 >> 14) & 0x3FFu;
                tmem_cache_->load_tlut(tile, count, rdram_);
                break;
            }

            case G_SETTILESIZE: {
                rsp_state_->flush_batch();
                uint32_t tile = (w1 >> 24) & 0x07u;
                uint32_t uls = (w0 >> 12) & 0xFFFu;
                uint32_t ult = w0 & 0xFFFu;
                uint32_t lrs = (w1 >> 12) & 0xFFFu;
                uint32_t lrt = w1 & 0xFFFu;
                tmem_cache_->set_tile_size(tile, static_cast<uint16_t>(uls), static_cast<uint16_t>(ult),
                                           static_cast<uint16_t>(lrs), static_cast<uint16_t>(lrt));
                break;
            }

            case G_LOADBLOCK: {
                rsp_state_->flush_batch();
                uint32_t tile = (w1 >> 24) & 0x07u;
                uint32_t uls = (w0 >> 12) & 0xFFFu;
                uint32_t ult = w0 & 0xFFFu;
                uint32_t lrs = (w1 >> 12) & 0xFFFu;
                uint32_t dxt = w1 & 0xFFFu;
                tmem_cache_->load_block(tile, uls, ult, lrs, dxt, rdram_);
                break;
            }

            case G_LOADTILE: {
                rsp_state_->flush_batch();
                uint32_t tile = (w1 >> 24) & 0x07u;
                uint32_t uls = (w0 >> 12) & 0xFFFu;
                uint32_t ult = w0 & 0xFFFu;
                uint32_t lrs = (w1 >> 12) & 0xFFFu;
                uint32_t lrt = w1 & 0xFFFu;
                tmem_cache_->load_tile(tile, uls, ult, lrs, lrt, rdram_);
                break;
            }

            case G_SETTILE: {
                rsp_state_->flush_batch();
                uint8_t fmt = static_cast<uint8_t>((w0 >> 21) & 0x07u);
                uint8_t siz = static_cast<uint8_t>((w0 >> 19) & 0x03u);
                uint16_t line = static_cast<uint16_t>((w0 >> 9) & 0x1FFu);
                uint16_t tmem = static_cast<uint16_t>(w0 & 0x1FFu);
                uint32_t tile = (w1 >> 24) & 0x07u;
                uint8_t pal = static_cast<uint8_t>((w1 >> 20) & 0x0Fu);
                bool clamp_t = (w1 & (1u << 19)) != 0;
                bool mirror_t = (w1 & (1u << 18)) != 0;
                uint8_t mask_t = static_cast<uint8_t>((w1 >> 14) & 0x0Fu);
                uint8_t shift_t = static_cast<uint8_t>((w1 >> 10) & 0x0Fu);
                bool clamp_s = (w1 & (1u << 9)) != 0;
                bool mirror_s = (w1 & (1u << 8)) != 0;
                uint8_t mask_s = static_cast<uint8_t>((w1 >> 4) & 0x0Fu);
                uint8_t shift_s = static_cast<uint8_t>(w1 & 0x0Fu);
                tmem_cache_->set_tile(tile, fmt, siz, line, tmem, pal, clamp_t, mirror_t, mask_t, shift_t, clamp_s, mirror_s, mask_s, shift_s);
                break;
            }

            case G_FILLRECT: {
                if (cimg_targets_zbuffer_) {
                    rsp_state_->submit_render_pass();
                    fbo_manager_->request_depth_clear();
                } else {
                    uint32_t lrx = (w0 >> 12) & 0xFFFu;
                    uint32_t lry = w0 & 0xFFFu;
                    uint32_t ulx = (w1 >> 12) & 0xFFFu;
                    uint32_t uly = w1 & 0xFFFu;
                    rsp_state_->draw_fill_rect(ulx, uly, lrx, lry);
                }
                break;
            }

            case G_SETFILLCOLOR:
                rsp_state_->set_fill_color(w1);
                break;

            case G_SETFOGCOLOR:
                rsp_state_->set_fog_color(w1);
                break;

            case G_SETBLENDCOLOR:
                rsp_state_->set_blend_color(w1);
                break;

            case G_SETPRIMCOLOR:
                rsp_state_->set_prim_color(w0, w1);
                break;

            case G_SETENVCOLOR:
                rsp_state_->set_env_color(w1);
                break;

            case G_SETCOMBINE:
                rsp_state_->set_combine(w0, w1);
                break;

            case G_SETTIMG: {
                rsp_state_->flush_batch();
                uint8_t fmt = static_cast<uint8_t>((w0 >> 21) & 0x07u);
                uint8_t siz = static_cast<uint8_t>((w0 >> 19) & 0x03u);
                uint32_t width = (w0 & 0xFFFu) + 1;
                uint32_t phys_timg = resolve_segment_address(w1, rsp_state_->get_segments());
                tmem_cache_->set_timg(fmt, siz, width, phys_timg);
                break;
            }

            case G_SETZIMG: {
                rsp_state_->submit_render_pass();
                zimg_phys_addr_ = resolve_segment_address(w1, rsp_state_->get_segments());
                break;
            }

            case G_SETCIMG: {
                rsp_state_->submit_render_pass();
                uint32_t phys_cimg = resolve_segment_address(w1, rsp_state_->get_segments());
                if (phys_cimg == zimg_phys_addr_) {
                    cimg_targets_zbuffer_ = true;
                } else {
                    cimg_targets_zbuffer_ = false;
                    fbo_manager_->bind_fbo(phys_cimg, render_w_, render_h_, rdram_);
                }
                break;
            }

            case G_EX: {
                uint32_t subcmd = w0 & 0xFFu;
                if (subcmd == G_EX_SETRECTALIGN) {
                    uint32_t origin_x = w1 & 0xFFFu;
                    uint32_t origin_y = (w1 >> 12) & 0xFFFu;
                    uint32_t next_w0 = read_rdram_u32(rdram_, pc);
                    uint32_t next_w1 = read_rdram_u32(rdram_, pc + 4);
                    pc += 8;
                    int32_t off_x = static_cast<int32_t>(static_cast<int16_t>(next_w0 >> 16));
                    int32_t off_y = static_cast<int32_t>(static_cast<int16_t>(next_w1 >> 16));
                    rsp_state_->ex_set_rect_align(origin_x, origin_y, off_x, off_y);
                } else if (subcmd == G_EX_PUSHSCISSOR) {
                    rsp_state_->ex_push_scissor();
                } else if (subcmd == G_EX_POPSCISSOR) {
                    rsp_state_->ex_pop_scissor();
                } else if (subcmd == G_EX_SETSCISSOR) {
                    pc += 8; // skip payload
                    rsp_state_->ex_set_scissor(0, 0, 320 * 4, 240 * 4);
                } else if (subcmd == G_EX_MATRIXGROUP) {
                    pc += 8; // skip payload
                }
                break;
            }

            case G_SPNOOP:
                // Gracefully ignore SPNOOP or gEXEnable
                break;

            default:
                break;
        }
    }

    rsp_state_->submit_render_pass();
}

void Gles3RendererContext::dump_frame_ppm(uint64_t frame_num) {
    char filename[64];
    std::snprintf(filename, sizeof(filename), "frame_%llu.ppm", static_cast<unsigned long long>(frame_num));

    gl::glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    std::vector<uint8_t> pixels(static_cast<size_t>(win_w_) * static_cast<size_t>(win_h_) * 4);
    gl::glPixelStorei(GL_PACK_ALIGNMENT, 1);
    gl::glReadPixels(0, 0, win_w_, win_h_, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    std::FILE* fp = std::fopen(filename, "wb");
    if (!fp) {
        std::fprintf(stderr, "Failed to open %s for writing\n", filename);
        return;
    }

    std::fprintf(fp, "P6\n%d %d\n255\n", win_w_, win_h_);
    std::vector<uint8_t> row_rgb(win_w_ * 3);
    for (int y = win_h_ - 1; y >= 0; --y) {
        const uint8_t* row_src = &pixels[static_cast<size_t>(y) * static_cast<size_t>(win_w_) * 4];
        for (int x = 0; x < win_w_; ++x) {
            row_rgb[x * 3 + 0] = row_src[x * 4 + 0];
            row_rgb[x * 3 + 1] = row_src[x * 4 + 1];
            row_rgb[x * 3 + 2] = row_src[x * 4 + 2];
        }
        std::fwrite(row_rgb.data(), 1, row_rgb.size(), fp);
    }
    std::fclose(fp);
    std::printf("[DEBUG] Dumped frame %llu to %s (%dx%d)\n",
                static_cast<unsigned long long>(frame_num), filename, win_w_, win_h_);
}

void Gles3RendererContext::check_perf_telemetry(uint32_t now_ticks) {
    if (last_perf_ticks_ == 0) {
        last_perf_ticks_ = now_ticks;
        return;
    }
    if (now_ticks - last_perf_ticks_ >= 1000) {
        auto rsp_stats = rsp_state_ ? rsp_state_->get_and_reset_stats() : RspState::RspStats{};
        auto tex_stats = tmem_cache_ ? tmem_cache_->get_and_reset_stats() : TextureCacheStats{};
        uint32_t shader_count = shader_cache_ ? static_cast<uint32_t>(shader_cache_->shader_count()) : 0;
        double avg_dl_ms = (dls_in_second_ > 0) ? (total_dl_ms_ / static_cast<double>(dls_in_second_)) : 0.0;
        double avg_sub_ms = (dls_in_second_ > 0) ? (rsp_stats.total_submit_ms / static_cast<double>(dls_in_second_)) : 0.0;
        double avg_swap_ms = (swaps_in_second_ > 0) ? (total_swap_ms_ / static_cast<double>(swaps_in_second_)) : 0.0;

        std::fprintf(stderr,
                     "[PERF] dls=%u swaps=%u skip_vi=%u passes=%u batches=%u tris=%u tex_fast=%u tex_hit=%u tex_upload=%u shaders=%u dl_ms(avg=%.2f max=%.2f sub=%.2f) swap_ms(avg=%.2f max=%.2f)\n",
                     dls_in_second_,
                     swaps_in_second_,
                     skipped_vis_in_second_,
                     rsp_stats.passes,
                     rsp_stats.batches,
                     rsp_stats.triangles,
                     tex_stats.fast_hits,
                     tex_stats.map_hits,
                     tex_stats.uploads,
                     shader_count,
                     avg_dl_ms,
                     max_dl_ms_,
                     avg_sub_ms,
                     avg_swap_ms,
                     max_swap_ms_);
        std::fflush(stderr);

        dls_in_second_ = 0;
        swaps_in_second_ = 0;
        skipped_vis_in_second_ = 0;
        total_dl_ms_ = 0.0;
        max_dl_ms_ = 0.0;
        total_swap_ms_ = 0.0;
        max_swap_ms_ = 0.0;
        last_perf_ticks_ = now_ticks;
    }
}

void Gles3RendererContext::update_screen() {
    int target_swap = s_target_swap_interval.load();
    if (applied_swap_interval_ != target_swap) {
        SDL_GL_SetSwapInterval(target_swap);
        applied_swap_interval_ = target_swap;
    }

    uint32_t vi_origin_phys = ultramodern::renderer::get_vi_regs()
        ? physical_rdram_offset(ultramodern::renderer::get_vi_regs()->VI_ORIGIN_REG)
        : 0xFFFFFFFFu;
    uint32_t present_fbo_addr = fbo_manager_->resolve_present_fbo_addr(vi_origin_phys);
    bool overlay_active = (s_overlay_active_callback != nullptr) && s_overlay_active_callback();
    uint32_t now_ticks = SDL_GetTicks();

    bool fbo_changed = (present_fbo_addr != 0 && present_fbo_addr != last_presented_fbo_addr_);
    bool single_buf_dirty = frame_dirty_ && (present_fbo_addr != 0 && present_fbo_addr == fbo_manager_->last_rendered_fbo_addr());
    bool dummy_dirty = frame_dirty_ && (dl_count_ == 0);
    bool overlay_due = overlay_active && (now_ticks - last_swap_ticks_ >= 16);

    bool should_swap = fbo_changed || single_buf_dirty || dummy_dirty || overlay_due;

    if (!should_swap) {
        skipped_vis_in_second_++;
        check_perf_telemetry(now_ticks);
        return;
    }

    frame_dirty_ = false;
    last_presented_fbo_addr_ = present_fbo_addr;
    last_swap_ticks_ = now_ticks;

    uint64_t swap_start = SDL_GetPerformanceCounter();

    fbo_manager_->present_vi_origin(vi_origin_phys, win_w_, win_h_, expand_aspect_);

    if (s_overlay_render_callback != nullptr) {
        s_overlay_render_callback(win_w_, win_h_);
    }

    if (dl_count_ > 0 && dl_count_ != last_dumped_dl_) {
        bool should_dump = false;
        for (uint64_t f : dump_frames_) {
            if (f == dl_count_) {
                should_dump = true;
                break;
            }
        }
        if (should_dump) {
            last_dumped_dl_ = dl_count_;
            dump_frame_ppm(dl_count_);
        }
    }

    SDL_GL_SwapWindow(window_handle_);

    uint64_t swap_end = SDL_GetPerformanceCounter();
    double swap_ms = static_cast<double>(swap_end - swap_start) * 1000.0 / static_cast<double>(SDL_GetPerformanceFrequency());
    total_swap_ms_ += swap_ms;
    if (swap_ms > max_swap_ms_) max_swap_ms_ = swap_ms;
    swaps_in_second_++;

    check_perf_telemetry(now_ticks);

    if (auto_quit_dls_ >= 0 && static_cast<int64_t>(dl_count_) >= auto_quit_dls_) {
        std::printf("[DEBUG] Reached auto-quit threshold (%llu DLs >= %lld). Quitting.\n",
                    static_cast<unsigned long long>(dl_count_), static_cast<long long>(auto_quit_dls_));
        std::fflush(stdout);
        std::fflush(stderr);
        std::_Exit(EXIT_SUCCESS);
    }
}

std::unique_ptr<ultramodern::renderer::RendererContext> create_renderer_context(
    uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
    return std::make_unique<Gles3RendererContext>(rdram, window_handle, developer_mode);
}

} // namespace snp::gles3
