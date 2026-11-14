#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace snp::patch {

constexpr size_t MIN_ROM_SIZE = 16 * 1024 * 1024;
constexpr size_t MAX_ROM_SIZE = 64 * 1024 * 1024;

constexpr std::array<uint8_t, 4> Z64_MAGIC = { 0x80, 0x37, 0x12, 0x40 };
constexpr std::array<uint8_t, 4> V64_MAGIC = { 0x37, 0x80, 0x40, 0x12 };
constexpr std::array<uint8_t, 4> N64_MAGIC = { 0x40, 0x12, 0x37, 0x80 };

constexpr uint32_t CRC32_POLYNOMIAL = 0xEDB88320u;
constexpr uint32_t CRC32_INITIAL = 0xFFFFFFFFu;
constexpr uint32_t CRC32_FINAL_XOR = 0xFFFFFFFFu;

constexpr uint32_t MAIN_VRAM = 0x80025C00u;
constexpr uint32_t MAIN_ROM = 0x1000u;
constexpr uint32_t MAIN_TEXT_SIZE = 0x31280u;
constexpr uint32_t LOAD_TABLE_ROM = 0x34BACu;
constexpr uint32_t SEGMENT_ENTRY_BYTES = 20u;

constexpr std::array<uint32_t, 21> VARYING_WORDS = {
    0x80025C00u, 0x80025C04u, 0x80025C08u, 0x80025C0Cu,
    0x8002BD5Cu, 0x8002BD8Cu, 0x8002BD98u,
    0x8003AB28u, 0x8003AB2Cu, 0x8003AB78u, 0x8003AB7Cu, 0x8003ABA4u, 0x8003ABA8u, 0x8003ABF0u, 0x8003ABF4u,
    0x8003C158u, 0x8003C15Cu, 0x8003C198u, 0x8003C19Cu, 0x8003C1ECu, 0x8003C1F0u,
};

constexpr uint64_t FNV1A_OFFSET_BASIS = 0xcbf29ce484222325ULL;
constexpr uint64_t FNV1A_PRIME = 0x100000001b3ULL;
constexpr uint64_t EXPECTED_CODE_FINGERPRINT = 0xe941bdc06663d9adULL;

constexpr const char* GAME_ID_JP = "snp.n64.jp.1.0";
constexpr const char* GAME_ID_EN = "snp.n64.en.fan";
constexpr size_t ROM_INTERNAL_NAME_OFFSET = 0x20u;
constexpr std::string_view JP_INTERNAL_NAME = "TSUMI TO BATSU";

constexpr uint64_t INITIAL_CIPHER_STATE = 0xe941bdc06663d9adULL;
constexpr uint64_t SPLITMIX_GAMMA = 0x9e3779b97f4a7c15ULL;
constexpr uint64_t SPLITMIX_MUL1 = 0xbf58476d1ce4e5b9ULL;
constexpr uint64_t SPLITMIX_MUL2 = 0x94d049bb133111ebULL;
constexpr size_t STREAM_CHUNK_SIZE = 65536;

constexpr size_t BPS_MIN_HEADER_SIZE = 19;
constexpr size_t BPS_FOOTER_SIZE = 12;
constexpr size_t UPS_MIN_HEADER_SIZE = 18;
constexpr size_t UPS_FOOTER_SIZE = 12;
constexpr size_t IPS_HEADER_SIZE = 5;

inline uint32_t read_le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
          (static_cast<uint32_t>(p[1]) << 8) |
          (static_cast<uint32_t>(p[2]) << 16) |
          (static_cast<uint32_t>(p[3]) << 24);
}

inline uint32_t read_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
            static_cast<uint32_t>(p[3]);
}

inline uint64_t read_le64(const uint8_t* p) {
    uint64_t value = 0;
    for (size_t i = 0; i < sizeof(uint64_t); ++i) {
        value |= static_cast<uint64_t>(p[i]) << (i * 8);
    }
    return value;
}

