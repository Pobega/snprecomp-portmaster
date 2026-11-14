#pragma once

#include <filesystem>
#include <istream>
#include <string>

#include "handheld_config.h"

namespace snp::handheld {

struct RaCredentials {
    std::string username;
    std::string token;
    std::string password;

    bool has_login() const {
        return !username.empty() && (!token.empty() || !password.empty());
    }
};

RaCredentials parse_nextui_ra_settings(std::istream& is);

RaCredentials parse_retroarch_ra_cfg(std::istream& is);

RaCredentials discover_ra_credentials(
    const RetroAchievementsConfig& cfg,
    const std::filesystem::path& working_dir = std::filesystem::current_path());

} // namespace snp::handheld
