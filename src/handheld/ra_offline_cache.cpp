#include "ra_offline_cache.h"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_set>
#include <vector>

#include "librecomp/game.hpp"
#include "rc_api_user.h"

namespace snp::handheld {

namespace {

int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::mutex s_tsv_mutex;

std::string to_lower_ascii(const std::string& in) {
    std::string out = in;
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string sanitize_user_key(const std::string& in) {
    return sanitize_filename(to_lower_ascii(in));
}

bool iequals_ascii(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::vector<PendingUnlock> load_pending_unlocks_unlocked(const std::filesystem::path& cache_dir) {
    std::filesystem::path tsv_path = cache_dir / "pending_unlocks.tsv";
    std::ifstream in(tsv_path);
    if (!in.is_open()) return {};

    std::vector<PendingUnlock> list;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::istringstream ss(line);
        std::string u, gid_str, ach_str, hash, epoch_str;
        if (std::getline(ss, u, '\t') &&
            std::getline(ss, gid_str, '\t') &&
            std::getline(ss, ach_str, '\t') &&
            std::getline(ss, hash, '\t') &&
            std::getline(ss, epoch_str, '\t')) {
            PendingUnlock p;
            p.username = u;
            p.game_hash = hash;
            try {
                p.game_id = static_cast<uint32_t>(std::stoul(gid_str));
                p.achievement_id = static_cast<uint32_t>(std::stoul(ach_str));
                p.unlock_epoch_sec = static_cast<uint64_t>(std::stoull(epoch_str));
            } catch (...) {
                continue;
            }
            list.push_back(std::move(p));
        }
    }
    return list;
}

bool write_atomic_cache_file(const std::filesystem::path& target_file,
                            const std::string& response_body,
                            bool owner_only_perms) {
    std::error_code ec;
    std::filesystem::path parent = target_file.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }

    std::filesystem::path tmp_file = target_file;
    tmp_file += ".tmp";

    std::ofstream out(tmp_file, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;

    if (owner_only_perms) {
        std::filesystem::permissions(tmp_file,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace, ec);
    }
    out.write(response_body.data(), static_cast<std::streamsize>(response_body.size()));
    out.close();

    std::filesystem::rename(tmp_file, target_file, ec);
    if (ec) {
        std::filesystem::remove(tmp_file, ec);
        return false;
    }
    return true;
}

} // namespace

std::string url_decode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '+') {
            out += ' ';
        } else if (in[i] == '%' && i + 2 < in.size()) {
            int h1 = hex_val(in[i + 1]);
            int h2 = hex_val(in[i + 2]);
            if (h1 >= 0 && h2 >= 0) {
                out += static_cast<char>((h1 << 4) | h2);
                i += 2;
            } else {
                out += in[i];
            }
        } else {
            out += in[i];
        }
    }
    return out;
}

std::unordered_map<std::string, std::string> parse_url_params(const std::string& query_or_post_data) {
    std::unordered_map<std::string, std::string> params;
    size_t start = 0;
    while (start < query_or_post_data.size()) {
        size_t end = query_or_post_data.find('&', start);
        if (end == std::string::npos) end = query_or_post_data.size();
        std::string token = query_or_post_data.substr(start, end - start);
        if (!token.empty()) {
            size_t eq = token.find('=');
            if (eq != std::string::npos) {
                std::string key = url_decode(token.substr(0, eq));
                std::string val = url_decode(token.substr(eq + 1));
                params[key] = val;
            } else {
                params[url_decode(token)] = "";
            }
        }
        start = end + 1;
    }
    return params;
}

std::string sanitize_filename(const std::string& in) {
    if (in.empty()) return "unknown";
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.') {
            out += c;
        } else {
            out += '_';
        }
    }
    return out.empty() ? "unknown" : out;
}

std::filesystem::path get_default_ra_cache_dir() {
    return recomp::get_config_path() / "ra_cache";
}