inline uint32_t compute_crc32(const uint8_t* data, size_t size) {
    static const auto table = []() {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? (CRC32_POLYNOMIAL ^ (c >> 1)) : (c >> 1);
            }
            t[i] = c;
        }
        return t;
    }();

    uint32_t crc = CRC32_INITIAL;
    for (size_t i = 0; i < size; ++i) {
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ CRC32_FINAL_XOR;
}

inline uint32_t compute_crc32(const std::vector<uint8_t>& data) {
    return compute_crc32(data.data(), data.size());
}

inline bool read_file_bytes(const std::filesystem::path& path, std::vector<uint8_t>& out, std::string& err) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        err = "Failed to open file: " + path.string();
        return false;
    }
    const std::streamsize size = file.tellg();
    if (size < 0) {
        err = "Failed to determine file size: " + path.string();
        return false;
    }
    out.resize(static_cast<size_t>(size));
    file.seekg(0, std::ios::beg);
    if (size > 0 && !file.read(reinterpret_cast<char*>(out.data()), size)) {
        err = "Failed to read file contents: " + path.string();
        return false;
    }
    return true;
}

inline bool write_file_atomic(const std::filesystem::path& target_path, const uint8_t* data, size_t size, std::string& err) {
    std::error_code ec;
    if (target_path.has_parent_path()) {
        std::filesystem::create_directories(target_path.parent_path(), ec);
        if (ec) {
            err = "Failed to create directory: " + ec.message();
            return false;
        }
    }
    std::filesystem::path tmp_path = target_path;
    tmp_path += ".tmp";

    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            err = "Failed to open temporary file for writing: " + tmp_path.string();
            return false;
        }
        if (size > 0 && !out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size))) {
            err = "Failed to write data to: " + tmp_path.string();
            return false;
        }
        out.flush();
        if (!out.good()) {
            err = "Failed to flush data to: " + tmp_path.string();
            return false;
        }
    }

    std::filesystem::rename(tmp_path, target_path, ec);
    if (ec) {
        err = "Failed to rename " + tmp_path.string() + " to " + target_path.string() + ": " + ec.message();
        std::filesystem::remove(tmp_path, ec);
        return false;
    }
    return true;
}

inline bool normalize_rom_to_z64(const std::vector<uint8_t>& in_rom, std::vector<uint8_t>& out_z64, std::string& err) {
    if (in_rom.size() < MIN_ROM_SIZE || in_rom.size() > MAX_ROM_SIZE) {
        err = "ROM file size (" + std::to_string(in_rom.size()) + " bytes) is outside valid range (16MB to 64MB).";
        return false;
    }

    std::array<uint8_t, 4> magic = { in_rom[0], in_rom[1], in_rom[2], in_rom[3] };
    out_z64 = in_rom;

    if (magic == Z64_MAGIC) {
        // Native big-endian .z64 format
    } else if (magic == V64_MAGIC) {
        // Byte-swapped 16-bit .v64 format
        for (size_t i = 0; i + 1 < out_z64.size(); i += 2) {
            std::swap(out_z64[i], out_z64[i + 1]);
        }
    } else if (magic == N64_MAGIC) {
        // Word-swapped 32-bit little-endian .n64 format
        for (size_t i = 0; i + 3 < out_z64.size(); i += 4) {
            std::reverse(out_z64.begin() + i, out_z64.begin() + i + 4);
        }
    } else {
        err = "Unrecognized ROM header magic (expected .z64, .v64, or .n64 Nintendo 64 format).";
        return false;
    }

    const size_t padded_size = (out_z64.size() + 3) & ~size_t(3);
    if (out_z64.size() < padded_size) {
        out_z64.resize(padded_size, 0);
    }
    return true;
}

inline bool decode_bps_varint(const uint8_t* data, size_t size, size_t& pos, size_t limit, uint64_t& out_val) {
    uint64_t val = 0;
    uint64_t shift = 1;
    while (true) {
        if (pos >= limit) {
            return false;
        }
        uint8_t b = data[pos++];
        val += static_cast<uint64_t>(b & 0x7F) * shift;
        if (b & 0x80) {
            break;
        }
        shift <<= 7;
        val += shift;
    }
    out_val = val;
    return true;
}

