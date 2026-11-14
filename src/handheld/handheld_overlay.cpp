#include "handheld_overlay.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <SDL2/SDL.h>
#include "gl_loader.h"
#include "handheld_config.h"
#include "handheld_font.h"
#include "ra_client.h"
#include "../gles3/gles3_renderer_context.h"
#include "ultramodern/ultramodern.hpp"

namespace snp::handheld {

namespace gl = snp::gles3::gl;

namespace {

constexpr int TOTAL_MENU_ROWS = 15;
constexpr float FONT_CHAR_WIDTH = 8.0f;
constexpr float FONT_CHAR_HEIGHT = 8.0f;
constexpr float SOLID_UV_U = 127.5f / 128.0f;
constexpr float SOLID_UV_V = 63.5f / 64.0f;

enum class MenuRow {
    ControlScheme = 0,
    SmartLockOn,
    AimCurve,
    AimSpeed,
    FaceButtons,
    SwapSticks,
    AspectRatio,
    Resolution,
    VSync,
    Rumble,
    MasterVolume,
    RetroAchievements,
    SyncAchievements,
    SaveAndResume,
    SaveAndQuit,
};

struct OverlayVertex {
    float x;
    float y;
    float u;
    float v;
    float r;
    float g;
    float b;
    float a;
};

struct ColorRgba {
    float r;
    float g;
    float b;
    float a;
};

constexpr ColorRgba COLOR_WHITE{1.0f, 1.0f, 1.0f, 1.0f};
constexpr ColorRgba COLOR_GOLD{1.0f, 0.84f, 0.20f, 1.0f};
constexpr ColorRgba COLOR_YELLOW{1.0f, 0.90f, 0.25f, 1.0f};
constexpr ColorRgba COLOR_CYAN{0.45f, 0.85f, 1.0f, 1.0f};
constexpr ColorRgba COLOR_MUTED_LABEL{0.75f, 0.80f, 0.88f, 1.0f};
constexpr ColorRgba COLOR_MUTED_VALUE{0.60f, 0.78f, 0.95f, 1.0f};
constexpr ColorRgba COLOR_MUTED_HINT{0.55f, 0.62f, 0.72f, 1.0f};
constexpr ColorRgba COLOR_BACKDROP{0.0f, 0.0f, 0.0f, 0.65f};
constexpr ColorRgba COLOR_PANEL_BG{0.06f, 0.08f, 0.12f, 0.94f};
constexpr ColorRgba COLOR_PANEL_BORDER{0.25f, 0.45f, 0.85f, 1.0f};
constexpr ColorRgba COLOR_ROW_HIGHLIGHT{0.18f, 0.32f, 0.60f, 0.85f};
constexpr ColorRgba COLOR_ROW_BORDER{0.35f, 0.65f, 1.0f, 0.90f};
constexpr ColorRgba COLOR_SEPARATOR{0.20f, 0.30f, 0.45f, 0.70f};
constexpr ColorRgba COLOR_BOX_BG{0.04f, 0.05f, 0.08f, 0.90f};

std::atomic<bool> s_overlay_open{false};
std::mutex s_overlay_mutex;
int s_selected_row = 0;

bool s_gl_initialized = false;
GLuint s_program = 0;
GLuint s_vao = 0;
GLuint s_vbo = 0;
GLuint s_font_texture = 0;
GLuint s_toast_badge_texture = 0;
std::vector<uint8_t> s_last_badge_pixels;
GLint s_loc_screen_size = -1;
GLint s_loc_texture = -1;

std::vector<OverlayVertex> s_vertices;
bool s_mesh_dirty = true;
int s_last_win_w = 0;
int s_last_win_h = 0;

void add_quad(float x0, float y0, float x1, float y1,
              float u0, float v0, float u1, float v1,
              const ColorRgba& c) {
    s_vertices.push_back({x0, y0, u0, v0, c.r, c.g, c.b, c.a});
    s_vertices.push_back({x1, y0, u1, v0, c.r, c.g, c.b, c.a});
    s_vertices.push_back({x1, y1, u1, v1, c.r, c.g, c.b, c.a});

    s_vertices.push_back({x0, y0, u0, v0, c.r, c.g, c.b, c.a});
    s_vertices.push_back({x1, y1, u1, v1, c.r, c.g, c.b, c.a});
    s_vertices.push_back({x0, y1, u0, v1, c.r, c.g, c.b, c.a});
}

void add_solid_rect(float x0, float y0, float x1, float y1, const ColorRgba& c) {
    add_quad(x0, y0, x1, y1, SOLID_UV_U, SOLID_UV_V, SOLID_UV_U, SOLID_UV_V, c);
}

void add_outline_rect(float x0, float y0, float x1, float y1, float border_width, const ColorRgba& c) {
    add_solid_rect(x0, y0, x1, y0 + border_width, c);
    add_solid_rect(x0, y1 - border_width, x1, y1, c);
    add_solid_rect(x0, y0, x0 + border_width, y1, c);
    add_solid_rect(x1 - border_width, y0, x1, y1, c);
}

void add_char(float x, float y, float scale, char ch, const ColorRgba& c) {
    int idx = (ch >= 32 && ch <= 126) ? (ch - 32) : ('?' - 32);
    int col = idx % 16;
    int row = idx / 16;

    float u0 = (col * FONT_CHAR_WIDTH) / 128.0f;
    float v0 = (row * FONT_CHAR_HEIGHT) / 64.0f;
    float u1 = ((col + 1) * FONT_CHAR_WIDTH) / 128.0f;
    float v1 = ((row + 1) * FONT_CHAR_HEIGHT) / 64.0f;

    float w = FONT_CHAR_WIDTH * scale;
    float h = FONT_CHAR_HEIGHT * scale;
    add_quad(x, y, x + w, y + h, u0, v0, u1, v1, c);
}

void add_string(float x, float y, float scale, const std::string& str, const ColorRgba& c) {
    float cur_x = x;
    float step = FONT_CHAR_WIDTH * scale;
    for (char ch : str) {
        add_char(cur_x, y, scale, ch, c);
        cur_x += step;
    }
}

GLuint compile_shader_stage(GLenum type, const std::string& source) {
    GLuint shader = gl::glCreateShader(type);
    const char* src_ptr = source.c_str();
    gl::glShaderSource(shader, 1, &src_ptr, nullptr);
    gl::glCompileShader(shader);

    GLint compiled = 0;
    gl::glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char log[512]{};
        gl::glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        std::fprintf(stderr, "[OVERLAY] Shader compilation failed: %s\n", log);
        gl::glDeleteShader(shader);
        return 0;
    }
    return shader;
}

void init_gl_resources() {
    if (s_gl_initialized) {
        return;
    }

    const char* gl_version = reinterpret_cast<const char*>(gl::glGetString(GL_VERSION));
    bool is_gles = (gl_version != nullptr) && (std::strstr(gl_version, "OpenGL ES") != nullptr);

    std::string vs_src;
    std::string fs_src;
    if (is_gles) {
        vs_src =
            "#version 300 es\n"
            "precision mediump float;\n"
            "layout(location = 0) in vec2 a_pos;\n"
            "layout(location = 1) in vec2 a_uv;\n"
            "layout(location = 2) in vec4 a_color;\n"
            "uniform vec2 u_screen_size;\n"
            "out vec2 v_uv;\n"
            "out vec4 v_color;\n"
            "void main() {\n"
            "    v_uv = a_uv;\n"
            "    v_color = a_color;\n"
            "    vec2 ndc = vec2((a_pos.x / u_screen_size.x) * 2.0 - 1.0, 1.0 - (a_pos.y / u_screen_size.y) * 2.0);\n"
            "    gl_Position = vec4(ndc, 0.0, 1.0);\n"
            "}\n";
        fs_src =
            "#version 300 es\n"
            "precision mediump float;\n"
            "in vec2 v_uv;\n"
            "in vec4 v_color;\n"
            "uniform sampler2D u_texture;\n"
            "out vec4 frag_color;\n"
            "void main() {\n"
            "    frag_color = v_color * texture(u_texture, v_uv);\n"
            "}\n";
    } else {
        vs_src =
            "#version 330 core\n"
            "layout(location = 0) in vec2 a_pos;\n"
            "layout(location = 1) in vec2 a_uv;\n"
            "layout(location = 2) in vec4 a_color;\n"
            "uniform vec2 u_screen_size;\n"
            "out vec2 v_uv;\n"
            "out vec4 v_color;\n"
            "void main() {\n"
            "    v_uv = a_uv;\n"
            "    v_color = a_color;\n"
            "    vec2 ndc = vec2((a_pos.x / u_screen_size.x) * 2.0 - 1.0, 1.0 - (a_pos.y / u_screen_size.y) * 2.0);\n"
            "    gl_Position = vec4(ndc, 0.0, 1.0);\n"
            "}\n";
        fs_src =
            "#version 330 core\n"
            "in vec2 v_uv;\n"
            "in vec4 v_color;\n"
            "uniform sampler2D u_texture;\n"
            "out vec4 frag_color;\n"
            "void main() {\n"
            "    frag_color = v_color * texture(u_texture, v_uv);\n"
            "}\n";
    }

    GLuint vs = compile_shader_stage(GL_VERTEX_SHADER, vs_src);
    GLuint fs = compile_shader_stage(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) {
        if (vs) gl::glDeleteShader(vs);
        if (fs) gl::glDeleteShader(fs);
        return;
    }

    s_program = gl::glCreateProgram();
    gl::glAttachShader(s_program, vs);
    gl::glAttachShader(s_program, fs);
    gl::glLinkProgram(s_program);
    gl::glDeleteShader(vs);
    gl::glDeleteShader(fs);

    GLint linked = 0;
    gl::glGetProgramiv(s_program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512]{};
        gl::glGetProgramInfoLog(s_program, sizeof(log), nullptr, log);
        std::fprintf(stderr, "[OVERLAY] Program link failed: %s\n", log);
        gl::glDeleteProgram(s_program);
        s_program = 0;
        return;
    }