bool cache_online_response(const std::string& post_data, const std::string& response_body,
                           const std::filesystem::path& cache_dir) {
    if (response_body.find("\"Success\":true") == std::string::npos &&
        response_body.find("\"Success\": true") == std::string::npos &&
        response_body.find("\"Success\":1") == std::string::npos) {
        return false;
    }

    auto params = parse_url_params(post_data);
    std::string r = params["r"];
    if (r.empty()) return false;

    if (r == "login2") {
        std::string u = params["u"];
        if (u.empty()) return false;
        std::filesystem::path primary_file = cache_dir / ("login_" + sanitize_user_key(u) + ".json");
        bool primary_ok = write_atomic_cache_file(primary_file, response_body, true);

        rc_api_server_response_t server_resp{};
        server_resp.body = response_body.data();
        server_resp.body_length = response_body.size();
        server_resp.http_status_code = 200;

        rc_api_login_response_t login_resp{};
        int res = rc_api_process_login_server_response(&login_resp, &server_resp);
        if (res == RC_OK && login_resp.response.succeeded &&
            login_resp.username != nullptr && login_resp.username[0] != '\0') {
            std::string server_user_key = sanitize_user_key(login_resp.username);
            if (server_user_key != sanitize_user_key(u)) {
                std::filesystem::path server_file = cache_dir / ("login_" + server_user_key + ".json");
                write_atomic_cache_file(server_file, response_body, true);
            }
        }
        rc_api_destroy_login_response(&login_resp);
        return primary_ok;
    }

    std::filesystem::path target_file;
    if (r == "gameid") {
        std::string m = params["m"];
        if (m.empty()) return false;
        target_file = cache_dir / ("gameid_" + sanitize_filename(m) + ".json");
    } else if (r == "patch") {
        std::string g_or_m = !params["g"].empty() ? params["g"] : params["m"];
        if (g_or_m.empty()) return false;
        target_file = cache_dir / ("patch_" + sanitize_filename(g_or_m) + ".json");
    } else if (r == "achievementsets") {
        std::string g_or_m = !params["g"].empty() ? params["g"] : params["m"];
        if (g_or_m.empty()) return false;
        target_file = cache_dir / ("achievementsets_" + sanitize_filename(g_or_m) + ".json");
    } else if (r == "startsession") {
        std::string u = params["u"];
        std::string g = params["g"];
        if (u.empty() || g.empty()) return false;
        target_file = cache_dir / ("startsession_" + sanitize_user_key(u) + "_" + sanitize_filename(g) + ".json");
    } else if (r == "unlocks") {
        std::string u = params["u"];
        std::string g = params["g"];
        std::string h = !params["h"].empty() ? params["h"] : "0";
        if (u.empty() || g.empty()) return false;
        target_file = cache_dir / ("unlocks_" + sanitize_user_key(u) + "_" + sanitize_filename(g) + "_" + sanitize_filename(h) + ".json");
    } else {
        return false;
    }

    return write_atomic_cache_file(target_file, response_body, false);
}

