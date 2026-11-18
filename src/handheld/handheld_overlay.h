#pragma once

#include <cstdint>
#include "handheld_config.h"

namespace snp::handheld {

void init_overlay();
void shutdown_overlay();

bool is_overlay_open();
void open_overlay();
void close_overlay();
void toggle_overlay();

bool is_overlay_listening();
void cancel_overlay_listening();
void deliver_overlay_listen_button(PhysicalButton btn);

void update_overlay_input(bool up, bool down, bool left, bool right,
                          bool btn_a, bool btn_b, bool btn_x, bool btn_y,
                          bool btn_start, bool btn_l1 = false, bool btn_r1 = false);

void render_overlay(int win_w, int win_h);

} // namespace snp::handheld
