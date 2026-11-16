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

constexpr float SNP_FONT_CHAR_WIDTH = 8.0f;
constexpr float SNP_FONT_CHAR_HEIGHT = 8.0f;
constexpr float SNP_FONT_TEX_SIZE = 128.0f;
constexpr float SNP_FONT_GOLD_ROW_OFFSET = 64.0f;
constexpr float SNP_SOLID_PIXEL_X = 127.0f;
constexpr float SNP_SOLID_PIXEL_Y = 63.0f;
constexpr float SNP_SOLID_UV_U = (SNP_SOLID_PIXEL_X + 0.5f) / SNP_FONT_TEX_SIZE;
constexpr float SNP_SOLID_UV_V = (SNP_SOLID_PIXEL_Y + 0.5f) / SNP_FONT_TEX_SIZE;

constexpr float TOAST_SLIDE_IN_DURATION = 0.35f;
constexpr float TOAST_FADE_OUT_DURATION = 0.50f;
constexpr float TOAST_SLIDE_IN_OFFSET = -320.0f;
constexpr float TOAST_FADE_OUT_OFFSET = -40.0f;
constexpr float TOAST_BASE_HEIGHT = 480.0f;
constexpr float TOAST_CARD_WIDTH = 476.0f;
constexpr float TOAST_CARD_HEIGHT = 74.0f;
constexpr float TOAST_MARGIN = 14.0f;
constexpr float TOAST_BADGE_SIZE = 48.0f;
constexpr float TOAST_BADGE_PAD_LEFT = 11.0f;
constexpr float TOAST_BADGE_PAD_RIGHT = 13.0f;
constexpr float TOAST_CARD_PAD_RIGHT = 12.0f;

enum class MenuPage {
    Main = 0,
    CustomizeControls,
};

enum class MainMenuRow {
    ControlScheme = 0,
    CustomizeControls,
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
constexpr int MAIN_MENU_ROW_COUNT = 16;

enum class CustomMenuRow {
    LoadPreset = 0,
    AimMode,
    Fire,
    Jump,
    Roll,
    RollLeft,
    RollRight,
    ManualAim,
    SwitchTarget,
    PrevTarget,
    NextTarget,
    StrafeLeft,
    StrafeRight,
    ResetToPreset,
    Back,
};
constexpr int CUSTOM_MENU_ROW_COUNT = 15;

enum class ListenSlot {
    None = 0,
    Primary,
    Secondary,
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

MenuPage s_current_page = MenuPage::Main;
int s_selected_main_row = 0;
int s_selected_custom_row = 0;
int s_custom_scroll_offset = 0;
int s_load_preset_idx = 0;

ListenSlot s_listen_slot = ListenSlot::None;
CustomMenuRow s_listen_row = CustomMenuRow::Fire;
uint32_t s_listen_start_time = 0;
constexpr uint32_t LISTEN_TIMEOUT_MS = 3000;

constexpr std::array<const char*, 6> PRESET_NAMES = {
    CONTROL_SCHEME_DPAD_STRAFE_SHIFT,
    CONTROL_SCHEME_BUMPER_STRAFE,
    CONTROL_SCHEME_FACE_STRAFE,
    CONTROL_SCHEME_DPAD_AIM,
    CONTROL_SCHEME_DPAD_STRAFE,
    CONTROL_SCHEME_TWIN_STICK,
};

const char* get_preset_display_name(const std::string& scheme) {
    if (scheme == CONTROL_SCHEME_DPAD_STRAFE_SHIFT) return "Strafe + Shift";
    if (scheme == CONTROL_SCHEME_BUMPER_STRAFE) return "Bumper Strafe";
    if (scheme == CONTROL_SCHEME_FACE_STRAFE) return "Face Strafe";
    if (scheme == CONTROL_SCHEME_DPAD_AIM) return "D-Pad Aim";
    if (scheme == CONTROL_SCHEME_DPAD_STRAFE) return "Face Aim";
    if (scheme == CONTROL_SCHEME_TWIN_STICK) return "Twin Stick";
    return "Custom";
}

ActionBinding* get_binding_for_row(ControlBindings& b, CustomMenuRow row) {
    switch (row) {
    case CustomMenuRow::Fire: return &b.fire;
    case CustomMenuRow::Jump: return &b.jump;
    case CustomMenuRow::Roll: return &b.roll;
    case CustomMenuRow::RollLeft: return &b.roll_left;
    case CustomMenuRow::RollRight: return &b.roll_right;
    case CustomMenuRow::ManualAim: return &b.manual_aim;
    case CustomMenuRow::SwitchTarget: return &b.switch_target;
    case CustomMenuRow::PrevTarget: return &b.prev_target;
    case CustomMenuRow::NextTarget: return &b.next_target;
    case CustomMenuRow::StrafeLeft: return &b.strafe_left;
    case CustomMenuRow::StrafeRight: return &b.strafe_right;
    default: return nullptr;
    }
}

const ActionBinding* get_binding_for_row(const ControlBindings& b, CustomMenuRow row) {
    switch (row) {
    case CustomMenuRow::Fire: return &b.fire;
    case CustomMenuRow::Jump: return &b.jump;
    case CustomMenuRow::Roll: return &b.roll;
    case CustomMenuRow::RollLeft: return &b.roll_left;
    case CustomMenuRow::RollRight: return &b.roll_right;
    case CustomMenuRow::ManualAim: return &b.manual_aim;
    case CustomMenuRow::SwitchTarget: return &b.switch_target;
    case CustomMenuRow::PrevTarget: return &b.prev_target;
    case CustomMenuRow::NextTarget: return &b.next_target;
    case CustomMenuRow::StrafeLeft: return &b.strafe_left;
    case CustomMenuRow::StrafeRight: return &b.strafe_right;
    default: return nullptr;
    }
}

PhysicalButton cycle_physical_button(PhysicalButton current, bool forward) {
    constexpr std::array<PhysicalButton, 10> kButtons = {
        PhysicalButton::None,
        PhysicalButton::A,
        PhysicalButton::B,
        PhysicalButton::X,
        PhysicalButton::Y,
        PhysicalButton::L1,
        PhysicalButton::R1,
        PhysicalButton::L2,
        PhysicalButton::R2,
        PhysicalButton::Start,
    };
    auto it = std::find(kButtons.begin(), kButtons.end(), current);
    int idx = (it != kButtons.end()) ? static_cast<int>(std::distance(kButtons.begin(), it)) : 0;
    int count = static_cast<int>(kButtons.size());
    idx = forward ? (idx + 1) % count : (idx + count - 1) % count;
    return kButtons[idx];
}

bool s_gl_initialized = false;
GLuint s_program = 0;
GLuint s_vao = 0;
GLuint s_vbo = 0;
GLuint s_toast_vao = 0;
GLuint s_toast_vbo = 0;
GLuint s_font_texture = 0;
GLuint s_snp_font_texture = 0;
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

