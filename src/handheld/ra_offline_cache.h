#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace snp::handheld {

struct PendingUnlock {
    std::string username;
    uint32_t game_id = 0;
    uint32_t achievement_id = 0;
    std::string game_hash;
    uint64_t unlock_epoch_sec = 0;
};

std::string url_decode(const std::string& in);
std::unordered_map<std::string, std::string> parse_url_params(const std::string& query_or_post_data);
std::string sanitize_filename(const std::string& in);

std::filesystem::path get_default_ra_cache_dir();

bool cache_online_response(const std::string& post_data, const std::string& response_body,
                           const std::filesystem::path& cache_dir = get_default_ra_cache_dir());

bool try_serve_offline_response(const std::string& post_data, std::string& out_body, int& out_status,
                                const std::filesystem::path& cache_dir = get_default_ra_cache_dir());

bool queue_pending_unlock(const PendingUnlock& unlock,
                          const std::filesystem::path& cache_dir = get_default_ra_cache_dir());
std::vector<PendingUnlock> load_pending_unlocks(const std::filesystem::path& cache_dir = get_default_ra_cache_dir());
bool remove_pending_unlocks(const std::vector<uint32_t>& synced_achievement_ids,
                            const std::string& username, uint32_t game_id,
                            const std::filesystem::path& cache_dir = get_default_ra_cache_dir());
size_t get_pending_unlock_count(const std::filesystem::path& cache_dir = get_default_ra_cache_dir());

std::string merge_start_session_unlocks(const std::string& original_json,
                                        const std::vector<PendingUnlock>& pending_for_game);

bool record_synced_unlocks_to_startsession(const std::string& username, uint32_t game_id,
                                           const std::vector<uint32_t>& synced_achievement_ids,
                                           const std::filesystem::path& cache_dir = get_default_ra_cache_dir());

} // namespace snp::handheld