    s_loc_screen_size = gl::glGetUniformLocation(s_program, "u_screen_size");
    s_loc_texture = gl::glGetUniformLocation(s_program, "u_texture");

    gl::glGenVertexArrays(1, &s_vao);
    gl::glGenBuffers(1, &s_vbo);

    gl::glBindVertexArray(s_vao);
    gl::glBindBuffer(GL_ARRAY_BUFFER, s_vbo);

    gl::glEnableVertexAttribArray(0);
    gl::glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(OverlayVertex), reinterpret_cast<void*>(0));
    gl::glEnableVertexAttribArray(1);
    gl::glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(OverlayVertex), reinterpret_cast<void*>(2 * sizeof(float)));
    gl::glEnableVertexAttribArray(2);
    gl::glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(OverlayVertex), reinterpret_cast<void*>(4 * sizeof(float)));

    gl::glBindVertexArray(0);
    gl::glBindBuffer(GL_ARRAY_BUFFER, 0);

    // Build 128x64 font texture containing 95 ASCII glyphs plus a solid white pixel at (127, 63)
    std::vector<uint8_t> texels(128 * 64 * 4, 0);
    for (int g = 0; g < 95; ++g) {
        int cell_x = (g % 16) * 8;
        int cell_y = (g / 16) * 8;
        for (int y = 0; y < 8; ++y) {
            uint8_t row_byte = FONT_8X8[g][y];
            for (int x = 0; x < 8; ++x) {
                bool pixel_on = (row_byte >> x) & 1;
                int px = cell_x + x;
                int py = cell_y + y;
                int offset = (py * 128 + px) * 4;
                texels[offset + 0] = 255;
                texels[offset + 1] = 255;
                texels[offset + 2] = 255;
                texels[offset + 3] = pixel_on ? 255 : 0;
            }
        }
    }

    // Set pixel (127, 63) to solid opaque white for untextured rectangles
    int solid_offset = (63 * 128 + 127) * 4;
    texels[solid_offset + 0] = 255;
    texels[solid_offset + 1] = 255;
    texels[solid_offset + 2] = 255;
    texels[solid_offset + 3] = 255;

    gl::glGenTextures(1, &s_font_texture);
    gl::glBindTexture(GL_TEXTURE_2D, s_font_texture);
    gl::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 128, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels.data());
    gl::glBindTexture(GL_TEXTURE_2D, 0);

    s_gl_initialized = true;
}

