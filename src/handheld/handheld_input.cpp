#include "handheld_input.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <SDL2/SDL.h>

#include "handheld_config.h"
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

SDL_GameController* s_controller = nullptr;
SDL_Joystick* s_joystick = nullptr;

struct RollStateMachine {
    bool roll_button_was_pressed = false;
    uint16_t move_button = 0;
    int current_frame = 0;
    bool last_move_was_left = true;
};

RollStateMachine s_roll_state;

void open_primary_controller() {
    if (s_controller != nullptr) {
        return;
    }

    int num_joysticks = SDL_NumJoysticks();
    for (int i = 0; i < num_joysticks; ++i) {
        if (SDL_IsGameController(i)) {
            s_controller = SDL_GameControllerOpen(i);
            if (s_controller != nullptr) {
                s_joystick = SDL_GameControllerGetJoystick(s_controller);
                break;
            }
        }
    }

    if (s_controller == nullptr && num_joysticks > 0) {
        s_joystick = SDL_JoystickOpen(0);
    }
}

float apply_axis_deadzone(int16_t raw_value, float deadzone) {
    float normalized = static_cast<float>(raw_value) / MAX_AXIS_VALUE;
    float magnitude = std::abs(normalized);
    if (magnitude < deadzone) {
        return 0.0f;
    }
    float scaled = (magnitude - deadzone) / (1.0f - deadzone);
    return std::clamp(std::copysign(scaled, normalized), -1.0f, 1.0f);
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

void init_input() {
    open_primary_controller();
}

void shutdown_input() {
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
                ultramodern::quit();
            }
            break;
        default:
            break;
        }
    }

    if (s_controller != nullptr) {
        bool select_held = SDL_GameControllerGetButton(s_controller, SDL_CONTROLLER_BUTTON_BACK) != 0;
        bool start_held = SDL_GameControllerGetButton(s_controller, SDL_CONTROLLER_BUTTON_START) != 0;
        bool guide_held = SDL_GameControllerGetButton(s_controller, SDL_CONTROLLER_BUTTON_GUIDE) != 0;
        if ((select_held && start_held) || guide_held) {
            ultramodern::quit();
        }
    }
}

