#include "handheld_input.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

#include <SDL2/SDL.h>

#include "handheld_config.h"
#include "handheld_overlay.h"
#include "snp_mod_api.h"
#include "snp_mouse.h"
#include "ultramodern/ultramodern.hpp"

bool snp_in_front_end_menus();
uint32_t snp_overlay_area_rom();

namespace snp::handheld {

namespace {

constexpr uint16_t N64_A = 0x8000;
constexpr uint16_t N64_B = 0x4000;
constexpr uint16_t N64_Z = 0x2000;
constexpr uint16_t N64_START = 0x1000;
constexpr uint16_t N64_DPAD_UP = 0x0800;
constexpr uint16_t N64_DPAD_DOWN = 0x0400;
constexpr uint16_t N64_DPAD_LEFT = 0x0200;
constexpr uint16_t N64_DPAD_RIGHT = 0x0100;
constexpr uint16_t N64_L = 0x0020;
constexpr uint16_t N64_R = 0x0010;
constexpr uint16_t N64_C_UP = 0x0008;
constexpr uint16_t N64_C_DOWN = 0x0004;
constexpr uint16_t N64_C_LEFT = 0x0002;
constexpr uint16_t N64_C_RIGHT = 0x0001;

constexpr int16_t AXIS_TRIGGER_THRESHOLD = 8000;
constexpr float MAX_AXIS_VALUE = 32767.0f;
constexpr int ROLL_STEP_FRAMES = 2;
constexpr int ROLL_TOTAL_STEPS = 4;
constexpr uint16_t RUMBLE_LOW_FREQ = 0x8000;
constexpr uint16_t RUMBLE_HIGH_FREQ = 0x8000;
constexpr uint32_t RUMBLE_DURATION_MS = 250;
constexpr uint32_t RETICLE_ACTIVE_TIMEOUT_MS = 150;
constexpr uint32_t LOAD_TABLE_ADDR = 0x800597ACu;
constexpr uint32_t LOAD_TABLE_STRIDE = 20u;
constexpr uint32_t FIRST_PLAYABLE_STAGE_ENTRY = 4u;
constexpr uint32_t LAST_PLAYABLE_STAGE_ENTRY = 13u;
constexpr uint32_t PAUSE_STATE_ADDR = 0x80068A97u;
constexpr uint8_t PAUSE_STATE_RUNNING = 1u;
constexpr uint8_t RETICLE_MENU_CURSOR_FLAG = 0x20u;

SDL_GameController* s_controller = nullptr;
SDL_Joystick* s_joystick = nullptr;

std::thread::id s_main_thread_id;
std::mutex s_input_mutex;

std::atomic<uint32_t> s_last_gameplay_reticle_ms{0};
std::atomic<uint32_t> s_last_menu_cursor_ms{0};
std::atomic<const uint8_t*> s_rdram_ptr{nullptr};

struct RollStateMachine {
    bool roll_button_was_pressed = false;
    uint16_t move_button = 0;
    int current_frame = 0;
    bool last_move_was_left = true;
};

RollStateMachine s_roll_state;

struct DigitalAimRamp {
    int hold_frames = 0;
    int last_dx = 0;
    int last_dy = 0;

    void reset() {
        hold_frames = 0;
        last_dx = 0;
        last_dy = 0;
    }

    void update(int dx, int dy, float& out_x, float& out_y) {
        if (dx == 0 && dy == 0) {
            reset();
            out_x = 0.0f;
            out_y = 0.0f;
            return;
        }

        if (dx != last_dx || dy != last_dy) {
            hold_frames = 1;
            last_dx = dx;
            last_dy = dy;
        } else {
            hold_frames++;
        }

        const auto& aim_curve = get_config().controls.aim_curve;
        float magnitude = 1.0f;

        if (aim_curve == AIM_CURVE_LINEAR) {
            constexpr float INITIAL_MAGNITUDE = 0.35f;
            constexpr float MAX_MAGNITUDE = 1.0f;
            constexpr int RAMP_STEPS = 9;
            float t = std::clamp(static_cast<float>(hold_frames - 1) / static_cast<float>(RAMP_STEPS), 0.0f, 1.0f);
            magnitude = INITIAL_MAGNITUDE + (MAX_MAGNITUDE - INITIAL_MAGNITUDE) * t;
        } else {
            if (hold_frames == 1) {
                magnitude = 0.18f;
            } else if (hold_frames == 2) {
                magnitude = 0.22f;
            } else if (hold_frames == 3) {
                magnitude = 0.28f;
            } else {
                float t = std::clamp(static_cast<float>(hold_frames - 3) / 9.0f, 0.0f, 1.0f);
                float hermite = t * t * (3.0f - 2.0f * t);
                magnitude = 0.28f + (1.0f - 0.28f) * hermite;
            }
        }

        constexpr float DIAG_FACTOR = 0.70710678f;
        float dir_x = static_cast<float>(dx);
        float dir_y = static_cast<float>(dy);
        if (dx != 0 && dy != 0) {
            dir_x *= DIAG_FACTOR;
            dir_y *= DIAG_FACTOR;
        }

        out_x = std::clamp(dir_x * magnitude, -1.0f, 1.0f);
        out_y = std::clamp(dir_y * magnitude, -1.0f, 1.0f);
    }
};

DigitalAimRamp s_aim_ramp;

std::atomic<bool> s_actual_lock_on{false};
std::atomic<uint32_t> s_reticle_tick{0};
std::atomic<int> s_flick_dx{0};
std::atomic<int> s_flick_dy{0};
std::atomic<bool> s_explicit_target_switch{false};
std::atomic<bool> s_recenter_requested{false};
bool s_pulse_active = false;
uint32_t s_pulse_start_tick = 0;

inline uint8_t read_rdram_byte(const uint8_t* rdram, uint32_t addr) {
    return rdram[(addr ^ 3u) & 0x1FFFFFFFu];
}

inline float read_rdram_float(const uint8_t* rdram, uint32_t addr) {
    float val = 0.0f;
    std::memcpy(&val, rdram + (addr & 0x1FFFFFFFu), sizeof(float));
    return val;
}

inline void write_rdram_float(uint8_t* rdram, uint32_t addr, float val) {
    std::memcpy(rdram + (addr & 0x1FFFFFFFu), &val, sizeof(float));
}

struct PhysicalPadState {
    float left_x = 0.0f;
    float left_y = 0.0f;
    float right_x = 0.0f;
    float right_y = 0.0f;