inline bool apply_bps_patch(const std::vector<uint8_t>& source_z64, const std::vector<uint8_t>& patch,
                            std::vector<uint8_t>& out_z64, bool& already_patched, std::string& err) {
    if (patch.size() < BPS_MIN_HEADER_SIZE) {
        err = "BPS patch file is truncated.";
        return false;
    }

    const size_t footer_offset = patch.size() - BPS_FOOTER_SIZE;
    const uint32_t patch_crc_expected = read_le32(patch.data() + patch.size() - 4);
    const uint32_t patch_crc_computed = compute_crc32(patch.data(), patch.size() - 4);
    if (patch_crc_computed != patch_crc_expected) {
        err = "BPS patch CRC32 checksum mismatch.";
        return false;
    }

    const uint32_t source_crc = read_le32(patch.data() + patch.size() - 12);
    const uint32_t target_crc = read_le32(patch.data() + patch.size() - 8);
    const uint32_t rom_crc = compute_crc32(source_z64);

    if (rom_crc == target_crc && rom_crc != source_crc) {
        already_patched = true;
        out_z64 = source_z64;
        return true;
    }

    if (rom_crc != source_crc) {
        err = "ROM CRC32 does not match BPS patch source CRC32 (expected unaltered Japanese Sin and Punishment ROM).";
        return false;
    }

    size_t patch_pos = 4;
    uint64_t source_size = 0;
    uint64_t target_size = 0;
    uint64_t metadata_size = 0;

    if (!decode_bps_varint(patch.data(), patch.size(), patch_pos, footer_offset, source_size) ||
        !decode_bps_varint(patch.data(), patch.size(), patch_pos, footer_offset, target_size) ||
        !decode_bps_varint(patch.data(), patch.size(), patch_pos, footer_offset, metadata_size)) {
        err = "Failed to parse BPS header varints.";
        return false;
    }

    if (source_size != source_z64.size()) {
        err = "BPS patch expected source size " + std::to_string(source_size) +
              ", but input ROM has size " + std::to_string(source_z64.size()) + ".";
        return false;
    }

    if (target_size < MIN_ROM_SIZE || target_size > MAX_ROM_SIZE) {
        err = "BPS target ROM size (" + std::to_string(target_size) + ") is outside valid range.";
        return false;
    }

    if (patch_pos + metadata_size > footer_offset) {
        err = "BPS metadata field overflows patch boundary.";
        return false;
    }
    patch_pos += static_cast<size_t>(metadata_size);

    out_z64.assign(static_cast<size_t>(target_size), 0);
    size_t output_offset = 0;
    int64_t source_rel_offset = 0;
    int64_t target_rel_offset = 0;

    while (patch_pos < footer_offset) {
        uint64_t command = 0;
        if (!decode_bps_varint(patch.data(), patch.size(), patch_pos, footer_offset, command)) {
            err = "Failed to decode BPS command.";
            return false;
        }

        const uint64_t length = (command >> 2) + 1;
        const uint64_t action = command & 3;

        if (action == 0) {
            // SourceRead
            if (output_offset + length > source_z64.size() || output_offset + length > out_z64.size()) {
                err = "BPS SourceRead out of bounds.";
                return false;
            }
            std::memcpy(out_z64.data() + output_offset, source_z64.data() + output_offset, static_cast<size_t>(length));
            output_offset += static_cast<size_t>(length);
        } else if (action == 1) {
            // TargetRead
            if (patch_pos + length > footer_offset || output_offset + length > out_z64.size()) {
                err = "BPS TargetRead out of bounds.";
                return false;
            }
            std::memcpy(out_z64.data() + output_offset, patch.data() + patch_pos, static_cast<size_t>(length));
            patch_pos += static_cast<size_t>(length);
            output_offset += static_cast<size_t>(length);
        } else if (action == 2) {
            // SourceCopy
            uint64_t d = 0;
            if (!decode_bps_varint(patch.data(), patch.size(), patch_pos, footer_offset, d)) {
                err = "Failed to decode BPS SourceCopy delta.";
                return false;
            }
            const int64_t signed_delta = (d & 1) ? -static_cast<int64_t>(d >> 1) : static_cast<int64_t>(d >> 1);
            source_rel_offset += signed_delta;
            if (source_rel_offset < 0 ||
                static_cast<uint64_t>(source_rel_offset) + length > source_z64.size() ||
                output_offset + length > out_z64.size()) {
                err = "BPS SourceCopy offset out of bounds.";
                return false;
            }
            std::memcpy(out_z64.data() + output_offset, source_z64.data() + source_rel_offset, static_cast<size_t>(length));
            source_rel_offset += static_cast<int64_t>(length);
            output_offset += static_cast<size_t>(length);
        } else if (action == 3) {
            // TargetCopy
            uint64_t d = 0;
            if (!decode_bps_varint(patch.data(), patch.size(), patch_pos, footer_offset, d)) {
                err = "Failed to decode BPS TargetCopy delta.";
                return false;
            }
            const int64_t signed_delta = (d & 1) ? -static_cast<int64_t>(d >> 1) : static_cast<int64_t>(d >> 1);
            target_rel_offset += signed_delta;
            if (target_rel_offset < 0 ||
                target_rel_offset >= static_cast<int64_t>(output_offset) ||
                output_offset + length > out_z64.size()) {
                err = "BPS TargetCopy offset out of bounds.";
                return false;
            }
            // Byte-by-byte copy supports overlapping repetition sequences.
            for (uint64_t i = 0; i < length; ++i) {
                out_z64[output_offset++] = out_z64[static_cast<size_t>(target_rel_offset++)];
            }
        }
    }

    if (output_offset != target_size) {
        err = "BPS output size mismatch (" + std::to_string(output_offset) + " vs " + std::to_string(target_size) + ").";
        return false;
    }

    if (compute_crc32(out_z64) != target_crc) {
        err = "BPS patched ROM target CRC32 verification failed.";
        return false;
    }

    return true;
}

