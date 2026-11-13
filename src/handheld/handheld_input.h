#pragma once

#include <cstdint>
#include "ultramodern/input.hpp"

namespace snp::handheld {

void init_input();
void shutdown_input();
void poll_input();
bool get_input(int controller_num, uint16_t* buttons, float* x, float* y);
void set_rumble(int controller_num, bool rumble);
ultramodern::input::connected_device_info_t get_connected_device_info(int controller_num);

} // namespace snp::handheld
