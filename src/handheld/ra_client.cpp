#include "ra_client.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <dlfcn.h>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "rc_client.h"
#include "rc_consoles.h"
#include "rc_api_user.h"
#include "rc_api_runtime.h"
#include "handheld_config.h"
#include "png_decode.h"
#include "ra_credentials.h"
#include "ra_offline_cache.h"
#include "rom_loader.h"
#include "librecomp/game.hpp"

namespace snp::handheld {

namespace {

constexpr uint32_t N64_RDRAM_MAX_SIZE = 8 * 1024 * 1024; // 8 MB
constexpr const char* RA_USER_AGENT = "SnPRecompiled/0.2.0 (Linux; PortMaster)";
constexpr const char* RA_BADGE_BASE_URL = "https://media.retroachievements.org/Badge/";
constexpr float DEFAULT_TOAST_DURATION = 4.5f;

enum class RaClientState {
    Disabled,
    NoCredentials,
    LoggingIn,
    Offline,
    LoggedIn,
    GameLoading,
    GameLoaded,
};

std::atomic<RaClientState> s_state{RaClientState::NoCredentials};
std::atomic<bool> s_is_offline_mode{false};
std::atomic<bool> s_is_syncing{false};
std::atomic<uint64_t> s_last_real_ping_time{0};

std::string s_current_username;
std::string s_current_token;
std::string s_discovered_password;
std::string s_game_title;
uint32_t s_current_game_id = 0;
std::string s_current_game_hash;
uint32_t s_unlocked_achievements = 0;
uint32_t s_total_achievements = 0;
bool s_offline_toast_shown = false;

rc_client_t* s_client = nullptr;
uint8_t* s_current_rdram = nullptr;

struct HttpRequestTask {
    std::string url;
    std::string post_data;
    std::string content_type;
    rc_client_server_callback_t callback = nullptr;
    void* callback_data = nullptr;
    bool is_badge_download = false;
    bool is_sync_task = false;
    std::string badge_name;
    std::filesystem::path badge_save_path;
};

struct HttpResponseItem {
    rc_client_server_callback_t callback = nullptr;
    void* callback_data = nullptr;
    std::string body;
    int http_status_code = -1;
    bool is_badge_download = false;
    std::string badge_name;
    std::filesystem::path badge_save_path;
};

std::mutex s_worker_mutex;
std::condition_variable s_worker_cv;
std::deque<HttpRequestTask> s_request_queue;
std::atomic<bool> s_workers_running{false};
std::thread s_worker_threads[2];

std::mutex s_response_mutex;
std::vector<HttpResponseItem> s_completed_responses;

std::mutex s_toast_mutex;
std::deque<RaToastItem> s_toast_queue;
std::chrono::steady_clock::time_point s_last_frame_time;

// libcurl dynamic function pointers
using curl_easy_init_fn = void* (*)();
using curl_easy_setopt_fn = int (*)(void*, int, ...);
using curl_easy_perform_fn = int (*)(void*);
using curl_easy_getinfo_fn = int (*)(void*, int, ...);
using curl_easy_cleanup_fn = void (*)(void*);
using curl_slist_append_fn = void* (*)(void*, const char*);
using curl_slist_free_all_fn = void (*)(void*);

void* s_libcurl_handle = nullptr;
curl_easy_init_fn s_curl_easy_init = nullptr;
curl_easy_setopt_fn s_curl_easy_setopt = nullptr;
curl_easy_perform_fn s_curl_easy_perform = nullptr;
curl_easy_getinfo_fn s_curl_easy_getinfo = nullptr;
curl_easy_cleanup_fn s_curl_easy_cleanup = nullptr;
curl_slist_append_fn s_curl_slist_append = nullptr;
curl_slist_free_all_fn s_curl_slist_free_all = nullptr;

constexpr int SNP_CURLOPT_URL = 10002;
constexpr int SNP_CURLOPT_POSTFIELDS = 10015;
constexpr int SNP_CURLOPT_USERAGENT = 10018;
constexpr int SNP_CURLOPT_HTTPHEADER = 10023;
constexpr int SNP_CURLOPT_TIMEOUT = 13;
constexpr int SNP_CURLOPT_FOLLOWLOCATION = 52;
constexpr int SNP_CURLOPT_WRITEFUNCTION = 20011;
constexpr int SNP_CURLOPT_WRITEDATA = 10001;
constexpr int SNP_CURLINFO_RESPONSE_CODE = 0x200000 + 2;

size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    size_t total = size * nmemb;
    auto* out = reinterpret_cast<std::string*>(userdata);
    if (out != nullptr) {
        out->append(ptr, total);
    }
    return total;
}

void init_libcurl() {
    if (s_libcurl_handle != nullptr) {
        return;
    }
    s_libcurl_handle = dlopen("libcurl.so.4", RTLD_LAZY);
    if (s_libcurl_handle == nullptr) {
        s_libcurl_handle = dlopen("libcurl.so", RTLD_LAZY);
    }
    if (s_libcurl_handle != nullptr) {
        s_curl_easy_init = reinterpret_cast<curl_easy_init_fn>(dlsym(s_libcurl_handle, "curl_easy_init"));
        s_curl_easy_setopt = reinterpret_cast<curl_easy_setopt_fn>(dlsym(s_libcurl_handle, "curl_easy_setopt"));
        s_curl_easy_perform = reinterpret_cast<curl_easy_perform_fn>(dlsym(s_libcurl_handle, "curl_easy_perform"));
        s_curl_easy_getinfo = reinterpret_cast<curl_easy_getinfo_fn>(dlsym(s_libcurl_handle, "curl_easy_getinfo"));
        s_curl_easy_cleanup = reinterpret_cast<curl_easy_cleanup_fn>(dlsym(s_libcurl_handle, "curl_easy_cleanup"));
        s_curl_slist_append = reinterpret_cast<curl_slist_append_fn>(dlsym(s_libcurl_handle, "curl_slist_append"));
        s_curl_slist_free_all = reinterpret_cast<curl_slist_free_all_fn>(dlsym(s_libcurl_handle, "curl_slist_free_all"));
    }
}

bool execute_curl_dlopen(const HttpRequestTask& task, HttpResponseItem& item) {
    if (s_curl_easy_init == nullptr || s_curl_easy_setopt == nullptr ||
        s_curl_easy_perform == nullptr || s_curl_easy_getinfo == nullptr ||
        s_curl_easy_cleanup == nullptr) {
        return false;
    }

    void* curl = s_curl_easy_init();
    if (curl == nullptr) {
        return false;
    }

    void* headers = nullptr;
    if (!task.content_type.empty() && s_curl_slist_append != nullptr) {
        std::string header_line = "Content-Type: " + task.content_type;
        headers = s_curl_slist_append(headers, header_line.c_str());
        s_curl_easy_setopt(curl, SNP_CURLOPT_HTTPHEADER, headers);
    }

    s_curl_easy_setopt(curl, SNP_CURLOPT_URL, task.url.c_str());
    s_curl_easy_setopt(curl, SNP_CURLOPT_USERAGENT, RA_USER_AGENT);
    s_curl_easy_setopt(curl, SNP_CURLOPT_TIMEOUT, 15L);
    s_curl_easy_setopt(curl, SNP_CURLOPT_FOLLOWLOCATION, 1L);
    s_curl_easy_setopt(curl, SNP_CURLOPT_WRITEFUNCTION, curl_write_cb);
    s_curl_easy_setopt(curl, SNP_CURLOPT_WRITEDATA, &item.body);

    if (!task.post_data.empty()) {
        s_curl_easy_setopt(curl, SNP_CURLOPT_POSTFIELDS, task.post_data.c_str());
    }

    int res = s_curl_easy_perform(curl);
    long http_code = 0;
    if (res == 0) {
        s_curl_easy_getinfo(curl, SNP_CURLINFO_RESPONSE_CODE, &http_code);
        item.http_status_code = static_cast<int>(http_code);
    } else {
        item.http_status_code = -1;
    }

    if (headers != nullptr && s_curl_slist_free_all != nullptr) {
        s_curl_slist_free_all(headers);
    }
    s_curl_easy_cleanup(curl);
    return true;
}

bool execute_curl_fork(const HttpRequestTask& task, HttpResponseItem& item) {
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        item.http_status_code = -1;
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        item.http_status_code = -1;
        return false;
    }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);

        std::vector<std::string> args;
        args.push_back("curl");
        args.push_back("-sS");
        args.push_back("-L");
        args.push_back("--max-time");
        args.push_back("15");
        args.push_back("-A");
        args.push_back(RA_USER_AGENT);
        args.push_back("-w");
        args.push_back("\n%{http_code}");

        if (!task.content_type.empty()) {
            args.push_back("-H");
            args.push_back("Content-Type: " + task.content_type);
        }

        if (!task.post_data.empty()) {
            args.push_back("-d");
            args.push_back(task.post_data);
        }

        args.push_back(task.url);

        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (auto& a : args) {
            argv.push_back(a.data());
        }
        argv.push_back(nullptr);

        execvp("curl", argv.data());
        _exit(127);
    }

    close(pipefd[1]);

    std::string output;
    char buffer[4096];
    ssize_t bytes_read = 0;
    while ((bytes_read = read(pipefd[0], buffer, sizeof(buffer))) > 0) {
        output.append(buffer, static_cast<size_t>(bytes_read));
    }
    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0 && !output.empty()) {
        size_t last_nl = output.rfind('\n');
        if (last_nl != std::string::npos) {
            std::string code_str = output.substr(last_nl + 1);
            try {
                item.http_status_code = std::stoi(code_str);
            } catch (...) {
                item.http_status_code = -1;
            }
            item.body = output.substr(0, last_nl);
        } else {
            item.http_status_code = 200;
            item.body = output;
        }
    } else {
        item.http_status_code = -1;
    }
    return true;
}

