#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace snp::handheld {

uint32_t ra_read_n64_memory(const uint8_t* rdram, uint32_t address, uint8_t* buffer, uint32_t num_bytes);

void ra_init();
void ra_shutdown();
void ra_do_frame(uint8_t* rdram);

void ra_set_enabled(bool enabled);
bool ra_is_enabled();

std::string ra_get_status_text();

void ra_sync_now();
size_t ra_get_pending_sync_count();
bool ra_is_syncing();
bool ra_is_offline_mode();

bool ra_has_active_toasts();

struct RaToastItem {
    std::string title;
    std::string subtitle;
    std::string detail;
    std::vector<uint8_t> rgba_pixels;
    float duration_sec = 4.5f;
    float elapsed_sec = 0.0f;
    int accent_kind = 0;
};

bool ra_get_current_toast(RaToastItem& out_toast);

std::vector<uint8_t> ra_generate_trophy_badge(uint32_t width = 64, uint32_t height = 64);

} // namespace snp::handheld