std::string get_row_label(MenuRow row) {
    switch (row) {
    case MenuRow::ControlScheme: return "Control Scheme";
    case MenuRow::SmartLockOn: return "Smart Lock-On";
    case MenuRow::AimCurve: return "Aim Curve";
    case MenuRow::AimSpeed: return "Aim Speed";
    case MenuRow::FaceButtons: return "Face Buttons";
    case MenuRow::SwapSticks: return "Swap Sticks";
    case MenuRow::AspectRatio: return "Aspect Ratio";
    case MenuRow::Resolution: return "Resolution";
    case MenuRow::VSync: return "VSync";
    case MenuRow::Rumble: return "Rumble";
    case MenuRow::MasterVolume: return "Master Volume";
    case MenuRow::RetroAchievements: return "RetroAchievements";
    case MenuRow::SyncAchievements: return "Sync Achievements";
    case MenuRow::SaveAndResume: return "[ Save & Resume ]";
    case MenuRow::SaveAndQuit: return "[ Save & Quit ]";
    }
    return "";
}

std::string get_row_value(MenuRow row, const HandheldConfig& cfg) {
    switch (row) {
    case MenuRow::ControlScheme:
        if (cfg.controls.scheme == CONTROL_SCHEME_DPAD_STRAFE_SHIFT) return "Stickless: Strafe + Shift";
        if (cfg.controls.scheme == CONTROL_SCHEME_BUMPER_STRAFE) return "Stickless: Bumper Strafe";
        if (cfg.controls.scheme == CONTROL_SCHEME_FACE_STRAFE) return "Stickless: Face Strafe";
        if (cfg.controls.scheme == CONTROL_SCHEME_DPAD_AIM) return "Legacy: D-Pad Aim";
        if (cfg.controls.scheme == CONTROL_SCHEME_DPAD_STRAFE) return "Legacy: Face Aim";
        return "Twin Stick";
    case MenuRow::SmartLockOn:
        if (cfg.controls.smart_lock_on == SMART_LOCK_HOLD) return "Hold (Hold=Lock)";
        if (cfg.controls.smart_lock_on == SMART_LOCK_OFF) return "Off (N64 Toggle)";
        return "Auto (Hold=Manual)";
    case MenuRow::AimCurve:
        if (cfg.controls.aim_curve == AIM_CURVE_LINEAR) return "Linear";
        return "S-Curve (Precision)";
    case MenuRow::AimSpeed:
        return std::to_string(static_cast<int>(std::round(cfg.controls.stick_sensitivity))) + "%";
    case MenuRow::FaceButtons:
        return (cfg.controls.face_layout == FACE_LAYOUT_POSITIONAL) ? "Positional (Swap)" : "Nintendo (B/A/Y/X)";
    case MenuRow::SwapSticks:
        return cfg.controls.swap_sticks ? "On" : "Off";
    case MenuRow::AspectRatio: {
        if (cfg.video.aspect_ratio != ASPECT_RATIO_EXPAND) {
            return "4:3 Original";
        }
        SDL_Window* window = SDL_GL_GetCurrentWindow();
        if (window != nullptr) {
            int width = 0, height = 0;
            SDL_GetWindowSize(window, &width, &height);
            if (width > 0 && height > 0) {
                float aspect = static_cast<float>(width) / static_cast<float>(height);
                constexpr float kTolerance = 0.05f;
                auto matches = [aspect](float target) {
                    return std::abs(aspect - target) <= kTolerance;
                };
                if (matches(1.0f)) return "1:1 Expand";
                if (matches(5.0f / 4.0f)) return "5:4 Expand";
                if (matches(4.0f / 3.0f)) return "4:3 Expand";
                if (matches(3.0f / 2.0f)) return "3:2 Expand";
                if (matches(16.0f / 10.0f)) return "16:10 Expand";
                if (matches(16.0f / 9.0f)) return "16:9 Expand";
            }
        }
        return "Full Expand";
    }
    case MenuRow::Resolution:
        return (cfg.video.resolution_scale >= 2) ? "2x (480p)" : "1x (240p)";
    case MenuRow::VSync:
        return cfg.video.vsync ? "On" : "Off";
    case MenuRow::Rumble:
        return cfg.controls.rumble ? "On" : "Off";
    case MenuRow::MasterVolume:
        return std::to_string(cfg.audio.master_volume) + "%";
    case MenuRow::RetroAchievements:
        return ra_get_status_text();
    case MenuRow::SyncAchievements: {
        if (!ra_is_enabled()) {
            return "Disabled";
        }
        if (ra_is_syncing()) {
            return "Syncing...";
        }
        size_t pending = ra_get_pending_sync_count();
        if (pending > 0) {
            return std::to_string(pending) + " Pending (Press A)";
        }
        std::string status = ra_get_status_text();
        if (status == "No Credentials") {
            return "Not Signed In";
        }
        if (ra_is_offline_mode() || status == "Offline") {
            return "Reconnect (Press A)";
        }
        return "Up to Date (Press A)";
    }
    case MenuRow::SaveAndResume:
    case MenuRow::SaveAndQuit:
        return "";
    }
    return "";
}