void push_toast(const std::string& title, const std::string& subtitle, const std::string& detail,
                std::vector<uint8_t> rgba_pixels = {}, float duration = DEFAULT_TOAST_DURATION, int accent = 0);

void load_or_fetch_badge(const std::string& badge_name, const std::string& badge_url);

std::vector<uint8_t> get_cached_badge_or_trophy(const std::string& badge_name);

void trigger_game_load(rc_client_t* client);

void trigger_login(rc_client_t* client);

void perform_sync_job() {
    s_is_syncing.store(true);

    auto pending = load_pending_unlocks();
    std::string token = get_config().retroachievements.token;
    if (token.empty()) {
        token = s_current_token;
    }
    if (token.empty()) {
        std::string user = !s_current_username.empty() ? s_current_username : get_config().retroachievements.username;
        std::string login_body;
        int login_status = 0;
        if (!user.empty() && try_serve_offline_response("r=login2&u=" + user, login_body, login_status)) {
            rc_api_login_response_t login_resp{};
            rc_api_server_response_t server_resp{};
            server_resp.body = login_body.c_str();
            server_resp.body_length = login_body.size();
            server_resp.http_status_code = login_status;
            if (rc_api_process_login_server_response(&login_resp, &server_resp) == RC_OK &&
                login_resp.response.succeeded && login_resp.api_token) {
                token = login_resp.api_token;
            }
            rc_api_destroy_login_response(&login_resp);
        }
    }

    if (!pending.empty()) {
        if (token.empty()) {
            push_toast("RetroAchievements: Not Signed In", "Sync skipped",
                       "Sign in online once to sync unlocks", {}, 3.5f, 3);
            s_is_syncing.store(false);
            return;
        }
        std::vector<PendingUnlock> synced_entries;
        uint64_t now_sec = static_cast<uint64_t>(std::time(nullptr));

        for (const auto& entry : pending) {
            uint32_t seconds_since_unlock = 0;
            if (now_sec >= 1700000000ULL && entry.unlock_epoch_sec >= 1700000000ULL && now_sec >= entry.unlock_epoch_sec) {
                seconds_since_unlock = static_cast<uint32_t>(now_sec - entry.unlock_epoch_sec);
            }

            rc_api_award_achievement_request_t req{};
            req.username = entry.username.c_str();
            req.api_token = token.c_str();
            req.achievement_id = entry.achievement_id;
            req.hardcore = 0;
            req.game_hash = entry.game_hash.c_str();
            req.seconds_since_unlock = seconds_since_unlock;

            rc_api_request_t api_req{};
            if (rc_api_init_award_achievement_request(&api_req, &req) == RC_OK) {
                HttpRequestTask task;
                if (api_req.url) task.url = api_req.url;
                if (api_req.post_data) task.post_data = api_req.post_data;
                if (api_req.content_type) task.content_type = api_req.content_type;

                HttpResponseItem item;
                if (!execute_curl_dlopen(task, item)) {
                    execute_curl_fork(task, item);
                }

                if (item.http_status_code == 200) {
                    rc_api_server_response_t server_resp{};
                    server_resp.body = item.body.c_str();
                    server_resp.body_length = item.body.size();
                    server_resp.http_status_code = item.http_status_code;
                    rc_api_award_achievement_response_t award_resp{};
                    if (rc_api_process_award_achievement_server_response(&award_resp, &server_resp) == RC_OK &&
                        award_resp.response.succeeded) {
                        synced_entries.push_back(entry);
                    }
                    rc_api_destroy_award_achievement_response(&award_resp);
                }
                rc_api_destroy_request(&api_req);
            }
        }

        if (!synced_entries.empty()) {
            for (const auto& synced : synced_entries) {
                std::vector<uint32_t> one_id = { synced.achievement_id };
                remove_pending_unlocks(one_id, synced.username, synced.game_id);
                record_synced_unlocks_to_startsession(synced.username, synced.game_id, one_id);
            }
            s_is_offline_mode.store(false);
            push_toast("RetroAchievements Synced",
                       "Uploaded " + std::to_string(synced_entries.size()) + " offline unlock(s)",
                       s_game_title, {}, 4.0f, 0);
        } else {
            push_toast("RetroAchievements: Still Offline", "Sync failed",
                       "Could not reach server", {}, 3.5f, 3);
        }
    } else {
        if (s_is_offline_mode.load()) {
            std::string user = !s_current_username.empty() ? s_current_username : get_config().retroachievements.username;
            rc_api_login_request_t req{};
            req.username = user.c_str();
            req.api_token = token.c_str();

            rc_api_request_t api_req{};
            if (rc_api_init_login_request(&api_req, &req) == RC_OK) {
                HttpRequestTask task;
                if (api_req.url) task.url = api_req.url;
                if (api_req.post_data) task.post_data = api_req.post_data;
                if (api_req.content_type) task.content_type = api_req.content_type;

                HttpResponseItem item;
                if (!execute_curl_dlopen(task, item)) {
                    execute_curl_fork(task, item);
                }

                if (item.http_status_code == 200 &&
                    item.body.find("\"Success\":true") != std::string::npos) {
                    s_is_offline_mode.store(false);
                    push_toast("RetroAchievements Online", "Reconnected",
                               "Up to date", {}, 3.5f, 1);
                } else {
                    push_toast("RetroAchievements: Still Offline", "Network probe failed",
                               "Could not reach server", {}, 3.5f, 3);
                }
                rc_api_destroy_request(&api_req);
            }
        } else {
            push_toast("RetroAchievements Synced", "Up to date",
                       "All achievements synchronized", {}, 3.0f, 1);
        }
    }

    s_is_syncing.store(false);
}