inline bool apply_ups_patch(const std::vector<uint8_t>& source_z64, const std::vector<uint8_t>& patch,
                            std::vector<uint8_t>& out_z64, bool& already_patched, std::string& err) {
    if (patch.size() < UPS_MIN_HEADER_SIZE) {
        err = "UPS patch file is truncated.";
        return false;
    }

    const size_t footer_offset = patch.size() - UPS_FOOTER_SIZE;
    const uint32_t patch_crc_expected = read_le32(patch.data() + patch.size() - 4);
    const uint32_t patch_crc_computed = compute_crc32(patch.data(), patch.size() - 4);
    if (patch_crc_computed != patch_crc_expected) {
        err = "UPS patch CRC32 checksum mismatch.";
        return false;
    }

    const uint32_t source_crc = read_le32(patch.data() + patch.size() - 12);
    const uint32_t target_crc = read_le32(patch.data() + patch.size() - 8);
    const uint32_t rom_crc = compute_crc32(source_z64);

    if (rom_crc == target_crc && rom_crc != source_crc) {
        already_patched = true;
        out_z64 = source_z64;
        return true;
    }

    if (rom_crc != source_crc) {
        err = "ROM CRC32 does not match UPS patch source CRC32 (expected unaltered Japanese Sin and Punishment ROM).";
        return false;
    }

    size_t patch_pos = 4;
    uint64_t source_size = 0;
    uint64_t target_size = 0;
    if (!decode_bps_varint(patch.data(), patch.size(), patch_pos, footer_offset, source_size) ||
        !decode_bps_varint(patch.data(), patch.size(), patch_pos, footer_offset, target_size)) {
        err = "Failed to parse UPS size varints.";
        return false;
    }

    if (source_size != source_z64.size()) {
        err = "UPS patch expected source size " + std::to_string(source_size) +
              ", but input ROM has size " + std::to_string(source_z64.size()) + ".";
        return false;
    }

    if (target_size < MIN_ROM_SIZE || target_size > MAX_ROM_SIZE) {
        err = "UPS target ROM size (" + std::to_string(target_size) + ") is outside valid range.";
        return false;
    }

    out_z64.assign(static_cast<size_t>(target_size), 0);
    const size_t copy_len = std::min(source_z64.size(), static_cast<size_t>(target_size));
    std::memcpy(out_z64.data(), source_z64.data(), copy_len);

    size_t offset = 0;
    while (patch_pos < footer_offset) {
        uint64_t skip = 0;
        if (!decode_bps_varint(patch.data(), patch.size(), patch_pos, footer_offset, skip)) {
            err = "Failed to decode UPS offset skip.";
            return false;
        }
        offset += static_cast<size_t>(skip);

        while (true) {
            if (patch_pos >= footer_offset) {
                err = "Unexpected end of UPS XOR stream.";
                return false;
            }
            const uint8_t xor_byte = patch[patch_pos++];
            if (xor_byte == 0) {
                offset++;
                break;
            }
            if (offset >= out_z64.size()) {
                err = "UPS patch offset exceeds target bounds.";
                return false;
            }
            out_z64[offset++] ^= xor_byte;
        }
    }

    if (compute_crc32(out_z64) != target_crc) {
        err = "UPS patched ROM target CRC32 verification failed.";
        return false;
    }

    return true;
}