void cycle_row_value(MenuRow row, bool forward) {
    auto& cfg = get_mutable_config();
    switch (row) {
    case MenuRow::ControlScheme: {
        const std::array<std::string, 6> schemes = {
            CONTROL_SCHEME_DPAD_STRAFE_SHIFT,
            CONTROL_SCHEME_BUMPER_STRAFE,
            CONTROL_SCHEME_FACE_STRAFE,
            CONTROL_SCHEME_DPAD_AIM,
            CONTROL_SCHEME_DPAD_STRAFE,
            CONTROL_SCHEME_TWIN_STICK,
        };
        auto it = std::find(schemes.begin(), schemes.end(), cfg.controls.scheme);
        int idx = (it != schemes.end()) ? static_cast<int>(std::distance(schemes.begin(), it)) : 0;
        idx = forward ? (idx + 1) % 6 : (idx + 5) % 6;
        cfg.controls.scheme = schemes[idx];
        break;
    }
    case MenuRow::SmartLockOn: {
        const std::array<std::string, 3> modes = {
            SMART_LOCK_AUTO,
            SMART_LOCK_HOLD,
            SMART_LOCK_OFF,
        };
        auto it = std::find(modes.begin(), modes.end(), cfg.controls.smart_lock_on);
        int idx = (it != modes.end()) ? static_cast<int>(std::distance(modes.begin(), it)) : 0;
        idx = forward ? (idx + 1) % 3 : (idx + 2) % 3;
        cfg.controls.smart_lock_on = modes[idx];
        break;
    }
    case MenuRow::AimCurve:
        cfg.controls.aim_curve = (cfg.controls.aim_curve == AIM_CURVE_S_CURVE)
            ? AIM_CURVE_LINEAR : AIM_CURVE_S_CURVE;
        break;
    case MenuRow::AimSpeed: {
        float delta = forward ? 10.0f : -10.0f;
        cfg.controls.stick_sensitivity = std::clamp(cfg.controls.stick_sensitivity + delta, 50.0f, 200.0f);
        snp::config::set_stick_sensitivity(cfg.controls.stick_sensitivity);
        break;
    }
    case MenuRow::FaceButtons:
        cfg.controls.face_layout = (cfg.controls.face_layout == FACE_LAYOUT_NINTENDO)
            ? FACE_LAYOUT_POSITIONAL : FACE_LAYOUT_NINTENDO;
        break;
    case MenuRow::SwapSticks:
        cfg.controls.swap_sticks = !cfg.controls.swap_sticks;
        break;
    case MenuRow::AspectRatio:
        cfg.video.aspect_ratio = (cfg.video.aspect_ratio == ASPECT_RATIO_EXPAND)
            ? ASPECT_RATIO_ORIGINAL : ASPECT_RATIO_EXPAND;
        apply_graphics_config();
        break;
    case MenuRow::Resolution:
        cfg.video.resolution_scale = (cfg.video.resolution_scale >= 2) ? 1 : 2;
        apply_graphics_config();
        break;
    case MenuRow::VSync:
        cfg.video.vsync = !cfg.video.vsync;
        snp::gles3::set_vsync_enabled(cfg.video.vsync);
        break;
    case MenuRow::Rumble:
        cfg.controls.rumble = !cfg.controls.rumble;
        break;
    case MenuRow::MasterVolume: {
        int delta = forward ? 10 : -10;
        cfg.audio.master_volume = std::clamp(cfg.audio.master_volume + delta, 0, 100);
        break;
    }
    case MenuRow::RetroAchievements:
        ra_set_enabled(!ra_is_enabled());
        break;
    case MenuRow::SyncAchievements:
        if (ra_is_enabled() && ra_get_status_text() != "No Credentials") {
            ra_sync_now();
        }
        break;
    case MenuRow::SaveAndResume:
        save_config();
        close_overlay();
        break;
    case MenuRow::SaveAndQuit:
        save_config();
        ultramodern::quit();
        break;
    }
}