    auto init_vertex_array = [](GLuint& vao, GLuint& vbo) {
        gl::glGenVertexArrays(1, &vao);
        gl::glGenBuffers(1, &vbo);

        gl::glBindVertexArray(vao);
        gl::glBindBuffer(GL_ARRAY_BUFFER, vbo);

        gl::glEnableVertexAttribArray(0);
        gl::glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(OverlayVertex), reinterpret_cast<void*>(0));
        gl::glEnableVertexAttribArray(1);
        gl::glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(OverlayVertex), reinterpret_cast<void*>(2 * sizeof(float)));
        gl::glEnableVertexAttribArray(2);
        gl::glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(OverlayVertex), reinterpret_cast<void*>(4 * sizeof(float)));

        gl::glBindVertexArray(0);
        gl::glBindBuffer(GL_ARRAY_BUFFER, 0);
    };

    init_vertex_array(s_vao, s_vbo);
    init_vertex_array(s_toast_vao, s_toast_vbo);

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

    // Build 128x128 S&P Cyber-Stencil font texture (top: white glyphs, bottom: gold glyphs)
    std::vector<uint8_t> snp_texels(128 * 128 * 4, 0);
    for (int g = 0; g < 95; ++g) {
        int cell_x = (g % 16) * 8;
        int cell_y = (g / 16) * 8;
        for (int y = 0; y < 8; ++y) {
            uint8_t row_byte = SNP_STENCIL_FONT[g][y];
            for (int x = 0; x < 8; ++x) {
                bool pixel_on = (row_byte >> x) & 1;
                int px = cell_x + x;

                // Top half: solid white glyphs for tinted outlines and shadows
                int py_top = cell_y + y;
                int offset_top = (py_top * 128 + px) * 4;
                snp_texels[offset_top + 0] = 255;
                snp_texels[offset_top + 1] = 255;
                snp_texels[offset_top + 2] = 255;
                snp_texels[offset_top + 3] = pixel_on ? 255 : 0;

                // Bottom half: metallic horizon gold glyphs
                int py_gold = 64 + cell_y + y;
                int offset_gold = (py_gold * 128 + px) * 4;
                snp_texels[offset_gold + 0] = SNP_GOLD_ROWS_RGB[y][0];
                snp_texels[offset_gold + 1] = SNP_GOLD_ROWS_RGB[y][1];
                snp_texels[offset_gold + 2] = SNP_GOLD_ROWS_RGB[y][2];
                snp_texels[offset_gold + 3] = pixel_on ? 255 : 0;
            }
        }
    }

    // Set pixel (127, 63) to solid opaque white for untextured primitives
    int snp_solid_offset = (static_cast<int>(SNP_SOLID_PIXEL_Y) * 128 + static_cast<int>(SNP_SOLID_PIXEL_X)) * 4;
    snp_texels[snp_solid_offset + 0] = 255;
    snp_texels[snp_solid_offset + 1] = 255;
    snp_texels[snp_solid_offset + 2] = 255;
    snp_texels[snp_solid_offset + 3] = 255;

    gl::glGenTextures(1, &s_snp_font_texture);
    gl::glBindTexture(GL_TEXTURE_2D, s_snp_font_texture);
    gl::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 128, 128, 0, GL_RGBA, GL_UNSIGNED_BYTE, snp_texels.data());
    gl::glBindTexture(GL_TEXTURE_2D, 0);

    s_gl_initialized = true;
}

std::string get_row_label(MainMenuRow row) {
    switch (row) {
    case MainMenuRow::ControlScheme: return "Control Scheme";
    case MainMenuRow::CustomizeControls: return "Customize Controls >";
    case MainMenuRow::SmartLockOn: return "Smart Lock-On";
    case MainMenuRow::AimCurve: return "Aim Curve";
    case MainMenuRow::AimSpeed: return "Aim Speed";
    case MainMenuRow::FaceButtons: return "Face Buttons";
    case MainMenuRow::SwapSticks: return "Swap Sticks";
    case MainMenuRow::AspectRatio: return "Aspect Ratio";
    case MainMenuRow::Resolution: return "Resolution";
    case MainMenuRow::VSync: return "VSync";
    case MainMenuRow::Rumble: return "Rumble";
    case MainMenuRow::MasterVolume: return "Master Volume";
    case MainMenuRow::RetroAchievements: return "RetroAchievements";
    case MainMenuRow::SyncAchievements: return "Sync Achievements";
    case MainMenuRow::SaveAndResume: return "[ Save & Resume ]";
    case MainMenuRow::SaveAndQuit: return "[ Save & Quit ]";
    }
    return "";
}