inline bool apply_ips_patch(const std::vector<uint8_t>& source_z64, const std::vector<uint8_t>& patch,
                            std::vector<uint8_t>& out_z64, std::string& err) {
    if (patch.size() < IPS_HEADER_SIZE + 3) {
        err = "IPS patch file is truncated.";
        return false;
    }

    out_z64 = source_z64;
    size_t pos = IPS_HEADER_SIZE;

    while (pos < patch.size()) {
        if (pos + 3 <= patch.size() &&
            patch[pos] == 'E' && patch[pos + 1] == 'O' && patch[pos + 2] == 'F') {
            pos += 3;
            if (pos + 3 == patch.size()) {
                const size_t target_size = (static_cast<size_t>(patch[pos]) << 16) |
                                           (static_cast<size_t>(patch[pos + 1]) << 8) |
                                            static_cast<size_t>(patch[pos + 2]);
                if (target_size < MIN_ROM_SIZE || target_size > MAX_ROM_SIZE) {
                    err = "IPS target truncate size (" + std::to_string(target_size) + ") is outside valid range.";
                    return false;
                }
                out_z64.resize(target_size, 0);
                pos += 3;
            }
            if (pos != patch.size()) {
                err = "IPS patch has trailing garbage bytes after EOF.";
                return false;
            }
            break;
        }

        if (pos + 5 > patch.size()) {
            err = "Truncated IPS record header.";
            return false;
        }

        const size_t offset = (static_cast<size_t>(patch[pos]) << 16) |
                              (static_cast<size_t>(patch[pos + 1]) << 8) |
                               static_cast<size_t>(patch[pos + 2]);
        const size_t size = (static_cast<size_t>(patch[pos + 3]) << 8) |
                             static_cast<size_t>(patch[pos + 4]);
        pos += 5;

        if (size > 0) {
            if (pos + size > patch.size()) {
                err = "Truncated IPS record payload.";
                return false;
            }
            if (offset + size > out_z64.size()) {
                if (offset + size > MAX_ROM_SIZE) {
                    err = "IPS record extends past maximum ROM size.";
                    return false;
                }
                out_z64.resize(offset + size, 0);
            }
            std::memcpy(out_z64.data() + offset, patch.data() + pos, size);
            pos += size;
        } else {
            // RLE record
            if (pos + 3 > patch.size()) {
                err = "Truncated IPS RLE record payload.";
                return false;
            }
            const size_t rle_count = (static_cast<size_t>(patch[pos]) << 8) |
                                      static_cast<size_t>(patch[pos + 1]);
            const uint8_t rle_val = patch[pos + 2];
            pos += 3;

            if (offset + rle_count > out_z64.size()) {
                if (offset + rle_count > MAX_ROM_SIZE) {
                    err = "IPS RLE record extends past maximum ROM size.";
                    return false;
                }
                out_z64.resize(offset + rle_count, 0);
            }
            std::memset(out_z64.data() + offset, rle_val, rle_count);
        }
    }

    return true;
}