void build_overlay_mesh(int win_w, int win_h) {
    const auto& cfg = get_config();

    add_solid_rect(0.0f, 0.0f, static_cast<float>(win_w), static_cast<float>(win_h), COLOR_BACKDROP);

    float panel_w = std::min(648.0f, std::max(580.0f, static_cast<float>(win_w) - 16.0f));
    float panel_h = std::min(444.0f, std::max(400.0f, static_cast<float>(win_h) - 16.0f));
    float panel_x0 = (static_cast<float>(win_w) - panel_w) * 0.5f;
    float panel_y0 = (static_cast<float>(win_h) - panel_h) * 0.5f;
    float panel_x1 = panel_x0 + panel_w;
    float panel_y1 = panel_y0 + panel_h;

    add_solid_rect(panel_x0, panel_y0, panel_x1, panel_y1, COLOR_PANEL_BG);
    add_outline_rect(panel_x0 - 2.0f, panel_y0 - 2.0f, panel_x1 + 2.0f, panel_y1 + 2.0f, 2.0f, COLOR_PANEL_BORDER);

    const std::string title = "SIN AND PUNISHMENT - QUICK SETTINGS";
    float title_scale = 1.4f;
    float title_w = title.size() * FONT_CHAR_WIDTH * title_scale;
    float title_x = panel_x0 + (panel_w - title_w) * 0.5f;
    float title_y = panel_y0 + 10.0f;
    add_string(title_x, title_y, title_scale, title, COLOR_GOLD);

    float sep_y = title_y + 15.0f;
    add_solid_rect(panel_x0 + 16.0f, sep_y, panel_x1 - 16.0f, sep_y + 1.0f, COLOR_SEPARATOR);

    float row_start_y = sep_y + 6.0f;
    float row_height = 18.0f;
    float label_x = panel_x0 + 36.0f;
    float value_x = panel_x0 + 260.0f;
    float text_scale = 1.25f;

    for (int i = 0; i < TOTAL_MENU_ROWS; ++i) {
        MenuRow row = static_cast<MenuRow>(i);
        float y = row_start_y + i * row_height;
        bool is_selected = (s_selected_row == i);

        if (is_selected) {
            add_solid_rect(panel_x0 + 16.0f, y - 2.0f, panel_x1 - 16.0f, y + 14.0f, COLOR_ROW_HIGHLIGHT);
            add_outline_rect(panel_x0 + 16.0f, y - 2.0f, panel_x1 - 16.0f, y + 14.0f, 1.0f, COLOR_ROW_BORDER);
            add_char(panel_x0 + 20.0f, y, text_scale, '>', COLOR_YELLOW);
        }

        std::string label = get_row_label(row);
        std::string value = get_row_value(row, cfg);

        if (row == MenuRow::SaveAndResume || row == MenuRow::SaveAndQuit) {
            float action_w = label.size() * FONT_CHAR_WIDTH * text_scale;
            float action_x = panel_x0 + (panel_w - action_w) * 0.5f;
            ColorRgba action_color = is_selected ? COLOR_WHITE : COLOR_MUTED_LABEL;
            add_string(action_x, y, text_scale, label, action_color);
        } else {
            ColorRgba label_color = is_selected ? COLOR_WHITE : COLOR_MUTED_LABEL;
            ColorRgba value_color = is_selected ? COLOR_CYAN : COLOR_MUTED_VALUE;
            add_string(label_x, y, text_scale, label, label_color);
            add_string(value_x, y, text_scale, value, value_color);
        }
    }

    float legend_box_y0 = row_start_y + TOTAL_MENU_ROWS * row_height + 6.0f;
    float legend_box_y1 = legend_box_y0 + 64.0f;
    float box_x0 = panel_x0 + 16.0f;
    float box_x1 = panel_x1 - 16.0f;

    add_solid_rect(box_x0, legend_box_y0, box_x1, legend_box_y1, COLOR_BOX_BG);
    add_outline_rect(box_x0, legend_box_y0, box_x1, legend_box_y1, 1.0f, COLOR_SEPARATOR);

    std::string lock_desc = (cfg.controls.smart_lock_on == SMART_LOCK_AUTO)
        ? "Auto"
        : ((cfg.controls.smart_lock_on == SMART_LOCK_OFF) ? "Toggle" : "Hold");

    std::string legend_l1;
    std::string legend_l2;
    if (cfg.controls.scheme == CONTROL_SCHEME_BUMPER_STRAFE) {
        legend_l1 = "Aim: D-Pad | Strafe/2xRoll: L1/R1 | Fire: A/Y/R2 | Jump: B";
        legend_l2 = "Lock-On: X/L2 (" + lock_desc + ") | Pause: Start";
    } else if (cfg.controls.scheme == CONTROL_SCHEME_FACE_STRAFE) {
        legend_l1 = "Aim: D-Pad | Strafe/2xRoll: Y(L) B(R) | Roll: A";
        legend_l2 = "Fire: R1/R2 | Jump: L1/X | Lock-On: L2 (" + lock_desc + ") | Start: Pause";
    } else if (cfg.controls.scheme == CONTROL_SCHEME_DPAD_STRAFE_SHIFT) {
        legend_l1 = "Move/2xRoll: D-Pad L/R | Hold L1/L2+D-Pad: Manual Aim";
        legend_l2 = "Fire: A/R1/R2 | Jump: B | Roll: Y | Next/Center: X";
    } else if (cfg.controls.scheme == CONTROL_SCHEME_DPAD_AIM) {
        legend_l1 = "Aim: D-Pad | Strafe: Y(L) A(R) | Fire: R1/R2 | Jump: L1";
        legend_l2 = "Roll: B/L2 | Lock-On: X (" + lock_desc + ") | Pause: Start";
    } else if (cfg.controls.scheme == CONTROL_SCHEME_DPAD_STRAFE) {
        legend_l1 = "Aim: Diamond(X/B/Y/A) | Strafe: D-Pad L/R";
        legend_l2 = "Fire: R1 | Jump: L1 | Roll: L2 | Lock-On: R2 (" + lock_desc + ")";
    } else {
        if (cfg.controls.swap_sticks) {
            legend_l1 = "Aim: Left Stick | Move: Right Stick | Fire: R1/R2";
        } else {
            legend_l1 = "Aim: Right Stick | Move: Left Stick | Fire: R1/R2";
        }
        legend_l2 = "Jump: L1/L2 | Roll: X | Lock-On: A (" + lock_desc + ") | Start: Pause";
    }

    add_string(box_x0 + 10.0f, legend_box_y0 + 5.0f, 1.0f, "ACTIVE CONTROLS (" + get_row_value(MenuRow::ControlScheme, cfg) + "):", COLOR_GOLD);
    add_string(box_x0 + 10.0f, legend_box_y0 + 22.0f, 1.1f, legend_l1, COLOR_WHITE);
    add_string(box_x0 + 10.0f, legend_box_y0 + 40.0f, 1.1f, legend_l2, COLOR_WHITE);

    const std::string footer = "D-Pad: Select/Change | A: Select | Select or Menu: Save & Close";
    float footer_w = footer.size() * FONT_CHAR_WIDTH * 1.0f;
    float footer_x = panel_x0 + (panel_w - footer_w) * 0.5f;
    float footer_y = panel_y1 - 16.0f;
    add_string(footer_x, footer_y, 1.0f, footer, COLOR_MUTED_HINT);
}

} // namespace