std::string get_row_value(MainMenuRow row, const HandheldConfig& cfg) {
    switch (row) {
    case MainMenuRow::ControlScheme:
        if (cfg.controls.scheme == CONTROL_SCHEME_DPAD_STRAFE_SHIFT) return "Stickless: Strafe + Shift";
        if (cfg.controls.scheme == CONTROL_SCHEME_BUMPER_STRAFE) return "Stickless: Bumper Strafe";
        if (cfg.controls.scheme == CONTROL_SCHEME_FACE_STRAFE) return "Stickless: Face Strafe";
        if (cfg.controls.scheme == CONTROL_SCHEME_DPAD_AIM) return "Legacy: D-Pad Aim";
        if (cfg.controls.scheme == CONTROL_SCHEME_DPAD_STRAFE) return "Legacy: Face Aim";
        if (cfg.controls.scheme == CONTROL_SCHEME_TWIN_STICK) return "Twin Stick";
        if (cfg.controls.scheme == CONTROL_SCHEME_CUSTOM) return "Custom";
        return "Custom";
    case MainMenuRow::CustomizeControls:
        return "[ Configure ]";
    case MainMenuRow::SmartLockOn:
        if (cfg.controls.smart_lock_on == SMART_LOCK_HOLD) return "Hold (Hold=Lock)";
        if (cfg.controls.smart_lock_on == SMART_LOCK_OFF) return "Off (N64 Toggle)";
        return "Auto (Hold=Manual)";
    case MainMenuRow::AimCurve:
        if (cfg.controls.aim_curve == AIM_CURVE_LINEAR) return "Linear";
        return "S-Curve (Precision)";
    case MainMenuRow::AimSpeed:
        return std::to_string(static_cast<int>(std::round(cfg.controls.stick_sensitivity))) + "%";
    case MainMenuRow::FaceButtons:
        return (cfg.controls.face_layout == FACE_LAYOUT_POSITIONAL) ? "Positional (Swap)" : "Nintendo (B/A/Y/X)";
    case MainMenuRow::SwapSticks:
        return cfg.controls.swap_sticks ? "On" : "Off";
    case MainMenuRow::AspectRatio: {
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
    case MainMenuRow::Resolution:
        return (cfg.video.resolution_scale >= 2) ? "2x (480p)" : "1x (240p)";
    case MainMenuRow::VSync:
        return cfg.video.vsync ? "On" : "Off";
    case MainMenuRow::Rumble:
        if (!cfg.controls.rumble || cfg.controls.rumble_strength <= 0) {
            return "Off";
        }
        return std::to_string(cfg.controls.rumble_strength) + "%";
    case MainMenuRow::MasterVolume:
        return std::to_string(cfg.audio.master_volume) + "%";
    case MainMenuRow::RetroAchievements:
        return ra_get_status_text();
    case MainMenuRow::SyncAchievements: {
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
    case MainMenuRow::SaveAndResume:
    case MainMenuRow::SaveAndQuit:
        return "";
    }
    return "";
}

void close_overlay_locked() {
    s_listen_slot = ListenSlot::None;
    s_current_page = MenuPage::Main;
    s_overlay_open.store(false, std::memory_order_relaxed);
}

void cycle_row_value(MainMenuRow row, bool forward) {
    auto& cfg = get_mutable_config();
    switch (row) {
    case MainMenuRow::ControlScheme: {
        const std::array<std::string, 7> schemes = {
            CONTROL_SCHEME_DPAD_STRAFE_SHIFT,
            CONTROL_SCHEME_BUMPER_STRAFE,
            CONTROL_SCHEME_FACE_STRAFE,
            CONTROL_SCHEME_DPAD_AIM,
            CONTROL_SCHEME_DPAD_STRAFE,
            CONTROL_SCHEME_TWIN_STICK,
            CONTROL_SCHEME_CUSTOM,
        };
        auto it = std::find(schemes.begin(), schemes.end(), cfg.controls.scheme);
        int idx = (it != schemes.end()) ? static_cast<int>(std::distance(schemes.begin(), it)) : 0;
        int count = static_cast<int>(schemes.size());
        idx = forward ? (idx + 1) % count : (idx + count - 1) % count;
        cfg.controls.scheme = schemes[idx];
        if (cfg.controls.scheme != CONTROL_SCHEME_CUSTOM) {
            cfg.controls.bindings = get_preset_bindings(cfg.controls.scheme);
        }
        break;
    }
    case MainMenuRow::CustomizeControls:
        s_current_page = MenuPage::CustomizeControls;
        s_selected_custom_row = 0;
        break;
    case MainMenuRow::SmartLockOn: {
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
    case MainMenuRow::AimCurve:
        cfg.controls.aim_curve = (cfg.controls.aim_curve == AIM_CURVE_S_CURVE)
            ? AIM_CURVE_LINEAR : AIM_CURVE_S_CURVE;
        break;
    case MainMenuRow::AimSpeed: {
        float delta = forward ? 10.0f : -10.0f;
        cfg.controls.stick_sensitivity = std::clamp(cfg.controls.stick_sensitivity + delta, 50.0f, 200.0f);
        snp::config::set_stick_sensitivity(cfg.controls.stick_sensitivity);
        break;
    }
    case MainMenuRow::FaceButtons:
        cfg.controls.face_layout = (cfg.controls.face_layout == FACE_LAYOUT_NINTENDO)
            ? FACE_LAYOUT_POSITIONAL : FACE_LAYOUT_NINTENDO;
        break;
    case MainMenuRow::SwapSticks:
        cfg.controls.swap_sticks = !cfg.controls.swap_sticks;
        break;
    case MainMenuRow::AspectRatio:
        cfg.video.aspect_ratio = (cfg.video.aspect_ratio == ASPECT_RATIO_EXPAND)
            ? ASPECT_RATIO_ORIGINAL : ASPECT_RATIO_EXPAND;
        apply_graphics_config();
        break;
    case MainMenuRow::Resolution:
        cfg.video.resolution_scale = (cfg.video.resolution_scale >= 2) ? 1 : 2;
        apply_graphics_config();
        break;
    case MainMenuRow::VSync:
        cfg.video.vsync = !cfg.video.vsync;
        snp::gles3::set_vsync_enabled(cfg.video.vsync);
        break;
    case MainMenuRow::Rumble: {
        int current = (cfg.controls.rumble && cfg.controls.rumble_strength > 0) ? cfg.controls.rumble_strength : 0;
        int next = std::clamp(current + (forward ? 10 : -10), 0, 100);
        cfg.controls.rumble = (next > 0);
        if (next > 0) {
            cfg.controls.rumble_strength = next;
        }
        break;
    }
    case MainMenuRow::MasterVolume: {
        int delta = forward ? 10 : -10;
        cfg.audio.master_volume = std::clamp(cfg.audio.master_volume + delta, 0, 100);
        break;
    }
    case MainMenuRow::RetroAchievements:
        ra_set_enabled(!ra_is_enabled());
        break;
    case MainMenuRow::SyncAchievements:
        if (ra_is_enabled() && ra_get_status_text() != "No Credentials") {
            ra_sync_now();
        }
        break;
    case MainMenuRow::SaveAndResume:
        save_config();
        close_overlay_locked();
        break;
    case MainMenuRow::SaveAndQuit:
        save_config();
        ultramodern::quit();
        break;
    }
}

std::string get_custom_row_label(CustomMenuRow row) {
    switch (row) {
    case CustomMenuRow::LoadPreset: return "Load Preset";
    case CustomMenuRow::AimMode: return "Aim / D-Pad Mode";
    case CustomMenuRow::Fire: return "Fire (Z)";
    case CustomMenuRow::Jump: return "Jump (R)";
    case CustomMenuRow::Roll: return "Roll";
    case CustomMenuRow::RollLeft: return "Roll Left";
    case CustomMenuRow::RollRight: return "Roll Right";
    case CustomMenuRow::ManualAim: return "Manual Aim";
    case CustomMenuRow::SwitchTarget: return "Switch Target";
    case CustomMenuRow::PrevTarget: return "Prev Target";
    case CustomMenuRow::NextTarget: return "Next Target";
    case CustomMenuRow::StrafeLeft: return "Strafe Left";
    case CustomMenuRow::StrafeRight: return "Strafe Right";
    case CustomMenuRow::ResetToPreset: return "[ Reset to Preset ]";
    case CustomMenuRow::Back: return "[ < Back ]";
    }
    return "";
}

std::string get_custom_row_value(CustomMenuRow row, const HandheldConfig& cfg) {
    if (s_listen_slot != ListenSlot::None && s_listen_row == row) {
        return (s_listen_slot == ListenSlot::Secondary) ? "Press alt button..." : "Press button...";
    }
    switch (row) {
    case CustomMenuRow::LoadPreset:
        return std::string("< ") + get_preset_display_name(PRESET_NAMES[s_load_preset_idx]) + " >";
    case CustomMenuRow::AimMode:
        return aim_dpad_mode_display_name(cfg.controls.bindings.aim_mode);
    case CustomMenuRow::ResetToPreset:
    case CustomMenuRow::Back:
        return "";
    default: {
        const ActionBinding* b = get_binding_for_row(cfg.controls.bindings, row);
        if (!b) return "";
        if (b->primary == PhysicalButton::None && b->secondary == PhysicalButton::None) {
            return "None";
        }
        if (b->primary != PhysicalButton::None && b->secondary != PhysicalButton::None) {
            return std::string(physical_button_display_name(b->primary)) + ", " +
                   physical_button_display_name(b->secondary);
        }
        if (b->primary != PhysicalButton::None) {
            return physical_button_display_name(b->primary);
        }
        return physical_button_display_name(b->secondary);
    }
    }
}

void build_main_menu_mesh(int win_w, int win_h) {
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
    float row_height = 17.0f;
    float label_x = panel_x0 + 36.0f;
    float value_x = panel_x0 + 260.0f;
    float text_scale = 1.25f;

    for (int i = 0; i < MAIN_MENU_ROW_COUNT; ++i) {
        MainMenuRow row = static_cast<MainMenuRow>(i);
        float y = row_start_y + i * row_height;
        bool is_selected = (s_selected_main_row == i);

        if (is_selected) {
            add_solid_rect(panel_x0 + 16.0f, y - 2.0f, panel_x1 - 16.0f, y + 13.0f, COLOR_ROW_HIGHLIGHT);
            add_outline_rect(panel_x0 + 16.0f, y - 2.0f, panel_x1 - 16.0f, y + 13.0f, 1.0f, COLOR_ROW_BORDER);
            add_char(panel_x0 + 20.0f, y, text_scale, '>', COLOR_YELLOW);
        }

        std::string label = get_row_label(row);
        std::string value = get_row_value(row, cfg);

        if (row == MainMenuRow::SaveAndResume || row == MainMenuRow::SaveAndQuit) {
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

    float legend_box_y0 = row_start_y + MAIN_MENU_ROW_COUNT * row_height + 5.0f;
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
        legend_l1 = "Aim: D-Pad | Strafe/2xRoll: L1/R1 | Fire: A/R2 | Jump: B";
        legend_l2 = "Lock-On: X/L2 (" + lock_desc + ") | Pause: Start";
    } else if (cfg.controls.scheme == CONTROL_SCHEME_FACE_STRAFE) {
        legend_l1 = "Aim: D-Pad | Strafe/2xRoll: Y(L) B(R) | Roll: A";
        legend_l2 = "Fire: R1/R2 | Jump: L1/X | Lock-On: L2 (" + lock_desc + ") | Start: Pause";
    } else if (cfg.controls.scheme == CONTROL_SCHEME_DPAD_STRAFE_SHIFT) {
        legend_l1 = "Move/2xRoll: D-Pad L/R | Hold L1/R1+D-Pad: Manual Aim";
        legend_l2 = "Fire: A | Jump: B | Roll: Y | Recenter/Switch: X";
    } else if (cfg.controls.scheme == CONTROL_SCHEME_DPAD_AIM) {
        legend_l1 = "Aim: D-Pad | Strafe: Y(L) A(R) | Fire: R1/R2 | Jump: L1";
        legend_l2 = "Roll: B/L2 | Lock-On: X (" + lock_desc + ") | Pause: Start";
    } else if (cfg.controls.scheme == CONTROL_SCHEME_DPAD_STRAFE) {
        legend_l1 = "Aim: Diamond(X/B/Y/A) | Strafe: D-Pad L/R";
        legend_l2 = "Fire: R1 | Jump: L1 | Roll: L2 | Lock-On: R2 (" + lock_desc + ")";
    } else if (cfg.controls.scheme == CONTROL_SCHEME_CUSTOM) {
        legend_l1 = "Custom Remap | Mode: " + std::string(aim_dpad_mode_display_name(cfg.controls.bindings.aim_mode));
        legend_l2 = "Fire: " + std::string(physical_button_display_name(cfg.controls.bindings.fire.primary)) +
                    " | Jump: " + std::string(physical_button_display_name(cfg.controls.bindings.jump.primary)) +
                    " | Select [Customize Controls] to edit";
    } else {
        if (cfg.controls.swap_sticks) {
            legend_l1 = "Aim: Left Stick | Move: Right Stick | Fire: R1/R2";
        } else {
            legend_l1 = "Aim: Right Stick | Move: Left Stick | Fire: R1/R2";
        }
        legend_l2 = "Jump: L1/L2 | Roll: X | Lock-On: A (" + lock_desc + ") | Start: Pause";
    }

    add_string(box_x0 + 10.0f, legend_box_y0 + 5.0f, 1.0f, "ACTIVE CONTROLS (" + get_row_value(MainMenuRow::ControlScheme, cfg) + "):", COLOR_GOLD);
    add_string(box_x0 + 10.0f, legend_box_y0 + 22.0f, 1.1f, legend_l1, COLOR_WHITE);
    add_string(box_x0 + 10.0f, legend_box_y0 + 40.0f, 1.1f, legend_l2, COLOR_WHITE);

    const std::string footer = "D-Pad: Select/Change | A: Select | Select or Menu: Save & Close";
    float footer_w = footer.size() * FONT_CHAR_WIDTH * 1.0f;
    float footer_x = panel_x0 + (panel_w - footer_w) * 0.5f;
    float footer_y = panel_y1 - 16.0f;
    add_string(footer_x, footer_y, 1.0f, footer, COLOR_MUTED_HINT);
}

void build_customize_controls_mesh(int win_w, int win_h) {
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

    const std::string title = "SIN AND PUNISHMENT - CUSTOMIZE CONTROLS";
    float title_scale = 1.4f;
    float title_w = title.size() * FONT_CHAR_WIDTH * title_scale;
    float title_x = panel_x0 + (panel_w - title_w) * 0.5f;
    float title_y = panel_y0 + 10.0f;
    add_string(title_x, title_y, title_scale, title, COLOR_GOLD);

    float sep_y = title_y + 15.0f;
    add_solid_rect(panel_x0 + 16.0f, sep_y, panel_x1 - 16.0f, sep_y + 1.0f, COLOR_SEPARATOR);

    float row_start_y = sep_y + 8.0f;
    float row_height = 20.0f;
    float label_x = panel_x0 + 36.0f;
    float value_x = panel_x0 + 260.0f;
    float text_scale = 1.25f;

    float available_height = (panel_y1 - 24.0f) - row_start_y;
    int max_visible_rows = std::max(5, static_cast<int>(available_height / row_height));
    int visible_count = std::min(CUSTOM_MENU_ROW_COUNT, max_visible_rows);

    if (s_selected_custom_row < s_custom_scroll_offset) {
        s_custom_scroll_offset = s_selected_custom_row;
    } else if (s_selected_custom_row >= s_custom_scroll_offset + visible_count) {
        s_custom_scroll_offset = s_selected_custom_row - visible_count + 1;
    }
    s_custom_scroll_offset = std::clamp(s_custom_scroll_offset, 0, std::max(0, CUSTOM_MENU_ROW_COUNT - visible_count));

    for (int vi = 0; vi < visible_count; ++vi) {
        int i = s_custom_scroll_offset + vi;
        if (i >= CUSTOM_MENU_ROW_COUNT) break;

        CustomMenuRow row = static_cast<CustomMenuRow>(i);
        float y = row_start_y + vi * row_height;
        bool is_selected = (s_selected_custom_row == i);

        if (is_selected) {
            add_solid_rect(panel_x0 + 16.0f, y - 2.0f, panel_x1 - 16.0f, y + 15.0f, COLOR_ROW_HIGHLIGHT);
            add_outline_rect(panel_x0 + 16.0f, y - 2.0f, panel_x1 - 16.0f, y + 15.0f, 1.0f, COLOR_ROW_BORDER);
            add_char(panel_x0 + 20.0f, y, text_scale, '>', COLOR_YELLOW);
        }

        std::string label = get_custom_row_label(row);
        std::string value = get_custom_row_value(row, cfg);

        if (row == CustomMenuRow::ResetToPreset || row == CustomMenuRow::Back) {
            float action_w = label.size() * FONT_CHAR_WIDTH * text_scale;
            float action_x = panel_x0 + (panel_w - action_w) * 0.5f;
            ColorRgba action_color = is_selected ? COLOR_WHITE : COLOR_MUTED_LABEL;
            add_string(action_x, y, text_scale, label, action_color);
        } else {
            ColorRgba label_color = is_selected ? COLOR_WHITE : COLOR_MUTED_LABEL;
            ColorRgba value_color = (s_listen_slot != ListenSlot::None && s_listen_row == row)
                ? COLOR_YELLOW
                : (is_selected ? COLOR_CYAN : COLOR_MUTED_VALUE);

            add_string(label_x, y, text_scale, label, label_color);
            add_string(value_x, y, text_scale, value, value_color);
        }
    }

    if (s_custom_scroll_offset > 0) {
        add_char(panel_x1 - 28.0f, row_start_y - 2.0f, 1.0f, '^', COLOR_YELLOW);
    }
    if (s_custom_scroll_offset + visible_count < CUSTOM_MENU_ROW_COUNT) {
        add_char(panel_x1 - 28.0f, row_start_y + (visible_count - 1) * row_height + 2.0f, 1.0f, 'v', COLOR_YELLOW);
    }

    const std::string footer = "A:Bind  L/R:Cycle  X:Alt  Y:Clear  B:Back";
    float footer_w = footer.size() * FONT_CHAR_WIDTH * 1.0f;
    float footer_x = panel_x0 + (panel_w - footer_w) * 0.5f;
    float footer_y = panel_y1 - 16.0f;
    add_string(footer_x, footer_y, 1.0f, footer, COLOR_MUTED_HINT);
}

void build_overlay_mesh(int win_w, int win_h) {
    if (s_current_page == MenuPage::CustomizeControls) {
        build_customize_controls_mesh(win_w, win_h);
    } else {
        build_main_menu_mesh(win_w, win_h);
    }
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
            if (s_snp_font_texture) gl::glDeleteTextures(1, &s_snp_font_texture);
            if (s_toast_vbo) gl::glDeleteBuffers(1, &s_toast_vbo);
            if (s_toast_vao) gl::glDeleteVertexArrays(1, &s_toast_vao);
            if (s_vbo) gl::glDeleteBuffers(1, &s_vbo);
            if (s_vao) gl::glDeleteVertexArrays(1, &s_vao);
            if (s_program) gl::glDeleteProgram(s_program);
        }
        s_toast_badge_texture = 0;
        s_last_badge_pixels.clear();
        s_font_texture = 0;
        s_snp_font_texture = 0;
        s_toast_vbo = 0;
        s_toast_vao = 0;
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
    s_current_page = MenuPage::Main;
    s_listen_slot = ListenSlot::None;
    s_mesh_dirty = true;
    s_overlay_open.store(true, std::memory_order_relaxed);
}

void close_overlay() {
    std::lock_guard<std::mutex> lock(s_overlay_mutex);
    close_overlay_locked();
}

void toggle_overlay() {
    std::lock_guard<std::mutex> lock(s_overlay_mutex);
    bool current = s_overlay_open.load(std::memory_order_relaxed);
    if (current) {
        save_config();
        close_overlay_locked();
    } else {
        s_current_page = MenuPage::Main;
        s_listen_slot = ListenSlot::None;
        s_mesh_dirty = true;
        s_overlay_open.store(true, std::memory_order_relaxed);
    }
}

bool is_overlay_listening() {
    std::lock_guard<std::mutex> lock(s_overlay_mutex);
    if (!s_overlay_open.load(std::memory_order_relaxed)) {
        return false;
    }
    if (s_listen_slot != ListenSlot::None) {
        if (SDL_GetTicks() - s_listen_start_time >= LISTEN_TIMEOUT_MS) {
            s_listen_slot = ListenSlot::None;
            s_mesh_dirty = true;
            return false;
        }
        return true;
    }
    return false;
}

void cancel_overlay_listening() {
    std::lock_guard<std::mutex> lock(s_overlay_mutex);
    if (s_listen_slot != ListenSlot::None) {
        s_listen_slot = ListenSlot::None;
        s_mesh_dirty = true;
    }
}

void deliver_overlay_listen_button(PhysicalButton btn) {
    std::lock_guard<std::mutex> lock(s_overlay_mutex);
    if (s_listen_slot == ListenSlot::None || btn == PhysicalButton::None) {
        return;
    }
    auto& cfg = get_mutable_config();
    ActionBinding* target = get_binding_for_row(cfg.controls.bindings, s_listen_row);
    if (target != nullptr) {
        if (s_listen_slot == ListenSlot::Primary) {
            target->primary = btn;
        } else if (s_listen_slot == ListenSlot::Secondary) {
            target->secondary = btn;
        }
        cfg.controls.scheme = CONTROL_SCHEME_CUSTOM;
    }
    s_listen_slot = ListenSlot::None;
    s_mesh_dirty = true;
}

void update_overlay_input(bool up, bool down, bool left, bool right,
                          bool btn_a, bool btn_b, bool btn_x, bool btn_y,
                          bool btn_start) {
    if (!s_overlay_open.load(std::memory_order_relaxed)) {
        return;
    }

    std::lock_guard<std::mutex> lock(s_overlay_mutex);

    if (up || down || left || right || btn_a || btn_b || btn_x || btn_y || btn_start) {
        s_mesh_dirty = true;
    }

    if (s_current_page == MenuPage::CustomizeControls) {
        if (up) {
            s_selected_custom_row = (s_selected_custom_row + CUSTOM_MENU_ROW_COUNT - 1) % CUSTOM_MENU_ROW_COUNT;
        }
        if (down) {
            s_selected_custom_row = (s_selected_custom_row + 1) % CUSTOM_MENU_ROW_COUNT;
        }

        CustomMenuRow cur_row = static_cast<CustomMenuRow>(s_selected_custom_row);
        auto& cfg = get_mutable_config();

        if (btn_b) {
            s_current_page = MenuPage::Main;
            s_listen_slot = ListenSlot::None;
            return;
        }

        if (cur_row == CustomMenuRow::Back) {
            if (btn_a || btn_start) {
                s_current_page = MenuPage::Main;
                s_listen_slot = ListenSlot::None;
                return;
            }
        } else if (cur_row == CustomMenuRow::ResetToPreset) {
            if (btn_a) {
                cfg.controls.bindings = get_preset_bindings(PRESET_NAMES[s_load_preset_idx]);
                cfg.controls.scheme = CONTROL_SCHEME_CUSTOM;
            }
        } else if (cur_row == CustomMenuRow::LoadPreset) {
            if (left || right) {
                int delta = right ? 1 : -1;
                int count = static_cast<int>(PRESET_NAMES.size());
                s_load_preset_idx = (s_load_preset_idx + delta + count) % count;
                cfg.controls.bindings = get_preset_bindings(PRESET_NAMES[s_load_preset_idx]);
                cfg.controls.scheme = CONTROL_SCHEME_CUSTOM;
            } else if (btn_a) {
                cfg.controls.bindings = get_preset_bindings(PRESET_NAMES[s_load_preset_idx]);
                cfg.controls.scheme = CONTROL_SCHEME_CUSTOM;
            }
        } else if (cur_row == CustomMenuRow::AimMode) {
            if (left || right || btn_a) {
                int mode_idx = static_cast<int>(cfg.controls.bindings.aim_mode);
                int count = 4;
                int delta = left ? -1 : 1;
                mode_idx = (mode_idx + delta + count) % count;
                cfg.controls.bindings.aim_mode = static_cast<AimDpadMode>(mode_idx);
                cfg.controls.scheme = CONTROL_SCHEME_CUSTOM;
            }
        } else {
            ActionBinding* binding = get_binding_for_row(cfg.controls.bindings, cur_row);
            if (binding != nullptr) {
                if (btn_a) {
                    s_listen_slot = ListenSlot::Primary;
                    s_listen_row = cur_row;
                    s_listen_start_time = SDL_GetTicks();
                } else if (btn_x) {
                    s_listen_slot = ListenSlot::Secondary;
                    s_listen_row = cur_row;
                    s_listen_start_time = SDL_GetTicks();
                } else if (left || right) {
                    binding->primary = cycle_physical_button(binding->primary, right);
                    cfg.controls.scheme = CONTROL_SCHEME_CUSTOM;
                } else if (btn_y) {
                    binding->primary = PhysicalButton::None;
                    binding->secondary = PhysicalButton::None;
                    cfg.controls.scheme = CONTROL_SCHEME_CUSTOM;
                }
            }
        }

        if (btn_start && cur_row != CustomMenuRow::Back) {
            save_config();
            close_overlay_locked();
        }
        return;
    }

    if (up) {
        s_selected_main_row = (s_selected_main_row + MAIN_MENU_ROW_COUNT - 1) % MAIN_MENU_ROW_COUNT;
    }
    if (down) {
        s_selected_main_row = (s_selected_main_row + 1) % MAIN_MENU_ROW_COUNT;
    }

    MainMenuRow current = static_cast<MainMenuRow>(s_selected_main_row);

    if (current == MainMenuRow::CustomizeControls) {
        if (btn_a || right) {
            s_current_page = MenuPage::CustomizeControls;
            s_selected_custom_row = 0;
            return;
        }
    } else {
        if (left) {
            cycle_row_value(current, false);
        }
        if (right) {
            cycle_row_value(current, true);
        }
        if (btn_a) {
            cycle_row_value(current, true);
        }
    }

    if (btn_start) {
        if (current == MainMenuRow::SaveAndQuit) {
            save_config();
            ultramodern::quit();
        } else {
            save_config();
            close_overlay_locked();
        }
    }
    if (btn_b) {
        save_config();
        s_listen_slot = ListenSlot::None;
        s_overlay_open.store(false, std::memory_order_relaxed);
    }
}

void render_menu(int win_w, int win_h) {
    if (s_listen_slot != ListenSlot::None) {
        if (SDL_GetTicks() - s_listen_start_time >= LISTEN_TIMEOUT_MS) {
            s_listen_slot = ListenSlot::None;
            s_mesh_dirty = true;
        }
    }
    static std::string s_last_ra_snapshot;
    std::string ra_snapshot = ra_get_status_text() + "|" +
                              (ra_is_syncing() ? "1" : "0") + "|" +
                              (ra_is_offline_mode() ? "1" : "0") + "|" +
                              std::to_string(ra_get_pending_sync_count());
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

std::string to_upper_ascii(const std::string& str) {
    std::string res;
    res.reserve(str.size());
    for (char c : str) {
        res.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return res;
}

float measure_snp_text(const std::string& text, float scale_x, float tracking) {
    if (text.empty()) return 0.0f;
    float total = 0.0f;
    for (size_t i = 0; i < text.size(); ++i) {
        char ch = text[i];
        int code = (ch >= 32 && ch <= 126) ? static_cast<int>(ch) : static_cast<int>('?');
        float adv = static_cast<float>(SNP_STENCIL_ADV[code - 32]) * scale_x;
        total += adv + (i + 1 < text.size() ? tracking : 0.0f);
    }
    return total;
}

std::string truncate_snp_text(const std::string& text, float scale_x, float tracking, float max_w) {
    if (max_w <= 0.0f || text.empty()) return "";
    if (measure_snp_text(text, scale_x, tracking) <= max_w) {
        return text;
    }
    float ell_w = measure_snp_text("...", scale_x, tracking);
    std::string trimmed;
    float cur = 0.0f;
    for (char ch : text) {
        int code = (ch >= 32 && ch <= 126) ? static_cast<int>(ch) : static_cast<int>('?');
        float cw = static_cast<float>(SNP_STENCIL_ADV[code - 32]) * scale_x + tracking;
        if (cur + cw + ell_w > max_w) {
            break;
        }
        trimmed.push_back(ch);
        cur += cw;
    }
    while (!trimmed.empty() && trimmed.back() == ' ') {
        trimmed.pop_back();
    }
    return trimmed + "...";
}

void add_solid_rect_to(std::vector<OverlayVertex>& list,
                       float x0, float y0, float x1, float y1,
                       const ColorRgba& c) {
    list.push_back({x0, y0, SNP_SOLID_UV_U, SNP_SOLID_UV_V, c.r, c.g, c.b, c.a});
    list.push_back({x1, y0, SNP_SOLID_UV_U, SNP_SOLID_UV_V, c.r, c.g, c.b, c.a});
    list.push_back({x1, y1, SNP_SOLID_UV_U, SNP_SOLID_UV_V, c.r, c.g, c.b, c.a});

    list.push_back({x0, y0, SNP_SOLID_UV_U, SNP_SOLID_UV_V, c.r, c.g, c.b, c.a});
    list.push_back({x1, y1, SNP_SOLID_UV_U, SNP_SOLID_UV_V, c.r, c.g, c.b, c.a});
    list.push_back({x0, y1, SNP_SOLID_UV_U, SNP_SOLID_UV_V, c.r, c.g, c.b, c.a});
}

void add_vgrad_rect_to(std::vector<OverlayVertex>& list,
                       float x0, float y0, float x1, float y1,
                       const ColorRgba& c_top, const ColorRgba& c_bot) {
    list.push_back({x0, y0, SNP_SOLID_UV_U, SNP_SOLID_UV_V, c_top.r, c_top.g, c_top.b, c_top.a});
    list.push_back({x1, y0, SNP_SOLID_UV_U, SNP_SOLID_UV_V, c_top.r, c_top.g, c_top.b, c_top.a});
    list.push_back({x1, y1, SNP_SOLID_UV_U, SNP_SOLID_UV_V, c_bot.r, c_bot.g, c_bot.b, c_bot.a});

    list.push_back({x0, y0, SNP_SOLID_UV_U, SNP_SOLID_UV_V, c_top.r, c_top.g, c_top.b, c_top.a});
    list.push_back({x1, y1, SNP_SOLID_UV_U, SNP_SOLID_UV_V, c_bot.r, c_bot.g, c_bot.b, c_bot.a});
    list.push_back({x0, y1, SNP_SOLID_UV_U, SNP_SOLID_UV_V, c_bot.r, c_bot.g, c_bot.b, c_bot.a});
}

void add_triangle_to(std::vector<OverlayVertex>& list,
                     float x0, float y0, const ColorRgba& c0,
                     float x1, float y1, const ColorRgba& c1,
                     float x2, float y2, const ColorRgba& c2) {
    list.push_back({x0, y0, SNP_SOLID_UV_U, SNP_SOLID_UV_V, c0.r, c0.g, c0.b, c0.a});
    list.push_back({x1, y1, SNP_SOLID_UV_U, SNP_SOLID_UV_V, c1.r, c1.g, c1.b, c1.a});
    list.push_back({x2, y2, SNP_SOLID_UV_U, SNP_SOLID_UV_V, c2.r, c2.g, c2.b, c2.a});
}

void add_badge_quad(std::vector<OverlayVertex>& list,
                    float bx0, float by0, float bx1, float by1, float alpha) {
    ColorRgba c{1.0f, 1.0f, 1.0f, alpha};
    list.push_back({bx0, by0, 0.0f, 0.0f, c.r, c.g, c.b, c.a});
    list.push_back({bx1, by0, 1.0f, 0.0f, c.r, c.g, c.b, c.a});
    list.push_back({bx1, by1, 1.0f, 1.0f, c.r, c.g, c.b, c.a});

    list.push_back({bx0, by0, 0.0f, 0.0f, c.r, c.g, c.b, c.a});
    list.push_back({bx1, by1, 1.0f, 1.0f, c.r, c.g, c.b, c.a});
    list.push_back({bx0, by1, 0.0f, 1.0f, c.r, c.g, c.b, c.a});
}

void add_snp_glyph(std::vector<OverlayVertex>& list,
                   float x, float y, float scale_x, float scale_y,
                   char ch, bool use_gold, const ColorRgba& c) {
    if (ch < 32 || ch > 126) ch = '?';
    int glyph_idx = ch - 32;
    int col = glyph_idx % 16;
    int row = glyph_idx / 16;
    float v_offset = use_gold ? SNP_FONT_GOLD_ROW_OFFSET : 0.0f;

    float u0 = (static_cast<float>(col) * SNP_FONT_CHAR_WIDTH) / SNP_FONT_TEX_SIZE;
    float v0 = (v_offset + static_cast<float>(row) * SNP_FONT_CHAR_HEIGHT) / SNP_FONT_TEX_SIZE;
    float u1 = ((static_cast<float>(col) + 1.0f) * SNP_FONT_CHAR_WIDTH) / SNP_FONT_TEX_SIZE;
    float v1 = (v_offset + (static_cast<float>(row) + 1.0f) * SNP_FONT_CHAR_HEIGHT) / SNP_FONT_TEX_SIZE;

    float w = SNP_FONT_CHAR_WIDTH * scale_x;
    float h = SNP_FONT_CHAR_HEIGHT * scale_y;

    list.push_back({x, y, u0, v0, c.r, c.g, c.b, c.a});
    list.push_back({x + w, y, u1, v0, c.r, c.g, c.b, c.a});
    list.push_back({x + w, y + h, u1, v1, c.r, c.g, c.b, c.a});

    list.push_back({x, y, u0, v0, c.r, c.g, c.b, c.a});
    list.push_back({x + w, y + h, u1, v1, c.r, c.g, c.b, c.a});
    list.push_back({x, y + h, u0, v1, c.r, c.g, c.b, c.a});
}

void add_snp_text(std::vector<OverlayVertex>& list,
                  float x, float y, float scale_x, float scale_y,
                  const std::string& text, float tracking,
                  bool outline_8way, int outline_px,
                  const ColorRgba* outline_color,
                  bool use_gold, const ColorRgba& text_color,
                  float max_w) {
    std::string clipped = truncate_snp_text(text, scale_x, tracking, max_w);
    if (clipped.empty()) return;

    if (outline_color != nullptr) {
        if (outline_8way) {
            for (int dy = -outline_px; dy <= outline_px; ++dy) {
                for (int dx = -outline_px; dx <= outline_px; ++dx) {
                    if (dx == 0 && dy == 0) continue;
                    float cx = x + static_cast<float>(dx);
                    for (char ch : clipped) {
                        int code = (ch >= 32 && ch <= 126) ? static_cast<int>(ch) : static_cast<int>('?');
                        add_snp_glyph(list, cx, y + static_cast<float>(dy), scale_x, scale_y, ch, false, *outline_color);
                        cx += static_cast<float>(SNP_STENCIL_ADV[code - 32]) * scale_x + tracking;
                    }
                }
            }
        } else {
            constexpr int OFFSETS[6][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {1, 1}, {0, 2}};
            float step = static_cast<float>(outline_px);
            for (const auto& off : OFFSETS) {
                float cx = x + static_cast<float>(off[0]) * step;
                for (char ch : clipped) {
                    int code = (ch >= 32 && ch <= 126) ? static_cast<int>(ch) : static_cast<int>('?');
                    add_snp_glyph(list, cx, y + static_cast<float>(off[1]) * step, scale_x, scale_y, ch, false, *outline_color);
                    cx += static_cast<float>(SNP_STENCIL_ADV[code - 32]) * scale_x + tracking;
                }
            }
        }
    }

    float cx = x;
    for (char ch : clipped) {
        int code = (ch >= 32 && ch <= 126) ? static_cast<int>(ch) : static_cast<int>('?');
        add_snp_glyph(list, cx, y, scale_x, scale_y, ch, use_gold, text_color);
        cx += static_cast<float>(SNP_STENCIL_ADV[code - 32]) * scale_x + tracking;
    }
}

void calculate_toast_animation(float elapsed, float duration, float& out_alpha, float& out_x_slide) {
    out_alpha = 1.0f;
    out_x_slide = 0.0f;
    if (elapsed < TOAST_SLIDE_IN_DURATION) {
        float t = elapsed / TOAST_SLIDE_IN_DURATION;
        out_alpha = t;
        out_x_slide = (1.0f - t) * TOAST_SLIDE_IN_OFFSET;
    } else if (elapsed > duration - TOAST_FADE_OUT_DURATION) {
        float t = std::clamp((duration - elapsed) / TOAST_FADE_OUT_DURATION, 0.0f, 1.0f);
        out_alpha = t;
        out_x_slide = (1.0f - t) * TOAST_FADE_OUT_OFFSET;
    }
}

void build_toast_badge_well(std::vector<OverlayVertex>& toast_verts,
                           std::vector<OverlayVertex>& badge_verts,
                           const RaToastItem& toast,
                           float x0, float y0, float card_h, float s,
                           float alpha, float& out_bx1) {
    float bsize = TOAST_BADGE_SIZE * s;
    float bx0 = x0 + TOAST_BADGE_PAD_LEFT * s;
    float by0 = y0 + (card_h - bsize) * 0.5f;
    float bx1 = bx0 + bsize;
    float by1 = by0 + bsize;
    out_bx1 = bx1;

    ColorRgba crimson_border{115.0f / 255.0f, 16.0f / 255.0f, 0.0f, 0.98f * alpha};
    if (toast.accent_kind == 3) {
        crimson_border = ColorRgba{185.0f / 255.0f, 28.0f / 255.0f, 12.0f / 255.0f, 0.98f * alpha};
    }
    ColorRgba oxblood_fill{24.0f / 255.0f, 0.0f, 0.0f, 0.96f * alpha};

    add_solid_rect_to(toast_verts, bx0 - 3.0f * s, by0 - 3.0f * s, bx1 + 3.0f * s, by1 + 3.0f * s, crimson_border);
    add_solid_rect_to(toast_verts, bx0 - 1.0f * s, by0 - 1.0f * s, bx1 + 1.0f * s, by1 + 1.0f * s, oxblood_fill);

    if (!toast.rgba_pixels.empty()) {
        add_badge_quad(badge_verts, bx0, by0, bx1, by1, alpha);
    }
}

void build_toast_header_banner(std::vector<OverlayVertex>& toast_verts,
                              const RaToastItem& toast,
                              float tx, float y0, float x1, float s, float alpha) {
    float avail = (x1 - TOAST_CARD_PAD_RIGHT * s) - tx;
    std::string hdr_text = to_upper_ascii(toast.title);
    float hdr_sx = 1.5f * s;
    float hdr_sy = 1.34f * s;
    float box_pad_x = 9.0f * s;
    float hdr_w = measure_snp_text(hdr_text, hdr_sx, 1.0f * s);
    if (hdr_w + box_pad_x * 2.0f > avail) {
        hdr_sx = 1.0f * s;
        hdr_sy = 1.0f * s;
        hdr_w = measure_snp_text(hdr_text, hdr_sx, 1.0f * s);
    }
    float hbox_x0 = tx;
    float hbox_x1 = std::min(x1 - TOAST_CARD_PAD_RIGHT * s, tx + hdr_w + box_pad_x * 2.0f);
    float hbox_y0 = y0 + 6.0f * s;
    float hbox_y1 = y0 + 25.0f * s;

    ColorRgba crimson_border{115.0f / 255.0f, 16.0f / 255.0f, 0.0f, 0.98f * alpha};
    if (toast.accent_kind == 3) {
        crimson_border = ColorRgba{185.0f / 255.0f, 28.0f / 255.0f, 12.0f / 255.0f, 0.98f * alpha};
    }
    ColorRgba oxblood_fill{24.0f / 255.0f, 0.0f, 0.0f, 0.96f * alpha};

    add_solid_rect_to(toast_verts, hbox_x0, hbox_y0, hbox_x1, hbox_y1, crimson_border);
    add_solid_rect_to(toast_verts, hbox_x0 + 2.0f * s, hbox_y0 + 2.0f * s,
                      hbox_x1 - 2.0f * s, hbox_y1 - 2.0f * s, oxblood_fill);

    ColorRgba crimson_outline{148.0f / 255.0f, 16.0f / 255.0f, 0.0f, 0.98f * alpha};
    ColorRgba white_tint{1.0f, 1.0f, 1.0f, alpha};
    int outline_px = std::max(1, static_cast<int>(std::round(1.0f * s)));
    add_snp_text(toast_verts,
                 hbox_x0 + box_pad_x, hbox_y0 + 4.0f * s,
                 hdr_sx, hdr_sy,
                 hdr_text, 1.0f * s,
                 true, outline_px, &crimson_outline,
                 true, white_tint,
                 (hbox_x1 - hbox_x0) - box_pad_x * 1.5f);
}

void build_toast_selection_cursor(std::vector<OverlayVertex>& toast_verts,
                                 float tx, float row2_y, float s, float alpha) {
    ColorRgba bronze_shadow{65.0f / 255.0f, 32.0f / 255.0f, 0.0f, 0.95f * alpha};
    ColorRgba cursor_gold_top{1.0f, 1.0f, 0.92f, alpha};
    ColorRgba cursor_gold_mid{1.0f, 0.85f, 0.50f, alpha};
    ColorRgba cursor_gold_bot{1.0f, 0.72f, 0.28f, alpha};

    float cy_top = row2_y + 2.0f * s;
    float cy_mid = row2_y + 8.0f * s;
    float cy_bot = row2_y + 14.0f * s;

    // Drop shadow for '|>' cursor
    add_solid_rect_to(toast_verts, tx + 1.0f * s, cy_top + 1.0f * s, tx + 3.0f * s, cy_bot + 1.0f * s, bronze_shadow);
    add_triangle_to(toast_verts,
                    tx + 5.0f * s, cy_top + 1.0f * s, bronze_shadow,
                    tx + 11.0f * s, cy_mid + 1.0f * s, bronze_shadow,
                    tx + 5.0f * s, cy_bot + 1.0f * s, bronze_shadow);

    // Foreground '|>' cursor
    add_vgrad_rect_to(toast_verts, tx, cy_top, tx + 2.0f * s, cy_bot, cursor_gold_top, cursor_gold_bot);
    add_triangle_to(toast_verts,
                    tx + 4.0f * s, cy_top, cursor_gold_top,
                    tx + 10.0f * s, cy_mid, cursor_gold_mid,
                    tx + 4.0f * s, cy_bot, cursor_gold_bot);
}

void build_toast_subtitle_row(std::vector<OverlayVertex>& toast_verts,
                              const RaToastItem& toast,
                              float tx, float row2_y, float x1, float s, float alpha) {
    float avail = (x1 - TOAST_CARD_PAD_RIGHT * s) - tx;
    build_toast_selection_cursor(toast_verts, tx, row2_y, s, alpha);

    std::string sub_upper = to_upper_ascii(toast.subtitle);
    float sub_scale = (sub_upper.length() <= 22 ? 2.0f : 1.0f) * s;
    ColorRgba bronze_shadow{65.0f / 255.0f, 32.0f / 255.0f, 0.0f, 0.95f * alpha};
    ColorRgba white_tint{1.0f, 1.0f, 1.0f, alpha};

    int outline_px = std::max(1, static_cast<int>(std::round(1.0f * s)));
    add_snp_text(toast_verts,
                 tx + 16.0f * s, row2_y,
                 sub_scale, sub_scale,
                 sub_upper, 0.0f,
                 false, outline_px, &bronze_shadow,
                 true, white_tint,
                 avail - 16.0f * s);
}

void build_toast_detail_row(std::vector<OverlayVertex>& toast_verts,
                            const RaToastItem& toast,
                            float tx, float y0, float x1, float s, float alpha) {
    if (toast.detail.empty()) return;
    float avail = (x1 - TOAST_CARD_PAD_RIGHT * s) - tx;
    std::string detail_upper = to_upper_ascii(toast.detail);
    ColorRgba detail_outline{38.0f / 255.0f, 16.0f / 255.0f, 0.0f, 0.96f * alpha};
    ColorRgba champagne_tint{1.0f, 0.96f, 0.88f, 0.96f * alpha};
    int outline_px = std::max(1, static_cast<int>(std::round(1.0f * s)));

    add_snp_text(toast_verts,
                 tx, y0 + 53.0f * s,
                 1.0f * s, 1.0f * s,
                 detail_upper, 1.0f * s,
                 true, outline_px, &detail_outline,
                 true, champagne_tint,
                 avail);
}

void render_toast_card(int win_w, int win_h) {
    RaToastItem toast;
    if (!ra_get_current_toast(toast)) {
        return;
    }

    float alpha = 1.0f;
    float x_slide = 0.0f;
    calculate_toast_animation(toast.elapsed_sec, toast.duration_sec, alpha, x_slide);

    float s = std::max(1.0f, std::floor(static_cast<float>(win_h) / TOAST_BASE_HEIGHT));
    float card_w = std::min(static_cast<float>(win_w) - 32.0f, TOAST_CARD_WIDTH * s);
    float card_h = TOAST_CARD_HEIGHT * s;
    float x0 = TOAST_MARGIN * s + x_slide;
    float y0 = static_cast<float>(win_h) - card_h - TOAST_MARGIN * s;
    float x1 = x0 + card_w;

    std::vector<OverlayVertex> toast_vertices;
    std::vector<OverlayVertex> badge_vertices;

    ColorRgba smoked_black{0.0f, 0.0f, 0.0f, 0.72f * alpha};
    add_solid_rect_to(toast_vertices, x0, y0, x1, y0 + card_h, smoked_black);

    float bx1 = 0.0f;
    build_toast_badge_well(toast_vertices, badge_vertices, toast, x0, y0, card_h, s, alpha, bx1);

    float tx = bx1 + TOAST_BADGE_PAD_RIGHT * s;

    build_toast_header_banner(toast_vertices, toast, tx, y0, x1, s, alpha);

    float row2_y = y0 + 30.0f * s;
    build_toast_subtitle_row(toast_vertices, toast, tx, row2_y, x1, s, alpha);

    build_toast_detail_row(toast_vertices, toast, tx, y0, x1, s, alpha);

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
        gl::glBindBuffer(GL_ARRAY_BUFFER, s_toast_vbo);
        gl::glBufferData(GL_ARRAY_BUFFER, toast_vertices.size() * sizeof(OverlayVertex), toast_vertices.data(), GL_STREAM_DRAW);
        gl::glBindTexture(GL_TEXTURE_2D, s_snp_font_texture);
        gl::glBindVertexArray(s_toast_vao);
        gl::glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(toast_vertices.size()));
    }

    if (!toast.rgba_pixels.empty() && !badge_vertices.empty()) {
        bool created_badge_tex = false;
        if (s_toast_badge_texture == 0) {
            gl::glGenTextures(1, &s_toast_badge_texture);
            gl::glBindTexture(GL_TEXTURE_2D, s_toast_badge_texture);
            gl::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            gl::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            gl::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            gl::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            created_badge_tex = true;
        } else {
            gl::glBindTexture(GL_TEXTURE_2D, s_toast_badge_texture);
        }

        if (created_badge_tex || s_last_badge_pixels != toast.rgba_pixels) {
            gl::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, toast.rgba_pixels.data());
            s_last_badge_pixels = toast.rgba_pixels;
        }

        gl::glBindBuffer(GL_ARRAY_BUFFER, s_toast_vbo);
        gl::glBufferData(GL_ARRAY_BUFFER, badge_vertices.size() * sizeof(OverlayVertex), badge_vertices.data(), GL_STREAM_DRAW);
        gl::glBindVertexArray(s_toast_vao);
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