bool get_input(int controller_num, uint16_t* buttons, float* x, float* y) {
    if (controller_num != 0 || buttons == nullptr || x == nullptr || y == nullptr) {
        return false;
    }

    const auto& config = get_config().controls;
    uint16_t pad_buttons = 0;
    float stick_x = 0.0f;
    float stick_y = 0.0f;
    bool roll_requested = false;

    if (s_controller != nullptr) {
        int16_t raw_left_x = SDL_GameControllerGetAxis(s_controller, SDL_CONTROLLER_AXIS_LEFTX);
        int16_t raw_left_y = SDL_GameControllerGetAxis(s_controller, SDL_CONTROLLER_AXIS_LEFTY);
        int16_t raw_right_x = SDL_GameControllerGetAxis(s_controller, SDL_CONTROLLER_AXIS_RIGHTX);
        int16_t raw_right_y = SDL_GameControllerGetAxis(s_controller, SDL_CONTROLLER_AXIS_RIGHTY);
        int16_t raw_trigger_l = SDL_GameControllerGetAxis(s_controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
        int16_t raw_trigger_r = SDL_GameControllerGetAxis(s_controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);

        float aim_raw_x = static_cast<float>(config.swap_sticks ? raw_left_x : raw_right_x);
        float aim_raw_y = static_cast<float>(config.swap_sticks ? raw_left_y : raw_right_y);
        float move_raw_x = static_cast<float>(config.swap_sticks ? raw_right_x : raw_left_x);

        float sensitivity_factor = config.stick_sensitivity / 100.0f;
        stick_x = apply_axis_deadzone(static_cast<int16_t>(aim_raw_x), config.deadzone) * sensitivity_factor;
        stick_y = -apply_axis_deadzone(static_cast<int16_t>(aim_raw_y), config.deadzone) * sensitivity_factor;

        float move_x = apply_axis_deadzone(static_cast<int16_t>(move_raw_x), config.deadzone);
        if (move_x < -0.3f) {
            pad_buttons |= N64_C_LEFT;
        } else if (move_x > 0.3f) {
            pad_buttons |= N64_C_RIGHT;
        }

        if (SDL_GameControllerGetButton(s_controller, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) {
            if (config.scheme == CONTROL_SCHEME_DPAD_AIM) {
                stick_x = -1.0f;
            } else {
                pad_buttons |= N64_C_LEFT;
            }
        }
        if (SDL_GameControllerGetButton(s_controller, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) {
            if (config.scheme == CONTROL_SCHEME_DPAD_AIM) {
                stick_x = 1.0f;
            } else {
                pad_buttons |= N64_C_RIGHT;
            }
        }
        if (SDL_GameControllerGetButton(s_controller, SDL_CONTROLLER_BUTTON_DPAD_UP)) {
            if (config.scheme == CONTROL_SCHEME_DPAD_AIM) {
                stick_y = 1.0f;
            } else {
                pad_buttons |= N64_DPAD_UP;
            }
        }
        if (SDL_GameControllerGetButton(s_controller, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) {
            if (config.scheme == CONTROL_SCHEME_DPAD_AIM) {
                stick_y = -1.0f;
            } else {
                pad_buttons |= N64_DPAD_DOWN;
            }
        }

        if (SDL_GameControllerGetButton(s_controller, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) ||
            raw_trigger_r > AXIS_TRIGGER_THRESHOLD) {
            pad_buttons |= N64_Z;
        }
        if (SDL_GameControllerGetButton(s_controller, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) ||
            raw_trigger_l > AXIS_TRIGGER_THRESHOLD) {
            pad_buttons |= N64_R;
        }

        if (SDL_GameControllerGetButton(s_controller, SDL_CONTROLLER_BUTTON_A)) {
            pad_buttons |= N64_A;
        }
        if (SDL_GameControllerGetButton(s_controller, SDL_CONTROLLER_BUTTON_B)) {
            pad_buttons |= N64_B;
        }
        if (SDL_GameControllerGetButton(s_controller, SDL_CONTROLLER_BUTTON_X)) {
            if (config.scheme == CONTROL_SCHEME_DPAD_AIM) {
                pad_buttons |= N64_C_LEFT;
            } else {
                roll_requested = true;
            }
        }
        if (SDL_GameControllerGetButton(s_controller, SDL_CONTROLLER_BUTTON_Y)) {
            if (config.scheme == CONTROL_SCHEME_DPAD_AIM) {
                pad_buttons |= N64_C_RIGHT;
            } else {
                pad_buttons |= N64_L;
            }
        }
        if (SDL_GameControllerGetButton(s_controller, SDL_CONTROLLER_BUTTON_START)) {
            pad_buttons |= N64_START;
        }
    }

    const uint8_t* keyboard = SDL_GetKeyboardState(nullptr);
    if (keyboard != nullptr) {
        if (keyboard[SDL_SCANCODE_A] || keyboard[SDL_SCANCODE_LEFT]) {
            pad_buttons |= N64_C_LEFT;
        }
        if (keyboard[SDL_SCANCODE_D] || keyboard[SDL_SCANCODE_RIGHT]) {
            pad_buttons |= N64_C_RIGHT;
        }
        if (keyboard[SDL_SCANCODE_SPACE]) {
            pad_buttons |= N64_A;
        }
        if (keyboard[SDL_SCANCODE_Z] || keyboard[SDL_SCANCODE_J]) {
            pad_buttons |= N64_Z;
        }
        if (keyboard[SDL_SCANCODE_X] || keyboard[SDL_SCANCODE_K]) {
            pad_buttons |= N64_R;
        }
        if (keyboard[SDL_SCANCODE_C] || keyboard[SDL_SCANCODE_L]) {
            roll_requested = true;
        }
        if (keyboard[SDL_SCANCODE_RETURN]) {
            pad_buttons |= N64_START;
        }
        if (keyboard[SDL_SCANCODE_UP]) {
            stick_y = 1.0f;
        }
        if (keyboard[SDL_SCANCODE_DOWN]) {
            stick_y = -1.0f;
        }
    }

    update_roll_state(roll_requested, pad_buttons);

    *buttons = pad_buttons;
    *x = std::clamp(stick_x, -1.0f, 1.0f);
    *y = std::clamp(stick_y, -1.0f, 1.0f);
    return true;
}

void set_rumble(int controller_num, bool rumble) {
    if (controller_num != 0 || s_controller == nullptr) {
        return;
    }
    if (!get_config().controls.rumble) {
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
    (void)rdram;
    (void)reticle;
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