    bool dpad_up = false;
    bool dpad_down = false;
    bool dpad_left = false;
    bool dpad_right = false;

    bool l1 = false;
    bool r1 = false;
    bool l2 = false;
    bool r2 = false;

    bool select = false;
    bool start = false;
    bool menu_guide = false;

    bool east_a = false;
    bool south_b = false;
    bool north_x = false;
    bool west_y = false;
};

PhysicalPadState s_cached_pad_state{};

// Edge detection states for overlay triggers and navigation
bool s_select_held = false;
bool s_select_combo_used = false;
bool s_prev_select = false;
bool s_guide_held = false;
bool s_guide_combo_used = false;
bool s_prev_guide = false;

bool s_prev_nav_up = false;
bool s_prev_nav_down = false;
bool s_prev_nav_left = false;
bool s_prev_nav_right = false;
bool s_prev_nav_a = false;
bool s_prev_nav_b = false;
bool s_prev_nav_start = false;

void log_detected_joysticks() {
    int num = SDL_NumJoysticks();
    std::fprintf(stderr, "[INPUT] SDL reports %d connected joystick(s):\n", num);
    for (int i = 0; i < num; ++i) {
        char guid_str[64]{};
        SDL_JoystickGUID guid = SDL_JoystickGetDeviceGUID(i);
        SDL_JoystickGetGUIDString(guid, guid_str, sizeof(guid_str));
        const char* name = SDL_JoystickNameForIndex(i);
        bool is_gc = SDL_IsGameController(i);

        SDL_Joystick* probe = SDL_JoystickOpen(i);
        int axes = probe ? SDL_JoystickNumAxes(probe) : 0;
        int buttons = probe ? SDL_JoystickNumButtons(probe) : 0;
        int hats = probe ? SDL_JoystickNumHats(probe) : 0;
        if (probe && (!s_joystick || probe != s_joystick)) {
            SDL_JoystickClose(probe);
        }

        std::fprintf(stderr, "  [%d] \"%s\" | GUID: %s | GameController: %s | %d axes, %d buttons, %d hats\n",
                     i, name ? name : "Unknown", guid_str, is_gc ? "yes" : "no", axes, buttons, hats);
    }
}

void open_primary_controller() {
    if (s_controller != nullptr || s_joystick != nullptr) {
        return;
    }

    int num_joysticks = SDL_NumJoysticks();
    for (int i = 0; i < num_joysticks; ++i) {
        if (SDL_IsGameController(i)) {
            s_controller = SDL_GameControllerOpen(i);
            if (s_controller != nullptr) {
                s_joystick = SDL_GameControllerGetJoystick(s_controller);
                std::fprintf(stderr, "[INPUT] Successfully opened primary SDL_GameController: \"%s\"\n",
                             SDL_GameControllerName(s_controller));
                break;
            }
        }
    }

    if (s_controller == nullptr && num_joysticks > 0) {
        s_joystick = SDL_JoystickOpen(0);
        if (s_joystick != nullptr) {
            std::fprintf(stderr, "[INPUT] Fallback opened raw SDL_Joystick: \"%s\" (%d axes, %d buttons, %d hats)\n",
                         SDL_JoystickName(s_joystick),
                         SDL_JoystickNumAxes(s_joystick),
                         SDL_JoystickNumButtons(s_joystick),
                         SDL_JoystickNumHats(s_joystick));
        }
    }
}

float apply_axis_deadzone(float normalized, float deadzone) {
    float magnitude = std::abs(normalized);
    if (magnitude < deadzone) {
        return 0.0f;
    }
    float scaled = (magnitude - deadzone) / (1.0f - deadzone);
    return std::clamp(std::copysign(scaled, normalized), -1.0f, 1.0f);
}

PhysicalPadState read_physical_pad_state() {
    PhysicalPadState pad{};
    const auto& config = get_config().controls;
    bool positional = (config.face_layout == FACE_LAYOUT_POSITIONAL);

    if (s_controller != nullptr) {
        int16_t raw_lx = SDL_GameControllerGetAxis(s_controller, SDL_CONTROLLER_AXIS_LEFTX);
        int16_t raw_ly = SDL_GameControllerGetAxis(s_controller, SDL_CONTROLLER_AXIS_LEFTY);
        int16_t raw_rx = SDL_GameControllerGetAxis(s_controller, SDL_CONTROLLER_AXIS_RIGHTX);
        int16_t raw_ry = SDL_GameControllerGetAxis(s_controller, SDL_CONTROLLER_AXIS_RIGHTY);
        int16_t raw_tl = SDL_GameControllerGetAxis(s_controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
        int16_t raw_tr = SDL_GameControllerGetAxis(s_controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);

        pad.left_x = static_cast<float>(raw_lx) / MAX_AXIS_VALUE;
        pad.left_y = static_cast<float>(raw_ly) / MAX_AXIS_VALUE;
        pad.right_x = static_cast<float>(raw_rx) / MAX_AXIS_VALUE;
        pad.right_y = static_cast<float>(raw_ry) / MAX_AXIS_VALUE;

        pad.dpad_up = SDL_GameControllerGetButton(s_controller, SDL_CONTROLLER_BUTTON_DPAD_UP) != 0;
        pad.dpad_down = SDL_GameControllerGetButton(s_controller, SDL_CONTROLLER_BUTTON_DPAD_DOWN) != 0;
        pad.dpad_left = SDL_GameControllerGetButton(s_controller, SDL_CONTROLLER_BUTTON_DPAD_LEFT) != 0;
        pad.dpad_right = SDL_GameControllerGetButton(s_controller, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) != 0;

        pad.l1 = SDL_GameControllerGetButton(s_controller, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) != 0;
        pad.r1 = SDL_GameControllerGetButton(s_controller, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) != 0;
        pad.l2 = raw_tl > AXIS_TRIGGER_THRESHOLD;
        pad.r2 = raw_tr > AXIS_TRIGGER_THRESHOLD;

        pad.select = SDL_GameControllerGetButton(s_controller, SDL_CONTROLLER_BUTTON_BACK) != 0;
        pad.start = SDL_GameControllerGetButton(s_controller, SDL_CONTROLLER_BUTTON_START) != 0;
        pad.menu_guide = SDL_GameControllerGetButton(s_controller, SDL_CONTROLLER_BUTTON_GUIDE) != 0;

        bool btn_a = SDL_GameControllerGetButton(s_controller, SDL_CONTROLLER_BUTTON_A) != 0;
        bool btn_b = SDL_GameControllerGetButton(s_controller, SDL_CONTROLLER_BUTTON_B) != 0;
        bool btn_x = SDL_GameControllerGetButton(s_controller, SDL_CONTROLLER_BUTTON_X) != 0;
        bool btn_y = SDL_GameControllerGetButton(s_controller, SDL_CONTROLLER_BUTTON_Y) != 0;

        if (positional) {
            pad.east_a = btn_b;
            pad.south_b = btn_a;
            pad.north_x = btn_y;
            pad.west_y = btn_x;
        } else {
            pad.east_a = btn_a;
            pad.south_b = btn_b;
            pad.north_x = btn_x;
            pad.west_y = btn_y;
        }
    } else if (s_joystick != nullptr) {
        int num_buttons = SDL_JoystickNumButtons(s_joystick);
        int num_axes = SDL_JoystickNumAxes(s_joystick);
        int num_hats = SDL_JoystickNumHats(s_joystick);

        auto btn = [&](int idx) -> bool {
            return (idx >= 0 && idx < num_buttons) && (SDL_JoystickGetButton(s_joystick, idx) != 0);
        };
        auto axis = [&](int idx) -> float {
            return (idx >= 0 && idx < num_axes) ? (static_cast<float>(SDL_JoystickGetAxis(s_joystick, idx)) / MAX_AXIS_VALUE) : 0.0f;
        };

        if (num_hats > 0) {
            Uint8 hat = SDL_JoystickGetHat(s_joystick, 0);
            pad.dpad_up = (hat & SDL_HAT_UP) != 0;
            pad.dpad_down = (hat & SDL_HAT_DOWN) != 0;
            pad.dpad_left = (hat & SDL_HAT_LEFT) != 0;
            pad.dpad_right = (hat & SDL_HAT_RIGHT) != 0;
        }
        if (btn(11)) pad.dpad_up = true;
        if (btn(12)) pad.dpad_down = true;
        if (btn(13)) pad.dpad_left = true;
        if (btn(14)) pad.dpad_right = true;

        pad.left_x = axis(0);
        pad.left_y = axis(1);
        pad.right_x = axis(2);
        pad.right_y = axis(3);

        pad.l1 = btn(4);
        pad.r1 = btn(5);
        pad.l2 = btn(6) || (axis(4) > 0.3f);
        pad.r2 = btn(7) || (axis(5) > 0.3f);

        pad.select = btn(8);
        pad.start = btn(9);
        pad.menu_guide = btn(10);

        bool btn0 = btn(0);
        bool btn1 = btn(1);
        bool btn2 = btn(2);
        bool btn3 = btn(3);

        if (positional) {
            pad.east_a = btn0;
            pad.south_b = btn1;
            pad.north_x = btn2;
            pad.west_y = btn3;
        } else {
            pad.east_a = btn1;
            pad.south_b = btn0;
            pad.north_x = btn3;
            pad.west_y = btn2;
        }
    }

    // Keyboard support
    const uint8_t* keyboard = SDL_GetKeyboardState(nullptr);
    if (keyboard != nullptr) {
        if (keyboard[SDL_SCANCODE_UP] || keyboard[SDL_SCANCODE_W]) pad.dpad_up = true;
        if (keyboard[SDL_SCANCODE_DOWN] || keyboard[SDL_SCANCODE_S]) pad.dpad_down = true;
        if (keyboard[SDL_SCANCODE_LEFT] || keyboard[SDL_SCANCODE_A]) pad.dpad_left = true;
        if (keyboard[SDL_SCANCODE_RIGHT] || keyboard[SDL_SCANCODE_D]) pad.dpad_right = true;

        if (keyboard[SDL_SCANCODE_SPACE]) pad.east_a = true;
        if (keyboard[SDL_SCANCODE_BACKSPACE] || keyboard[SDL_SCANCODE_ESCAPE]) pad.south_b = true;
        if (keyboard[SDL_SCANCODE_X]) pad.north_x = true;
        if (keyboard[SDL_SCANCODE_C]) pad.west_y = true;

        if (keyboard[SDL_SCANCODE_Z] || keyboard[SDL_SCANCODE_J]) pad.r1 = true;
        if (keyboard[SDL_SCANCODE_K]) pad.l1 = true;
        if (keyboard[SDL_SCANCODE_L]) pad.l2 = true;

        if (keyboard[SDL_SCANCODE_RETURN]) pad.start = true;
        if (keyboard[SDL_SCANCODE_RSHIFT] || keyboard[SDL_SCANCODE_LSHIFT]) pad.select = true;
        if (keyboard[SDL_SCANCODE_F1] || keyboard[SDL_SCANCODE_TAB]) pad.menu_guide = true;
    }

    return pad;
}

void update_roll_state(bool roll_requested, int aim_dx, uint16_t& buttons) {
    bool roll_trigger = roll_requested && !s_roll_state.roll_button_was_pressed;
    s_roll_state.roll_button_was_pressed = roll_requested;

    if ((buttons & N64_C_LEFT) && !(buttons & N64_C_RIGHT)) {
        s_roll_state.last_move_was_left = true;
    } else if ((buttons & N64_C_RIGHT) && !(buttons & N64_C_LEFT)) {
        s_roll_state.last_move_was_left = false;
    }

    if (s_roll_state.move_button == 0 && roll_trigger) {
        if ((buttons & (N64_C_LEFT | N64_C_RIGHT)) == 0) {
            if (aim_dx < 0) {
                s_roll_state.last_move_was_left = true;
            } else if (aim_dx > 0) {
                s_roll_state.last_move_was_left = false;
            }
        }
        s_roll_state.move_button = s_roll_state.last_move_was_left ? N64_C_LEFT : N64_C_RIGHT;
        s_roll_state.current_frame = 0;
    }

    if (s_roll_state.move_button != 0) {
        int step = s_roll_state.current_frame / ROLL_STEP_FRAMES;
        buttons &= ~(N64_C_LEFT | N64_C_RIGHT);

        if (step == 1 || step == 3) {
            buttons |= s_roll_state.move_button;
        }

        s_roll_state.current_frame++;
        if (s_roll_state.current_frame >= ROLL_STEP_FRAMES * ROLL_TOTAL_STEPS) {
            s_roll_state.move_button = 0;
            s_roll_state.current_frame = 0;
        }
    }
}

bool is_playable_stage_overlay(const uint8_t* rdram) {
    if (rdram == nullptr || snp_in_front_end_menus()) {
        return false;
    }
    uint32_t rom = snp_overlay_area_rom();
    if (rom == 0) {
        return false;
    }
    for (uint32_t i = FIRST_PLAYABLE_STAGE_ENTRY; i <= LAST_PLAYABLE_STAGE_ENTRY; ++i) {
        uint32_t entry_rom = 0;
        std::memcpy(&entry_rom, rdram + ((LOAD_TABLE_ADDR + i * LOAD_TABLE_STRIDE) & 0x1FFFFFFFu), sizeof(uint32_t));
        if (entry_rom == rom) {
            return true;
        }
    }
    return false;
}

bool is_stage_combat_reticle(const uint8_t* rdram, uint8_t reticle_flags) {
    if (!is_playable_stage_overlay(rdram)) {
        return false;
    }
    if (read_rdram_byte(rdram, PAUSE_STATE_ADDR) != PAUSE_STATE_RUNNING) {
        return false;
    }
    return (reticle_flags & RETICLE_MENU_CURSOR_FLAG) == 0;
}

bool is_menu_cursor_active() {
    uint32_t last = s_last_menu_cursor_ms.load(std::memory_order_relaxed);
    if (last == 0) {
        return false;
    }
    return (SDL_GetTicks() - last) <= RETICLE_ACTIVE_TIMEOUT_MS;
}

} // namespace

bool is_gameplay_reticle_active() {
    if (snp_in_front_end_menus()) {
        return false;
    }
    const uint8_t* rdram = s_rdram_ptr.load(std::memory_order_relaxed);
    if (rdram != nullptr) {
        if (!is_playable_stage_overlay(rdram)) {
            return false;
        }
        if (read_rdram_byte(rdram, PAUSE_STATE_ADDR) != PAUSE_STATE_RUNNING) {
            return false;
        }
    }
    uint32_t last = s_last_gameplay_reticle_ms.load(std::memory_order_relaxed);
    if (last == 0) {
        return false;
    }
    uint32_t now = SDL_GetTicks();
    return (now - last) <= RETICLE_ACTIVE_TIMEOUT_MS;
}

void init_input() {
    s_main_thread_id = std::this_thread::get_id();
    log_detected_joysticks();
    open_primary_controller();
}

void shutdown_input() {
    std::lock_guard<std::mutex> lock(s_input_mutex);
    if (s_controller != nullptr) {
        SDL_GameControllerClose(s_controller);
        s_controller = nullptr;
        s_joystick = nullptr;
    } else if (s_joystick != nullptr) {
        SDL_JoystickClose(s_joystick);
        s_joystick = nullptr;
    }
}

void poll_input() {
    if (std::this_thread::get_id() != s_main_thread_id) {
        return;
    }

    std::lock_guard<std::mutex> lock(s_input_mutex);

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_QUIT:
            ultramodern::quit();
            break;
        case SDL_CONTROLLERDEVICEADDED:
            if (s_controller == nullptr) {
                open_primary_controller();
            }
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            if (s_controller != nullptr &&
                event.cdevice.which == SDL_JoystickInstanceID(s_joystick)) {
                SDL_GameControllerClose(s_controller);
                s_controller = nullptr;
                s_joystick = nullptr;
                open_primary_controller();
            }
            break;
        case SDL_JOYDEVICEADDED:
            if (s_controller == nullptr && s_joystick == nullptr) {
                open_primary_controller();
            }
            break;
        case SDL_JOYDEVICEREMOVED:
            if (s_joystick != nullptr &&
                event.jdevice.which == SDL_JoystickInstanceID(s_joystick)) {
                SDL_JoystickClose(s_joystick);
                s_joystick = nullptr;
                open_primary_controller();
            }
            break;
        case SDL_KEYDOWN:
            if (event.key.keysym.sym == SDLK_ESCAPE) {
                if (is_overlay_open()) {
                    save_config();
                    close_overlay();
                } else {
                    ultramodern::quit();
                }
            }
            break;
        default:
            break;
        }
    }

