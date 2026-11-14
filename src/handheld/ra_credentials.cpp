#include "ra_credentials.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <vector>

namespace snp::handheld {

namespace {

constexpr int MAX_PARENT_DIRECTORY_HOPS = 6;

std::string trim_whitespace(const std::string& input) {
    auto start = std::find_if_not(input.begin(), input.end(), [](unsigned char ch) {
        return std::isspace(ch);
    });
    auto end = std::find_if_not(input.rbegin(), input.rend(), [](unsigned char ch) {
        return std::isspace(ch);
    }).base();
    return (start < end) ? std::string(start, end) : std::string{};
}

std::string strip_enclosing_quotes(const std::string& input) {
    std::string trimmed = trim_whitespace(input);
    if (trimmed.size() >= 2) {
        char first = trimmed.front();
        char last = trimmed.back();
        if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
            return trimmed.substr(1, trimmed.size() - 2);
        }
    }
    return trimmed;
}

std::vector<std::filesystem::path> get_nextui_search_paths(const std::filesystem::path& working_dir) {
    std::vector<std::filesystem::path> paths;

    const char* env_sd = std::getenv("SDCARD_PATH");
    if (env_sd != nullptr && env_sd[0] != '\0') {
        paths.emplace_back(std::filesystem::path(env_sd) / ".userdata" / "shared" / "minuisettings.txt");
    }

    std::filesystem::path current = std::filesystem::absolute(working_dir);
    for (int hop = 0; hop <= MAX_PARENT_DIRECTORY_HOPS; ++hop) {
        paths.emplace_back(current / ".userdata" / "shared" / "minuisettings.txt");
        if (!current.has_parent_path() || current == current.parent_path()) {
            break;
        }
        current = current.parent_path();
    }

    paths.emplace_back("/mnt/SDCARD/.userdata/shared/minuisettings.txt");
    paths.emplace_back("/mnt/sdcard/.userdata/shared/minuisettings.txt");
    paths.emplace_back("/mnt/mmc/.userdata/shared/minuisettings.txt");

    return paths;
}

std::vector<std::filesystem::path> get_retroarch_search_paths() {
    std::vector<std::filesystem::path> paths;

    const char* env_xdg = std::getenv("XDG_CONFIG_HOME");
    if (env_xdg != nullptr && env_xdg[0] != '\0') {
        paths.emplace_back(std::filesystem::path(env_xdg) / "retroarch" / "retroarch.cfg");
    }

    const char* env_home = std::getenv("HOME");
    if (env_home != nullptr && env_home[0] != '\0') {
        paths.emplace_back(std::filesystem::path(env_home) / ".config" / "retroarch" / "retroarch.cfg");
    }

    paths.emplace_back("/mnt/mmc/MUOS/retroarch/retroarch.cfg");
    paths.emplace_back("/opt/muos/share/info/config/retroarch.cfg");
    paths.emplace_back("/userdata/system/configs/retroarch/retroarch.cfg");
    paths.emplace_back("/storage/.config/retroarch/retroarch.cfg");
    paths.emplace_back("/home/ark/.config/retroarch/retroarch.cfg");

    return paths;
}

} // namespace

RaCredentials parse_nextui_ra_settings(std::istream& is) {
    std::string raw_username;
    std::string raw_server_username;
    std::string raw_token;
    std::string raw_password;

    std::string line;
    while (std::getline(is, line)) {
        auto eq_pos = line.find('=');
        if (eq_pos == std::string::npos) {
            continue;
        }
        std::string key = trim_whitespace(line.substr(0, eq_pos));
        std::string val = trim_whitespace(line.substr(eq_pos + 1));

        if (key == "raUsername") {
            raw_username = val;
        } else if (key == "raServerUsername") {
            raw_server_username = val;
        } else if (key == "raToken") {
            raw_token = val;
        } else if (key == "raPassword") {
            raw_password = val;
        }
    }

    RaCredentials result;
    if (!raw_token.empty()) {
        result.username = !raw_server_username.empty() ? raw_server_username : raw_username;
        result.token = raw_token;
        result.password = raw_password;
    } else if (!raw_password.empty()) {
        result.username = raw_username;
        result.token = "";
        result.password = raw_password;
    }
    return result;
}

RaCredentials parse_retroarch_ra_cfg(std::istream& is) {
    std::string username;
    std::string token;
    std::string password;

    std::string line;
    while (std::getline(is, line)) {
        std::string trimmed = trim_whitespace(line);
        if (trimmed.empty() || trimmed.front() == '#') {
            continue;
        }

        auto eq_pos = trimmed.find('=');
        if (eq_pos == std::string::npos) {
            continue;
        }

        std::string key = trim_whitespace(trimmed.substr(0, eq_pos));
        std::string val = strip_enclosing_quotes(trimmed.substr(eq_pos + 1));

        if (key == "cheevos_username") {
            username = val;
        } else if (key == "cheevos_token") {
            token = val;
        } else if (key == "cheevos_password") {
            password = val;
        }
    }

    if (!username.empty() && (!token.empty() || !password.empty())) {
        return RaCredentials{username, token, password};
    }
    return {};
}

RaCredentials discover_ra_credentials(const RetroAchievementsConfig& cfg, const std::filesystem::path& working_dir) {
    if (!cfg.username.empty() && (!cfg.token.empty() || !cfg.password.empty())) {
        return RaCredentials{cfg.username, cfg.token, cfg.password};
    }

    if (!cfg.import_system_credentials) {
        return {};
    }

    for (const auto& path : get_nextui_search_paths(working_dir)) {
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) {
            std::ifstream file(path);
            if (file.is_open()) {
                RaCredentials creds = parse_nextui_ra_settings(file);
                if (creds.has_login()) {
                    return creds;
                }
            }
        }
    }

    for (const auto& path : get_retroarch_search_paths()) {
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) {
            std::ifstream file(path);
            if (file.is_open()) {
                RaCredentials creds = parse_retroarch_ra_cfg(file);
                if (creds.has_login()) {
                    return creds;
                }
            }
        }
    }

    return {};
}

} // namespace snp::handheld