bool try_serve_offline_response(const std::string& post_data, std::string& out_body, int& out_status,
                                const std::filesystem::path& cache_dir) {
    auto params = parse_url_params(post_data);
    std::string r = params["r"];
    if (r.empty()) return false;

    std::filesystem::path target_file;
    bool is_startsession = false;

    if (r == "login2") {
        std::string u = params["u"];
        target_file = cache_dir / ("login_" + sanitize_user_key(u) + ".json");
    } else if (r == "gameid") {
        std::string m = params["m"];
        target_file = cache_dir / ("gameid_" + sanitize_filename(m) + ".json");
    } else if (r == "patch") {
        std::string g_or_m = !params["g"].empty() ? params["g"] : params["m"];
        target_file = cache_dir / ("patch_" + sanitize_filename(g_or_m) + ".json");
    } else if (r == "achievementsets") {
        std::string g_or_m = !params["g"].empty() ? params["g"] : params["m"];
        target_file = cache_dir / ("achievementsets_" + sanitize_filename(g_or_m) + ".json");
    } else if (r == "startsession") {
        std::string u = params["u"];
        std::string g = params["g"];
        target_file = cache_dir / ("startsession_" + sanitize_user_key(u) + "_" + sanitize_filename(g) + ".json");
        is_startsession = true;
    } else if (r == "unlocks") {
        std::string u = params["u"];
        std::string g = params["g"];
        std::string h = !params["h"].empty() ? params["h"] : "0";
        target_file = cache_dir / ("unlocks_" + sanitize_user_key(u) + "_" + sanitize_filename(g) + "_" + sanitize_filename(h) + ".json");
    } else {
        return false;
    }

    std::error_code ec;
    if (!std::filesystem::exists(target_file, ec)) {
        return false;
    }

    std::ifstream in(target_file, std::ios::binary | std::ios::ate);
    if (!in.is_open()) return false;
    auto sz = in.tellg();
    if (sz <= 0) return false;
    std::string file_content(static_cast<size_t>(sz), '\0');
    in.seekg(0, std::ios::beg);
    in.read(&file_content[0], sz);
    in.close();

    if (is_startsession) {
        std::string u = params["u"];
        uint32_t gid = 0;
        try {
            gid = static_cast<uint32_t>(std::stoul(params["g"]));
        } catch (...) {}
        auto pending = load_pending_unlocks(cache_dir);
        std::vector<PendingUnlock> for_this_game;
        for (const auto& p : pending) {
            if ((p.username.empty() || iequals_ascii(p.username, u)) && (p.game_id == 0 || p.game_id == gid)) {
                for_this_game.push_back(p);
            }
        }
        out_body = merge_start_session_unlocks(file_content, for_this_game);
    } else {
        out_body = std::move(file_content);
    }

    out_status = 200;
    return true;
}

std::string merge_start_session_unlocks(const std::string& original_json,
                                        const std::vector<PendingUnlock>& pending_for_game) {
    rc_api_start_session_response_t response{};
    rc_api_server_response_t server_resp{};
    server_resp.body = original_json.c_str();
    server_resp.body_length = original_json.size();

    int parse_res = rc_api_process_start_session_server_response(&response, &server_resp);

    struct UnlockItem {
        uint32_t id = 0;
        time_t when = 0;
    };

    std::vector<UnlockItem> unlocks;
    std::unordered_set<uint32_t> seen;
    std::vector<UnlockItem> hardcore_unlocks;
    time_t server_now = std::time(nullptr);

    if (parse_res == RC_OK && response.response.succeeded) {
        if (response.server_now > 0) {
            server_now = response.server_now;
        }
        for (uint32_t i = 0; i < response.num_unlocks; ++i) {
            unlocks.push_back({ response.unlocks[i].achievement_id, response.unlocks[i].when });
            seen.insert(response.unlocks[i].achievement_id);
        }
        for (uint32_t i = 0; i < response.num_hardcore_unlocks; ++i) {
            hardcore_unlocks.push_back({ response.hardcore_unlocks[i].achievement_id, response.hardcore_unlocks[i].when });
        }
    }

    for (const auto& p : pending_for_game) {
        if (p.achievement_id > 0 && seen.find(p.achievement_id) == seen.end()) {
            time_t when_val = (p.unlock_epoch_sec > 0) ? static_cast<time_t>(p.unlock_epoch_sec) : server_now;
            unlocks.push_back({ p.achievement_id, when_val });
            seen.insert(p.achievement_id);
        }
    }

    rc_api_destroy_start_session_response(&response);

    std::ostringstream ss;
    ss << "{\"Success\":true,\"Unlocks\":[";
    for (size_t i = 0; i < unlocks.size(); ++i) {
        if (i > 0) ss << ",";
        ss << "{\"ID\":" << unlocks[i].id << ",\"When\":" << unlocks[i].when << "}";
    }
    ss << "],\"HardcoreUnlocks\":[";
    for (size_t i = 0; i < hardcore_unlocks.size(); ++i) {
        if (i > 0) ss << ",";
        ss << "{\"ID\":" << hardcore_unlocks[i].id << ",\"When\":" << hardcore_unlocks[i].when << "}";
    }
    ss << "],\"ServerNow\":" << server_now << "}";

    return ss.str();
}