    PhysicalPadState pad = read_physical_pad_state();
    s_cached_pad_state = pad;

    // Exit immediately when Menu+Start or Select+Start are held together
    if ((pad.select && pad.start) || (pad.menu_guide && pad.start)) {
        s_select_combo_used = true;
        s_guide_combo_used = true;
        save_config();
        ultramodern::quit();
        return;
    }

    // Select alone tap trigger
    if (pad.select && !s_prev_select) {
        s_select_held = true;
        s_select_combo_used = false;
    } else if (!pad.select && s_prev_select) {
        if (s_select_held && !s_select_combo_used) {
            toggle_overlay();
        }
        s_select_held = false;
    }
    s_prev_select = pad.select;

    // Menu/Guide / F1 / Tab alone tap trigger
    if (pad.menu_guide && !s_prev_guide) {
        s_guide_held = true;
        s_guide_combo_used = false;
    } else if (!pad.menu_guide && s_prev_guide) {
        if (s_guide_held && !s_guide_combo_used) {
            toggle_overlay();
        }
        s_guide_held = false;
    }
    s_prev_guide = pad.menu_guide;

    // Process active overlay menu inputs during poll_input (main thread / 60 Hz)
    if (is_overlay_open()) {
        bool nav_up = pad.dpad_up && !s_prev_nav_up;
        bool nav_down = pad.dpad_down && !s_prev_nav_down;
        bool nav_left = pad.dpad_left && !s_prev_nav_left;
        bool nav_right = pad.dpad_right && !s_prev_nav_right;
        bool nav_a = pad.east_a && !s_prev_nav_a;
        bool nav_b = pad.south_b && !s_prev_nav_b;
        bool nav_start = pad.start && !s_prev_nav_start;

        s_prev_nav_up = pad.dpad_up;
        s_prev_nav_down = pad.dpad_down;
        s_prev_nav_left = pad.dpad_left;
        s_prev_nav_right = pad.dpad_right;
        s_prev_nav_a = pad.east_a;
        s_prev_nav_b = pad.south_b;
        s_prev_nav_start = pad.start;

        update_overlay_input(nav_up, nav_down, nav_left, nav_right, nav_a, nav_b, nav_start);

        s_roll_state = {};
        s_aim_ramp = {};
        return;
    }