void init_overlay() {
    const char* test_env = std::getenv("SNP_TEST_OPEN_OVERLAY");
    if (test_env != nullptr && (std::strcmp(test_env, "1") == 0 || std::strcmp(test_env, "true") == 0)) {
        s_overlay_open.store(true, std::memory_order_relaxed);
    }
}

void shutdown_overlay() {
    std::lock_guard<std::mutex> lock(s_overlay_mutex);
    if (s_gl_initialized) {
        if (SDL_GL_GetCurrentContext() != nullptr) {
            if (s_toast_badge_texture) gl::glDeleteTextures(1, &s_toast_badge_texture);
            if (s_font_texture) gl::glDeleteTextures(1, &s_font_texture);
            if (s_vbo) gl::glDeleteBuffers(1, &s_vbo);
            if (s_vao) gl::glDeleteVertexArrays(1, &s_vao);
            if (s_program) gl::glDeleteProgram(s_program);
        }
        s_toast_badge_texture = 0;
        s_last_badge_pixels.clear();
        s_font_texture = 0;
        s_vbo = 0;
        s_vao = 0;
        s_program = 0;
        s_gl_initialized = false;
    }
}

bool is_overlay_open() {
    return s_overlay_open.load(std::memory_order_relaxed);
}

void open_overlay() {
    std::lock_guard<std::mutex> lock(s_overlay_mutex);
    s_mesh_dirty = true;
    s_overlay_open.store(true, std::memory_order_relaxed);
}

void close_overlay() {
    s_overlay_open.store(false, std::memory_order_relaxed);
}

void toggle_overlay() {
    std::lock_guard<std::mutex> lock(s_overlay_mutex);
    bool current = s_overlay_open.load(std::memory_order_relaxed);
    if (current) {
        save_config();
        s_overlay_open.store(false, std::memory_order_relaxed);
    } else {
        s_mesh_dirty = true;
        s_overlay_open.store(true, std::memory_order_relaxed);
    }
}

void update_overlay_input(bool up, bool down, bool left, bool right,
                          bool btn_a, bool btn_b, bool btn_start) {
    if (!s_overlay_open.load(std::memory_order_relaxed)) {
        return;
    }

    std::lock_guard<std::mutex> lock(s_overlay_mutex);

    if (up || down || left || right || btn_a) {
        s_mesh_dirty = true;
    }

    if (up) {
        s_selected_row = (s_selected_row + TOTAL_MENU_ROWS - 1) % TOTAL_MENU_ROWS;
    }
    if (down) {
        s_selected_row = (s_selected_row + 1) % TOTAL_MENU_ROWS;
    }

    MenuRow current = static_cast<MenuRow>(s_selected_row);

    if (left) {
        cycle_row_value(current, false);
    }
    if (right) {
        cycle_row_value(current, true);
    }
    if (btn_a) {
        cycle_row_value(current, true);
    }
    if (btn_start) {
        if (current == MenuRow::SaveAndQuit) {
            save_config();
            ultramodern::quit();
        } else {
            save_config();
            close_overlay();
        }
    }
    if (btn_b) {
        save_config();
        close_overlay();
    }
}

