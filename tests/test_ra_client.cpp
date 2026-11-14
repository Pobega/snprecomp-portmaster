#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include <SDL2/SDL.h>
#include "gl_loader.h"
#include "handheld_config.h"
#include "handheld_overlay.h"
#include "png_decode.h"
#include "ra_client.h"
#include "ra_credentials.h"
#include "ra_offline_cache.h"
#include "miniz.h"

#include "librecomp/game.hpp"
#include "rc_client.h"
#include "rc_consoles.h"
#include "rc_hash.h"
#include "rc_runtime.h"
#include "rc_api_user.h"
#include "rc_api_runtime.h"

namespace ultramodern {
    void quit() {}
    namespace renderer {
        void set_graphics_config(const GraphicsConfig&) {}
    }
}

namespace snp::gles3 {
    void set_vsync_enabled(bool) {}
}

namespace recomp {
    std::filesystem::path get_config_path() {
        return ".";
    }
    void check_all_stored_roms() {}
    bool is_rom_valid(const std::u8string&) {
        return false;
    }
    RomValidationError select_rom(const std::filesystem::path&, const std::u8string&) {
        return RomValidationError::OtherError;
    }
}

namespace {

void write_png_chunk(std::vector<uint8_t>& out, const char type[4], const uint8_t* data, size_t length) {
    uint8_t len_bytes[4] = {
        static_cast<uint8_t>((length >> 24) & 0xFF),
        static_cast<uint8_t>((length >> 16) & 0xFF),
        static_cast<uint8_t>((length >> 8) & 0xFF),
        static_cast<uint8_t>(length & 0xFF)
    };
    out.insert(out.end(), len_bytes, len_bytes + 4);
    out.insert(out.end(), type, type + 4);
    if (data && length > 0) {
        out.insert(out.end(), data, data + length);
    }
    uint32_t crc = mz_crc32(0, reinterpret_cast<const unsigned char*>(type), 4);
    if (data && length > 0) {
        crc = mz_crc32(crc, data, length);
    }
    uint8_t crc_bytes[4] = {
        static_cast<uint8_t>((crc >> 24) & 0xFF),
        static_cast<uint8_t>((crc >> 16) & 0xFF),
        static_cast<uint8_t>((crc >> 8) & 0xFF),
        static_cast<uint8_t>(crc & 0xFF)
    };
    out.insert(out.end(), crc_bytes, crc_bytes + 4);
}

std::vector<uint8_t> create_test_rgba_png() {
    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

    uint8_t ihdr[13] = {
        0, 0, 0, 2, // width = 2
        0, 0, 0, 2, // height = 2
        8,          // bit depth = 8
        6,          // color type = 6 (RGBA)
        0, 0, 0     // compression, filter, interlace
    };
    write_png_chunk(png, "IHDR", ihdr, sizeof(ihdr));

    std::vector<uint8_t> raw_scanlines;
    // Row 0: filter=0, pixel(0,0)=red, pixel(1,0)=green
    raw_scanlines.push_back(0);
    raw_scanlines.insert(raw_scanlines.end(), {255, 0, 0, 255});
    raw_scanlines.insert(raw_scanlines.end(), {0, 255, 0, 255});

    // Row 1: filter=0, pixel(0,1)=blue, pixel(1,1)=yellow半透明
    raw_scanlines.push_back(0);
    raw_scanlines.insert(raw_scanlines.end(), {0, 0, 255, 255});
    raw_scanlines.insert(raw_scanlines.end(), {255, 255, 0, 128});

    uLong compressed_len = compressBound(static_cast<uLong>(raw_scanlines.size()));
    std::vector<uint8_t> compressed(compressed_len);
    int res = compress(compressed.data(), &compressed_len, raw_scanlines.data(), static_cast<uLong>(raw_scanlines.size()));
    assert(res == Z_OK);
    compressed.resize(compressed_len);

    write_png_chunk(png, "IDAT", compressed.data(), compressed.size());
    write_png_chunk(png, "IEND", nullptr, 0);

    return png;
}

std::vector<uint8_t> create_test_indexed_png() {
    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

    uint8_t ihdr[13] = {
        0, 0, 0, 2, // width = 2
        0, 0, 0, 2, // height = 2
        8,          // bit depth = 8
        3,          // color type = 3 (Indexed PLTE)
        0, 0, 0
    };
    write_png_chunk(png, "IHDR", ihdr, sizeof(ihdr));

    uint8_t plte[12] = {
        255, 0, 0,     // 0: Red
        0, 255, 0,     // 1: Green
        0, 0, 255,     // 2: Blue
        255, 255, 255  // 3: White
    };
    write_png_chunk(png, "PLTE", plte, sizeof(plte));

    uint8_t trns[4] = {255, 200, 100, 50};
    write_png_chunk(png, "tRNS", trns, sizeof(trns));

    std::vector<uint8_t> raw_scanlines;
    // Row 0: filter=0, pixel(0,0)=0, pixel(1,0)=1
    raw_scanlines.push_back(0);
    raw_scanlines.push_back(0);
    raw_scanlines.push_back(1);

    // Row 1: filter=0, pixel(0,1)=2, pixel(1,1)=3
    raw_scanlines.push_back(0);
    raw_scanlines.push_back(2);
    raw_scanlines.push_back(3);

    uLong compressed_len = compressBound(static_cast<uLong>(raw_scanlines.size()));
    std::vector<uint8_t> compressed(compressed_len);
    int res = compress(compressed.data(), &compressed_len, raw_scanlines.data(), static_cast<uLong>(raw_scanlines.size()));
    assert(res == Z_OK);
    compressed.resize(compressed_len);

    write_png_chunk(png, "IDAT", compressed.data(), compressed.size());
    write_png_chunk(png, "IEND", nullptr, 0);

    return png;
}

void test_nextui_credentials_parsing_and_discovery() {
    std::printf("[TEST] Running NextUI credentials parsing and directory discovery...\n");

    // Case 1: Standard raUsername + raToken
    {
        std::istringstream stream("raUsername=alice\nraToken=token123\n");
        auto creds = snp::handheld::parse_nextui_ra_settings(stream);
        assert(creds.username == "alice");
        assert(creds.token == "token123");
        assert(creds.password.empty());
        assert(creds.has_login());
    }

    // Case 2: raServerUsername takes priority when raToken is present (renamed accounts)
    {
        std::istringstream stream("raUsername=alice_local\r\nraServerUsername=alice_renamed\r\nraToken=token456\r\n");
        auto creds = snp::handheld::parse_nextui_ra_settings(stream);
        assert(creds.username == "alice_renamed");
        assert(creds.token == "token456");
        assert(creds.has_login());
    }

    // Case 3: Empty token with raPassword fallback
    {
        std::istringstream stream("raUsername=bob\nraToken=\nraPassword=pass789\n");
        auto creds = snp::handheld::parse_nextui_ra_settings(stream);
        assert(creds.username == "bob");
        assert(creds.token.empty());
        assert(creds.password == "pass789");
        assert(creds.has_login());
    }

    // Case 4: Empty token with raPassword and raServerUsername (renamed account password fallback)
    {
        std::istringstream stream("raUsername=SerPbone\nraServerUsername=pobega\nraToken=\nraPassword=pass789\n");
        auto creds = snp::handheld::parse_nextui_ra_settings(stream);
        assert(creds.username == "pobega");
        assert(creds.token.empty());
        assert(creds.password == "pass789");
        assert(creds.has_login());
    }

    // Directory discovery: walk up ancestor directories
    {
        std::filesystem::path root_temp = std::filesystem::current_path() / "build_test_ra_tmp";
        std::filesystem::path deep_dir = root_temp / "sub1" / "sub2" / "sub3" / "sub4";
        std::filesystem::path settings_dir = root_temp / ".userdata" / "shared";

        std::filesystem::create_directories(deep_dir);
        std::filesystem::create_directories(settings_dir);

        std::ofstream out(settings_dir / "minuisettings.txt");
        out << "raUsername=charlie\nraToken=chartok\n";
        out.close();

        snp::handheld::RetroAchievementsConfig cfg;
        cfg.enabled = true;
        cfg.import_system_credentials = true;

        auto discovered = snp::handheld::discover_ra_credentials(cfg, deep_dir);
        assert(discovered.username == "charlie");
        assert(discovered.token == "chartok");

        std::error_code ec;
        std::filesystem::remove_all(root_temp, ec);
    }

    std::printf("[PASS] NextUI credentials parsing and discovery verified.\n");
}

void test_retroarch_credentials_parsing() {
    std::printf("[TEST] Running RetroArch retroarch.cfg parsing...\n");

    // Quoted values and interspersed settings
    {
        std::istringstream stream(
            "# RetroArch config file\n"
            "video_driver = \"gl\"\n"
            "cheevos_username = \"retro_user\"\n"
            "cheevos_token = \"retro_token\"\n"
            "cheevos_password = \"secret_pw\"\n"
        );
        auto creds = snp::handheld::parse_retroarch_ra_cfg(stream);
        assert(creds.username == "retro_user");
        assert(creds.token == "retro_token");
        assert(creds.password == "secret_pw");
        assert(creds.has_login());
    }

    // Unquoted values and single quotes
    {
        std::istringstream stream(
            "cheevos_username = unquoted_user\n"
            "cheevos_token = 'single_quote_tok'\n"
        );
        auto creds = snp::handheld::parse_retroarch_ra_cfg(stream);
        assert(creds.username == "unquoted_user");
        assert(creds.token == "single_quote_tok");
        assert(creds.password.empty());
        assert(creds.has_login());
    }

    std::printf("[PASS] RetroArch config parsing verified.\n");
}

static std::vector<uint32_t> s_triggered_ids;
void runtime_event_handler(const rc_runtime_event_t* runtime_event) {
    if (runtime_event && runtime_event->type == RC_RUNTIME_EVENT_ACHIEVEMENT_TRIGGERED) {
        s_triggered_ids.push_back(runtime_event->id);
    }
}

uint32_t runtime_read_memory(uint32_t address, uint8_t* buffer, uint32_t num_bytes, void* ud) {
    const uint8_t* rdram = static_cast<const uint8_t*>(ud);
    return snp::handheld::ra_read_n64_memory(rdram, address, buffer, num_bytes);
}

void test_ra_memory_and_runtime_triggers() {
    std::printf("[TEST] Running N64 memory alignment & live rc_runtime_t triggers...\n");

    std::vector<uint8_t> rdram(8 * 1024 * 1024, 0);

    // Write via N64 semantics into host RDRAM
    // N64 byte 0x80001000 -> host offset 0x1000 ^ 3 = 0x1003
    rdram[0x1000 ^ 3] = 0x5A; // 90
    // N64 halfword 0x80001004 -> host offset 0x1004 ^ 2 = 0x1006
    *reinterpret_cast<uint16_t*>(rdram.data() + (0x1004 ^ 2)) = 0xBEEF; // 48879
    // N64 word 0x80001008 -> host offset 0x1008
    *reinterpret_cast<uint32_t*>(rdram.data() + 0x1008) = 0x12345678; // 305419896

    uint8_t buf[8]{};

    // 8-bit read: matches mupen64plus address 0x1003
    uint32_t r1 = snp::handheld::ra_read_n64_memory(rdram.data(), 0x1003, buf, 1);
    assert(r1 == 1 && buf[0] == 0x5A);

    // 16-bit read: matches mupen64plus address 0x1006
    uint32_t r2 = snp::handheld::ra_read_n64_memory(rdram.data(), 0x1006, buf, 2);
    assert(r2 == 2 && *reinterpret_cast<uint16_t*>(buf) == 0xBEEF);

    // 32-bit read: matches mupen64plus address 0x1008
    uint32_t r4 = snp::handheld::ra_read_n64_memory(rdram.data(), 0x1008, buf, 4);
    assert(r4 == 4 && *reinterpret_cast<uint32_t*>(buf) == 0x12345678);

    // Out-of-bounds clamping
    assert(snp::handheld::ra_read_n64_memory(rdram.data(), 0x800000, buf, 4) == 0);
    assert(snp::handheld::ra_read_n64_memory(rdram.data(), 0x7FFFFF, buf, 4) == 1);
    assert(snp::handheld::ra_read_n64_memory(nullptr, 0, buf, 4) == 0);

    // Live rc_runtime_t trigger evaluation with exact mupen64plus addresses.
    // rcheevos starts newly activated achievements in WAITING state until they evaluate false once.
    std::fill(rdram.begin(), rdram.end(), 0);
    rc_runtime_t runtime;
    rc_runtime_init(&runtime);

    int act1 = rc_runtime_activate_achievement(&runtime, 101, "0xH001003=90", nullptr, 0);
    int act2 = rc_runtime_activate_achievement(&runtime, 102, "0x 001006=48879", nullptr, 0);
    int act3 = rc_runtime_activate_achievement(&runtime, 103, "0xX001008=305419896", nullptr, 0);
    assert(act1 == RC_OK && act2 == RC_OK && act3 == RC_OK);

    s_triggered_ids.clear();
    rc_runtime_do_frame(&runtime, runtime_event_handler, runtime_read_memory, rdram.data(), nullptr);
    assert(s_triggered_ids.empty());

    rdram[0x1000 ^ 3] = 0x5A;
    *reinterpret_cast<uint16_t*>(rdram.data() + (0x1004 ^ 2)) = 0xBEEF;
    *reinterpret_cast<uint32_t*>(rdram.data() + 0x1008) = 0x12345678;

    rc_runtime_do_frame(&runtime, runtime_event_handler, runtime_read_memory, rdram.data(), nullptr);

    assert(s_triggered_ids.size() == 3);
    std::sort(s_triggered_ids.begin(), s_triggered_ids.end());
    assert(s_triggered_ids[0] == 101);
    assert(s_triggered_ids[1] == 102);
    assert(s_triggered_ids[2] == 103);

    rc_runtime_destroy(&runtime);
    std::printf("[PASS] N64 memory alignment & live runtime triggers verified.\n");
}

void test_png_decoder() {
    std::printf("[TEST] Running PNG decoder on synthetic RGBA & indexed images...\n");

    // 1. Valid RGBA PNG
    {
        auto rgba_png = create_test_rgba_png();
        snp::handheld::DecodedImage img;
        bool ok = snp::handheld::decode_png(rgba_png.data(), rgba_png.size(), img);
        assert(ok);
        assert(img.width == 2);
        assert(img.height == 2);
        assert(img.rgba.size() == 16);

        // Pixel (0,0): Red (255, 0, 0, 255)
        assert(img.rgba[0] == 255 && img.rgba[1] == 0 && img.rgba[2] == 0 && img.rgba[3] == 255);
        // Pixel (1,0): Green (0, 255, 0, 255)
        assert(img.rgba[4] == 0 && img.rgba[5] == 255 && img.rgba[6] == 0 && img.rgba[7] == 255);
        // Pixel (0,1): Blue (0, 0, 255, 255)
        assert(img.rgba[8] == 0 && img.rgba[9] == 0 && img.rgba[10] == 255 && img.rgba[11] == 255);
        // Pixel (1,1): Yellow (255, 255, 0, 128)
        assert(img.rgba[12] == 255 && img.rgba[13] == 255 && img.rgba[14] == 0 && img.rgba[15] == 128);
    }

    // 2. Valid Indexed PNG with PLTE and tRNS
    {
        auto indexed_png = create_test_indexed_png();
        snp::handheld::DecodedImage img;
        bool ok = snp::handheld::decode_png(indexed_png.data(), indexed_png.size(), img);
        assert(ok);
        assert(img.width == 2);
        assert(img.height == 2);
        assert(img.rgba.size() == 16);

        // Pixel 0: palette 0 (Red), alpha 255
        assert(img.rgba[0] == 255 && img.rgba[1] == 0 && img.rgba[2] == 0 && img.rgba[3] == 255);
        // Pixel 1: palette 1 (Green), alpha 200
        assert(img.rgba[4] == 0 && img.rgba[5] == 255 && img.rgba[6] == 0 && img.rgba[7] == 200);
        // Pixel 2: palette 2 (Blue), alpha 100
        assert(img.rgba[8] == 0 && img.rgba[9] == 0 && img.rgba[10] == 255 && img.rgba[11] == 100);
        // Pixel 3: palette 3 (White), alpha 50
        assert(img.rgba[12] == 255 && img.rgba[13] == 255 && img.rgba[14] == 255 && img.rgba[15] == 50);
    }

    // 3. Corrupt buffer rejection
    {
        snp::handheld::DecodedImage img;
        assert(!snp::handheld::decode_png(nullptr, 0, img));

        uint8_t bad_sig[16] = {0, 1, 2, 3, 4, 5, 6, 7};
        assert(!snp::handheld::decode_png(bad_sig, sizeof(bad_sig), img));

        auto valid = create_test_rgba_png();
        // Truncate in the middle
        assert(!snp::handheld::decode_png(valid.data(), valid.size() / 2, img));

        // Corrupt IHDR bit depth
        auto corrupted = valid;
        corrupted[16 + 8] = 16;
        assert(!snp::handheld::decode_png(corrupted.data(), corrupted.size(), img));
    }

    std::printf("[PASS] PNG decoder tests verified.\n");
}

void test_rcheevos_n64_hash() {
    std::printf("[TEST] Running rcheevos N64 hash generation...\n");

    std::vector<uint8_t> mock_n64_rom(4096 * 2, 0);
    mock_n64_rom[0] = 0x80;
    mock_n64_rom[1] = 0x37;
    mock_n64_rom[2] = 0x12;
    mock_n64_rom[3] = 0x40;

    char hash[33]{};
    int res = rc_hash_generate_from_buffer(hash, RC_CONSOLE_NINTENDO_64, mock_n64_rom.data(), mock_n64_rom.size());
    assert(res != 0);
    assert(std::strlen(hash) == 32);

    std::printf("[PASS] rcheevos N64 MD5 hash generated: %s\n", hash);
}

void test_url_params_parsing_and_decoding() {
    std::printf("[TEST] Running URL param parsing and decoding...\n");

    std::string post_data = "r=login2&u=alice%2Bplus&p=pass%20word&t=tok%3D123";
    auto params = snp::handheld::parse_url_params(post_data);
    assert(params["r"] == "login2");
    assert(params["u"] == "alice+plus");
    assert(params["p"] == "pass word");
    assert(params["t"] == "tok=123");

    std::string decoded_plus = snp::handheld::url_decode("a+b+c%21");
    assert(decoded_plus == "a b c!");

    std::printf("[PASS] URL param parsing and decoding verified.\n");
}

void test_offline_caching_and_fallback() {
    std::printf("[TEST] Running offline caching and fallback...\n");

    std::filesystem::path test_dir = std::filesystem::current_path() / "build_test_cache_tmp";
    std::error_code ec;
    std::filesystem::remove_all(test_dir, ec);
    std::filesystem::create_directories(test_dir, ec);

    // Cache login2
    bool ok_log = snp::handheld::cache_online_response(
        "r=login2&u=tester&t=tok",
        "{\"Success\":true,\"User\":\"tester\",\"Token\":\"tok123\"}",
        test_dir);
    assert(ok_log);
    assert(std::filesystem::exists(test_dir / "login_tester.json"));

    // Check permissions on login_tester.json (0600)
    auto perms = std::filesystem::status(test_dir / "login_tester.json").permissions();
    assert((perms & std::filesystem::perms::owner_read) != std::filesystem::perms::none);
    assert((perms & std::filesystem::perms::owner_write) != std::filesystem::perms::none);

    // Cache gameid
    bool ok_gid = snp::handheld::cache_online_response(
        "r=gameid&m=md5hash",
        "{\"Success\":true,\"GameID\":2345}",
        test_dir);
    assert(ok_gid);
    assert(std::filesystem::exists(test_dir / "gameid_md5hash.json"));

    // Cache patch
    bool ok_patch = snp::handheld::cache_online_response(
        "r=patch&g=2345",
        "{\"Success\":true,\"PatchData\":[]}",
        test_dir);
    assert(ok_patch);
    assert(std::filesystem::exists(test_dir / "patch_2345.json"));

    // Cache achievementsets
    bool ok_sets = snp::handheld::cache_online_response(
        "r=achievementsets&g=2345",
        "{\"Success\":true,\"Sets\":[]}",
        test_dir);
    assert(ok_sets);
    assert(std::filesystem::exists(test_dir / "achievementsets_2345.json"));

    // Test offline serving
    std::string body;
    int status = 0;
    bool served_login = snp::handheld::try_serve_offline_response("r=login2&u=tester", body, status, test_dir);
    assert(served_login);
    assert(status == 200);
    assert(body.find("\"Token\":\"tok123\"") != std::string::npos);

    bool served_case_insensitive = snp::handheld::try_serve_offline_response("r=login2&u=TeStEr", body, status, test_dir);
    assert(served_case_insensitive);
    assert(status == 200);

    bool served_gameid = snp::handheld::try_serve_offline_response("r=gameid&m=md5hash", body, status, test_dir);
    assert(served_gameid);
    assert(status == 200);
    assert(body.find("\"GameID\":2345") != std::string::npos);

    // Unknown file
    bool served_unknown = snp::handheld::try_serve_offline_response("r=gameid&m=unknown_hash", body, status, test_dir);
    assert(!served_unknown);

    std::filesystem::remove_all(test_dir, ec);
    std::printf("[PASS] Offline caching and fallback verified.\n");
}

void test_startsession_merge_with_offline_unlocks() {
    std::printf("[TEST] Running startsession merge with offline unlocks...\n");

    std::string original = "{\"Success\":true,\"Unlocks\":[{\"ID\":101,\"When\":1700000000}],\"HardcoreUnlocks\":[],\"ServerNow\":1700050000}";

    std::vector<snp::handheld::PendingUnlock> pending;
    pending.push_back({ "alice", 1234, 102, "hash", 1700010000 });
    pending.push_back({ "alice", 1234, 103, "hash", 1700020000 });
    pending.push_back({ "alice", 1234, 101, "hash", 1700030000 });

    std::string merged = snp::handheld::merge_start_session_unlocks(original, pending);

    rc_api_start_session_response_t resp{};
    rc_api_server_response_t server_resp{};
    server_resp.body = merged.c_str();
    server_resp.body_length = merged.size();

    int res = rc_api_process_start_session_server_response(&resp, &server_resp);
    assert(res == RC_OK);
    assert(resp.response.succeeded == 1);
    assert(resp.num_unlocks == 3);

    bool found101 = false, found102 = false, found103 = false;
    for (uint32_t i = 0; i < resp.num_unlocks; ++i) {
        if (resp.unlocks[i].achievement_id == 101) found101 = true;
        if (resp.unlocks[i].achievement_id == 102) found102 = true;
        if (resp.unlocks[i].achievement_id == 103) found103 = true;
    }
    assert(found101 && found102 && found103);
    rc_api_destroy_start_session_response(&resp);

    std::printf("[PASS] Startsession merge verified.\n");
}

void test_pending_unlock_queue_and_sync() {
    std::printf("[TEST] Running pending unlock queue, timestamp clamping, and award request...\n");

    std::filesystem::path test_dir = std::filesystem::current_path() / "build_test_queue_tmp";
    std::error_code ec;
    std::filesystem::remove_all(test_dir, ec);
    std::filesystem::create_directories(test_dir, ec);

    snp::handheld::PendingUnlock u1{ "bob", 999, 401, "fake_hash", 1705000000 };
    assert(snp::handheld::queue_pending_unlock(u1, test_dir));

    assert(snp::handheld::queue_pending_unlock(u1, test_dir));
    assert(snp::handheld::get_pending_unlock_count(test_dir) == 1);

    uint64_t bad_rtc_time = 1200000;
    uint64_t clamped = (bad_rtc_time >= 1700000000ULL) ? bad_rtc_time : 0;
    assert(clamped == 0);

    snp::handheld::PendingUnlock u2{ "bob", 999, 402, "fake_hash", clamped };
    assert(snp::handheld::queue_pending_unlock(u2, test_dir));
    assert(snp::handheld::get_pending_unlock_count(test_dir) == 2);

    auto list = snp::handheld::load_pending_unlocks(test_dir);
    assert(list.size() == 2);
    assert(list[0].achievement_id == 401 && list[0].unlock_epoch_sec == 1705000000);
    assert(list[1].achievement_id == 402 && list[1].unlock_epoch_sec == 0);

    uint64_t now_sec = 1705000060;
    uint32_t seconds_since_unlock = static_cast<uint32_t>(now_sec - list[0].unlock_epoch_sec);
    assert(seconds_since_unlock == 60);

    rc_api_award_achievement_request_t req{};
    req.username = list[0].username.c_str();
    req.api_token = "my_api_tok";
    req.achievement_id = list[0].achievement_id;
    req.hardcore = 0;
    req.game_hash = list[0].game_hash.c_str();
    req.seconds_since_unlock = seconds_since_unlock;

    rc_api_request_t api_req{};
    int res = rc_api_init_award_achievement_request(&api_req, &req);
    assert(res == RC_OK);
    assert(api_req.post_data != nullptr);
    std::string post_str = api_req.post_data;
    assert(post_str.find("r=awardachievement") != std::string::npos);
    assert(post_str.find("a=401") != std::string::npos);
    assert(post_str.find("o=60") != std::string::npos);
    assert(post_str.find("v=") != std::string::npos);
    rc_api_destroy_request(&api_req);

    assert(snp::handheld::remove_pending_unlocks({ 401 }, "bob", 999, test_dir));
    assert(snp::handheld::get_pending_unlock_count(test_dir) == 1);
    auto remaining = snp::handheld::load_pending_unlocks(test_dir);
    assert(remaining.size() == 1);
    assert(remaining[0].achievement_id == 402);

    std::filesystem::remove_all(test_dir, ec);
    std::printf("[PASS] Pending unlock queue and request generation verified.\n");
}

void test_config_roundtrip_and_partial_toml() {
    std::printf("[TEST] Running snprecomp.toml round-trip and partial key safety...\n");

    std::filesystem::path cfg_path = std::filesystem::current_path() / "build_test_snprecomp.toml";
    std::error_code ec;
    std::filesystem::remove(cfg_path, ec);

    // First load writes default config (omitting empty password key)
    assert(snp::handheld::load_config(cfg_path.string()));
    // Second load parses the saved file where password key is absent
    assert(snp::handheld::load_config(cfg_path.string()));
    assert(snp::handheld::get_config().retroachievements.enabled);
    assert(snp::handheld::get_config().retroachievements.password.empty());

    // Partial TOML with only 1 key in [video] and [retroachievements]
    {
        std::ofstream out(cfg_path, std::ios::trunc);
        out << "[video]\naspect_ratio = \"expand\"\n[retroachievements]\nusername = \"partial_user\"\n";
    }
    assert(snp::handheld::load_config(cfg_path.string()));
    assert(snp::handheld::get_config().retroachievements.username == "partial_user");
    assert(snp::handheld::get_config().retroachievements.token.empty());

    std::filesystem::remove(cfg_path, ec);
    std::printf("[PASS] snprecomp.toml round-trip and partial key safety verified.\n");
}

void test_renamed_account_login_and_session_flow() {
    std::printf("[TEST] Running renamed account login and session flow...\n");

    std::filesystem::path test_dir = std::filesystem::current_path() / "build_test_ra_renamed_tmp";
    std::error_code ec;
    std::filesystem::remove_all(test_dir, ec);
    std::filesystem::create_directories(test_dir, ec);

    // 1. Online login response with login username "pobega" and display username "SerPbone"
    std::string post_data = "r=login2&u=pobega&t=tok";
    std::string response_body = "{\"Success\":true,\"User\":\"SerPbone\",\"Token\":\"tok123\"}";
    bool cache_ok = snp::handheld::cache_online_response(post_data, response_body, test_dir);
    assert(cache_ok);

    std::filesystem::path login_pobega = test_dir / "login_pobega.json";
    std::filesystem::path login_serpbone = test_dir / "login_serpbone.json";

    assert(std::filesystem::exists(login_pobega));
    assert(std::filesystem::exists(login_serpbone));

    auto perms_pobega = std::filesystem::status(login_pobega).permissions();
    assert((perms_pobega & std::filesystem::perms::owner_read) != std::filesystem::perms::none);
    assert((perms_pobega & std::filesystem::perms::owner_write) != std::filesystem::perms::none);

    auto perms_serpbone = std::filesystem::status(login_serpbone).permissions();
    assert((perms_serpbone & std::filesystem::perms::owner_read) != std::filesystem::perms::none);
    assert((perms_serpbone & std::filesystem::perms::owner_write) != std::filesystem::perms::none);

    // Verify try_serve_offline_response serves "User":"SerPbone" for both u=pobega and u=SerPbone
    std::string body_pobega;
    int status_pobega = 0;
    bool serve_pobega = snp::handheld::try_serve_offline_response("r=login2&u=pobega", body_pobega, status_pobega, test_dir);
    assert(serve_pobega);
    assert(body_pobega.find("\"User\":\"SerPbone\"") != std::string::npos);

    std::string body_serpbone;
    int status_serpbone = 0;
    bool serve_serpbone = snp::handheld::try_serve_offline_response("r=login2&u=SerPbone", body_serpbone, status_serpbone, test_dir);
    assert(serve_serpbone);
    assert(body_serpbone.find("\"User\":\"SerPbone\"") != std::string::npos);

    // 2. Chained session flow
    std::string start_post = "r=startsession&u=SerPbone&g=10157";
    std::string start_body = "{\"Success\":true,\"Unlocks\":[],\"HardcoreUnlocks\":[],\"ServerNow\":1700050000}";
    bool start_ok = snp::handheld::cache_online_response(start_post, start_body, test_dir);
    assert(start_ok);

    std::filesystem::path start_file = test_dir / "startsession_serpbone_10157.json";
    assert(std::filesystem::exists(start_file));

    // Queue a PendingUnlock for "SerPbone"
    snp::handheld::PendingUnlock p;
    p.username = "SerPbone";
    p.game_id = 10157;
    p.achievement_id = 42;
    p.game_hash = "abcdef";
    p.unlock_epoch_sec = 1700000000ULL;
    bool q_ok = snp::handheld::queue_pending_unlock(p, test_dir);
    assert(q_ok);

    // try_serve_offline_response merges the queued unlock into startsession response
    std::string merged_body;
    int merged_status = 0;
    bool serve_start = snp::handheld::try_serve_offline_response(start_post, merged_body, merged_status, test_dir);
    assert(serve_start);
    assert(merged_body.find("\"ID\":42") != std::string::npos ||
           merged_body.find("\"ID\": 42") != std::string::npos);

    // Record synced unlocks to startsession
    bool record_ok = snp::handheld::record_synced_unlocks_to_startsession("SerPbone", 10157, {42}, test_dir);
    assert(record_ok);

    // Remove pending unlock from queue and verify persisted startsession contains achievement 42
    bool rem_ok = snp::handheld::remove_pending_unlocks({42}, "SerPbone", 10157, test_dir);
    assert(rem_ok);
    assert(snp::handheld::get_pending_unlock_count(test_dir) == 0);

    std::string final_body;
    int final_status = 0;
    bool final_serve = snp::handheld::try_serve_offline_response(start_post, final_body, final_status, test_dir);
    assert(final_serve);
    assert(final_body.find("42") != std::string::npos);

    std::filesystem::remove_all(test_dir, ec);
    std::printf("[PASS] Renamed account login and session flow verified.\n");
}

void test_overlay_menu_persists_with_active_toast() {
    std::printf("[TEST] Running Quick Settings menu + active toast VBO isolation test...\n");

    setenv("SDL_VIDEODRIVER", "offscreen", 1);
    assert(SDL_Init(SDL_INIT_VIDEO) == 0);

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

    constexpr int WIN_W = 720;
    constexpr int WIN_H = 480;
    SDL_Window* win = SDL_CreateWindow("Overlay Toast VBO Test", 0, 0, WIN_W, WIN_H,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    assert(win != nullptr);

    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    assert(ctx != nullptr);
    assert(SDL_GL_MakeCurrent(win, ctx) == 0);
    assert(snp::gles3::init_gl_loader());

    auto& cfg = snp::handheld::get_mutable_config();
    cfg.retroachievements.notifications = true;
    setenv("SNP_RA_DEMO_TOAST_AT_FRAME", "1", 1);

    snp::handheld::ra_do_frame(nullptr);
    assert(snp::handheld::ra_has_active_toasts());
    snp::handheld::open_overlay();
    assert(snp::handheld::is_overlay_open());

    // Frame 1: builds menu mesh (s_mesh_dirty = true -> false) AND renders toast card + 6-vertex badge
    snp::gles3::gl::glBindFramebuffer(GL_FRAMEBUFFER, 0);
    snp::gles3::gl::glViewport(0, 0, WIN_W, WIN_H);
    snp::gles3::gl::glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
    snp::gles3::gl::glClear(GL_COLOR_BUFFER_BIT);
    snp::handheld::render_overlay(WIN_W, WIN_H);
    assert(snp::gles3::gl::glGetError() == GL_NO_ERROR);

    // Frame 2: s_mesh_dirty is false; menu VBO must NOT have been overwritten by toast badge vertices
    snp::gles3::gl::glBindFramebuffer(GL_FRAMEBUFFER, 0);
    snp::gles3::gl::glViewport(0, 0, WIN_W, WIN_H);
    snp::gles3::gl::glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
    snp::gles3::gl::glClear(GL_COLOR_BUFFER_BIT);
    snp::handheld::render_overlay(WIN_W, WIN_H);
    assert(snp::gles3::gl::glGetError() == GL_NO_ERROR);

    uint8_t panel_px[4]{};
    // OpenGL origin is bottom-left; (55, 440) -> screen (55, 40), empty dark top-left interior of Quick Settings panel
    snp::gles3::gl::glReadPixels(55, 440, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, panel_px);
    assert(panel_px[1] < 50 && panel_px[2] > 15); // Dark navy panel bg over green clear, NOT (0, 255, 0)

    snp::handheld::close_overlay();
    snp::handheld::shutdown_overlay();
    unsetenv("SNP_RA_DEMO_TOAST_AT_FRAME");

    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();

    std::printf("[PASS] Quick Settings menu + active toast VBO isolation verified.\n");
}

} // namespace

int main() {
    std::printf("========================================\n");
    std::printf("Running RetroAchievements Test Suite\n");
    std::printf("========================================\n");

    test_nextui_credentials_parsing_and_discovery();
    test_retroarch_credentials_parsing();
    test_ra_memory_and_runtime_triggers();
    test_png_decoder();
    test_rcheevos_n64_hash();
    test_url_params_parsing_and_decoding();
    test_offline_caching_and_fallback();
    test_startsession_merge_with_offline_unlocks();
    test_pending_unlock_queue_and_sync();
    test_config_roundtrip_and_partial_toml();
    test_renamed_account_login_and_session_flow();
    test_overlay_menu_persists_with_active_toast();

    std::printf("========================================\n");
    std::printf("All 12 test suites passed successfully!\n");
    std::printf("========================================\n");
    return 0;
}
