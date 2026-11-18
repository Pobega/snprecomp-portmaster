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

struct RaAchievementEntry {
    uint32_t id = 0;
    std::string title;
    std::string description;
    std::string badge_name;
    std::string progress_text;
    uint32_t points = 0;
    bool unlocked = false;
    bool pending_offline_sync = false;
    std::vector<uint8_t> badge_rgba;
    int badge_w = 0;
    int badge_h = 0;
};

bool ra_get_current_toast(RaToastItem& out_toast);

void ra_get_game_summary(uint32_t& out_unlocked, uint32_t& out_total,
                         uint32_t& out_points_unlocked, uint32_t& out_points_available);
bool ra_load_cached_badge_rgba(const std::string& badge_name, std::vector<uint8_t>& out_rgba,
                               int& out_w, int& out_h);
std::vector<RaAchievementEntry> ra_get_achievements();

std::vector<uint8_t> ra_generate_trophy_badge(uint32_t width = 64, uint32_t height = 64);

} // namespace snp::handheld