void render_menu(int win_w, int win_h) {
    static std::string s_last_ra_snapshot;
    std::string ra_snapshot = ra_get_status_text() + "|" +
                              (ra_is_syncing() ? "1" : "0") + "|" +
                              (ra_is_offline_mode() ? "1" : "0");
    if (ra_snapshot != s_last_ra_snapshot) {
        s_last_ra_snapshot = std::move(ra_snapshot);
        s_mesh_dirty = true;
    }

    if (win_w != s_last_win_w || win_h != s_last_win_h) {
        s_mesh_dirty = true;
        s_last_win_w = win_w;
        s_last_win_h = win_h;
    }

    if (s_mesh_dirty) {
        s_vertices.clear();
        build_overlay_mesh(win_w, win_h);
        if (!s_vertices.empty()) {
            gl::glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
            gl::glBufferData(GL_ARRAY_BUFFER, s_vertices.size() * sizeof(OverlayVertex), s_vertices.data(), GL_STREAM_DRAW);
        }
        s_mesh_dirty = false;
    }

    if (s_vertices.empty()) {
        return;
    }

    // Set 2D state cleanly
    gl::glBindFramebuffer(GL_FRAMEBUFFER, 0);
    gl::glViewport(0, 0, win_w, win_h);
    gl::glDisable(GL_SCISSOR_TEST);
    gl::glDisable(GL_DEPTH_TEST);
    gl::glDepthMask(GL_FALSE);
    gl::glDisable(GL_CULL_FACE);
    gl::glEnable(GL_BLEND);
    gl::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    gl::glUseProgram(s_program);
    gl::glUniform2f(s_loc_screen_size, static_cast<float>(win_w), static_cast<float>(win_h));
    gl::glUniform1i(s_loc_texture, 0);

    gl::glActiveTexture(GL_TEXTURE0);
    gl::glBindTexture(GL_TEXTURE_2D, s_font_texture);

    gl::glBindVertexArray(s_vao);
    gl::glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(s_vertices.size()));

    // Cleanup state
    gl::glDisable(GL_BLEND);
    gl::glDepthMask(GL_TRUE);
    gl::glBindVertexArray(0);
    gl::glBindBuffer(GL_ARRAY_BUFFER, 0);
    gl::glUseProgram(0);
}

