#include "snp_patch_lib.h"

#include <iostream>
#include <string_view>

namespace {

void print_usage(const char* prog_name) {
    std::cerr << "Usage:\n"
              << "  " << prog_name << " --verify <rom_path>\n"
              << "  " << prog_name << " --normalize <in_rom> <out_z64>\n"
              << "  " << prog_name << " --setup-rom <in_rom> <out_dir> [--patch <patch_file>]\n"
              << "  " << prog_name << " --crypt <rom_path> <in_file> <out_file_or_dash>\n";
}

int handle_verify(const std::filesystem::path& rom_path) {
    std::vector<uint8_t> raw_rom;
    std::string err;
    if (!snp::patch::read_file_bytes(rom_path, raw_rom, err)) {
        std::cerr << "snp-patch: " << err << "\n";
        return 1;
    }

    std::vector<uint8_t> z64_rom;
    if (!snp::patch::normalize_rom_to_z64(raw_rom, z64_rom, err)) {
        std::cerr << "snp-patch: " << err << "\n";
        return 1;
    }

    std::vector<uint8_t> canonical_code;
    std::string game_id;
    if (!snp::patch::extract_canonical_code(z64_rom, canonical_code, game_id, err)) {
        std::cerr << "snp-patch: " << err << "\n";
        return 1;
    }

    std::cout << "GAME_ID=" << game_id << "\n";
    return 0;
}

int handle_normalize(const std::filesystem::path& in_rom, const std::filesystem::path& out_z64) {
    std::vector<uint8_t> raw_rom;
    std::string err;
    if (!snp::patch::read_file_bytes(in_rom, raw_rom, err)) {
        std::cerr << "snp-patch: " << err << "\n";
        return 1;
    }

    std::vector<uint8_t> z64_rom;
    if (!snp::patch::normalize_rom_to_z64(raw_rom, z64_rom, err)) {
        std::cerr << "snp-patch: " << err << "\n";
        return 1;
    }

    if (!snp::patch::write_file_atomic(out_z64, z64_rom.data(), z64_rom.size(), err)) {
        std::cerr << "snp-patch: " << err << "\n";
        return 1;
    }

    return 0;
}

int handle_setup_rom(const std::filesystem::path& in_rom, const std::filesystem::path& out_dir,
                     const std::filesystem::path& patch_file) {
    std::vector<uint8_t> raw_rom;
    std::string err;
    if (!snp::patch::read_file_bytes(in_rom, raw_rom, err)) {
        std::cerr << "snp-patch: " << err << "\n";
        return 1;
    }

    std::vector<uint8_t> z64_rom;
    if (!snp::patch::normalize_rom_to_z64(raw_rom, z64_rom, err)) {
        std::cerr << "snp-patch: " << err << "\n";
        return 1;
    }

    if (!patch_file.empty()) {
        std::vector<uint8_t> patch_bytes;
        if (!snp::patch::read_file_bytes(patch_file, patch_bytes, err)) {
            std::cerr << "snp-patch: failed reading translation patch: " << err << "\n";
            return 1;
        }

        std::vector<uint8_t> patched_z64;
        bool already_patched = false;
        if (!snp::patch::apply_rom_patch(z64_rom, patch_bytes, patched_z64, already_patched, err)) {
            std::cerr << "snp-patch: failed applying translation patch: " << err << "\n";
            return 1;
        }
        if (!already_patched) {
            z64_rom = std::move(patched_z64);
        }
    }

    std::vector<uint8_t> canonical_code;
    std::string game_id;
    if (!snp::patch::extract_canonical_code(z64_rom, canonical_code, game_id, err)) {
        std::cerr << "snp-patch: ROM code verification failed: " << err << "\n";
        return 1;
    }

    const std::filesystem::path target_path = out_dir / (game_id + ".z64");
    bool skip_write = false;

    std::error_code ec;
    if (std::filesystem::exists(target_path, ec)) {
        const uintmax_t existing_size = std::filesystem::file_size(target_path, ec);
        if (!ec && existing_size == z64_rom.size()) {
            std::vector<uint8_t> existing_rom;
            std::string read_err;
            if (snp::patch::read_file_bytes(target_path, existing_rom, read_err)) {
                if (snp::patch::compute_crc32(existing_rom) == snp::patch::compute_crc32(z64_rom)) {
                    skip_write = true;
                }
            }
        }
    }

    if (!skip_write) {
        if (!snp::patch::write_file_atomic(target_path, z64_rom.data(), z64_rom.size(), err)) {
            std::cerr << "snp-patch: failed saving verified ROM: " << err << "\n";
            return 1;
        }
    }

    std::cout << "GAME_ID=" << game_id << "\n";
    std::cout << "STORED_ROM=" << target_path.string() << "\n";
    return 0;
}

int handle_crypt(const std::filesystem::path& rom_path, const std::filesystem::path& in_file_path,
                 const std::string& out_file_or_dash) {
    std::vector<uint8_t> raw_rom;
    std::string err;
    if (!snp::patch::read_file_bytes(rom_path, raw_rom, err)) {
        std::cerr << "snp-patch: failed to read key ROM: " << err << "\n";
        return 1;
    }

    std::vector<uint8_t> z64_rom;
    if (!snp::patch::normalize_rom_to_z64(raw_rom, z64_rom, err)) {
        std::cerr << "snp-patch: failed to normalize key ROM: " << err << "\n";
        return 1;
    }

    std::vector<uint8_t> canonical_code;
    std::string game_id;
    if (!snp::patch::extract_canonical_code(z64_rom, canonical_code, game_id, err)) {
        std::cerr << "snp-patch: failed to extract key code: " << err << "\n";
        return 1;
    }

    std::ifstream in_file(in_file_path, std::ios::binary);
    if (!in_file.is_open()) {
        std::cerr << "snp-patch: failed to open input payload: " << in_file_path.string() << "\n";
        return 1;
    }

    FILE* out_fp = nullptr;
    const bool write_stdout = (out_file_or_dash == "-");
    if (write_stdout) {
#ifdef _WIN32
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        out_fp = stdout;
    } else {
        out_fp = std::fopen(out_file_or_dash.c_str(), "wb");
        if (!out_fp) {
            std::cerr << "snp-patch: failed to open output file: " << out_file_or_dash << "\n";
            return 1;
        }
    }

    if (!snp::patch::crypt_stream(canonical_code, in_file, out_fp, err)) {
        std::cerr << "snp-patch: cipher streaming error: " << err << "\n";
        if (!write_stdout) {
            std::fclose(out_fp);
        }
        return 1;
    }

    if (!write_stdout) {
        std::fclose(out_fp);
    }
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc >= 3 && std::string_view(argv[1]) == "--verify") {
        return handle_verify(argv[2]);
    }

    if (argc >= 4 && std::string_view(argv[1]) == "--normalize") {
        return handle_normalize(argv[2], argv[3]);
    }

    if (argc >= 4 && std::string_view(argv[1]) == "--setup-rom") {
        std::filesystem::path patch_path;
        if (argc >= 6 && std::string_view(argv[4]) == "--patch") {
            patch_path = argv[5];
        }
        return handle_setup_rom(argv[2], argv[3], patch_path);
    }

    if (argc >= 5 && std::string_view(argv[1]) == "--crypt") {
        return handle_crypt(argv[2], argv[3], argv[4]);
    }

    print_usage(argv[0]);
    return 1;
}
