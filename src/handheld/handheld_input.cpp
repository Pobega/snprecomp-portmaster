#include "handheld_input.h"

#include <algorithm>
#include <atomic>
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

SDL_GameController* s_controller = nullptr;
SDL_Joystick* s_joystick = nullptr;

std::thread::id s_main_thread_id;
std::mutex s_input_mutex;

std::atomic<uint32_t> s_last_gameplay_reticle_ms{0};

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

    void update(int dx, int dy, float sensitivity_percent, float& out_x, float& out_y) {
        if (dx == 0 && dy == 0) {
            hold_frames = 0;
            last_dx = 0;
            last_dy = 0;
            out_x = 0.0f;
            out_y = 0.0f;
            return;
        }

        if (dx != last_dx || dy != last_dy) {
            hold_frames = 1;
            last_dx = dx;
            last_dy = dy;
        } else {
            if (hold_frames < 10) {
                hold_frames++;
            }
        }

        constexpr float INITIAL_MAGNITUDE = 0.35f;
        constexpr float MAX_MAGNITUDE = 1.0f;
        constexpr int RAMP_STEPS = 9;
        float t = static_cast<float>(hold_frames - 1) / static_cast<float>(RAMP_STEPS);
        float magnitude = INITIAL_MAGNITUDE + (MAX_MAGNITUDE - INITIAL_MAGNITUDE) * t;

        constexpr float DIAG_FACTOR = 0.70710678f;
        float dir_x = static_cast<float>(dx);
        float dir_y = static_cast<float>(dy);
        if (dx != 0 && dy != 0) {
            dir_x *= DIAG_FACTOR;
            dir_y *= DIAG_FACTOR;
        }

        float factor = sensitivity_percent / 100.0f;
        out_x = std::clamp(dir_x * magnitude * factor, -1.0f, 1.0f);
        out_y = std::clamp(dir_y * magnitude * factor, -1.0f, 1.0f);
    }
};

DigitalAimRamp s_aim_ramp;

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