inline bool apply_rom_patch(const std::vector<uint8_t>& source_z64, const std::vector<uint8_t>& patch,
                            std::vector<uint8_t>& out_z64, bool& already_patched, std::string& err) {
    already_patched = false;

    bool ok = false;
    if (patch.size() >= 4 && patch[0] == 'B' && patch[1] == 'P' && patch[2] == 'S' && patch[3] == '1') {
        ok = apply_bps_patch(source_z64, patch, out_z64, already_patched, err);
    } else if (patch.size() >= 4 && patch[0] == 'U' && patch[1] == 'P' && patch[2] == 'S' && patch[3] == '1') {
        ok = apply_ups_patch(source_z64, patch, out_z64, already_patched, err);
    } else if (patch.size() >= 5 && patch[0] == 'P' && patch[1] == 'A' && patch[2] == 'T' && patch[3] == 'C' && patch[4] == 'H') {
        ok = apply_ips_patch(source_z64, patch, out_z64, err);
    } else {
        err = "Unsupported patch format (expected BPS1, UPS1, or PATCH header).";
        return false;
    }

    if (!ok) {
        return false;
    }

    const size_t padded = (out_z64.size() + 3) & ~size_t(3);
    if (out_z64.size() < padded) {
        out_z64.resize(padded, 0);
    }

    if (out_z64.size() < 4 ||
        out_z64[0] != Z64_MAGIC[0] || out_z64[1] != Z64_MAGIC[1] ||
        out_z64[2] != Z64_MAGIC[2] || out_z64[3] != Z64_MAGIC[3]) {
        err = "Patched ROM is missing big-endian Nintendo 64 header magic.";
        return false;
    }

    return true;
}

inline bool extract_canonical_code(const std::vector<uint8_t>& z64_rom,
                                   std::vector<uint8_t>& canonical_code,
                                   std::string& game_id,
                                   std::string& err) {
    if (z64_rom.size() < LOAD_TABLE_ROM + SEGMENT_ENTRY_BYTES) {
        err = "ROM is too small to contain Sin and Punishment segment load table.";
        return false;
    }
    if (z64_rom.size() < MAIN_ROM + MAIN_TEXT_SIZE) {
        err = "ROM is too small to contain main text segment.";
        return false;
    }

    canonical_code.clear();
    std::vector<uint8_t> main_code(z64_rom.begin() + MAIN_ROM, z64_rom.begin() + MAIN_ROM + MAIN_TEXT_SIZE);
    for (uint32_t vram : VARYING_WORDS) {
        if (vram < MAIN_VRAM || vram + 4 > MAIN_VRAM + MAIN_TEXT_SIZE) {
            continue;
        }
        std::memset(main_code.data() + (vram - MAIN_VRAM), 0, 4);
    }

    canonical_code.insert(canonical_code.end(), main_code.begin(), main_code.end());

    uint64_t h = FNV1A_OFFSET_BASIS;
    for (uint8_t b : main_code) {
        h = (h ^ b) * FNV1A_PRIME;
    }

    for (size_t p = LOAD_TABLE_ROM; p + SEGMENT_ENTRY_BYTES <= z64_rom.size(); p += SEGMENT_ENTRY_BYTES) {
        canonical_code.insert(canonical_code.end(), z64_rom.data() + p, z64_rom.data() + p + SEGMENT_ENTRY_BYTES);
        for (size_t i = 0; i < SEGMENT_ENTRY_BYTES; ++i) {
            h = (h ^ z64_rom[p + i]) * FNV1A_PRIME;
        }

        const uint32_t seg_rom = read_be32(z64_rom.data() + p);
        const uint32_t seg_vram = read_be32(z64_rom.data() + p + 4);
        const uint32_t text_end = read_be32(z64_rom.data() + p + 8);

        if (seg_rom == 0 || seg_vram == 0) {
            if (h != EXPECTED_CODE_FINGERPRINT) {
                err = "ROM code fingerprint mismatch (expected 0xe941bdc06663d9ad).";
                return false;
            }
            if (z64_rom.size() >= ROM_INTERNAL_NAME_OFFSET + JP_INTERNAL_NAME.size() &&
                std::string_view(reinterpret_cast<const char*>(z64_rom.data()) + ROM_INTERNAL_NAME_OFFSET,
                                 JP_INTERNAL_NAME.size()) == JP_INTERNAL_NAME) {
                game_id = GAME_ID_JP;
            } else {
                game_id = GAME_ID_EN;
            }
            return true;
        }

        if (text_end < seg_vram || static_cast<uint64_t>(seg_rom) + (text_end - seg_vram) > z64_rom.size()) {
            err = "Invalid segment entry in load table.";
            return false;
        }

        const size_t seg_len = text_end - seg_vram;
        canonical_code.insert(canonical_code.end(), z64_rom.data() + seg_rom, z64_rom.data() + seg_rom + seg_len);
        for (size_t i = 0; i < seg_len; ++i) {
            h = (h ^ z64_rom[seg_rom + i]) * FNV1A_PRIME;
        }
    }

    err = "Load table terminator not found.";
    return false;
}

