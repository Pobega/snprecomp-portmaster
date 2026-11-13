#pragma once

#include <memory>
#include <vector>
#include <SDL2/SDL.h>
#include "ultramodern/renderer_context.hpp"
#include "gl_loader.h"
#include "tmem_texture_cache.h"
#include "combiner_shader_cache.h"
#include "fbo_manager.h"
#include "rsp_state.h"

namespace snp::gles3 {

class Gles3RendererContext : public ultramodern::renderer::RendererContext {
public:
    Gles3RendererContext(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode);
    ~Gles3RendererContext() override;

    bool valid() override;
    bool update_config(const ultramodern::renderer::GraphicsConfig& old_config,
                       const ultramodern::renderer::GraphicsConfig& new_config) override;

    void enable_instant_present() override;
    void send_dl(const OSTask* task) override;
    void send_dummy_workload(uint32_t fb_address) override;
    void update_screen() override;
    void shutdown() override;
    uint32_t get_display_framerate() const override;
    float get_resolution_scale() const override;

private:
    void execute_display_list(uint32_t start_phys_addr);
    void dump_frame_ppm(uint64_t frame_num);

    uint8_t* rdram_ = nullptr;
    ultramodern::renderer::WindowHandle window_handle_ = nullptr;
    bool developer_mode_ = false;

    SDL_GLContext gl_ctx_ = nullptr;
    int win_w_ = 720;
    int win_h_ = 480;
    int render_w_ = 720;
    int render_h_ = 480;
    bool expand_aspect_ = true;

    uint64_t dl_count_ = 0;
    uint64_t last_dumped_dl_ = 0;
    std::vector<uint64_t> dump_frames_;
    int64_t auto_quit_dls_ = -1;

    uint32_t zimg_phys_addr_ = 0xFFFFFFFFu;
    bool cimg_targets_zbuffer_ = false;
    uint32_t rdp_half_1_ = 0;

    std::unique_ptr<TmemTextureCache> tmem_cache_;
    std::unique_ptr<CombinerShaderCache> shader_cache_;
    std::unique_ptr<FboManager> fbo_manager_;
    std::unique_ptr<RspState> rsp_state_;
};

std::unique_ptr<ultramodern::renderer::RendererContext> create_renderer_context(
    uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode);

} // namespace snp::gles3
