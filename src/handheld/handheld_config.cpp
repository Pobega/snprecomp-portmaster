#include "handheld_config.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "toml++/toml.hpp"
#include "ultramodern/config.hpp"
#include "snp_recomp_hooks.h"

namespace snp::gles3 {
void set_vsync_enabled(bool enabled);
} // namespace snp::gles3

namespace snp::handheld {

namespace {
HandheldConfig g_config;
} // namespace

const HandheldConfig& get_config() {
    return g_config;
}

HandheldConfig& get_mutable_config() {
    return g_config;
}

int get_env_analog_sticks() {
    const char* env_sticks = std::getenv("ANALOG_STICKS");
    if (!env_sticks || env_sticks[0] == '\0') {
        env_sticks = std::getenv("SNP_ANALOG_STICKS");
    }
    if (env_sticks && env_sticks[0] != '\0') {
        try {
            return std::stoi(env_sticks);
        } catch (...) {
            return -1;
        }
    }
    return -1;
}

std::string resolve_default_control_scheme() {
    int sticks = get_env_analog_sticks();
    if (sticks >= 2) {
        return CONTROL_SCHEME_TWIN_STICK;
    }
    return CONTROL_SCHEME_DPAD_STRAFE_SHIFT;
}

std::string resolve_default_smart_lock_on(const std::string& scheme) {
    if (scheme == CONTROL_SCHEME_TWIN_STICK || scheme == CONTROL_SCHEME_TWIN_STICK_SWAPPED) {
        return SMART_LOCK_OFF;
    }
    if (get_env_analog_sticks() >= 1) {
        return SMART_LOCK_OFF;
    }
    return SMART_LOCK_ALWAYS;
}

ControlBindings get_preset_bindings(const std::string& scheme) {
    ControlBindings b{};
    if (scheme == CONTROL_SCHEME_BUMPER_STRAFE) {
        b.aim_mode = AimDpadMode::DpadAim;
        b.strafe_left = {PhysicalButton::L1};
        b.strafe_right = {PhysicalButton::R1};
        b.fire = {PhysicalButton::A, PhysicalButton::R2};
        b.jump = {PhysicalButton::B};
        b.manual_aim = {PhysicalButton::X, PhysicalButton::L2};
        b.pause = {PhysicalButton::Start};
    } else if (scheme == CONTROL_SCHEME_FACE_STRAFE) {
        b.aim_mode = AimDpadMode::DpadAim;
        b.strafe_left = {PhysicalButton::Y};
        b.strafe_right = {PhysicalButton::B};
        b.roll = {PhysicalButton::A};
        b.fire = {PhysicalButton::R1, PhysicalButton::R2};
        b.jump = {PhysicalButton::L1, PhysicalButton::X};
        b.manual_aim = {PhysicalButton::L2};
        b.pause = {PhysicalButton::Start};
    } else if (scheme == CONTROL_SCHEME_DPAD_AIM) {
        b.aim_mode = AimDpadMode::DpadAim;
        b.strafe_left = {PhysicalButton::Y};
        b.strafe_right = {PhysicalButton::A};
        b.fire = {PhysicalButton::R1, PhysicalButton::R2};
        b.jump = {PhysicalButton::L1};
        b.roll = {PhysicalButton::B, PhysicalButton::L2};
        b.manual_aim = {PhysicalButton::X};
        b.pause = {PhysicalButton::Start};
    } else if (scheme == CONTROL_SCHEME_DPAD_STRAFE) {
        b.aim_mode = AimDpadMode::FaceAim;
        b.fire = {PhysicalButton::R1};
        b.jump = {PhysicalButton::L1};
        b.roll = {PhysicalButton::L2};
        b.manual_aim = {PhysicalButton::R2};
        b.pause = {PhysicalButton::Start};
    } else if (scheme == CONTROL_SCHEME_TWIN_STICK || scheme == CONTROL_SCHEME_TWIN_STICK_SWAPPED) {
        b.aim_mode = (scheme == CONTROL_SCHEME_TWIN_STICK_SWAPPED)
            ? AimDpadMode::TwinStickSwapped
            : AimDpadMode::TwinStick;
        b.fire = {PhysicalButton::R1, PhysicalButton::A};
        b.jump = {PhysicalButton::L1, PhysicalButton::B};
        b.roll = {PhysicalButton::L2, PhysicalButton::Y};
        b.manual_aim = {PhysicalButton::R2, PhysicalButton::X};
        b.pause = {PhysicalButton::Start};
    } else {
        b.aim_mode = AimDpadMode::StrafeShift;
        b.fire = {PhysicalButton::A};
        b.jump = {PhysicalButton::B};
        b.roll = {PhysicalButton::Y};
        b.manual_aim = {PhysicalButton::L1, PhysicalButton::R1};
        b.switch_target = {PhysicalButton::X};
        b.prev_target = {PhysicalButton::L2};
        b.next_target = {PhysicalButton::R2};
        b.pause = {PhysicalButton::Start};
    }
    return b;
}

const char* physical_button_to_string(PhysicalButton btn) {
    switch (btn) {
    case PhysicalButton::A: return "a";
    case PhysicalButton::B: return "b";
    case PhysicalButton::X: return "x";
    case PhysicalButton::Y: return "y";
    case PhysicalButton::L1: return "l1";
    case PhysicalButton::R1: return "r1";
    case PhysicalButton::L2: return "l2";
    case PhysicalButton::R2: return "r2";
    case PhysicalButton::Start: return "start";
    case PhysicalButton::None:
    default: return "none";
    }
}

PhysicalButton string_to_physical_button(const std::string& str) {
    std::string lower;
    lower.reserve(str.size());
    for (char ch : str) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    if (lower == "a") return PhysicalButton::A;
    if (lower == "b") return PhysicalButton::B;
    if (lower == "x") return PhysicalButton::X;
    if (lower == "y") return PhysicalButton::Y;
    if (lower == "l1" || lower == "l") return PhysicalButton::L1;
    if (lower == "r1" || lower == "r") return PhysicalButton::R1;
    if (lower == "l2" || lower == "zl") return PhysicalButton::L2;
    if (lower == "r2" || lower == "zr") return PhysicalButton::R2;
    if (lower == "start") return PhysicalButton::Start;
    return PhysicalButton::None;
}

const char* physical_button_display_name(PhysicalButton btn) {
    switch (btn) {
    case PhysicalButton::A: return "A";
    case PhysicalButton::B: return "B";
    case PhysicalButton::X: return "X";
    case PhysicalButton::Y: return "Y";
    case PhysicalButton::L1: return "L1";
    case PhysicalButton::R1: return "R1";
    case PhysicalButton::L2: return "L2";
    case PhysicalButton::R2: return "R2";
    case PhysicalButton::Start: return "Start";
    case PhysicalButton::None:
    default: return "None";
    }
}

const char* aim_dpad_mode_to_string(AimDpadMode mode) {
    switch (mode) {
    case AimDpadMode::StrafeShift: return "strafe_shift";
    case AimDpadMode::DpadAim: return "dpad_aim";
    case AimDpadMode::FaceAim: return "face_aim";
    case AimDpadMode::TwinStick: return "twin_stick";
    case AimDpadMode::TwinStickSwapped: return "twin_stick_swapped";
    default: return "strafe_shift";
    }
}

AimDpadMode string_to_aim_dpad_mode(const std::string& str) {
    std::string lower;
    lower.reserve(str.size());
    for (char ch : str) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    if (lower == "twin_stick_swapped" || lower == "twin_swapped") return AimDpadMode::TwinStickSwapped;
    if (lower == "dpad_aim" || lower == "dpad") return AimDpadMode::DpadAim;
    if (lower == "face_aim" || lower == "face") return AimDpadMode::FaceAim;
    if (lower == "twin_stick" || lower == "twin") return AimDpadMode::TwinStick;
    return AimDpadMode::StrafeShift;
}

const char* aim_dpad_mode_display_name(AimDpadMode mode) {
    switch (mode) {
    case AimDpadMode::StrafeShift: return "Strafe + Shift";
    case AimDpadMode::DpadAim: return "D-Pad Aim";
    case AimDpadMode::FaceAim: return "Face Aim";
    case AimDpadMode::TwinStick: return "Twin Stick";
    case AimDpadMode::TwinStickSwapped: return "Twin Stick: Swapped";
    default: return "Strafe + Shift";
    }
}

const char* resolution_scale_display_name(int scale) {
    switch (scale) {
    case 0: return "Native";
    case 1: return "1x (240p)";
    case 2: return "2x (480p)";
    case 3: return "3x (720p)";
    case 4: return "4x (960p)";
    case 5: return "5x (1200p)";
    default: return "Native";
    }
}

void apply_graphics_config() {
    snp::gles3::set_vsync_enabled(g_config.video.vsync);

    ultramodern::renderer::GraphicsConfig config{};
    config.developer_mode = false;
    config.ar_option = (g_config.video.aspect_ratio == ASPECT_RATIO_EXPAND)
        ? ultramodern::renderer::AspectRatio::Expand
        : ultramodern::renderer::AspectRatio::Original;

    int scale = std::clamp(g_config.video.resolution_scale, MIN_RESOLUTION_SCALE, MAX_RESOLUTION_SCALE);
    if (scale == RESOLUTION_SCALE_NATIVE) {
        config.res_option = ultramodern::renderer::Resolution::Auto;
        config.ds_option = 0;
    } else if (scale == 1) {
        config.res_option = ultramodern::renderer::Resolution::Original;
        config.ds_option = 1;
    } else {
        config.res_option = ultramodern::renderer::Resolution::Original2x;
        config.ds_option = scale;
    }

    config.rr_option = ultramodern::renderer::RefreshRate::Original;
    config.wm_option = ultramodern::renderer::WindowMode::Fullscreen;
    config.hr_option = ultramodern::renderer::HUDRatioMode::Full;
    config.api_option = ultramodern::renderer::GraphicsApi::Auto;
    config.msaa_option = ultramodern::renderer::Antialiasing::None;
    config.hpfb_option = ultramodern::renderer::HighPrecisionFramebuffer::Off;
    ultramodern::renderer::set_graphics_config(config);
}

void save_config(const std::string& config_path) {
    std::ofstream out(config_path);
    if (!out.is_open()) {
        std::cerr << "Failed to write configuration: " << config_path << "\n";
        return;
    }

    out << "# Sin and Punishment Recompiled - Handheld Configuration\n\n"
        << "[video]\n"
        << "# Aspect ratio: \"expand\" (widescreen full screen) or \"original\" (4:3 pillarboxed)\n"
        << "aspect_ratio = \"" << g_config.video.aspect_ratio << "\"\n"
        << "# Resolution scale: 0 (Native window), 1 (240p), 2 (480p), 3 (720p), 4 (960p), or 5 (1200p)\n"
        << "resolution_scale = " << g_config.video.resolution_scale << "\n"
        << "vsync = " << (g_config.video.vsync ? "true" : "false") << "\n"
        << "target_fps = " << g_config.video.target_fps << "\n"
        << "sharp_textures = " << (g_config.video.sharp_textures ? "true" : "false") << "\n"
        << "sharp_hud = " << (g_config.video.sharp_hud ? "true" : "false") << "\n\n"
        << "[audio]\n"
        << "master_volume = " << g_config.audio.master_volume << "\n"
        << "music_volume = " << g_config.audio.music_volume << "\n"
        << "sfx_volume = " << g_config.audio.sfx_volume << "\n\n"
        << "[controls]\n"
        << "# Scheme: \"auto\" (detect sticks), \"dpad_strafe_shift\" (Stickless default), \"bumper_strafe\", \"face_strafe\", \"dpad_aim\", \"dpad_strafe\", \"twin_stick\", \"twin_stick_swapped\", or \"custom\"\n"
        << "scheme = \"" << g_config.controls.scheme << "\"\n"
        << "# Smart Lock-On: \"auto\" (detect sticks), \"always\" (Hold=Manual default on stickless), \"hold\" (Hold=Lock), or \"off\" (N64 toggle default on stick devices)\n"
        << "smart_lock_on = \"" << g_config.controls.smart_lock_on << "\"\n"
        << "aim_curve = \"" << g_config.controls.aim_curve << "\"\n"
        << "# Face layout: \"nintendo\" (East A, South B, North X, West Y) or \"positional\" (swapped A<->B, X<->Y)\n"
        << "face_layout = \"" << g_config.controls.face_layout << "\"\n"
        << "stick_sensitivity = " << g_config.controls.stick_sensitivity << "\n"
        << "deadzone = " << g_config.controls.deadzone << "\n"
        << "analog_aim = " << (g_config.controls.analog_aim ? "true" : "false") << "\n"
        << "rumble = " << (g_config.controls.rumble ? "true" : "false") << "\n"
        << "rumble_strength = " << g_config.controls.rumble_strength << "\n"
        << "swap_sticks = " << (g_config.controls.swap_sticks ? "true" : "false") << "\n\n"
        << "[controls.bindings]\n"
        << "# Aim / D-Pad Mode: \"strafe_shift\", \"dpad_aim\", \"face_aim\", \"twin_stick\", or \"twin_stick_swapped\"\n"
        << "aim_mode = \"" << aim_dpad_mode_to_string(g_config.controls.bindings.aim_mode) << "\"\n"
        << "fire = [\"" << physical_button_to_string(g_config.controls.bindings.fire.primary) << "\", \"" << physical_button_to_string(g_config.controls.bindings.fire.secondary) << "\"]\n"
        << "jump = [\"" << physical_button_to_string(g_config.controls.bindings.jump.primary) << "\", \"" << physical_button_to_string(g_config.controls.bindings.jump.secondary) << "\"]\n"
        << "roll = [\"" << physical_button_to_string(g_config.controls.bindings.roll.primary) << "\", \"" << physical_button_to_string(g_config.controls.bindings.roll.secondary) << "\"]\n"
        << "roll_left = [\"" << physical_button_to_string(g_config.controls.bindings.roll_left.primary) << "\", \"" << physical_button_to_string(g_config.controls.bindings.roll_left.secondary) << "\"]\n"
        << "roll_right = [\"" << physical_button_to_string(g_config.controls.bindings.roll_right.primary) << "\", \"" << physical_button_to_string(g_config.controls.bindings.roll_right.secondary) << "\"]\n"
        << "manual_aim = [\"" << physical_button_to_string(g_config.controls.bindings.manual_aim.primary) << "\", \"" << physical_button_to_string(g_config.controls.bindings.manual_aim.secondary) << "\"]\n"
        << "switch_target = [\"" << physical_button_to_string(g_config.controls.bindings.switch_target.primary) << "\", \"" << physical_button_to_string(g_config.controls.bindings.switch_target.secondary) << "\"]\n"
        << "prev_target = [\"" << physical_button_to_string(g_config.controls.bindings.prev_target.primary) << "\", \"" << physical_button_to_string(g_config.controls.bindings.prev_target.secondary) << "\"]\n"
        << "next_target = [\"" << physical_button_to_string(g_config.controls.bindings.next_target.primary) << "\", \"" << physical_button_to_string(g_config.controls.bindings.next_target.secondary) << "\"]\n"
        << "strafe_left = [\"" << physical_button_to_string(g_config.controls.bindings.strafe_left.primary) << "\", \"" << physical_button_to_string(g_config.controls.bindings.strafe_left.secondary) << "\"]\n"
        << "strafe_right = [\"" << physical_button_to_string(g_config.controls.bindings.strafe_right.primary) << "\", \"" << physical_button_to_string(g_config.controls.bindings.strafe_right.secondary) << "\"]\n"
        << "pause = [\"" << physical_button_to_string(g_config.controls.bindings.pause.primary) << "\", \"" << physical_button_to_string(g_config.controls.bindings.pause.secondary) << "\"]\n\n"
        << "[cheats]\n"
        << "unlock_options = " << (g_config.cheats.unlock_options ? "true" : "false") << "\n"
        << "infinite_health = " << (g_config.cheats.infinite_health ? "true" : "false") << "\n"
        << "infinite_time = " << (g_config.cheats.infinite_time ? "true" : "false") << "\n"
        << "infinite_continues = " << (g_config.cheats.infinite_continues ? "true" : "false") << "\n"
        << "max_score = " << (g_config.cheats.max_score ? "true" : "false") << "\n"
        << "invincible = " << (g_config.cheats.invincible ? "true" : "false") << "\n"
        << "infinite_air_jumps = " << (g_config.cheats.infinite_air_jumps ? "true" : "false") << "\n\n"
        << "[rom]\n"
        << "# Preferred ROM region: \"auto\" (prefers English if present, else Japanese), \"en\", or \"jp\"\n"
        << "preferred_region = \"" << g_config.rom.preferred_region << "\"\n\n"
        << "[retroachievements]\n"
        << "enabled = " << (g_config.retroachievements.enabled ? "true" : "false") << "\n"
        << "import_system_credentials = " << (g_config.retroachievements.import_system_credentials ? "true" : "false") << "\n"
        << "notifications = " << (g_config.retroachievements.notifications ? "true" : "false") << "\n"
        << "username = \"" << g_config.retroachievements.username << "\"\n"
        << "token = \"" << g_config.retroachievements.token << "\"\n";
    if (!g_config.retroachievements.password.empty()) {
        out << "password = \"" << g_config.retroachievements.password << "\"\n";
    }
}

bool load_config(const std::string& config_path) {
    g_config.controls.scheme = resolve_default_control_scheme();
    g_config.controls.smart_lock_on = resolve_default_smart_lock_on(g_config.controls.scheme);

    if (!std::filesystem::exists(config_path)) {
        save_config(config_path);
        return true;
    }

    try {
        toml::table tbl = toml::parse_file(config_path);

        if (tbl["video"].is_table()) {
            auto video = tbl["video"];
            g_config.video.aspect_ratio = video["aspect_ratio"].value_or(ASPECT_RATIO_EXPAND);
            g_config.video.resolution_scale = std::clamp(
                static_cast<int>(video["resolution_scale"].value_or(int64_t(DEFAULT_RESOLUTION_SCALE))),
                MIN_RESOLUTION_SCALE,
                MAX_RESOLUTION_SCALE);
            g_config.video.vsync = video["vsync"].value_or(true);
            g_config.video.target_fps = static_cast<int>(video["target_fps"].value_or(int64_t(DEFAULT_TARGET_FPS)));
            g_config.video.sharp_textures = video["sharp_textures"].value_or(false);
            g_config.video.sharp_hud = video["sharp_hud"].value_or(true);
        }

        if (tbl["audio"].is_table()) {
            auto audio = tbl["audio"];
            g_config.audio.master_volume = static_cast<int>(audio["master_volume"].value_or(int64_t(DEFAULT_VOLUME)));
            g_config.audio.music_volume = static_cast<int>(audio["music_volume"].value_or(int64_t(DEFAULT_VOLUME)));
            g_config.audio.sfx_volume = static_cast<int>(audio["sfx_volume"].value_or(int64_t(DEFAULT_VOLUME)));
        }

        if (tbl["controls"].is_table()) {
            auto controls = tbl["controls"];
            g_config.controls.scheme = controls["scheme"].value_or(CONTROL_SCHEME_AUTO);
            if (g_config.controls.scheme == CONTROL_SCHEME_AUTO) {
                g_config.controls.scheme = resolve_default_control_scheme();
            }
            g_config.controls.smart_lock_on = controls["smart_lock_on"].value_or(SMART_LOCK_AUTO);
            g_config.controls.aim_curve = controls["aim_curve"].value_or(AIM_CURVE_S_CURVE);
            g_config.controls.face_layout = controls["face_layout"].value_or(FACE_LAYOUT_NINTENDO);
            g_config.controls.stick_sensitivity = static_cast<float>(controls["stick_sensitivity"].value_or(double(DEFAULT_STICK_SENSITIVITY)));
            g_config.controls.deadzone = static_cast<float>(controls["deadzone"].value_or(double(DEFAULT_DEADZONE)));
            g_config.controls.analog_aim = controls["analog_aim"].value_or(true);
            g_config.controls.rumble = controls["rumble"].value_or(true);
            g_config.controls.rumble_strength = std::clamp(
                static_cast<int>(controls["rumble_strength"].value_or(int64_t(DEFAULT_RUMBLE_STRENGTH))),
                0,
                100);
            if (g_config.controls.rumble_strength == 0) {
                g_config.controls.rumble = false;
            }
            g_config.controls.swap_sticks = controls["swap_sticks"].value_or(false);

            if (g_config.controls.scheme != CONTROL_SCHEME_DPAD_STRAFE_SHIFT &&
                g_config.controls.scheme != CONTROL_SCHEME_BUMPER_STRAFE &&
                g_config.controls.scheme != CONTROL_SCHEME_FACE_STRAFE &&
                g_config.controls.scheme != CONTROL_SCHEME_DPAD_AIM &&
                g_config.controls.scheme != CONTROL_SCHEME_DPAD_STRAFE &&
                g_config.controls.scheme != CONTROL_SCHEME_TWIN_STICK &&
                g_config.controls.scheme != CONTROL_SCHEME_TWIN_STICK_SWAPPED &&
                g_config.controls.scheme != CONTROL_SCHEME_CUSTOM) {
                g_config.controls.scheme = resolve_default_control_scheme();
            }

            if (g_config.controls.scheme != CONTROL_SCHEME_CUSTOM) {
                g_config.controls.bindings = get_preset_bindings(g_config.controls.scheme);
            } else {
                g_config.controls.bindings = get_preset_bindings(CONTROL_SCHEME_DPAD_STRAFE_SHIFT);
                auto b_node = controls["bindings"];
                if (!b_node.is_table()) {
                    b_node = tbl["controls"]["bindings"];
                }
                if (!b_node.is_table()) {
                    b_node = tbl["controls.bindings"];
                }
                if (b_node.is_table()) {
                    if (auto aim_str = b_node["aim_mode"].value<std::string>()) {
                        g_config.controls.bindings.aim_mode = string_to_aim_dpad_mode(*aim_str);
                    }
                    auto parse_binding = [](auto&& node, const ActionBinding& def) -> ActionBinding {
                        ActionBinding b = def;
                        if (auto arr = node.as_array()) {
                            if (arr->size() > 0) {
                                if (auto val = (*arr)[0].template value<std::string>()) {
                                    b.primary = string_to_physical_button(*val);
                                }
                            }
                            if (arr->size() > 1) {
                                if (auto val = (*arr)[1].template value<std::string>()) {
                                    b.secondary = string_to_physical_button(*val);
                                }
                            }
                        } else if (auto val = node.template value<std::string>()) {
                            b.primary = string_to_physical_button(*val);
                            b.secondary = PhysicalButton::None;
                        }
                        return b;
                    };

                    g_config.controls.bindings.fire = parse_binding(b_node["fire"], g_config.controls.bindings.fire);
                    g_config.controls.bindings.jump = parse_binding(b_node["jump"], g_config.controls.bindings.jump);
                    g_config.controls.bindings.roll = parse_binding(b_node["roll"], g_config.controls.bindings.roll);
                    g_config.controls.bindings.roll_left = parse_binding(b_node["roll_left"], g_config.controls.bindings.roll_left);
                    g_config.controls.bindings.roll_right = parse_binding(b_node["roll_right"], g_config.controls.bindings.roll_right);
                    g_config.controls.bindings.manual_aim = parse_binding(b_node["manual_aim"], g_config.controls.bindings.manual_aim);
                    g_config.controls.bindings.switch_target = parse_binding(b_node["switch_target"], g_config.controls.bindings.switch_target);
                    g_config.controls.bindings.prev_target = parse_binding(b_node["prev_target"], g_config.controls.bindings.prev_target);
                    g_config.controls.bindings.next_target = parse_binding(b_node["next_target"], g_config.controls.bindings.next_target);
                    g_config.controls.bindings.strafe_left = parse_binding(b_node["strafe_left"], g_config.controls.bindings.strafe_left);
                    g_config.controls.bindings.strafe_right = parse_binding(b_node["strafe_right"], g_config.controls.bindings.strafe_right);
                    g_config.controls.bindings.pause = parse_binding(b_node["pause"], g_config.controls.bindings.pause);
                }
            }

            if (g_config.controls.smart_lock_on == SMART_LOCK_AUTO) {
                g_config.controls.smart_lock_on = resolve_default_smart_lock_on(g_config.controls.scheme);
            } else if (g_config.controls.smart_lock_on != SMART_LOCK_ALWAYS &&
                       g_config.controls.smart_lock_on != SMART_LOCK_HOLD &&
                       g_config.controls.smart_lock_on != SMART_LOCK_OFF) {
                g_config.controls.smart_lock_on = resolve_default_smart_lock_on(g_config.controls.scheme);
            }

            if (g_config.controls.aim_curve != AIM_CURVE_S_CURVE &&
                g_config.controls.aim_curve != AIM_CURVE_LINEAR) {
                g_config.controls.aim_curve = AIM_CURVE_S_CURVE;
            }

            g_config.controls.stick_sensitivity = std::clamp(g_config.controls.stick_sensitivity, 50.0f, 200.0f);
            snp::config::set_stick_sensitivity(g_config.controls.stick_sensitivity);
        }

        if (tbl["cheats"].is_table()) {
            auto cheats = tbl["cheats"];
            g_config.cheats.unlock_options = cheats["unlock_options"].value_or(false);
            g_config.cheats.infinite_health = cheats["infinite_health"].value_or(false);
            g_config.cheats.infinite_time = cheats["infinite_time"].value_or(false);
            g_config.cheats.infinite_continues = cheats["infinite_continues"].value_or(false);
            g_config.cheats.max_score = cheats["max_score"].value_or(false);
            g_config.cheats.invincible = cheats["invincible"].value_or(false);
            g_config.cheats.infinite_air_jumps = cheats["infinite_air_jumps"].value_or(false);
        }

        if (tbl["rom"].is_table()) {
            g_config.rom.preferred_region = tbl["rom"]["preferred_region"].value_or(ROM_PREFERRED_AUTO);
        }

        if (tbl["retroachievements"].is_table()) {
            auto ra = tbl["retroachievements"];
            g_config.retroachievements.enabled = ra["enabled"].value_or(true);
            g_config.retroachievements.import_system_credentials = ra["import_system_credentials"].value_or(true);
            g_config.retroachievements.notifications = ra["notifications"].value_or(true);
            g_config.retroachievements.username = ra["username"].value_or(std::string(""));
            g_config.retroachievements.token = ra["token"].value_or(std::string(""));
            g_config.retroachievements.password = ra["password"].value_or(std::string(""));
        }
    } catch (const std::exception& e) {
        std::cerr << "Warning: Error parsing " << config_path << ": " << e.what() << ", using defaults\n";
        return false;
    }

    return true;
}

} // namespace snp::handheld