    s_prev_nav_up = pad.dpad_up;
    s_prev_nav_down = pad.dpad_down;
    s_prev_nav_left = pad.dpad_left;
    s_prev_nav_right = pad.dpad_right;
    s_prev_nav_a = pad.east_a;
    s_prev_nav_b = pad.south_b;
    s_prev_nav_start = pad.start;
}

bool get_input(int controller_num, uint16_t* buttons, float* x, float* y) {
    if (controller_num != 0 || buttons == nullptr || x == nullptr || y == nullptr) {
        return false;
    }

    if (is_overlay_open()) {
        *buttons = 0;
        *x = 0.0f;
        *y = 0.0f;
        return true;
    }

    PhysicalPadState pad{};
    {
        std::lock_guard<std::mutex> lock(s_input_mutex);
        pad = s_cached_pad_state;
    }

    if (is_overlay_open()) {
        *buttons = 0;
        *x = 0.0f;
        *y = 0.0f;
        return true;
    }

    const auto& config = get_config().controls;
    uint16_t pad_buttons = 0;
    float stick_x = 0.0f;
    float stick_y = 0.0f;
    bool roll_requested = false;

    bool in_combat = is_gameplay_reticle_active();

    if (!in_combat) {
        // Automatic Menu Context (Title Screen, Options, File Select, Name Entry, Cutscenes, Pause)
        if (pad.dpad_up) pad_buttons |= N64_DPAD_UP;
        if (pad.dpad_down) pad_buttons |= N64_DPAD_DOWN;
        if (pad.dpad_left) pad_buttons |= N64_DPAD_LEFT;
        if (pad.dpad_right) pad_buttons |= N64_DPAD_RIGHT;

        int menu_dx = (pad.dpad_right ? 1 : 0) - (pad.dpad_left ? 1 : 0);
        int menu_dy = (pad.dpad_up ? 1 : 0) - (pad.dpad_down ? 1 : 0);
        if (is_menu_cursor_active()) {
            // Free-floating 2D menu cursor (Main Menu, OPT., File Select, Name Entry): smooth ramp
            s_aim_ramp.update(menu_dx, menu_dy, stick_x, stick_y);
        } else {
            // Discrete list menu (In-Stage Pause, Continue): immediate full-step navigation
            s_aim_ramp.reset();
            stick_x = static_cast<float>(menu_dx);
            stick_y = static_cast<float>(menu_dy);
        }

        float lx = apply_axis_deadzone(pad.left_x, config.deadzone);
        float ly = apply_axis_deadzone(-pad.left_y, config.deadzone);
        if (std::abs(lx) > 0.01f) stick_x = std::clamp(stick_x + lx, -1.0f, 1.0f);
        if (std::abs(ly) > 0.01f) stick_y = std::clamp(stick_y + ly, -1.0f, 1.0f);
        if (lx < -0.5f) pad_buttons |= N64_DPAD_LEFT;
        else if (lx > 0.5f) pad_buttons |= N64_DPAD_RIGHT;
        if (ly > 0.5f) pad_buttons |= N64_DPAD_UP;
        else if (ly < -0.5f) pad_buttons |= N64_DPAD_DOWN;

        if (pad.east_a) pad_buttons |= N64_A;
        if (pad.south_b) pad_buttons |= N64_B;
        if (pad.start) pad_buttons |= N64_START;

        // Shoulder/Z shortcuts in menus
        if (pad.l1) pad_buttons |= N64_L;
        if (pad.r1) pad_buttons |= N64_R;
        if (pad.r2) pad_buttons |= N64_Z;

        s_roll_state = {};
        s_pulse_active = false;
        s_pulse_start_tick = 0;
        s_flick_dx.store(0, std::memory_order_relaxed);
        s_flick_dy.store(0, std::memory_order_relaxed);
        s_explicit_target_switch.store(false, std::memory_order_relaxed);
    } else {
        // Stage Combat Context
        int aim_dx = 0;
        int aim_dy = 0;
        bool lock_btn = false;

        if (config.scheme == CONTROL_SCHEME_BUMPER_STRAFE) {
            aim_dx = (pad.dpad_right ? 1 : 0) - (pad.dpad_left ? 1 : 0);
            aim_dy = (pad.dpad_up ? 1 : 0) - (pad.dpad_down ? 1 : 0);
            s_aim_ramp.update(aim_dx, aim_dy, stick_x, stick_y);
            s_flick_dx.store(aim_dx, std::memory_order_relaxed);
            s_flick_dy.store(aim_dy, std::memory_order_relaxed);

            float lx = apply_axis_deadzone(pad.left_x, config.deadzone);
            float ly = apply_axis_deadzone(-pad.left_y, config.deadzone);
            if (std::abs(lx) > 0.01f) stick_x = std::clamp(stick_x + lx, -1.0f, 1.0f);
            if (std::abs(ly) > 0.01f) stick_y = std::clamp(stick_y + ly, -1.0f, 1.0f);

            if (pad.l1) pad_buttons |= N64_C_LEFT;
            if (pad.r1) pad_buttons |= N64_C_RIGHT;
            if (pad.east_a || pad.west_y || pad.r2) pad_buttons |= N64_Z;
            if (pad.south_b) pad_buttons |= N64_R;
            lock_btn = pad.north_x || pad.l2;
            if (pad.start) pad_buttons |= N64_START;

        } else if (config.scheme == CONTROL_SCHEME_FACE_STRAFE) {
            aim_dx = (pad.dpad_right ? 1 : 0) - (pad.dpad_left ? 1 : 0);
            aim_dy = (pad.dpad_up ? 1 : 0) - (pad.dpad_down ? 1 : 0);
            s_aim_ramp.update(aim_dx, aim_dy, stick_x, stick_y);
            s_flick_dx.store(aim_dx, std::memory_order_relaxed);
            s_flick_dy.store(aim_dy, std::memory_order_relaxed);

            float lx = apply_axis_deadzone(pad.left_x, config.deadzone);
            float ly = apply_axis_deadzone(-pad.left_y, config.deadzone);
            if (std::abs(lx) > 0.01f) stick_x = std::clamp(stick_x + lx, -1.0f, 1.0f);
            if (std::abs(ly) > 0.01f) stick_y = std::clamp(stick_y + ly, -1.0f, 1.0f);

            if (pad.west_y) pad_buttons |= N64_C_LEFT;
            if (pad.south_b) pad_buttons |= N64_C_RIGHT;
            if (pad.east_a) roll_requested = true;
            if (pad.r1 || pad.r2) pad_buttons |= N64_Z;
            if (pad.l1 || pad.north_x) pad_buttons |= N64_R;
            lock_btn = pad.l2;
            if (pad.start) pad_buttons |= N64_START;

        } else if (config.scheme == CONTROL_SCHEME_DPAD_STRAFE_SHIFT) {
            bool aim_shift = pad.l1 || pad.l2;
            static bool s_prev_shift_x = false;
            static bool s_prev_strafe_x = false;

            if (aim_shift) {
                aim_dx = (pad.dpad_right ? 1 : 0) - (pad.dpad_left ? 1 : 0);
                aim_dy = (pad.dpad_up ? 1 : 0) - (pad.dpad_down ? 1 : 0);
                s_aim_ramp.update(aim_dx, aim_dy, stick_x, stick_y);
                s_flick_dx.store(0, std::memory_order_relaxed);
                s_flick_dy.store(0, std::memory_order_relaxed);

                if (pad.north_x && !s_prev_shift_x) {
                    s_recenter_requested.store(true, std::memory_order_relaxed);
                }
            } else {
                s_aim_ramp.reset();
                if (pad.dpad_left) pad_buttons |= N64_C_LEFT;
                if (pad.dpad_right) pad_buttons |= N64_C_RIGHT;
                int flick_dy = (pad.dpad_up ? 1 : 0) - (pad.dpad_down ? 1 : 0);
                s_flick_dx.store(0, std::memory_order_relaxed);
                s_flick_dy.store(flick_dy, std::memory_order_relaxed);

                if (pad.north_x && !s_prev_strafe_x) {
                    if (config.smart_lock_on != SMART_LOCK_OFF) {
                        s_explicit_target_switch.store(true, std::memory_order_relaxed);
                    }
                }
            }
            s_prev_shift_x = aim_shift && pad.north_x;
            s_prev_strafe_x = !aim_shift && pad.north_x;

            float lx = apply_axis_deadzone(pad.left_x, config.deadzone);
            float ly = apply_axis_deadzone(-pad.left_y, config.deadzone);
            if (std::abs(lx) > 0.01f) stick_x = std::clamp(stick_x + lx, -1.0f, 1.0f);
            if (std::abs(ly) > 0.01f) stick_y = std::clamp(stick_y + ly, -1.0f, 1.0f);

            if (pad.east_a || pad.r1 || pad.r2) pad_buttons |= N64_Z;
            if (pad.south_b) pad_buttons |= N64_R;
            if (pad.west_y) roll_requested = true;
            lock_btn = pad.north_x;
            if (pad.start) pad_buttons |= N64_START;

        } else if (config.scheme == CONTROL_SCHEME_DPAD_AIM) {
            aim_dx = (pad.dpad_right ? 1 : 0) - (pad.dpad_left ? 1 : 0);
            aim_dy = (pad.dpad_up ? 1 : 0) - (pad.dpad_down ? 1 : 0);
            s_aim_ramp.update(aim_dx, aim_dy, stick_x, stick_y);
            s_flick_dx.store(aim_dx, std::memory_order_relaxed);
            s_flick_dy.store(aim_dy, std::memory_order_relaxed);

            float rx = apply_axis_deadzone(pad.right_x, config.deadzone);
            float ry = apply_axis_deadzone(-pad.right_y, config.deadzone);
            if (std::abs(rx) > 0.01f) stick_x = std::clamp(stick_x + rx, -1.0f, 1.0f);
            if (std::abs(ry) > 0.01f) stick_y = std::clamp(stick_y + ry, -1.0f, 1.0f);

            float lx = apply_axis_deadzone(pad.left_x, config.deadzone);
            if (lx < -0.3f) pad_buttons |= N64_C_LEFT;
            else if (lx > 0.3f) pad_buttons |= N64_C_RIGHT;

            if (pad.west_y) pad_buttons |= N64_C_LEFT;
            if (pad.east_a) pad_buttons |= N64_C_RIGHT;
            if (pad.r1 || pad.r2) pad_buttons |= N64_Z;
            if (pad.l1) pad_buttons |= N64_R;
            if (pad.south_b || pad.l2) roll_requested = true;
            lock_btn = pad.north_x;
            if (pad.start) pad_buttons |= N64_START;

        } else if (config.scheme == CONTROL_SCHEME_DPAD_STRAFE) {
            if (pad.dpad_left) pad_buttons |= N64_C_LEFT;
            if (pad.dpad_right) pad_buttons |= N64_C_RIGHT;

            aim_dx = (pad.east_a ? 1 : 0) - (pad.west_y ? 1 : 0);
            aim_dy = (pad.north_x ? 1 : 0) - (pad.south_b ? 1 : 0);
            s_aim_ramp.update(aim_dx, aim_dy, stick_x, stick_y);
            s_flick_dx.store(aim_dx, std::memory_order_relaxed);
            s_flick_dy.store(aim_dy, std::memory_order_relaxed);

            if (pad.r1) pad_buttons |= N64_Z;
            if (pad.l1) pad_buttons |= N64_R;
            if (pad.l2) roll_requested = true;
            lock_btn = pad.r2;
            if (pad.start) pad_buttons |= N64_START;

        } else {
            float aim_raw_x = config.swap_sticks ? pad.left_x : pad.right_x;
            float aim_raw_y = config.swap_sticks ? pad.left_y : pad.right_y;
            float move_raw_x = config.swap_sticks ? pad.right_x : pad.left_x;

            stick_x = apply_axis_deadzone(aim_raw_x, config.deadzone);
            stick_y = apply_axis_deadzone(-aim_raw_y, config.deadzone);

            if (std::hypot(stick_x, stick_y) > 0.5f) {
                s_flick_dx.store(stick_x > 0.2f ? 1 : (stick_x < -0.2f ? -1 : 0), std::memory_order_relaxed);
                s_flick_dy.store(stick_y > 0.2f ? 1 : (stick_y < -0.2f ? -1 : 0), std::memory_order_relaxed);
            } else {
                s_flick_dx.store(0, std::memory_order_relaxed);
                s_flick_dy.store(0, std::memory_order_relaxed);
            }

            float move_x = apply_axis_deadzone(move_raw_x, config.deadzone);
            if (move_x < -0.3f) pad_buttons |= N64_C_LEFT;
            else if (move_x > 0.3f) pad_buttons |= N64_C_RIGHT;

            if (pad.dpad_left) pad_buttons |= N64_C_LEFT;
            if (pad.dpad_right) pad_buttons |= N64_C_RIGHT;
            if (pad.dpad_up) pad_buttons |= N64_DPAD_UP;
            if (pad.dpad_down) pad_buttons |= N64_DPAD_DOWN;

            if (pad.r1 || pad.r2) pad_buttons |= N64_Z;
            if (pad.l1 || pad.l2) pad_buttons |= N64_R;
            if (pad.north_x) roll_requested = true;
            lock_btn = pad.east_a;
            if (pad.south_b) pad_buttons |= N64_B;
            if (pad.west_y) pad_buttons |= N64_L;
            if (pad.start) pad_buttons |= N64_START;
        }

        if (config.smart_lock_on == SMART_LOCK_OFF) {
            if (lock_btn) {
                pad_buttons |= N64_A;
            }
        } else {
            bool want_lock_on = false;
            if (config.scheme == CONTROL_SCHEME_DPAD_STRAFE_SHIFT) {
                want_lock_on = !(pad.l1 || pad.l2);
            } else if (config.smart_lock_on == SMART_LOCK_HOLD) {
                want_lock_on = lock_btn;
            } else {
                want_lock_on = !lock_btn;
            }

            uint32_t cur_tick = s_reticle_tick.load(std::memory_order_relaxed);
            if (s_pulse_active) {
                if (cur_tick == s_pulse_start_tick) {
                    pad_buttons |= N64_A;
                } else if (cur_tick == s_pulse_start_tick + 1) {
                    // release
                } else if (cur_tick >= s_pulse_start_tick + 2 || cur_tick < s_pulse_start_tick) {
                    s_pulse_active = false;
                }
            }
            if (!s_pulse_active && cur_tick > 0 && want_lock_on != s_actual_lock_on.load(std::memory_order_relaxed)) {
                s_pulse_active = true;
                s_pulse_start_tick = cur_tick;
                pad_buttons |= N64_A;
            }
        }

        update_roll_state(roll_requested, aim_dx, pad_buttons);
    }

    *buttons = pad_buttons;
    *x = std::clamp(stick_x, -1.0f, 1.0f);
    *y = std::clamp(stick_y, -1.0f, 1.0f);
    return true;
}

