#include "rom_loader.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

#include "handheld_config.h"
#include "librecomp/game.hpp"

namespace snp::handheld {

namespace {

constexpr std::array<uint8_t, 4> Z64_MAGIC = { 0x80, 0x37, 0x12, 0x40 };
constexpr std::array<uint8_t, 4> V64_MAGIC = { 0x37, 0x80, 0x40, 0x12 };
constexpr std::array<uint8_t, 4> N64_MAGIC = { 0x40, 0x12, 0x37, 0x80 };

constexpr size_t MIN_ROM_SIZE = 16 * 1024 * 1024;
constexpr size_t MAX_ROM_SIZE = 64 * 1024 * 1024;

const std::u8string GAME_ID_JP = u8"snp.n64.jp.1.0";
const std::u8string GAME_ID_EN = u8"snp.n64.en.fan";

bool is_rom_extension(const std::filesystem::path& path) {
    auto ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".z64" || ext == ".n64" || ext == ".v64";
}

bool normalize_rom_file(const std::filesystem::path& input_path, std::filesystem::path& output_path) {
    std::ifstream in(input_path, std::ios::binary);
    if (!in.is_open()) {
        return false;
    }

    in.seekg(0, std::ios::end);
    size_t size = in.tellg();
    in.seekg(0, std::ios::beg);

    if (size < MIN_ROM_SIZE || size > MAX_ROM_SIZE) {
        return false;
    }

    std::array<uint8_t, 4> magic{};
    in.read(reinterpret_cast<char*>(magic.data()), magic.size());
    if (magic == Z64_MAGIC) {
        output_path = input_path;
        return true;
    }

    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(size);
    in.read(reinterpret_cast<char*>(data.data()), size);
    in.close();

    if (magic == V64_MAGIC) {
        std::cout << "Converting byte-swapped .v64 ROM to big-endian .z64...\n";
        for (size_t i = 0; i + 1 < data.size(); i += 2) {
            std::swap(data[i], data[i + 1]);
        }
    } else if (magic == N64_MAGIC) {
        std::cout << "Converting word-swapped .n64 ROM to big-endian .z64...\n";
        for (size_t i = 0; i + 3 < data.size(); i += 4) {
            std::reverse(data.begin() + i, data.begin() + i + 4);
        }
    } else {
        std::cerr << "Unrecognized ROM header magic in: " << input_path << "\n";
        return false;
    }

    std::filesystem::path config_dir = recomp::get_config_path();
    std::filesystem::create_directories(config_dir);
    output_path = config_dir / "normalized_rom.z64";

    std::ofstream out(output_path, std::ios::binary);
    if (!out.is_open()) {
        std::cerr << "Failed to write normalized ROM: " << output_path << "\n";
        return false;
    }
    out.write(reinterpret_cast<const char*>(data.data()), data.size());
    out.close();

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

    std::filesystem::path config_dir = recomp::get_config_path();
    if (!config_dir.empty() && std::filesystem::is_directory(config_dir)) {
        search_dirs.push_back(config_dir);
    }

    return search_dirs;
}

} // namespace

std::string find_and_select_rom() {
    std::vector<std::filesystem::path> candidate_dirs = get_candidate_search_dirs();
    std::vector<std::filesystem::path> candidate_files;

    for (const auto& dir : candidate_dirs) {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (entry.is_regular_file() && is_rom_extension(entry.path())) {
                candidate_files.push_back(entry.path());
            }
        }
    }

    if (candidate_files.empty()) {
        std::cerr << "No Sin and Punishment ROM found (.z64, .n64, .v64).\n"
                  << "Please place your ROM in ./roms/ or set SNP_ROM_DIR.\n";
        return {};
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

    for (const auto& raw_path : candidate_files) {
        std::filesystem::path normalized_path;
        if (!normalize_rom_file(raw_path, normalized_path)) {
            continue;
        }

        for (const auto& game_id : game_id_priority) {
            auto err = recomp::select_rom(normalized_path, game_id);
            if (err == recomp::RomValidationError::Good) {
                if (recomp::load_stored_rom(game_id)) {
                    std::cout << "Selected and loaded ROM: " << normalized_path << " ("
                              << reinterpret_cast<const char*>(game_id.c_str()) << ")\n";
                    return std::string(reinterpret_cast<const char*>(game_id.c_str()));
                }
            }
        }
    }

    std::cerr << "ROM files were found, but none matched Sin and Punishment code fingerprint or CRC.\n";
    return {};
}

} // namespace snp::handheld