namespace {
constexpr uint32_t MUSIC_HANDLE_ADDRESS = 0x80074540;
static uint32_t s_last_music_handle = 0;

inline uint32_t read_rdram_u32(uint8_t* rdram, uint32_t vram) {
    return *reinterpret_cast<uint32_t*>(rdram + (vram - 0x80000000u));
}
} // namespace

extern "C" uint32_t snp_track_volume(uint8_t* rdram, uint32_t track, uint32_t volume) {
    uint32_t music_handle = read_rdram_u32(rdram, MUSIC_HANDLE_ADDRESS);
    if (music_handle != 0) {
        s_last_music_handle = music_handle;
    }
    uint32_t handle = read_rdram_u32(rdram, track + 0x44);
    bool is_music = (handle != 0 && handle == s_last_music_handle);

    const auto& config = snp::handheld::get_config();
    float channel_percent = is_music ? static_cast<float>(config.audio.music_volume) : static_cast<float>(config.audio.sfx_volume);
    float master_factor = static_cast<float>(config.audio.master_volume) / 100.0f;
    float factor = (channel_percent / 100.0f) * master_factor;

    if (factor <= 0.0f) {
        return 0;
    }
    if (factor >= 1.0f) {
        return volume;
    }
    return static_cast<uint32_t>(volume * factor);
}

