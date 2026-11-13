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

void apply_graphics_config() {
    snp::gles3::set_vsync_enabled(g_config.video.vsync);

    ultramodern::renderer::GraphicsConfig config{};
    config.developer_mode = false;
    config.ar_option = (g_config.video.aspect_ratio == ASPECT_RATIO_EXPAND)
        ? ultramodern::renderer::AspectRatio::Expand
        : ultramodern::renderer::AspectRatio::Original;
    config.res_option = (g_config.video.resolution_scale >= 2)
        ? ultramodern::renderer::Resolution::Original2x
        : ultramodern::renderer::Resolution::Original;
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

    out << "# Sin and Punishment Recompiled - Handheld Configuration\n"
        << "# Tuned for Anbernic RG34XX SP / RGSP (720x480 3:2, Allwinner H700)\n\n"
        << "[video]\n"
        << "# Aspect ratio: \"expand\" (3:2 720x480 full screen) or \"original\" (4:3 pillarboxed)\n"
        << "aspect_ratio = \"" << g_config.video.aspect_ratio << "\"\n"
        << "# Resolution scale: 2 (native 480p match for H700) or 1 (original 240p)\n"
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
        << "# Scheme: \"dpad_aim\" (RGSP stickless default), \"dpad_strafe\", or \"twin_stick\"\n"
        << "scheme = \"" << g_config.controls.scheme << "\"\n"
        << "# Face layout: \"nintendo\" (East A, South B, North X, West Y) or \"positional\" (swapped A<->B, X<->Y)\n"
        << "face_layout = \"" << g_config.controls.face_layout << "\"\n"
        << "stick_sensitivity = " << g_config.controls.stick_sensitivity << "\n"
        << "deadzone = " << g_config.controls.deadzone << "\n"
        << "analog_aim = " << (g_config.controls.analog_aim ? "true" : "false") << "\n"
        << "rumble = " << (g_config.controls.rumble ? "true" : "false") << "\n"
        << "swap_sticks = " << (g_config.controls.swap_sticks ? "true" : "false") << "\n\n"
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
        << "preferred_region = \"" << g_config.rom.preferred_region << "\"\n";
}

bool load_config(const std::string& config_path) {
    if (!std::filesystem::exists(config_path)) {
        save_config(config_path);
        return true;
    }

    try {
        toml::table tbl = toml::parse_file(config_path);

        if (auto video = tbl["video"].as_table()) {
            g_config.video.aspect_ratio = video->get("aspect_ratio")->value_or(ASPECT_RATIO_EXPAND);
            g_config.video.resolution_scale = static_cast<int>(video->get("resolution_scale")->value_or(int64_t(DEFAULT_RESOLUTION_SCALE)));
            g_config.video.vsync = video->get("vsync")->value_or(true);
            g_config.video.target_fps = static_cast<int>(video->get("target_fps")->value_or(int64_t(DEFAULT_TARGET_FPS)));
            g_config.video.sharp_textures = video->get("sharp_textures")->value_or(false);
            g_config.video.sharp_hud = video->get("sharp_hud")->value_or(true);
        }

        if (auto audio = tbl["audio"].as_table()) {
            g_config.audio.master_volume = static_cast<int>(audio->get("master_volume")->value_or(int64_t(DEFAULT_VOLUME)));
            g_config.audio.music_volume = static_cast<int>(audio->get("music_volume")->value_or(int64_t(DEFAULT_VOLUME)));
            g_config.audio.sfx_volume = static_cast<int>(audio->get("sfx_volume")->value_or(int64_t(DEFAULT_VOLUME)));
        }

        if (auto controls = tbl["controls"].as_table()) {
            g_config.controls.scheme = controls->get("scheme")->value_or(CONTROL_SCHEME_DPAD_AIM);
            g_config.controls.face_layout = controls->get("face_layout")->value_or(FACE_LAYOUT_NINTENDO);
            g_config.controls.stick_sensitivity = static_cast<float>(controls->get("stick_sensitivity")->value_or(double(DEFAULT_STICK_SENSITIVITY)));
            g_config.controls.deadzone = static_cast<float>(controls->get("deadzone")->value_or(double(DEFAULT_DEADZONE)));
            g_config.controls.analog_aim = controls->get("analog_aim")->value_or(true);
            g_config.controls.rumble = controls->get("rumble")->value_or(true);
            g_config.controls.swap_sticks = controls->get("swap_sticks")->value_or(false);
        }

        if (auto cheats = tbl["cheats"].as_table()) {
            g_config.cheats.unlock_options = cheats->get("unlock_options")->value_or(false);
            g_config.cheats.infinite_health = cheats->get("infinite_health")->value_or(false);
            g_config.cheats.infinite_time = cheats->get("infinite_time")->value_or(false);
            g_config.cheats.infinite_continues = cheats->get("infinite_continues")->value_or(false);
            g_config.cheats.max_score = cheats->get("max_score")->value_or(false);
            g_config.cheats.invincible = cheats->get("invincible")->value_or(false);
            g_config.cheats.infinite_air_jumps = cheats->get("infinite_air_jumps")->value_or(false);
        }

        if (auto rom = tbl["rom"].as_table()) {
            g_config.rom.preferred_region = rom->get("preferred_region")->value_or(ROM_PREFERRED_AUTO);
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
