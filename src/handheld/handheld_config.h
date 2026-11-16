#pragma once

#include <cstdint>
#include <string>

#include "snp_config.h"

namespace snp::handheld {

constexpr const char* DEFAULT_CONFIG_FILE = "snprecomp.toml";
constexpr const char* ASPECT_RATIO_EXPAND = "expand";
constexpr const char* ASPECT_RATIO_ORIGINAL = "original";
constexpr const char* CONTROL_SCHEME_AUTO = "auto";
constexpr const char* CONTROL_SCHEME_BUMPER_STRAFE = "bumper_strafe";
constexpr const char* CONTROL_SCHEME_FACE_STRAFE = "face_strafe";
constexpr const char* CONTROL_SCHEME_DPAD_STRAFE_SHIFT = "dpad_strafe_shift";
constexpr const char* CONTROL_SCHEME_DPAD_AIM = "dpad_aim";
constexpr const char* CONTROL_SCHEME_DPAD_STRAFE = "dpad_strafe";
constexpr const char* CONTROL_SCHEME_TWIN_STICK = "twin_stick";
constexpr const char* CONTROL_SCHEME_CUSTOM = "custom";

enum class PhysicalButton : uint8_t {
    None = 0,
    A,
    B,
    X,
    Y,
    L1,
    R1,
    L2,
    R2,
    Start,
};

enum class AimDpadMode : uint8_t {
    StrafeShift = 0,
    DpadAim,
    FaceAim,
    TwinStick,
};

struct ActionBinding {
    PhysicalButton primary = PhysicalButton::None;
    PhysicalButton secondary = PhysicalButton::None;

    constexpr ActionBinding() = default;
    constexpr ActionBinding(PhysicalButton p) : primary(p), secondary(PhysicalButton::None) {}
    constexpr ActionBinding(PhysicalButton p, PhysicalButton s) : primary(p), secondary(s) {}

    bool matches(PhysicalButton btn) const {
        if (btn == PhysicalButton::None) return false;
        return (primary == btn || secondary == btn);
    }
};

struct ControlBindings {
    AimDpadMode aim_mode = AimDpadMode::StrafeShift;
    ActionBinding fire;
    ActionBinding jump;
    ActionBinding roll;
    ActionBinding roll_left;
    ActionBinding roll_right;
    ActionBinding manual_aim;
    ActionBinding switch_target;
    ActionBinding prev_target;
    ActionBinding next_target;
    ActionBinding strafe_left;
    ActionBinding strafe_right;
    ActionBinding pause{PhysicalButton::Start};
};

constexpr const char* SMART_LOCK_HOLD = "hold";
constexpr const char* SMART_LOCK_AUTO = "auto";
constexpr const char* SMART_LOCK_OFF = "off";

constexpr const char* AIM_CURVE_S_CURVE = "s_curve";
constexpr const char* AIM_CURVE_LINEAR = "linear";

constexpr const char* FACE_LAYOUT_NINTENDO = "nintendo";
constexpr const char* FACE_LAYOUT_POSITIONAL = "positional";
constexpr const char* ROM_PREFERRED_AUTO = "auto";
constexpr const char* ROM_PREFERRED_EN = "en";
constexpr const char* ROM_PREFERRED_JP = "jp";

constexpr int DEFAULT_RESOLUTION_SCALE = 2;
constexpr int DEFAULT_TARGET_FPS = 60;
constexpr int DEFAULT_VOLUME = 100;
constexpr int DEFAULT_RUMBLE_STRENGTH = 100;
constexpr float DEFAULT_STICK_SENSITIVITY = 100.0f;
constexpr float DEFAULT_DEADZONE = 0.15f;

struct VideoSettings {
    std::string aspect_ratio = ASPECT_RATIO_EXPAND;
    int resolution_scale = DEFAULT_RESOLUTION_SCALE;
    bool vsync = true;
    int target_fps = DEFAULT_TARGET_FPS;
    bool sharp_textures = false;
    bool sharp_hud = true;
};

struct AudioSettings {
    int master_volume = DEFAULT_VOLUME;
    int music_volume = DEFAULT_VOLUME;
    int sfx_volume = DEFAULT_VOLUME;
};

struct ControlsSettings {
    std::string scheme = CONTROL_SCHEME_DPAD_STRAFE_SHIFT;
    std::string smart_lock_on = SMART_LOCK_AUTO;
    std::string aim_curve = AIM_CURVE_S_CURVE;
    std::string face_layout = FACE_LAYOUT_NINTENDO;
    float stick_sensitivity = DEFAULT_STICK_SENSITIVITY;
    float deadzone = DEFAULT_DEADZONE;
    bool analog_aim = true;
    bool rumble = true;
    int rumble_strength = DEFAULT_RUMBLE_STRENGTH;
    bool swap_sticks = false;
    ControlBindings bindings;
};

struct CheatsSettings {
    bool unlock_options = false;
    bool infinite_health = false;
    bool infinite_time = false;
    bool infinite_continues = false;
    bool max_score = false;
    bool invincible = false;
    bool infinite_air_jumps = false;
};

struct RomSettings {
    std::string preferred_region = ROM_PREFERRED_AUTO;
};

struct RetroAchievementsConfig {
    bool enabled = true;
    bool import_system_credentials = true;
    bool notifications = true;
    std::string username;
    std::string token;
    std::string password;
};

struct HandheldConfig {
    VideoSettings video;
    AudioSettings audio;
    ControlsSettings controls;
    CheatsSettings cheats;
    RomSettings rom;
    RetroAchievementsConfig retroachievements;
};

ControlBindings get_preset_bindings(const std::string& scheme);
const char* physical_button_to_string(PhysicalButton btn);
PhysicalButton string_to_physical_button(const std::string& str);
const char* physical_button_display_name(PhysicalButton btn);
const char* aim_dpad_mode_to_string(AimDpadMode mode);
AimDpadMode string_to_aim_dpad_mode(const std::string& str);
const char* aim_dpad_mode_display_name(AimDpadMode mode);

bool load_config(const std::string& config_path = DEFAULT_CONFIG_FILE);
void save_config(const std::string& config_path = DEFAULT_CONFIG_FILE);

const HandheldConfig& get_config();
HandheldConfig& get_mutable_config();

void apply_graphics_config();

} // namespace snp::handheld