namespace snp::config {

void add_general_options(recomp::config::Config&) {}
bool get_rumble_pak() {
    return snp::handheld::get_config().controls.rumble;
}
SecondPlayer get_second_player() {
    return SecondPlayer::Off;
}
MouseAim get_mouse_aim() {
    return MouseAim::Off;
}
float get_mouse_sensitivity() {
    return 100.0f;
}
float get_stick_sensitivity() {
    return snp::handheld::get_config().controls.stick_sensitivity;
}
void set_stick_sensitivity(float sensitivity) {
    snp::handheld::get_mutable_config().controls.stick_sensitivity = std::clamp(sensitivity, 50.0f, 200.0f);
}
void add_graphics_options(recomp::config::Config&) {}
uint32_t get_screen_filter() {
    return 0;
}
bool get_screen_filter_menus_only() {
    return false;
}
uint32_t get_bloom() {
    return 0;
}
bool get_sharp_textures() {
    return snp::handheld::get_config().video.sharp_textures;
}
bool get_sharp_hud() {
    return snp::handheld::get_config().video.sharp_hud;
}
void apply_texture_packs() {}
void lock_game_start_options() {}
void add_extras_tabs() {}
void add_sound_volume_options(recomp::config::Config&) {}
void add_spanish_voices_option(recomp::config::Config&) {}
void lock_spanish_voices_option() {}

bool get_option(Option option) {
    const auto& cheats = snp::handheld::get_config().cheats;
    switch (option) {
    case Option::UnlockOptions:
        return cheats.unlock_options;
    case Option::AnalogAim:
        return snp::handheld::get_config().controls.analog_aim;
    case Option::SmoothAiming:
        return false;
    case Option::InfiniteHealth:
        return cheats.infinite_health;
    case Option::InfiniteTime:
        return cheats.infinite_time;
    case Option::InfiniteContinues:
        return cheats.infinite_continues;
    case Option::MaxScore:
        return cheats.max_score;
    case Option::Invincible:
        return cheats.invincible;
    case Option::InfiniteAirJumps:
        return cheats.infinite_air_jumps;
    default:
        return false;
    }
}

} // namespace snp::config