void set_rumble(int controller_num, bool rumble) {
    if (controller_num != 0) {
        return;
    }
    if (!get_config().controls.rumble) {
        return;
    }

    std::lock_guard<std::mutex> lock(s_input_mutex);
    if (s_controller == nullptr) {
        return;
    }

    if (rumble) {
        SDL_GameControllerRumble(s_controller, RUMBLE_LOW_FREQ, RUMBLE_HIGH_FREQ, RUMBLE_DURATION_MS);
    } else {
        SDL_GameControllerRumble(s_controller, 0, 0, 0);
    }
}

ultramodern::input::connected_device_info_t get_connected_device_info(int controller_num) {
    if (controller_num != 0) {
        return {
            .connected_device = ultramodern::input::Device::None,
            .connected_pak = ultramodern::input::Pak::None,
        };
    }
    return {
        .connected_device = ultramodern::input::Device::Controller,
        .connected_pak = get_config().controls.rumble ? ultramodern::input::Pak::RumblePak : ultramodern::input::Pak::None,
    };
}

} // namespace snp::handheld

extern "C" void snp_mouse_reticle(uint8_t* rdram, uint32_t reticle) {
    if (rdram == nullptr || reticle < 0x80000000u || reticle >= 0x80800000u) {
        return;
    }
    snp::handheld::s_rdram_ptr.store(rdram, std::memory_order_relaxed);
    if (snp::handheld::read_rdram_byte(rdram, 0x800D4F00u) != 0) {
        return;
    }

    uint8_t reticle_flags = snp::handheld::read_rdram_byte(rdram, reticle);
    if (!snp::handheld::is_stage_combat_reticle(rdram, reticle_flags)) {
        snp::handheld::s_last_menu_cursor_ms.store(SDL_GetTicks(), std::memory_order_relaxed);
        snp::handheld::s_last_gameplay_reticle_ms.store(0, std::memory_order_relaxed);
        return;
    }

    snp::handheld::s_last_gameplay_reticle_ms.store(SDL_GetTicks(), std::memory_order_relaxed);

    bool is_lock_on_mode = !(reticle_flags & 0x20u) && ((reticle_flags & 0x04u) != 0);
    bool is_target_locked = is_lock_on_mode && (snp::handheld::read_rdram_byte(rdram, reticle + 2u) != 0);

    if (snp::handheld::s_recenter_requested.exchange(false, std::memory_order_relaxed) && !is_target_locked) {
        snp::handheld::write_rdram_float(rdram, reticle + 0x28u, 0.0f);
        snp::handheld::write_rdram_float(rdram, reticle + 0x2Cu, 0.0f);
        snp::handheld::write_rdram_float(rdram, reticle + 0x58u, 0.0f);
        snp::handheld::write_rdram_float(rdram, reticle + 0x5Cu, 0.0f);
    }

    if (!is_lock_on_mode) {
        float sensitivity = snp::handheld::get_config().controls.stick_sensitivity / 100.0f;
        if (std::abs(sensitivity - 1.0f) > 0.001f) {
            for (uint32_t axis = 0; axis < 2; ++axis) {
                float velocity = snp::handheld::read_rdram_float(rdram, reticle + 0x58u + axis * 4u);
                float position = snp::handheld::read_rdram_float(rdram, reticle + 0x28u + axis * 4u);
                float new_pos = position + velocity * (sensitivity - 1.0f);
                if (std::isfinite(new_pos)) {
                    snp::handheld::write_rdram_float(rdram, reticle + 0x28u + axis * 4u, new_pos);
                }
            }
        }
    }
}

