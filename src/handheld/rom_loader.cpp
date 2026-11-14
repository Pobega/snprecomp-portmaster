#include "rom_loader.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <vector>

#include "handheld_config.h"
#include "librecomp/game.hpp"

namespace snp::handheld {

namespace {

const std::u8string GAME_ID_JP = u8"snp.n64.jp.1.0";
const std::u8string GAME_ID_EN = u8"snp.n64.en.fan";

bool is_rom_extension(const std::filesystem::path& path) {
    auto ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".z64" || ext == ".n64" || ext == ".v64";
}

bool is_candidate_rom_file(const std::filesystem::path& path) {
    if (!is_rom_extension(path)) {
        return false;
    }
    const auto filename = path.filename().string();
    if (filename == "snp.n64.jp.1.0.z64" ||
        filename == "snp.n64.en.fan.z64" ||
        filename == "normalized_rom.z64") {
        return false;
    }
    return true;
}

std::vector<std::filesystem::path> get_candidate_search_dirs() {
    std::vector<std::filesystem::path> search_dirs;

    const char* env_dir = std::getenv("SNP_ROM_DIR");
    if (env_dir != nullptr && std::filesystem::is_directory(env_dir)) {
        search_dirs.emplace_back(env_dir);
    }

    if (std::filesystem::is_directory("roms")) {
        search_dirs.emplace_back("roms");
    }

    search_dirs.emplace_back(".");

    return search_dirs;
}

} // namespace

std::string find_and_select_rom() {
    const std::filesystem::path config_dir = recomp::get_config_path();
    if (!config_dir.empty()) {
        std::error_code ec;
        std::filesystem::remove(config_dir / "normalized_rom.z64", ec);
    }

    const auto& config = get_config();
    std::vector<std::u8string> game_id_priority;
    if (config.rom.preferred_region == ROM_PREFERRED_JP) {
        game_id_priority.push_back(GAME_ID_JP);
        game_id_priority.push_back(GAME_ID_EN);
    } else {
        game_id_priority.push_back(GAME_ID_EN);
        game_id_priority.push_back(GAME_ID_JP);
    }

    recomp::check_all_stored_roms();

    if (recomp::is_rom_valid(game_id_priority[0])) {
        std::cout << "Using verified stored ROM ("
                  << reinterpret_cast<const char*>(game_id_priority[0].c_str()) << ")\n";
        return std::string(reinterpret_cast<const char*>(game_id_priority[0].c_str()));
    }

    std::vector<std::filesystem::path> candidate_dirs = get_candidate_search_dirs();
    std::vector<std::filesystem::path> candidate_files;

    for (const auto& dir : candidate_dirs) {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (entry.is_regular_file() && is_candidate_rom_file(entry.path())) {
                candidate_files.push_back(entry.path());
            }
        }
    }

    if (recomp::is_rom_valid(game_id_priority[1])) {
        for (const auto& raw_path : candidate_files) {
            auto err = recomp::select_rom(raw_path, game_id_priority[0]);
            if (err == recomp::RomValidationError::Good) {
                std::cout << "Selected ROM: " << raw_path << " ("
                          << reinterpret_cast<const char*>(game_id_priority[0].c_str()) << ")\n";
                return std::string(reinterpret_cast<const char*>(game_id_priority[0].c_str()));
            }
        }
        std::cout << "Using verified stored ROM ("
                  << reinterpret_cast<const char*>(game_id_priority[1].c_str()) << ")\n";
        return std::string(reinterpret_cast<const char*>(game_id_priority[1].c_str()));
    }

    if (candidate_files.empty()) {
        std::cerr << "No Sin and Punishment ROM found (.z64, .n64, .v64).\n"
                  << "Please place your ROM in ./roms/ or set SNP_ROM_DIR.\n";
        return {};
    }

    for (const auto& raw_path : candidate_files) {
        for (const auto& game_id : game_id_priority) {
            auto err = recomp::select_rom(raw_path, game_id);
            if (err == recomp::RomValidationError::Good) {
                std::cout << "Selected ROM: " << raw_path << " ("
                          << reinterpret_cast<const char*>(game_id.c_str()) << ")\n";
                return std::string(reinterpret_cast<const char*>(game_id.c_str()));
            }
        }
    }

    std::cerr << "ROM files were found, but none matched Sin and Punishment code fingerprint or CRC.\n";
    return {};
}

} // namespace snp::handheld