void http_worker_thread_func() {
    while (s_workers_running.load()) {
        HttpRequestTask task;
        {
            std::unique_lock<std::mutex> lock(s_worker_mutex);
            s_worker_cv.wait(lock, [] {
                return !s_workers_running.load() || !s_request_queue.empty();
            });

            if (!s_workers_running.load() && s_request_queue.empty()) {
                break;
            }

            task = std::move(s_request_queue.front());
            s_request_queue.pop_front();
        }

        if (task.is_sync_task) {
            perform_sync_job();
            continue;
        }

        HttpResponseItem item;
        item.callback = task.callback;
        item.callback_data = task.callback_data;
        item.is_badge_download = task.is_badge_download;
        item.badge_name = std::move(task.badge_name);
        item.badge_save_path = std::move(task.badge_save_path);

        if (task.is_badge_download) {
            if (!execute_curl_dlopen(task, item)) {
                execute_curl_fork(task, item);
            }

            if (item.http_status_code == 200 && !item.body.empty() && !item.badge_save_path.empty()) {
                std::error_code ec;
                std::filesystem::create_directories(item.badge_save_path.parent_path(), ec);
                std::ofstream out(item.badge_save_path, std::ios::binary);
                if (out.is_open()) {
                    out.write(item.body.data(), item.body.size());
                }
            }
        } else {
            auto params = parse_url_params(task.post_data);
            std::string r = params["r"];

            if (r == "ping") {
                uint64_t now_sec = static_cast<uint64_t>(std::time(nullptr));
                uint64_t last_ping = s_last_real_ping_time.load();
                bool allow_real = (now_sec >= last_ping + 60) || (!s_is_offline_mode.load());
                if (allow_real) {
                    s_last_real_ping_time.store(now_sec);
                    if (!execute_curl_dlopen(task, item)) {
                        execute_curl_fork(task, item);
                    }
                    if (item.http_status_code == 200 && item.body.find("\"Success\":true") != std::string::npos) {
                        if (s_is_offline_mode.load()) {
                            s_is_offline_mode.store(false);
                            ra_sync_now();
                        }
                    } else if (item.http_status_code <= 0) {
                        item.http_status_code = 200;
                        item.body = "{\"Success\":true}";
                    }
                } else {
                    item.http_status_code = 200;
                    item.body = "{\"Success\":true}";
                }
            } else if (r == "awardachievement") {
                if (!execute_curl_dlopen(task, item)) {
                    execute_curl_fork(task, item);
                }
                if (item.http_status_code == 200 && item.body.find("\"Success\":true") != std::string::npos) {
                    if (s_is_offline_mode.load()) {
                        s_is_offline_mode.store(false);
                        if (get_pending_unlock_count() > 0) {
                            ra_sync_now();
                        }
                    }
                } else if (item.http_status_code <= 0) {
                    s_is_offline_mode.store(true);
                    PendingUnlock p;
                    p.username = !params["u"].empty() ? params["u"] : s_current_username;
                    p.game_id = s_current_game_id;
                    try {
                        p.achievement_id = static_cast<uint32_t>(std::stoul(params["a"]));
                    } catch (...) {}
                    p.game_hash = !params["m"].empty() ? params["m"] : s_current_game_hash;
                    uint64_t now_sec = static_cast<uint64_t>(std::time(nullptr));
                    p.unlock_epoch_sec = (now_sec >= 1700000000ULL) ? now_sec : 0;
                    queue_pending_unlock(p);

                    item.http_status_code = 200;
                    item.body = "{\"Success\":true,\"AchievementID\":" + std::to_string(p.achievement_id) + ",\"AchievementsRemaining\":0}";
                }
            } else {
                bool served_from_cache = false;
                if (s_is_offline_mode.load()) {
                    std::string cache_body;
                    int cache_status = 0;
                    if (try_serve_offline_response(task.post_data, cache_body, cache_status)) {
                        item.body = std::move(cache_body);
                        item.http_status_code = cache_status;
                        served_from_cache = true;
                    }
                }

                if (!served_from_cache) {
                    if (!execute_curl_dlopen(task, item)) {
                        execute_curl_fork(task, item);
                    }
                    if (item.http_status_code == 200 &&
                        (item.body.find("\"Success\":true") != std::string::npos ||
                         item.body.find("\"Success\": true") != std::string::npos)) {
                        cache_online_response(task.post_data, item.body);
                    } else if (item.http_status_code <= 0) {
                        s_is_offline_mode.store(true);
                        std::string cache_body;
                        int cache_status = 0;
                        if (try_serve_offline_response(task.post_data, cache_body, cache_status)) {
                            item.body = std::move(cache_body);
                            item.http_status_code = cache_status;
                        }
                    }
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(s_response_mutex);
            s_completed_responses.push_back(std::move(item));
        }
    }
}

void push_toast(const std::string& title, const std::string& subtitle, const std::string& detail,
                std::vector<uint8_t> rgba_pixels, float duration, int accent) {
    if (!get_config().retroachievements.notifications) {
        return;
    }
    RaToastItem item;
    item.title = title;
    item.subtitle = subtitle;
    item.detail = detail;
    item.rgba_pixels = rgba_pixels.empty() ? ra_generate_trophy_badge() : std::move(rgba_pixels);
    item.duration_sec = duration;
    item.elapsed_sec = 0.0f;
    item.accent_kind = accent;

    std::lock_guard<std::mutex> lock(s_toast_mutex);
    s_toast_queue.push_back(std::move(item));
}

void load_or_fetch_badge(const std::string& badge_name, const std::string& badge_url) {
    if (badge_name.empty()) {
        return;
    }
    const std::filesystem::path config_dir = recomp::get_config_path();
    std::filesystem::path badge_path = config_dir / "ra_badges" / (badge_name + ".png");

    std::error_code ec;
    if (std::filesystem::exists(badge_path, ec)) {
        return;
    }

    HttpRequestTask task;
    task.url = !badge_url.empty() ? badge_url : (std::string(RA_BADGE_BASE_URL) + badge_name + ".png");
    task.is_badge_download = true;
    task.badge_name = badge_name;
    task.badge_save_path = badge_path;

    std::lock_guard<std::mutex> lock(s_worker_mutex);
    s_request_queue.push_back(std::move(task));
    s_worker_cv.notify_one();
}

std::vector<uint8_t> get_cached_badge_or_trophy(const std::string& badge_name) {
    if (!badge_name.empty()) {
        const std::filesystem::path config_dir = recomp::get_config_path();
        std::filesystem::path badge_path = config_dir / "ra_badges" / (badge_name + ".png");

        std::error_code ec;
        if (std::filesystem::exists(badge_path, ec)) {
            std::ifstream file(badge_path, std::ios::binary | std::ios::ate);
            if (file.is_open()) {
                auto file_size = file.tellg();
                if (file_size > 0) {
                    std::vector<uint8_t> buffer(static_cast<size_t>(file_size));
                    file.seekg(0, std::ios::beg);
                    file.read(reinterpret_cast<char*>(buffer.data()), file_size);

                    DecodedImage img;
                    if (decode_png(buffer.data(), buffer.size(), img) && img.width == 64 && img.height == 64) {
                        return img.rgba;
                    }
                }
            }
        }
    }
    return ra_generate_trophy_badge();
}

void on_server_call(const rc_api_request_t* request, rc_client_server_callback_t callback, void* callback_data, rc_client_t* client) {
    (void)client;
    HttpRequestTask task;
    if (request->url) {
        task.url = request->url;
    }
    if (request->post_data) {
        task.post_data = request->post_data;
    }
    if (request->content_type) {
        task.content_type = request->content_type;
    }
    task.callback = callback;
    task.callback_data = callback_data;

    std::lock_guard<std::mutex> lock(s_worker_mutex);
    s_request_queue.push_back(std::move(task));
    s_worker_cv.notify_one();
}

uint32_t on_read_memory(uint32_t address, uint8_t* buffer, uint32_t num_bytes, rc_client_t* client) {
    (void)client;
    if (s_current_rdram == nullptr) {
        return 0;
    }
    return ra_read_n64_memory(s_current_rdram, address, buffer, num_bytes);
}

void on_game_load_completed(int result, const char* error_message, rc_client_t* client, void* userdata) {
    (void)userdata;
    if (result == RC_OK) {
        s_state.store(RaClientState::GameLoaded);
        const rc_client_game_t* game = rc_client_get_game_info(client);
        if (game) {
            s_current_game_id = game->id;
            s_current_game_hash = (game->hash && game->hash[0]) ? game->hash : "";
            s_game_title = (game->title && game->title[0]) ? game->title : "Sin and Punishment";
        }

        rc_client_user_game_summary_t summary{};
        rc_client_get_user_game_summary(client, &summary);
        s_unlocked_achievements = summary.num_unlocked_achievements;
        s_total_achievements = summary.num_promoted_achievements;

        std::string badge_name = (game && game->badge_name) ? game->badge_name : "";
        std::string badge_url = (game && game->badge_url) ? game->badge_url : "";
        if (!badge_name.empty()) {
            load_or_fetch_badge(badge_name, badge_url);
        }

        if (!s_is_offline_mode.load()) {
            rc_client_achievement_list_t* ach_list = rc_client_create_achievement_list(
                client, RC_CLIENT_ACHIEVEMENT_CATEGORY_PROMOTED_AND_UNPROMOTED,
                RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_PROGRESS);
            if (ach_list) {
                for (uint32_t b = 0; b < ach_list->num_buckets; ++b) {
                    const auto& bucket = ach_list->buckets[b];
                    for (uint32_t a = 0; a < bucket.num_achievements; ++a) {
                        const rc_client_achievement_t* ach = bucket.achievements[a];
                        if (ach && ach->badge_name[0] != '\0') {
                            std::string aname = ach->badge_name;
                            std::string aurl = (ach->badge_url && ach->badge_url[0]) ? ach->badge_url : "";
                            load_or_fetch_badge(aname, aurl);
                        }
                    }
                }
                rc_client_destroy_achievement_list(ach_list);
            }

            if (get_pending_unlock_count() > 0) {
                ra_sync_now();
            }
        }

        std::string sub_header = s_is_offline_mode.load()
            ? "RetroAchievements - Offline (Cached)"
            : "RetroAchievements - Softcore";
        std::string detail = std::to_string(s_unlocked_achievements) + "/" +
                             std::to_string(s_total_achievements) + " Unlocked (" +
                             std::to_string(summary.points_unlocked) + "/" +
                             std::to_string(summary.points_available) + " pts)";
        push_toast(sub_header, s_game_title, detail,
                   get_cached_badge_or_trophy(badge_name), DEFAULT_TOAST_DURATION, 1);
    } else {
        s_state.store(RaClientState::LoggedIn);
        std::fprintf(stderr, "[RA] Game identification failed: %s\n", error_message ? error_message : "Unknown");
    }
}

void trigger_game_load(rc_client_t* client) {
    s_state.store(RaClientState::GameLoading);
    std::filesystem::path rom_path = get_selected_rom_path();
    if (!rom_path.empty()) {
        rc_client_begin_identify_and_load_game(client, RC_CONSOLE_NINTENDO_64,
                                               rom_path.string().c_str(), nullptr, 0,
                                               on_game_load_completed, nullptr);
    } else {
        s_state.store(RaClientState::LoggedIn);
    }
}

void on_login_password_completed(int result, const char* error_message, rc_client_t* client, void* userdata);

void on_login_token_completed(int result, const char* error_message, rc_client_t* client, void* userdata) {
    (void)userdata;
    if (result == RC_OK) {
        s_state.store(RaClientState::LoggedIn);
        const rc_client_user_t* user = rc_client_get_user_info(client);
        if (user) {
            if (user->display_name) {
                s_current_username = user->display_name;
            } else if (user->username) {
                s_current_username = user->username;
            }
            if (user->token) {
                s_current_token = user->token;
            }
        }
        trigger_game_load(client);
    } else {
        if (!s_discovered_password.empty()) {
            rc_client_begin_login_with_password(client, s_current_username.c_str(),
                                                s_discovered_password.c_str(),
                                                on_login_password_completed, nullptr);
        } else {
            s_state.store(RaClientState::Offline);
            if (!s_offline_toast_shown) {
                s_offline_toast_shown = true;
                push_toast("RetroAchievements: Offline", "Sign-in failed",
                           error_message ? error_message : "Network error", {}, 3.5f, 3);
            }
        }
    }
}

void on_login_password_completed(int result, const char* error_message, rc_client_t* client, void* userdata) {
    (void)userdata;
    if (result == RC_OK) {
        s_state.store(RaClientState::LoggedIn);
        const rc_client_user_t* user = rc_client_get_user_info(client);
        if (user) {
            if (user->display_name) {
                s_current_username = user->display_name;
            } else if (user->username) {
                s_current_username = user->username;
            }
            if (user->token) {
                s_current_token = user->token;
                auto& cfg = get_mutable_config().retroachievements;
                cfg.username = s_current_username;
                cfg.token = user->token;
                cfg.password.clear();
                save_config();
            }
        }
        trigger_game_load(client);
    } else {
        s_state.store(RaClientState::Offline);
        if (!s_offline_toast_shown) {
            s_offline_toast_shown = true;
            push_toast("RetroAchievements: Offline", "Sign-in failed",
                       error_message ? error_message : "Invalid credentials", {}, 3.5f, 3);
        }
    }
}

void trigger_login(rc_client_t* client) {
    const auto& cfg = get_config().retroachievements;
    if (!cfg.enabled) {
        s_state.store(RaClientState::Disabled);
        return;
    }

    RaCredentials creds = discover_ra_credentials(cfg);
    if (!creds.has_login()) {
        s_state.store(RaClientState::NoCredentials);
        return;
    }

    s_current_username = creds.username;
    s_discovered_password = creds.password;
    if (!creds.token.empty()) {
        s_current_token = creds.token;
    }

    s_state.store(RaClientState::LoggingIn);
    if (!creds.token.empty()) {
        rc_client_begin_login_with_token(client, creds.username.c_str(), creds.token.c_str(),
                                         on_login_token_completed, nullptr);
    } else if (!creds.password.empty()) {
        rc_client_begin_login_with_password(client, creds.username.c_str(), creds.password.c_str(),
                                            on_login_password_completed, nullptr);
    }
}

void on_rc_event(const rc_client_event_t* event, rc_client_t* client) {
    (void)client;
    if (!event) {
        return;
    }

    switch (event->type) {
    case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
        if (event->achievement) {
            s_unlocked_achievements++;
            std::string badge_name = event->achievement->badge_name[0] != '\0' ? event->achievement->badge_name : "";
            std::string badge_url = (event->achievement->badge_url && event->achievement->badge_url[0]) ? event->achievement->badge_url : "";
            if (!badge_name.empty()) {
                load_or_fetch_badge(badge_name, badge_url);
            }

            std::string title = "ACHIEVEMENT UNLOCKED";
            std::string sub = event->achievement->title ? event->achievement->title : "Achievement";
            std::string desc = std::string(event->achievement->description ? event->achievement->description : "") +
                               " (" + std::to_string(event->achievement->points) + " pts)";
            if (s_is_offline_mode.load()) {
                desc += " (Saved offline)";
            }

            push_toast(title, sub, desc, get_cached_badge_or_trophy(badge_name), 5.0f, 0);
        }
        break;

    case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_SHOW:
    case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_UPDATE:
        if (event->achievement) {
            std::string sub = event->achievement->title ? event->achievement->title : "Progress";
            std::string desc = event->achievement->measured_progress[0] != '\0' ? event->achievement->measured_progress : "";
            push_toast("ACHIEVEMENT PROGRESS", sub, desc, {}, 2.5f, 2);
        }
        break;

    case RC_CLIENT_EVENT_SUBSET_COMPLETED:
    case RC_CLIENT_EVENT_GAME_COMPLETED:
        push_toast("MASTERY COMPLETE", s_game_title, "All achievements completed!", {}, 6.0f, 0);
        break;

    case RC_CLIENT_EVENT_SERVER_ERROR:
        if (event->server_error && !s_offline_toast_shown && !s_is_offline_mode.load()) {
            s_offline_toast_shown = true;
            push_toast("RetroAchievements: Offline", "Server error",
                       event->server_error->error_message ? event->server_error->error_message : "Unknown",
                       {}, 3.5f, 3);
        }
        break;

    default:
        break;
    }
}

} // namespace

uint32_t ra_read_n64_memory(const uint8_t* rdram, uint32_t address, uint8_t* buffer, uint32_t num_bytes) {
    if (rdram == nullptr || buffer == nullptr || address >= N64_RDRAM_MAX_SIZE || num_bytes == 0) {
        return 0;
    }

    uint32_t avail = std::min(num_bytes, N64_RDRAM_MAX_SIZE - address);
    std::memcpy(buffer, rdram + address, avail);
    return avail;
}

void ra_init() {
    init_libcurl();

    s_workers_running.store(true);
    for (auto& thread : s_worker_threads) {
        thread = std::thread(http_worker_thread_func);
    }

    s_last_frame_time = std::chrono::steady_clock::now();

    const auto& cfg = get_config().retroachievements;
    if (!cfg.enabled) {
        s_state.store(RaClientState::Disabled);
        return;
    }

    s_client = rc_client_create(on_read_memory, on_server_call);
    rc_client_set_hardcore_enabled(s_client, 0); // Enforce Softcore mode
    rc_client_set_allow_background_memory_reads(s_client, 0);
    rc_client_set_event_handler(s_client, on_rc_event);

    trigger_login(s_client);

    if (std::getenv("SNP_RA_DEMO_TOAST") != nullptr) {
        push_toast("ACHIEVEMENT UNLOCKED", "Target Confirmed",
                   "Defeat the Stage 1 boss without taking damage (10 pts)",
                   ra_generate_trophy_badge(), 5.0f, 0);
    }
}

void ra_shutdown() {
    s_workers_running.store(false);
    s_worker_cv.notify_all();
    for (auto& thread : s_worker_threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    if (s_client) {
        rc_client_destroy(s_client);
        s_client = nullptr;
    }

    if (s_libcurl_handle) {
        dlclose(s_libcurl_handle);
        s_libcurl_handle = nullptr;
    }
}

void ra_do_frame(uint8_t* rdram) {
    s_current_rdram = rdram;

    std::vector<HttpResponseItem> completed;
    {
        std::lock_guard<std::mutex> lock(s_response_mutex);
        completed.swap(s_completed_responses);
    }

    for (const auto& item : completed) {
        if (item.callback) {
            rc_api_server_response_t server_resp{};
            server_resp.body = item.body.data();
            server_resp.body_length = item.body.size();
            server_resp.http_status_code = item.http_status_code;
            item.callback(&server_resp, item.callback_data);
        }
    }

    if (s_client) {
        if (rdram != nullptr) {
            rc_client_do_frame(s_client);
        } else {
            rc_client_idle(s_client);
        }
    }

    s_current_rdram = nullptr;

    auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - s_last_frame_time).count();
    s_last_frame_time = now;

    std::lock_guard<std::mutex> lock(s_toast_mutex);
    if (!s_toast_queue.empty()) {
        s_toast_queue.front().elapsed_sec += dt;
        if (s_toast_queue.front().elapsed_sec >= s_toast_queue.front().duration_sec) {
            s_toast_queue.pop_front();
        }
    }
}

void ra_set_enabled(bool enabled) {
    auto& cfg = get_mutable_config().retroachievements;
    cfg.enabled = enabled;
    save_config();

    if (!enabled) {
        s_state.store(RaClientState::Disabled);
    } else {
        if (s_client != nullptr) {
            trigger_login(s_client);
        }
    }
}

bool ra_is_enabled() {
    return get_config().retroachievements.enabled;
}

std::string ra_get_status_text() {
    if (!ra_is_enabled()) {
        return "Off";
    }

    if (s_is_syncing.load()) {
        return "Syncing...";
    }

    RaClientState st = s_state.load();
    switch (st) {
    case RaClientState::Disabled:
        return "Off";
    case RaClientState::NoCredentials:
        return "No Credentials";
    case RaClientState::LoggingIn:
    case RaClientState::GameLoading:
        return "Signing in...";
    case RaClientState::Offline:
        return "Offline";
    case RaClientState::LoggedIn:
    case RaClientState::GameLoaded: {
        size_t pending = ra_get_pending_sync_count();
        if (s_is_offline_mode.load()) {
            if (pending > 0) {
                return "Offline - " + std::to_string(pending) + " Queued (" +
                       std::to_string(s_unlocked_achievements) + "/" +
                       std::to_string(s_total_achievements) + ")";
            }
            return "Offline - Cached (" + std::to_string(s_unlocked_achievements) + "/" +
                   std::to_string(s_total_achievements) + ")";
        }
        if (pending > 0) {
            return s_current_username + " (" + std::to_string(s_unlocked_achievements) + "/" +
                   std::to_string(s_total_achievements) + " - " + std::to_string(pending) + " queued)";
        }
        return s_current_username + " (" + std::to_string(s_unlocked_achievements) + "/" +
               std::to_string(s_total_achievements) + ")";
    }
    }
    return "Off";
}

void ra_sync_now() {
    if (s_is_syncing.load()) {
        return;
    }

    if (s_state.load() == RaClientState::Offline && !s_is_offline_mode.load()) {
        if (s_client != nullptr) {
            trigger_login(s_client);
        }
        return;
    }

    HttpRequestTask task;
    task.is_sync_task = true;

    std::lock_guard<std::mutex> lock(s_worker_mutex);
    s_request_queue.push_back(std::move(task));
    s_worker_cv.notify_one();
}

size_t ra_get_pending_sync_count() {
    return get_pending_unlock_count();
}

bool ra_is_syncing() {
    return s_is_syncing.load();
}

bool ra_is_offline_mode() {
    return s_is_offline_mode.load();
}

bool ra_has_active_toasts() {
    if (!get_config().retroachievements.notifications) {
        return false;
    }
    std::lock_guard<std::mutex> lock(s_toast_mutex);
    return !s_toast_queue.empty();
}

bool ra_get_current_toast(RaToastItem& out_toast) {
    if (!get_config().retroachievements.notifications) {
        return false;
    }
    std::lock_guard<std::mutex> lock(s_toast_mutex);
    if (s_toast_queue.empty()) {
        return false;
    }
    out_toast = s_toast_queue.front();
    return true;
}

std::vector<uint8_t> ra_generate_trophy_badge(uint32_t width, uint32_t height) {
    std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4, 0);

    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            uint8_t r = 0, g = 0, b = 0, a = 0;

            if (y >= 14 && y <= 34) {
                int inset = (y > 22) ? (y - 22) / 2 : 0;
                if (x >= 20 + inset && x <= 43 - inset) {
                    r = 245; g = 197; b = 24; a = 255;
                    if (y <= 16 || x == 20 + inset || x == 43 - inset) {
                        r = 255; g = 224; b = 102;
                    }
                }
            }

            if (y >= 18 && y <= 28) {
                if ((x >= 14 && x <= 19) || (x >= 44 && x <= 49)) {
                    if (y == 18 || y == 28 || x == 14 || x == 49) {
                        r = 212; g = 158; b = 14; a = 255;
                    }
                }
            }

            if (y >= 35 && y <= 45 && x >= 30 && x <= 33) {
                r = 220; g = 170; b = 18; a = 255;
            }

            if (y >= 46 && y <= 52 && x >= 22 && x <= 41) {
                r = 245; g = 197; b = 24; a = 255;
                if (y == 46 || x == 22 || x == 41) {
                    r = 255; g = 224; b = 102;
                }
            }

            size_t idx = (static_cast<size_t>(y) * width + x) * 4;
            rgba[idx + 0] = r;
            rgba[idx + 1] = g;
            rgba[idx + 2] = b;
            rgba[idx + 3] = a;
        }
    }

    return rgba;
}

} // namespace snp::handheld
