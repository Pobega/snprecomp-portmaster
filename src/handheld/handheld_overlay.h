#pragma once

#include <cstdint>

namespace snp::handheld {

void init_overlay();
void shutdown_overlay();

bool is_overlay_open();
void open_overlay();
void close_overlay();
void toggle_overlay();

void update_overlay_input(bool up, bool down, bool left, bool right,
                          bool btn_a, bool btn_b, bool btn_start);

void render_overlay(int win_w, int win_h);

} // namespace snp::handheld