void render_toast_card(int win_w, int win_h) {
    RaToastItem toast;
    if (!ra_get_current_toast(toast)) {
        return;
    }

    float elapsed = toast.elapsed_sec;
    float duration = toast.duration_sec;

    float alpha = 1.0f;
    float x_slide = 0.0f;
    constexpr float SLIDE_IN_DURATION = 0.35f;
    constexpr float FADE_OUT_DURATION = 0.5f;

    if (elapsed < SLIDE_IN_DURATION) {
        float t = elapsed / SLIDE_IN_DURATION;
        alpha = t;
        x_slide = (1.0f - t) * -320.0f;
    } else if (elapsed > duration - FADE_OUT_DURATION) {
        float t = std::clamp((duration - elapsed) / FADE_OUT_DURATION, 0.0f, 1.0f);
        alpha = t;
        x_slide = (1.0f - t) * -40.0f;
    }

    float scale = std::clamp(static_cast<float>(win_h) / 480.0f, 0.75f, 1.5f);
    float card_w = std::min(static_cast<float>(win_w) - 32.0f, 500.0f * scale);
    float card_h = 64.0f * scale;
    float card_x0 = 16.0f * scale + x_slide;
    float card_y0 = static_cast<float>(win_h) - card_h - 16.0f * scale;
    float card_x1 = card_x0 + card_w;
    float card_y1 = card_y0 + card_h;

    std::vector<OverlayVertex> toast_vertices;
    std::vector<OverlayVertex> badge_vertices;

    ColorRgba bg_color{0.06f, 0.08f, 0.12f, 0.94f * alpha};
    ColorRgba border_color{0.95f, 0.78f, 0.20f, 0.95f * alpha};
    ColorRgba bar_color{1.0f, 0.85f, 0.25f, 1.0f * alpha};
    if (toast.accent_kind == 3) {
        border_color = ColorRgba{0.85f, 0.30f, 0.25f, 0.95f * alpha};
        bar_color = ColorRgba{1.0f, 0.35f, 0.30f, 1.0f * alpha};
    } else if (toast.accent_kind == 1) {
        border_color = ColorRgba{0.35f, 0.70f, 1.0f, 0.95f * alpha};
        bar_color = ColorRgba{0.45f, 0.85f, 1.0f, 1.0f * alpha};
    }

    auto add_quad_to = [](std::vector<OverlayVertex>& list, float x0, float y0, float x1, float y1,
                          float u0, float v0, float u1, float v1, const ColorRgba& c) {
        list.push_back({x0, y0, u0, v0, c.r, c.g, c.b, c.a});
        list.push_back({x1, y0, u1, v0, c.r, c.g, c.b, c.a});
        list.push_back({x1, y1, u1, v1, c.r, c.g, c.b, c.a});
        list.push_back({x0, y0, u0, v0, c.r, c.g, c.b, c.a});
        list.push_back({x1, y1, u1, v1, c.r, c.g, c.b, c.a});
        list.push_back({x0, y1, u0, v1, c.r, c.g, c.b, c.a});
    };

    auto add_rect_to = [&](std::vector<OverlayVertex>& list, float x0, float y0, float x1, float y1, const ColorRgba& c) {
        add_quad_to(list, x0, y0, x1, y1, SOLID_UV_U, SOLID_UV_V, SOLID_UV_U, SOLID_UV_V, c);
    };

    add_rect_to(toast_vertices, card_x0, card_y0, card_x1, card_y1, bg_color);
    add_rect_to(toast_vertices, card_x0, card_y0, card_x1, card_y0 + 1.5f, border_color);
    add_rect_to(toast_vertices, card_x0, card_y1 - 1.5f, card_x1, card_y1, border_color);
    add_rect_to(toast_vertices, card_x0, card_y0, card_x0 + 1.5f, card_y1, border_color);
    add_rect_to(toast_vertices, card_x1 - 1.5f, card_y0, card_x1, card_y1, border_color);
    add_rect_to(toast_vertices, card_x0, card_y0, card_x0 + 4.0f * scale, card_y1, bar_color);

    float badge_size = 46.0f * scale;
    float badge_x0 = card_x0 + 10.0f * scale;
    float badge_y0 = card_y0 + (card_h - badge_size) * 0.5f;
    float badge_x1 = badge_x0 + badge_size;
    float badge_y1 = badge_y0 + badge_size;

    bool has_badge = !toast.rgba_pixels.empty();
    if (has_badge) {
        ColorRgba badge_col{1.0f, 1.0f, 1.0f, alpha};
        add_quad_to(badge_vertices, badge_x0, badge_y0, badge_x1, badge_y1, 0.0f, 0.0f, 1.0f, 1.0f, badge_col);
    }

    float text_x = badge_x1 + 10.0f * scale;
    float crisp_scale = std::max(1.0f, std::floor(scale));

    auto add_string_to = [&](std::vector<OverlayVertex>& list, float x, float y, float s, const std::string& str, const ColorRgba& c) {
        float avail_w = (card_x1 - 8.0f * scale) - x;
        size_t max_chars = avail_w > 0.0f ? static_cast<size_t>(avail_w / (FONT_CHAR_WIDTH * s)) : 0;
        std::string clipped = str;
        if (clipped.size() > max_chars) {
            clipped = (max_chars > 3) ? (clipped.substr(0, max_chars - 3) + "...") : clipped.substr(0, max_chars);
        }
        float cx = x;
        for (char ch : clipped) {
            if (ch < 32 || ch > 126) ch = '?';
            int glyph = ch - 32;
            int col = glyph % 16;
            int row = glyph / 16;
            float u0 = (col * 8) / 128.0f;
            float v0 = (row * 8) / 64.0f;
            float u1 = (col * 8 + 8) / 128.0f;
            float v1 = (row * 8 + 8) / 64.0f;
            add_quad_to(list, cx, y, cx + FONT_CHAR_WIDTH * s, y + FONT_CHAR_HEIGHT * s, u0, v0, u1, v1, c);
            cx += FONT_CHAR_WIDTH * s;
        }
    };

    ColorRgba title_color = (toast.accent_kind == 0) ? ColorRgba{1.0f, 0.84f, 0.20f, alpha} : ColorRgba{0.45f, 0.85f, 1.0f, alpha};
    ColorRgba sub_color{1.0f, 1.0f, 1.0f, alpha};
    ColorRgba detail_color{0.75f, 0.80f, 0.88f, 0.90f * alpha};

    add_string_to(toast_vertices, text_x, card_y0 + 10.0f * scale, crisp_scale, toast.title, title_color);
    add_string_to(toast_vertices, text_x, card_y0 + 26.0f * scale, crisp_scale * 1.25f, toast.subtitle, sub_color);
    if (!toast.detail.empty()) {
        add_string_to(toast_vertices, text_x, card_y0 + 45.0f * scale, crisp_scale, toast.detail, detail_color);
    }

    gl::glBindFramebuffer(GL_FRAMEBUFFER, 0);
    gl::glViewport(0, 0, win_w, win_h);
    gl::glDisable(GL_SCISSOR_TEST);
    gl::glDisable(GL_DEPTH_TEST);
    gl::glDepthMask(GL_FALSE);
    gl::glDisable(GL_CULL_FACE);
    gl::glEnable(GL_BLEND);
    gl::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    gl::glUseProgram(s_program);
    gl::glUniform2f(s_loc_screen_size, static_cast<float>(win_w), static_cast<float>(win_h));
    gl::glUniform1i(s_loc_texture, 0);

    gl::glActiveTexture(GL_TEXTURE0);

    if (!toast_vertices.empty()) {
        gl::glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
        gl::glBufferData(GL_ARRAY_BUFFER, toast_vertices.size() * sizeof(OverlayVertex), toast_vertices.data(), GL_STREAM_DRAW);
        gl::glBindTexture(GL_TEXTURE_2D, s_font_texture);
        gl::glBindVertexArray(s_vao);
        gl::glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(toast_vertices.size()));
    }

    if (has_badge && !badge_vertices.empty()) {
        if (s_toast_badge_texture == 0) {
            gl::glGenTextures(1, &s_toast_badge_texture);
            gl::glBindTexture(GL_TEXTURE_2D, s_toast_badge_texture);
            gl::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            gl::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            gl::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            gl::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        } else {
            gl::glBindTexture(GL_TEXTURE_2D, s_toast_badge_texture);
        }

        if (s_last_badge_pixels != toast.rgba_pixels) {
            gl::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, toast.rgba_pixels.data());
            s_last_badge_pixels = toast.rgba_pixels;
        }

        gl::glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
        gl::glBufferData(GL_ARRAY_BUFFER, badge_vertices.size() * sizeof(OverlayVertex), badge_vertices.data(), GL_STREAM_DRAW);
        gl::glBindVertexArray(s_vao);
        gl::glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(badge_vertices.size()));
    }

    gl::glDisable(GL_BLEND);
    gl::glDepthMask(GL_TRUE);
    gl::glBindVertexArray(0);
    gl::glBindBuffer(GL_ARRAY_BUFFER, 0);
    gl::glUseProgram(0);
}

void render_overlay(int win_w, int win_h) {
    bool menu_open = s_overlay_open.load(std::memory_order_relaxed);
    bool toasts_active = ra_has_active_toasts();

    if (!menu_open && !toasts_active) {
        return;
    }

    std::lock_guard<std::mutex> lock(s_overlay_mutex);

    if (!s_gl_initialized) {
        init_gl_resources();
    }
    if (!s_gl_initialized) {
        return;
    }

    if (menu_open) {
        render_menu(win_w, win_h);
    }

    if (toasts_active) {
        render_toast_card(win_w, win_h);
    }
}

} // namespace snp::handheld
