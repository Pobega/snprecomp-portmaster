#include <cassert>
#include <cstdlib>
#include <iostream>
#include <vector>

#include "snp_patch_lib.h"

namespace {

void encode_varint(uint64_t val, std::vector<uint8_t>& out) {
    while (true) {
        uint8_t b = static_cast<uint8_t>(val & 0x7F);
        val >>= 7;
        if (val == 0) {
            out.push_back(b | 0x80);
            break;
        }
        out.push_back(b);
        val -= 1;
    }
}

void append_le32(uint32_t val, std::vector<uint8_t>& out) {
    out.push_back(static_cast<uint8_t>(val & 0xFF));
    out.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
}

void test_endianness_normalization() {
    std::cout << "[TEST] 1. Endianness normalization and header/size validation...\n";

    const size_t test_size = 16 * 1024 * 1024;
    std::vector<uint8_t> base_rom(test_size, 0);

    // 1a. Native .z64
    base_rom[0] = snp::patch::Z64_MAGIC[0];
    base_rom[1] = snp::patch::Z64_MAGIC[1];
    base_rom[2] = snp::patch::Z64_MAGIC[2];
    base_rom[3] = snp::patch::Z64_MAGIC[3];
    base_rom[4] = 0xAA;
    base_rom[5] = 0xBB;
    base_rom[6] = 0xCC;
    base_rom[7] = 0xDD;

    std::vector<uint8_t> out_z64;
    std::string err;
    assert(snp::patch::normalize_rom_to_z64(base_rom, out_z64, err));
    assert(out_z64 == base_rom);

    // 1b. Byte-swapped .v64
    std::vector<uint8_t> v64_rom = base_rom;
    v64_rom[0] = snp::patch::V64_MAGIC[0];
    v64_rom[1] = snp::patch::V64_MAGIC[1];
    v64_rom[2] = snp::patch::V64_MAGIC[2];
    v64_rom[3] = snp::patch::V64_MAGIC[3];
    v64_rom[4] = 0xBB;
    v64_rom[5] = 0xAA;
    v64_rom[6] = 0xDD;
    v64_rom[7] = 0xCC;

    assert(snp::patch::normalize_rom_to_z64(v64_rom, out_z64, err));
    assert(out_z64[0] == snp::patch::Z64_MAGIC[0]);
    assert(out_z64[1] == snp::patch::Z64_MAGIC[1]);
    assert(out_z64[2] == snp::patch::Z64_MAGIC[2]);
    assert(out_z64[3] == snp::patch::Z64_MAGIC[3]);
    assert(out_z64[4] == 0xAA);
    assert(out_z64[5] == 0xBB);
    assert(out_z64[6] == 0xCC);
    assert(out_z64[7] == 0xDD);

    // 1c. Word-swapped .n64
    std::vector<uint8_t> n64_rom = base_rom;
    n64_rom[0] = snp::patch::N64_MAGIC[0];
    n64_rom[1] = snp::patch::N64_MAGIC[1];
    n64_rom[2] = snp::patch::N64_MAGIC[2];
    n64_rom[3] = snp::patch::N64_MAGIC[3];
    n64_rom[4] = 0xDD;
    n64_rom[5] = 0xCC;
    n64_rom[6] = 0xBB;
    n64_rom[7] = 0xAA;

    assert(snp::patch::normalize_rom_to_z64(n64_rom, out_z64, err));
    assert(out_z64[0] == snp::patch::Z64_MAGIC[0]);
    assert(out_z64[1] == snp::patch::Z64_MAGIC[1]);
    assert(out_z64[2] == snp::patch::Z64_MAGIC[2]);
    assert(out_z64[3] == snp::patch::Z64_MAGIC[3]);
    assert(out_z64[4] == 0xAA);
    assert(out_z64[5] == 0xBB);
    assert(out_z64[6] == 0xCC);
    assert(out_z64[7] == 0xDD);

    // 1d. Size bounds checks
    std::vector<uint8_t> small_rom(15 * 1024 * 1024, 0);
    assert(!snp::patch::normalize_rom_to_z64(small_rom, out_z64, err));

    // 1e. Invalid header magic
    std::vector<uint8_t> bad_magic_rom(test_size, 0);
    bad_magic_rom[0] = 0x00;
    assert(!snp::patch::normalize_rom_to_z64(bad_magic_rom, out_z64, err));

    std::cout << "  Passed endianness normalization tests.\n";
}

void test_synthetic_jp_en_cross_compatibility(const std::filesystem::path& rom_path) {
    std::cout << "[TEST] 2 & 3. Synthetic JP vs EN code extraction and keystream equivalence...\n";

    if (!std::filesystem::exists(rom_path)) {
        std::cout << "  Notice: " << rom_path << " not found, skipping real ROM derivative test.\n";
        return;
    }

    std::vector<uint8_t> base_rom;
    std::string err;
    assert(snp::patch::read_file_bytes(rom_path, base_rom, err));
    assert(base_rom.size() >= snp::patch::MIN_ROM_SIZE);

    // Create synthetic JP release from base ROM
    std::vector<uint8_t> jp_rom = base_rom;
    const std::string_view jp_title = "TSUMI TO BATSU";
    std::memcpy(jp_rom.data() + 0x20, jp_title.data(), jp_title.size());

    // Mutate varying words in JP
    for (size_t i = 0; i < snp::patch::VARYING_WORDS.size(); ++i) {
        uint32_t vram = snp::patch::VARYING_WORDS[i];
        size_t off = snp::patch::MAIN_ROM + (vram - snp::patch::MAIN_VRAM);
        uint32_t fake_val = 0x12340000u + static_cast<uint32_t>(i);
        jp_rom[off] = static_cast<uint8_t>(fake_val >> 24);
        jp_rom[off + 1] = static_cast<uint8_t>(fake_val >> 16);
        jp_rom[off + 2] = static_cast<uint8_t>(fake_val >> 8);
        jp_rom[off + 3] = static_cast<uint8_t>(fake_val);
    }
    // Mutate non-code assets in JP
    if (jp_rom.size() > 0x800000) {
        jp_rom[0x800000] ^= 0xFF;
    }

    // Create synthetic EN release from base ROM
    std::vector<uint8_t> en_rom = base_rom;
    const std::string_view en_title = "Sin and Punishment";
    std::memcpy(en_rom.data() + 0x20, en_title.data(), en_title.size());

    // Mutate varying words differently in EN
    for (size_t i = 0; i < snp::patch::VARYING_WORDS.size(); ++i) {
        uint32_t vram = snp::patch::VARYING_WORDS[i];
        size_t off = snp::patch::MAIN_ROM + (vram - snp::patch::MAIN_VRAM);
        uint32_t fake_val = 0xABCD0000u + static_cast<uint32_t>(i);
        en_rom[off] = static_cast<uint8_t>(fake_val >> 24);
        en_rom[off + 1] = static_cast<uint8_t>(fake_val >> 16);
        en_rom[off + 2] = static_cast<uint8_t>(fake_val >> 8);
        en_rom[off + 3] = static_cast<uint8_t>(fake_val);
    }
    // Mutate non-code assets in EN
    if (en_rom.size() > 0x800000) {
        en_rom[0x800000] ^= 0x55;
    }

    std::vector<uint8_t> jp_canonical_code;
    std::string jp_game_id;
    assert(snp::patch::extract_canonical_code(jp_rom, jp_canonical_code, jp_game_id, err));
    assert(jp_game_id == snp::patch::GAME_ID_JP);

    std::vector<uint8_t> en_canonical_code;
    std::string en_game_id;
    assert(snp::patch::extract_canonical_code(en_rom, en_canonical_code, en_game_id, err));
    assert(en_game_id == snp::patch::GAME_ID_EN);

    // Verify canonical code buffers are byte-for-byte identical
    assert(jp_canonical_code.size() == en_canonical_code.size());
    assert(jp_canonical_code == en_canonical_code);

    // Test cross-ROM cipher encryption/decryption
    const std::string original_text = "PortMaster Sin and Punishment clean-room ROM-keyed patch!";
    std::vector<uint8_t> cipher_buffer(original_text.begin(), original_text.end());

    // Encrypt with JP canonical code
    snp::patch::crypt_buffer(jp_canonical_code, cipher_buffer.data(), cipher_buffer.size());
    assert(std::string_view(reinterpret_cast<const char*>(cipher_buffer.data()), cipher_buffer.size()) != original_text);

    // Decrypt with EN canonical code
    snp::patch::crypt_buffer(en_canonical_code, cipher_buffer.data(), cipher_buffer.size());
    assert(std::string_view(reinterpret_cast<const char*>(cipher_buffer.data()), cipher_buffer.size()) == original_text);

    // 3. Test 1-bit flip in non-varying code segment fails fingerprint verification
    std::vector<uint8_t> corrupt_rom = base_rom;
    corrupt_rom[snp::patch::MAIN_ROM + 0x40] ^= 1;
    std::vector<uint8_t> bad_code;
    std::string bad_id;
    assert(!snp::patch::extract_canonical_code(corrupt_rom, bad_code, bad_id, err));
    assert(err.find("fingerprint mismatch") != std::string::npos);

    std::cout << "  Passed synthetic JP vs EN equivalence and 1-bit tamper detection tests.\n";
}

void test_bps_patching() {
    std::cout << "[TEST] 4. BPS patch application, RLE copy, and verification...\n";

    const size_t rom_size = 16 * 1024 * 1024;
    std::vector<uint8_t> src(rom_size, 0x11);
    src[0] = snp::patch::Z64_MAGIC[0];
    src[1] = snp::patch::Z64_MAGIC[1];
    src[2] = snp::patch::Z64_MAGIC[2];
    src[3] = snp::patch::Z64_MAGIC[3];
    for (size_t i = 1000; i < 2000; ++i) {
        src[i] = static_cast<uint8_t>(i & 0xFF);
    }

    std::vector<uint8_t> tgt = src;
    // Target Read chunk at offset 500
    for (size_t i = 500; i < 600; ++i) {
        tgt[i] = 0x42;
    }
    // Source Copy chunk at offset 600 (copy from offset 1200)
    for (size_t i = 0; i < 200; ++i) {
        tgt[600 + i] = src[1200 + i];
    }
    // Target Copy (overlapping RLE) at offset 800: write 0x99, then repeat 50 times
    tgt[800] = 0x99;
    for (size_t i = 1; i <= 50; ++i) {
        tgt[800 + i] = 0x99;
    }

    std::vector<uint8_t> patch;
    patch.push_back('B');
    patch.push_back('P');
    patch.push_back('S');
    patch.push_back('1');

    encode_varint(src.size(), patch);
    encode_varint(tgt.size(), patch);
    encode_varint(0, patch); // metadata size = 0

    // Command 1: SourceRead (length = 500)
    encode_varint(((500 - 1) << 2) | 0, patch);

    // Command 2: TargetRead (length = 100)
    encode_varint(((100 - 1) << 2) | 1, patch);
    for (size_t i = 0; i < 100; ++i) {
        patch.push_back(0x42);
    }

    // Command 3: SourceCopy (length = 200, delta = +1200)
    encode_varint(((200 - 1) << 2) | 2, patch);
    encode_varint(1200u << 1, patch);

    // Command 4: TargetRead (length = 1, byte = 0x99)
    encode_varint(((1 - 1) << 2) | 1, patch);
    patch.push_back(0x99);

    // Command 5: TargetCopy overlapping (length = 50, delta from 0 is +800)
    encode_varint(((50 - 1) << 2) | 3, patch);
    encode_varint(800u << 1, patch); // target_rel_offset becomes 0 + 800 = 800

    // Command 6: SourceRead remainder
    const size_t rem = tgt.size() - 851;
    encode_varint(((rem - 1) << 2) | 0, patch);

    // Footer
    const uint32_t src_crc = snp::patch::compute_crc32(src);
    const uint32_t tgt_crc = snp::patch::compute_crc32(tgt);
    append_le32(src_crc, patch);
    append_le32(tgt_crc, patch);

    const uint32_t patch_crc = snp::patch::compute_crc32(patch);
    append_le32(patch_crc, patch);

    // Apply patch
    std::vector<uint8_t> out;
    bool already_patched = false;
    std::string err;
    bool ok = snp::patch::apply_rom_patch(src, patch, out, already_patched, err);
    if (!ok) {
        std::cerr << "UPS error: " << err << "\n";
    }
    assert(ok);
    assert(!already_patched);
    assert(out == tgt);

    // Test idempotency
    assert(snp::patch::apply_rom_patch(tgt, patch, out, already_patched, err));
    assert(already_patched);
    assert(out == tgt);

    // Test CRC mismatch
    std::vector<uint8_t> corrupted_src = src;
    corrupted_src[10] ^= 1;
    assert(!snp::patch::apply_rom_patch(corrupted_src, patch, out, already_patched, err));
    assert(err.find("CRC32 does not match") != std::string::npos);

    std::cout << "  Passed BPS patching tests.\n";
}

void test_ups_patching() {
    std::cout << "[TEST] 5. UPS patch application and idempotency...\n";

    const size_t rom_size = 16 * 1024 * 1024;
    std::vector<uint8_t> src(rom_size, 0x22);
    src[0] = snp::patch::Z64_MAGIC[0];
    src[1] = snp::patch::Z64_MAGIC[1];
    src[2] = snp::patch::Z64_MAGIC[2];
    src[3] = snp::patch::Z64_MAGIC[3];

    std::vector<uint8_t> tgt = src;
    tgt[100] ^= 0x5A;
    tgt[500] ^= 0x3C;

    std::vector<uint8_t> patch;
    patch.push_back('U');
    patch.push_back('P');
    patch.push_back('S');
    patch.push_back('1');

    encode_varint(src.size(), patch);
    encode_varint(tgt.size(), patch);

    // Block 1: skip 100, XOR 0x5A, terminator 0x00 (offset advances to 102)
    encode_varint(100, patch);
    patch.push_back(0x5A);
    patch.push_back(0x00);

    // Block 2: offset is now 102. To reach 500, skip is 500 - 102 = 398.
    encode_varint(398, patch);
    patch.push_back(0x3C);
    patch.push_back(0x00);

    // Footer
    const uint32_t src_crc = snp::patch::compute_crc32(src);
    const uint32_t tgt_crc = snp::patch::compute_crc32(tgt);
    append_le32(src_crc, patch);
    append_le32(tgt_crc, patch);

    const uint32_t patch_crc = snp::patch::compute_crc32(patch);
    append_le32(patch_crc, patch);

    // Apply patch
    std::vector<uint8_t> out;
    bool already_patched = false;
    std::string err;
    bool ok = snp::patch::apply_rom_patch(src, patch, out, already_patched, err);
    if (!ok) {
        std::cerr << "UPS error: " << err << "\n";
    }
    assert(ok);
    assert(!already_patched);
    assert(out == tgt);

    // Test idempotency (ensure XOR is NOT reapplied)
    assert(snp::patch::apply_rom_patch(tgt, patch, out, already_patched, err));
    assert(already_patched);
    assert(out == tgt);

    // Test CRC mismatch
    std::vector<uint8_t> corrupted_src = src;
    corrupted_src[10] ^= 1;
    assert(!snp::patch::apply_rom_patch(corrupted_src, patch, out, already_patched, err));
    assert(err.find("CRC32 does not match") != std::string::npos);

    std::cout << "  Passed UPS patching tests.\n";
}

void test_ips_patching() {
    std::cout << "[TEST] 6. IPS patch literal and RLE application...\n";

    const size_t rom_size = 16 * 1024 * 1024;
    std::vector<uint8_t> src(rom_size, 0x33);
    src[0] = snp::patch::Z64_MAGIC[0];
    src[1] = snp::patch::Z64_MAGIC[1];
    src[2] = snp::patch::Z64_MAGIC[2];
    src[3] = snp::patch::Z64_MAGIC[3];

    std::vector<uint8_t> patch;
    patch.push_back('P');
    patch.push_back('A');
    patch.push_back('T');
    patch.push_back('C');
    patch.push_back('H');

    // Literal record at offset 0x000100, length 4: 0xDE, 0xAD, 0xBE, 0xEF
    patch.push_back(0x00);
    patch.push_back(0x01);
    patch.push_back(0x00);
    patch.push_back(0x00);
    patch.push_back(0x04);
    patch.push_back(0xDE);
    patch.push_back(0xAD);
    patch.push_back(0xBE);
    patch.push_back(0xEF);

    // RLE record at offset 0x000200, length 0, rle_count 8, val 0x77
    patch.push_back(0x00);
    patch.push_back(0x02);
    patch.push_back(0x00);
    patch.push_back(0x00);
    patch.push_back(0x00); // size = 0 indicates RLE
    patch.push_back(0x00);
    patch.push_back(0x08); // count = 8
    patch.push_back(0x77); // val

    // EOF
    patch.push_back('E');
    patch.push_back('O');
    patch.push_back('F');

    std::vector<uint8_t> out;
    bool already_patched = false;
    std::string err;
    assert(snp::patch::apply_rom_patch(src, patch, out, already_patched, err));
    assert(out[0x100] == 0xDE);
    assert(out[0x101] == 0xAD);
    assert(out[0x102] == 0xBE);
    assert(out[0x103] == 0xEF);
    for (size_t i = 0; i < 8; ++i) {
        assert(out[0x200 + i] == 0x77);
    }

    std::cout << "  Passed IPS patching tests.\n";
}

void test_real_rom_fingerprint(const std::filesystem::path& rom_path) {
    std::cout << "[TEST] 7. Real ROM verification on disk...\n";

    if (!std::filesystem::exists(rom_path)) {
        std::cout << "  Notice: " << rom_path << " not found, skipping real ROM disk check.\n";
        return;
    }

    std::vector<uint8_t> raw_rom;
    std::string err;
    assert(snp::patch::read_file_bytes(rom_path, raw_rom, err));

    std::vector<uint8_t> z64_rom;
    assert(snp::patch::normalize_rom_to_z64(raw_rom, z64_rom, err));

    std::vector<uint8_t> code;
    std::string game_id;
    assert(snp::patch::extract_canonical_code(z64_rom, code, game_id, err));
    assert(game_id == snp::patch::GAME_ID_EN);
    assert(!code.empty());

    std::cout << "  Passed real ROM fingerprint check (" << game_id << ", " << code.size() << " bytes canonical code).\n";
}

} // namespace

int main() {
    std::cout << "=== Running snp_patch Unit & Integration Tests ===\n";

    test_endianness_normalization();

    const std::filesystem::path real_rom_path = "snp.n64.en.fan.z64";
    test_synthetic_jp_en_cross_compatibility(real_rom_path);

    test_bps_patching();
    test_ups_patching();
    test_ips_patching();

    test_real_rom_fingerprint(real_rom_path);

    std::cout << "All snp_patch tests passed successfully!\n";
    return 0;
}