void update_roll_state(bool roll_requested, uint16_t& buttons) {
    bool roll_trigger = roll_requested && !s_roll_state.roll_button_was_pressed;
    s_roll_state.roll_button_was_pressed = roll_requested;

    if ((buttons & N64_C_LEFT) && !(buttons & N64_C_RIGHT)) {
        s_roll_state.last_move_was_left = true;
    } else if ((buttons & N64_C_RIGHT) && !(buttons & N64_C_LEFT)) {
        s_roll_state.last_move_was_left = false;
    }

    if (s_roll_state.move_button == 0 && roll_trigger) {
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

} // namespace

bool is_gameplay_reticle_active() {
    uint32_t last = s_last_gameplay_reticle_ms.load(std::memory_order_relaxed);
    if (last == 0) {
        return false;
    }
    uint32_t now = SDL_GetTicks();
    return (now - last) <= RETICLE_ACTIVE_TIMEOUT_MS;
}

void init_input() {
    s_main_thread_id = std::this_thread::get_id();

    const char* env_db = std::getenv("SDL_GAMECONTROLLERCONFIG_FILE");
    if (env_db != nullptr && std::filesystem::exists(env_db)) {
        int count = SDL_GameControllerAddMappingsFromFile(env_db);
        std::fprintf(stderr, "[INPUT] Loaded %d mapping(s) from SDL_GAMECONTROLLERCONFIG_FILE: %s\n", count, env_db);
    }

    if (std::filesystem::exists("gamecontrollerdb.txt")) {
        int count = SDL_GameControllerAddMappingsFromFile("gamecontrollerdb.txt");
        std::fprintf(stderr, "[INPUT] Loaded %d mapping(s) from gamecontrollerdb.txt\n", count);
    }

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

    // Check Select + Start held together
    if (pad.select && pad.start) {
        s_select_combo_used = true;
        save_config();
        ultramodern::quit();
        return;
    }

    // Select alone edge trigger
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

    // Menu/Guide / F1 / Tab edge trigger
    if (pad.menu_guide && !s_prev_guide) {
        toggle_overlay();
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
        // Automatic Menu Context (Title Screen, Options, Character Select, Cutscenes, Pause)
        if (pad.dpad_up) { pad_buttons |= N64_DPAD_UP; stick_y = 1.0f; }
        if (pad.dpad_down) { pad_buttons |= N64_DPAD_DOWN; stick_y = -1.0f; }
        if (pad.dpad_left) { pad_buttons |= N64_DPAD_LEFT; stick_x = -1.0f; }
        if (pad.dpad_right) { pad_buttons |= N64_DPAD_RIGHT; stick_x = 1.0f; }

        float lx = apply_axis_deadzone(pad.left_x, config.deadzone);
        float ly = apply_axis_deadzone(-pad.left_y, config.deadzone);
        if (lx < -0.3f) { pad_buttons |= N64_DPAD_LEFT; stick_x = lx; }
        else if (lx > 0.3f) { pad_buttons |= N64_DPAD_RIGHT; stick_x = lx; }
        if (ly > 0.3f) { pad_buttons |= N64_DPAD_UP; stick_y = ly; }
        else if (ly < -0.3f) { pad_buttons |= N64_DPAD_DOWN; stick_y = ly; }

        if (pad.east_a) pad_buttons |= N64_A;
        if (pad.south_b) pad_buttons |= N64_B;
        if (pad.start) pad_buttons |= N64_START;

        // Shoulder/Z shortcuts in menus
        if (pad.l1) pad_buttons |= N64_L;
        if (pad.r1) pad_buttons |= N64_R;
        if (pad.r2) pad_buttons |= N64_Z;

        s_aim_ramp = {};
        s_roll_state = {};
    } else {
        // Stage Combat Context
        if (config.scheme == CONTROL_SCHEME_DPAD_AIM) {
            int dx = (pad.dpad_right ? 1 : 0) - (pad.dpad_left ? 1 : 0);
            int dy = (pad.dpad_up ? 1 : 0) - (pad.dpad_down ? 1 : 0);
            s_aim_ramp.update(dx, dy, config.stick_sensitivity, stick_x, stick_y);

            float rx = apply_axis_deadzone(pad.right_x, config.deadzone) * (config.stick_sensitivity / 100.0f);
            float ry = apply_axis_deadzone(-pad.right_y, config.deadzone) * (config.stick_sensitivity / 100.0f);
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
            if (pad.north_x) pad_buttons |= N64_A;
            if (pad.start) pad_buttons |= N64_START;
        } else if (config.scheme == CONTROL_SCHEME_DPAD_STRAFE) {
            if (pad.dpad_left) pad_buttons |= N64_C_LEFT;
            if (pad.dpad_right) pad_buttons |= N64_C_RIGHT;

            int dx = (pad.east_a ? 1 : 0) - (pad.west_y ? 1 : 0);
            int dy = (pad.north_x ? 1 : 0) - (pad.south_b ? 1 : 0);
            s_aim_ramp.update(dx, dy, config.stick_sensitivity, stick_x, stick_y);

            if (pad.r1) pad_buttons |= N64_Z;
            if (pad.l1) pad_buttons |= N64_R;
            if (pad.l2) roll_requested = true;
            if (pad.r2) pad_buttons |= N64_A;
            if (pad.start) pad_buttons |= N64_START;
        } else {
            float aim_raw_x = config.swap_sticks ? pad.left_x : pad.right_x;
            float aim_raw_y = config.swap_sticks ? pad.left_y : pad.right_y;
            float move_raw_x = config.swap_sticks ? pad.right_x : pad.left_x;

            float sens = config.stick_sensitivity / 100.0f;
            stick_x = apply_axis_deadzone(aim_raw_x, config.deadzone) * sens;
            stick_y = apply_axis_deadzone(-aim_raw_y, config.deadzone) * sens;

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
            if (pad.east_a) pad_buttons |= N64_A;
            if (pad.south_b) pad_buttons |= N64_B;
            if (pad.west_y) pad_buttons |= N64_L;
            if (pad.start) pad_buttons |= N64_START;
        }

        update_roll_state(roll_requested, pad_buttons);
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
    (void)reticle;
    if (rdram == nullptr) {
        return;
    }
    uint8_t demo = rdram[(0x800D4F00u - 0x80000000u) ^ 3];
    if (demo == 0) {
        snp::handheld::s_last_gameplay_reticle_ms.store(SDL_GetTicks(), std::memory_order_relaxed);
    }
}

extern "C" void snp_aim_stick(uint8_t* rdram, uint32_t reticle, int32_t raw_x, int32_t raw_y, int32_t* x, int32_t* y) {
    (void)reticle;
    uint8_t demo = rdram[(0x800D4F00u - 0x80000000u) ^ 3];
    if (demo != 0) {
        return;
    }
    if (snp::config::get_option(snp::config::Option::AnalogAim)) {
        *x = std::clamp(raw_x, -64, 64);
        *y = std::clamp(raw_y, -64, 64);
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