bool queue_pending_unlock(const PendingUnlock& unlock, const std::filesystem::path& cache_dir) {
    if (unlock.achievement_id == 0) return false;
    std::lock_guard<std::mutex> lock(s_tsv_mutex);

    auto existing = load_pending_unlocks_unlocked(cache_dir);
    for (const auto& e : existing) {
        if (iequals_ascii(e.username, unlock.username) && e.game_id == unlock.game_id && e.achievement_id == unlock.achievement_id) {
            return true;
        }
    }

    std::error_code ec;
    std::filesystem::create_directories(cache_dir, ec);

    std::filesystem::path tsv_path = cache_dir / "pending_unlocks.tsv";
    std::ofstream out(tsv_path, std::ios::app);
    if (!out.is_open()) return false;

    out << unlock.username << '\t'
        << unlock.game_id << '\t'
        << unlock.achievement_id << '\t'
        << unlock.game_hash << '\t'
        << unlock.unlock_epoch_sec << '\n';

    return true;
}

std::vector<PendingUnlock> load_pending_unlocks(const std::filesystem::path& cache_dir) {
    std::lock_guard<std::mutex> lock(s_tsv_mutex);
    return load_pending_unlocks_unlocked(cache_dir);
}

bool remove_pending_unlocks(const std::vector<uint32_t>& synced_achievement_ids,
                            const std::string& username, uint32_t game_id,
                            const std::filesystem::path& cache_dir) {
    if (synced_achievement_ids.empty()) return true;
    std::lock_guard<std::mutex> lock(s_tsv_mutex);

    auto existing = load_pending_unlocks_unlocked(cache_dir);
    std::unordered_set<uint32_t> to_remove(synced_achievement_ids.begin(), synced_achievement_ids.end());

    std::vector<PendingUnlock> remaining;
    for (const auto& e : existing) {
        bool match = (username.empty() || iequals_ascii(e.username, username)) &&
                     (game_id == 0 || e.game_id == game_id) &&
                     (to_remove.find(e.achievement_id) != to_remove.end());
        if (!match) {
            remaining.push_back(e);
        }
    }

    std::filesystem::path tsv_path = cache_dir / "pending_unlocks.tsv";
    if (remaining.empty()) {
        std::error_code ec;
        std::filesystem::remove(tsv_path, ec);
        return true;
    }

    std::ofstream out(tsv_path, std::ios::trunc);
    if (!out.is_open()) return false;

    for (const auto& r : remaining) {
        out << r.username << '\t'
            << r.game_id << '\t'
            << r.achievement_id << '\t'
            << r.game_hash << '\t'
            << r.unlock_epoch_sec << '\n';
    }
    return true;
}

size_t get_pending_unlock_count(const std::filesystem::path& cache_dir) {
    return load_pending_unlocks(cache_dir).size();
}

bool record_synced_unlocks_to_startsession(const std::string& username, uint32_t game_id,
                                           const std::vector<uint32_t>& synced_achievement_ids,
                                           const std::filesystem::path& cache_dir) {
    if (username.empty() || game_id == 0 || synced_achievement_ids.empty()) {
        return false;
    }
    std::filesystem::path target_file = cache_dir /
        ("startsession_" + sanitize_user_key(username) + "_" + std::to_string(game_id) + ".json");

    std::error_code ec;
    if (!std::filesystem::exists(target_file, ec)) {
        return false;
    }

    std::ifstream in(target_file, std::ios::binary | std::ios::ate);
    if (!in.is_open()) return false;
    auto sz = in.tellg();
    if (sz <= 0) return false;
    std::string content(static_cast<size_t>(sz), '\0');
    in.seekg(0, std::ios::beg);
    in.read(&content[0], sz);
    in.close();

    std::vector<PendingUnlock> dummy_pending;
    uint64_t now = static_cast<uint64_t>(std::time(nullptr));
    for (uint32_t id : synced_achievement_ids) {
        PendingUnlock p;
        p.username = username;
        p.game_id = game_id;
        p.achievement_id = id;
        p.unlock_epoch_sec = now;
        dummy_pending.push_back(p);
    }

    std::string merged = merge_start_session_unlocks(content, dummy_pending);
    return write_atomic_cache_file(target_file, merged, false);
}

} // namespace snp::handheld