extern "C" void snp_aim_stick(uint8_t* rdram, uint32_t reticle, int32_t raw_x, int32_t raw_y, int32_t* x, int32_t* y) {
    if (rdram == nullptr || reticle < 0x80000000u || reticle >= 0x80800000u) {
        return;
    }
    snp::handheld::s_rdram_ptr.store(rdram, std::memory_order_relaxed);
    if (snp::handheld::read_rdram_byte(rdram, 0x800D4F00u) != 0) {
        return;
    }

    if (snp::config::get_option(snp::config::Option::AnalogAim)) {
        *x = std::clamp(raw_x, -64, 64);
        *y = std::clamp(raw_y, -64, 64);
    }

    uint8_t reticle_flags = snp::handheld::read_rdram_byte(rdram, reticle);
    if (!snp::handheld::is_stage_combat_reticle(rdram, reticle_flags)) {
        snp::handheld::s_last_menu_cursor_ms.store(SDL_GetTicks(), std::memory_order_relaxed);
        snp::handheld::s_last_gameplay_reticle_ms.store(0, std::memory_order_relaxed);
        snp::handheld::s_actual_lock_on.store(false, std::memory_order_relaxed);
        return;
    }

    bool actual_lock_on = !(reticle_flags & 0x20u) && ((reticle_flags & 0x04u) != 0);
    bool target_locked = actual_lock_on && (snp::handheld::read_rdram_byte(rdram, reticle + 2u) != 0);
    bool search_busy = (reticle_flags & 0x10u) != 0;

    snp::handheld::s_actual_lock_on.store(actual_lock_on, std::memory_order_relaxed);
    snp::handheld::s_reticle_tick.fetch_add(1, std::memory_order_relaxed);

    static int s_prev_flick_dx = 0;
    static int s_prev_flick_dy = 0;
    static int s_flick_ticks_remaining = 0;
    static float s_flick_norm_x = 0.0f;
    static float s_flick_norm_y = 0.0f;
    static uint32_t s_auto_acquire_tick = 0;
    static int s_cardinal_cycle = 0;

    if (!actual_lock_on) {
        s_prev_flick_dx = 0;
        s_prev_flick_dy = 0;
        s_flick_ticks_remaining = 0;
        s_auto_acquire_tick = 0;
        snp::handheld::s_explicit_target_switch.store(false, std::memory_order_relaxed);
        return;
    }

    int flick_dx = snp::handheld::s_flick_dx.load(std::memory_order_relaxed);
    int flick_dy = snp::handheld::s_flick_dy.load(std::memory_order_relaxed);
    bool explicit_switch = snp::handheld::s_explicit_target_switch.exchange(false, std::memory_order_relaxed);

    bool new_flick_direction = (flick_dx != s_prev_flick_dx || flick_dy != s_prev_flick_dy) && (flick_dx != 0 || flick_dy != 0);
    s_prev_flick_dx = flick_dx;
    s_prev_flick_dy = flick_dy;

    if (new_flick_direction || explicit_switch) {
        float fx = static_cast<float>(flick_dx);
        float fy = static_cast<float>(flick_dy);
        if (explicit_switch && fx == 0.0f && fy == 0.0f) {
            static int s_explicit_dir = 1;
            s_explicit_dir = -s_explicit_dir;
            fx = static_cast<float>(s_explicit_dir);
        }
        float len = std::hypot(fx, fy);
        if (len > 0.001f) {
            s_flick_norm_x = fx / len;
            s_flick_norm_y = fy / len;
            s_flick_ticks_remaining = 2;
        }
    }

    if (s_flick_ticks_remaining > 0) {
        if (search_busy) {
            *x = 0;
            *y = 0;
        } else {
            *x = static_cast<int32_t>(std::lround(s_flick_norm_x * 56.0f));
            *y = static_cast<int32_t>(std::lround(s_flick_norm_y * 56.0f));
            s_flick_ticks_remaining--;
        }
    } else if (flick_dx != 0 || flick_dy != 0) {
        if (snp::handheld::get_config().controls.smart_lock_on != snp::handheld::SMART_LOCK_OFF) {
            *x = 0;
            *y = 0;
        }
    } else if (!target_locked && snp::handheld::get_config().controls.smart_lock_on != snp::handheld::SMART_LOCK_OFF) {
        if (search_busy) {
            *x = 0;
            *y = 0;
        } else if (s_auto_acquire_tick % 2 == 0) {
            float rx = snp::handheld::read_rdram_float(rdram, reticle + 0x28u);
            float ry = snp::handheld::read_rdram_float(rdram, reticle + 0x2Cu);
            float rdist = std::hypot(rx, ry);
            if (rdist > 8.0f && s_auto_acquire_tick == 0) {
                *x = static_cast<int32_t>(std::lround((-rx / rdist) * 56.0f));
                *y = static_cast<int32_t>(std::lround((-ry / rdist) * 56.0f));
            } else {
                constexpr int CARDINAL_DIRS[4][2] = {
                    { 56, 0 }, { 0, 56 }, { -56, 0 }, { 0, -56 }
                };
                *x = CARDINAL_DIRS[s_cardinal_cycle][0];
                *y = CARDINAL_DIRS[s_cardinal_cycle][1];
                s_cardinal_cycle = (s_cardinal_cycle + 1) % 4;
            }
            s_auto_acquire_tick++;
        } else {
            *x = 0;
            *y = 0;
            s_auto_acquire_tick++;
        }
    }

    if (target_locked) {
        s_auto_acquire_tick = 0;
    }
}

namespace snp::mouse {

void init() {}
void set_touch_aim(TouchAim) {}
void set_touch_aim_speed(float) {}
TouchAim get_touch_aim() { return TouchAim::Off; }
void touch_aim_down(float, float, int, int) {}
void touch_aim_move(float, float) {}
uint32_t live_reticle_id(uint8_t*, float&, float&) { return 0; }
uint32_t live_id(uint32_t, float, float) { return 0; }
void decode_live_id(uint32_t, uint32_t& slot, float& x, float& y) {
    slot = 0;
    x = 0.0f;
    y = 0.0f;
}
uint32_t recent_reticle_positions(float*, float*, uint32_t) { return 0; }
void set_live_previous(uint32_t, float, float) {}
void aim_log(const char*, ...) {}

} // namespace snp::mouse

namespace snp {

void register_mod_exports() {}
void register_printf_export() {}
uint32_t mod_live_reticle_id(float, float) { return 0; }
bool mod_live_offset(uint32_t, float, float, float*, float*) { return false; }
uint32_t mod_overlay_list() { return 0; }

} // namespace snp