inline uint64_t next_keystream_word(uint64_t& state, const uint8_t* key_data, size_t key_size, size_t word_index) {
    const size_t aligned_key_size = key_size & ~size_t(7);
    const size_t byte_offset = (word_index * sizeof(uint64_t)) % aligned_key_size;
    const uint64_t key_word = read_le64(key_data + byte_offset);

    state += SPLITMIX_GAMMA ^ key_word;
    uint64_t z = state;
    z = (z ^ (z >> 30)) * SPLITMIX_MUL1;
    z = (z ^ (z >> 27)) * SPLITMIX_MUL2;
    return z ^ (z >> 31);
}

inline void crypt_buffer(const std::vector<uint8_t>& key, uint8_t* data, size_t size) {
    if (key.size() < sizeof(uint64_t) || size == 0) {
        return;
    }

    uint64_t state = INITIAL_CIPHER_STATE;
    uint64_t current_word = 0;
    for (size_t i = 0; i < size; ++i) {
        if ((i & 7) == 0) {
            current_word = next_keystream_word(state, key.data(), key.size(), i / 8);
        }
        data[i] ^= static_cast<uint8_t>((current_word >> ((i & 7) * 8)) & 0xFF);
    }
}

inline bool crypt_stream(const std::vector<uint8_t>& key, std::istream& in, FILE* out_fp, std::string& err) {
    if (key.size() < sizeof(uint64_t)) {
        err = "Encryption key size is too small.";
        return false;
    }
    if (!out_fp) {
        err = "Null output file pointer provided.";
        return false;
    }

    uint64_t state = INITIAL_CIPHER_STATE;
    size_t stream_pos = 0;
    std::vector<uint8_t> chunk(STREAM_CHUNK_SIZE);
    uint64_t current_word = 0;

    while (in) {
        in.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
        const size_t bytes_read = static_cast<size_t>(in.gcount());
        if (bytes_read == 0) {
            break;
        }

        for (size_t i = 0; i < bytes_read; ++i) {
            const size_t pos = stream_pos + i;
            if ((pos & 7) == 0) {
                current_word = next_keystream_word(state, key.data(), key.size(), pos / 8);
            }
            chunk[i] ^= static_cast<uint8_t>((current_word >> ((pos & 7) * 8)) & 0xFF);
        }
        stream_pos += bytes_read;

        if (std::fwrite(chunk.data(), 1, bytes_read, out_fp) != bytes_read) {
            err = "Failed writing encrypted/decrypted chunk to output stream.";
            return false;
        }
    }

    std::fflush(out_fp);
    return true;
}

} // namespace snp::patch
