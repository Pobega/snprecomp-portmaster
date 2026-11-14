// Widescreen & tallscreen state shared with the game fixes.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "snp_config.h"

namespace {
    constexpr float kDefaultScale = 1.0f;
    constexpr float kHalfWidth = 160.0f;
    constexpr float kHalfHeight = 120.0f;
    constexpr float kReticleMargin = 16.0f;

    std::atomic<float> widescreen_scale{ kDefaultScale };
    std::atomic<float> tallscreen_scale{ kDefaultScale };

    snp::TallHudMode init_tall_hud_mode() {
        const char* env = std::getenv("SNP_TALL_HUD");
        if (env != nullptr && std::strcmp(env, "center") == 0) {
            return snp::TallHudMode::Center;
        }
        return snp::TallHudMode::Edge;
    }

    std::atomic<snp::TallHudMode> tall_hud_mode{ init_tall_hud_mode() };
}

void snp::set_widescreen_scale(float scale) {
    widescreen_scale.store(std::max(kDefaultScale, scale), std::memory_order_relaxed);
}

float snp::get_widescreen_scale() {
    return widescreen_scale.load(std::memory_order_relaxed);
}

void snp::set_tallscreen_scale(float scale) {
    tallscreen_scale.store(std::max(kDefaultScale, scale), std::memory_order_relaxed);
}

float snp::get_tallscreen_scale() {
    return tallscreen_scale.load(std::memory_order_relaxed);
}

void snp::set_tall_hud_mode(snp::TallHudMode mode) {
    tall_hud_mode.store(mode, std::memory_order_relaxed);
}

snp::TallHudMode snp::get_tall_hud_mode() {
    return tall_hud_mode.load(std::memory_order_relaxed);
}

// func_80029070's left/right visibility limit (snp.jp.toml): wider in Expand.
extern "C" float snp_cull_width_scale() {
    return snp::get_widescreen_scale();
}

// func_80029070's top/bottom visibility limit (snp.jp.toml): taller in Vert+ tallscreen.
extern "C" float snp_cull_height_scale() {
    return snp::get_tallscreen_scale();
}

// func_800AB924's left (side -1) and right (side 1) limits for the reticle's
// offset from the screen centre (snp.jp.toml), as float bits: 16 short of the
// screen's edge, 144 at 4:3.
extern "C" uint32_t snp_reticle_limit_bits(int32_t side) {
    float limit = static_cast<float>(side) * (kHalfWidth * snp::get_widescreen_scale() - kReticleMargin);
    uint32_t bits = 0;
    std::memcpy(&bits, &limit, sizeof(bits));
    return bits;
}

// func_800AB924's top (side 1) and bottom (side -1) limits for the reticle's
// vertical offset from the screen centre (snp.jp.toml), as float bits:
// 16 short of the screen's edge, 104 at 4:3, 144 at 1:1.
extern "C" uint32_t snp_reticle_vlimit_bits(int32_t side) {
    float limit = static_cast<float>(side) * (kHalfHeight * snp::get_tallscreen_scale() - kReticleMargin);
    uint32_t bits = 0;
    std::memcpy(&bits, &limit, sizeof(bits));
    return bits;
}
