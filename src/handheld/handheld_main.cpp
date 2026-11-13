#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <SDL2/SDL.h>

#include "handheld_config.h"
#include "handheld_input.h"
#include "handheld_overlay.h"
#include "rom_loader.h"

#include "librecomp/game.hpp"
#include "librecomp/helpers.hpp"
#include "librecomp/sections.h"
#include "snp_recomp_hooks.h"
#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"

#if __has_include("../gles3/gles3_renderer_context.h")
#include "../gles3/gles3_renderer_context.h"
#else
namespace snp::gles3 {
std::unique_ptr<ultramodern::renderer::RendererContext> create_renderer_context(
    uint8_t* rdram,
    ultramodern::renderer::WindowHandle window_handle,
    bool developer_mode);
} // namespace snp::gles3
#endif

// Forward declaration matching lib-patches/N64ModernRuntime/0002-rom-check.patch
namespace recomp {
using RomCheck = bool (*)(const GameEntry& game, const std::vector<uint8_t>& rom_data);
void set_rom_check(RomCheck check);
} // namespace recomp

extern "C" void recomp_entrypoint(uint8_t* rdram, recomp_context* ctx);

void register_snp_overlays();
bool snp_rom_check(const recomp::GameEntry& game, const std::vector<uint8_t>& rom);

namespace {

const std::string PROGRAM_NAME = "Sin and Punishment";
const std::string VERSION_STRING = "0.2.11";

SDL_Window* s_window = nullptr;

SDL_AudioDeviceID s_audio_device = 0;
std::mutex s_audio_mutex;
constexpr uint32_t AUDIO_CHANNELS = 2;
uint32_t s_game_frequency = 44100;
constexpr uint32_t AUDIO_HEADROOM_VIS = 0;
constexpr uint32_t AUDIO_SOFT_CAP_MS = 200;
constexpr uint32_t AUDIO_HARD_CAP_MS = 500;

std::deque<size_t> s_queued_chunks;
size_t s_queued_chunk_frames = 0;

size_t played_up_to_now() {
    size_t queued = s_audio_device != 0
        ? SDL_GetQueuedAudioSize(s_audio_device) / (AUDIO_CHANNELS * sizeof(float))
        : 0;
    while (!s_queued_chunks.empty() && s_queued_chunk_frames - s_queued_chunks.front() >= queued) {
        s_queued_chunk_frames -= s_queued_chunks.front();
        s_queued_chunks.pop_front();
    }
    return queued;
}

bool open_audio_device(uint32_t frequency) {
    if (!SDL_WasInit(SDL_INIT_AUDIO)) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            fprintf(stderr, "Failed to initialize SDL audio: %s\n", SDL_GetError());
            return false;
        }
    }
    std::lock_guard<std::mutex> lock(s_audio_mutex);
    if (s_audio_device != 0) {
        SDL_CloseAudioDevice(s_audio_device);
        s_audio_device = 0;
    }

    SDL_AudioSpec desired{};
    desired.freq = static_cast<int>(frequency);
    desired.format = AUDIO_F32SYS;
    desired.channels = static_cast<Uint8>(AUDIO_CHANNELS);
    desired.samples = 512;

    s_audio_device = SDL_OpenAudioDevice(nullptr, 0, &desired, nullptr, 0);
    s_game_frequency = frequency;

    if (s_audio_device == 0) {
        fprintf(stderr, "Failed to open audio device: %s\n", SDL_GetError());
        return false;
    }
    SDL_PauseAudioDevice(s_audio_device, 0);
    return true;
}

void set_frequency(uint32_t frequency) {
    if (frequency != s_game_frequency) {
        open_audio_device(frequency);
    }
}

void queue_samples(int16_t* audio_data, size_t sample_count) {
    if (s_audio_device == 0) {
        return;
    }

    static std::vector<float> buffer;
    buffer.resize(sample_count);
    float master_scale = static_cast<float>(snp::handheld::get_config().audio.master_volume) / 100.0f;
    float scale = master_scale / 32768.0f;
    for (size_t i = 0; i + 1 < sample_count; i += 2) {
        buffer[i + 0] = audio_data[i + 1] * scale;
        buffer[i + 1] = audio_data[i + 0] * scale;
    }

    std::lock_guard<std::mutex> lock(s_audio_mutex);
    const size_t bytes_per_ms = s_game_frequency * AUDIO_CHANNELS * sizeof(float) / 1000;
    const size_t queued_bytes = SDL_GetQueuedAudioSize(s_audio_device);

    if (queued_bytes > AUDIO_HARD_CAP_MS * bytes_per_ms) {
        SDL_ClearQueuedAudio(s_audio_device);
        s_queued_chunks.clear();
        s_queued_chunk_frames = 0;
    } else if (queued_bytes > AUDIO_SOFT_CAP_MS * bytes_per_ms) {
        return;
    }

    SDL_QueueAudio(s_audio_device, buffer.data(), static_cast<Uint32>(sample_count * sizeof(float)));
    s_queued_chunks.push_back(sample_count / AUDIO_CHANNELS);
    s_queued_chunk_frames += sample_count / AUDIO_CHANNELS;
}

size_t get_frames_remaining() {
    if (s_audio_device == 0) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(s_audio_mutex);
    size_t queued_frames = SDL_GetQueuedAudioSize(s_audio_device) / (AUDIO_CHANNELS * sizeof(float));
    size_t headroom_frames = s_game_frequency / 60 * AUDIO_HEADROOM_VIS;
    return queued_frames > headroom_frames ? queued_frames - headroom_frames : 0;
}

ultramodern::gfx_callbacks_t::gfx_data_t create_gfx() {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "Failed to initialize SDL2: %s\n", SDL_GetError());
    }
    snp::handheld::init_input();
    return {};
}

ultramodern::renderer::WindowHandle create_window(ultramodern::gfx_callbacks_t::gfx_data_t) {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    uint32_t flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE;
    const char* video_driver = SDL_GetCurrentVideoDriver();
    if (!video_driver || std::strcmp(video_driver, "offscreen") != 0) {
        flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    }
    s_window = SDL_CreateWindow(PROGRAM_NAME.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 720, 480, flags);
    if (s_window == nullptr) {
        fprintf(stderr, "Failed to create SDL window: %s\n", SDL_GetError());
        std::exit(EXIT_FAILURE);
    }

    return ultramodern::renderer::WindowHandle{ s_window };
}

void update_gfx(void*) {
    snp::handheld::poll_input();

    float widescreen_scale = 1.0f;
    if (ultramodern::renderer::get_graphics_config().ar_option == ultramodern::renderer::AspectRatio::Expand) {
        int width = 0, height = 0;
        if (s_window != nullptr) {
            SDL_GetWindowSize(s_window, &width, &height);
            if (width > 0 && height > 0) {
                widescreen_scale = std::max(1.0f, (static_cast<float>(width) / static_cast<float>(height)) / (4.0f / 3.0f));
            }
        }
    }
    snp::set_widescreen_scale(widescreen_scale);
}

std::string get_game_thread_name(const OSThread* t) {
    return "Game " + std::to_string(t->id);
}

} // namespace

#ifdef SNP_HAS_RSP_AUDIO
RspExitReason n_aspMain(uint8_t* rdram, uint32_t ucode_addr);
#endif

namespace {

RspUcodeFunc* get_rsp_microcode(const OSTask* task) {
    switch (task->t.type) {
#ifdef SNP_HAS_RSP_AUDIO
    case M_AUDTASK:
        return n_aspMain;
#endif
    default:
        fprintf(stderr, "No RSP microcode registered for task type %" PRIu32 ".\n", task->t.type);
        return nullptr;
    }
}

} // namespace

extern std::atomic_bool exited;

namespace {

void start_quit_watchdog() {
    std::thread([] {
        while (!exited.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
        fflush(stdout);
        fflush(stderr);
        std::_Exit(EXIT_SUCCESS);
    }).detach();
}

} // namespace

void snp_install_crash_handler(const SectionTableEntry* sections, size_t num_sections) {
    (void)sections;
    (void)num_sections;
}

extern "C" uint32_t snp_ai_length() {
    std::lock_guard<std::mutex> lock(s_audio_mutex);
    size_t queued = played_up_to_now();
    if (s_queued_chunks.empty()) {
        return 0;
    }
    size_t after_current = s_queued_chunk_frames - s_queued_chunks.front();
    size_t current = queued > after_current ? queued - after_current : 0;
    return static_cast<uint32_t>(current * 2 * sizeof(int16_t));
}

extern "C" uint32_t snp_ai_status() {
    std::lock_guard<std::mutex> lock(s_audio_mutex);
    played_up_to_now();
    return s_queued_chunks.size() >= 3 ? 0x80000000u : 0;
}

extern "C" void recomp_run_ui_callbacks(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    snp_extras_frame(rdram);
}

extern "C" void recomp_run_ui_callbacks_recomp(uint8_t* rdram, recomp_context* ctx) {
    recomp_run_ui_callbacks(rdram, ctx);
}

void snp_report_fatal_error(const char* message) {
    fprintf(stderr, "[FATAL ERROR] %s\n", message);
    std::ofstream log("crash_log.txt", std::ios::app);
    if (log.is_open()) {
        log << "[FATAL ERROR] " << message << "\n";
    }
}

std::vector<recomp::GameEntry> supported_games = {
    {
        .rom_hash = 0xec641b62243b72b9ULL,
        .internal_name = "TSUMI TO BATSU",
        .display_name = "Sin and Punishment (Japan)",
        .game_id = u8"snp.n64.jp.1.0",
        .mod_game_id = "snp",
        .save_type = recomp::SaveType::Eep4k,
        .thumbnail_bytes = {},
        .is_enabled = true,
        .decompression_routine = nullptr,
        .has_compressed_code = false,
        .entrypoint_address = (gpr)(int32_t)0x80025C00u,
        .entrypoint = recomp_entrypoint,
        .on_init_callback = nullptr,
    },
    {
        .rom_hash = 0x912f22306cafedd2ULL,
        .internal_name = "Sin and Punishment",
        .display_name = "Sin and Punishment (English translation)",
        .game_id = u8"snp.n64.en.fan",
        .mod_game_id = "snp",
        .save_type = recomp::SaveType::Eep4k,
        .thumbnail_bytes = {},
        .is_enabled = true,
        .decompression_routine = nullptr,
        .has_compressed_code = false,
        .entrypoint_address = (gpr)(int32_t)0x80025C00u,
        .entrypoint = recomp_entrypoint,
        .on_init_callback = nullptr,
    },
};

int main(int argc, char** argv) {
    snp::handheld::load_config();
    snp::handheld::apply_graphics_config();
    snp::handheld::init_overlay();

    recomp::Version project_version{};
    if (!recomp::Version::from_string(VERSION_STRING, project_version)) {
        fprintf(stderr, "Invalid version string: %s\n", VERSION_STRING.c_str());
        return EXIT_FAILURE;
    }

    register_snp_overlays();

    for (const auto& game : supported_games) {
        recomp::register_game(game);
    }
    recomp::set_rom_check(snp_rom_check);

    recomp::register_config_path(std::filesystem::current_path());

    open_audio_device(s_game_frequency);

    std::string selected_game_id = snp::handheld::find_and_select_rom();
    if (selected_game_id.empty()) {
        fprintf(stderr, "No supported Sin and Punishment ROM found. Exiting.\n");
        return EXIT_FAILURE;
    }

    bool has_game_argument = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--game") == 0) {
            has_game_argument = true;
            break;
        }
    }

    std::vector<char*> final_argv;
    for (int i = 0; i < argc; ++i) {
        final_argv.push_back(argv[i]);
    }

    std::string game_flag = "--game";
    if (!has_game_argument && !selected_game_id.empty()) {
        final_argv.push_back(game_flag.data());
        final_argv.push_back(selected_game_id.data());
    }
    int final_argc = static_cast<int>(final_argv.size());

    recomp::rsp::callbacks_t rsp_callbacks{
        .get_rsp_microcode = get_rsp_microcode,
    };

    ultramodern::renderer::callbacks_t renderer_callbacks{
        .create_render_context = [](uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
            snp::gles3::set_overlay_render_callback(snp::handheld::render_overlay);
            return snp::gles3::create_renderer_context(rdram, window_handle, developer_mode);
        },
    };

    ultramodern::gfx_callbacks_t gfx_callbacks{
        .create_gfx = create_gfx,
        .create_window = create_window,
        .update_gfx = update_gfx,
    };

    ultramodern::audio_callbacks_t audio_callbacks{
        .queue_samples = queue_samples,
        .get_frames_remaining = get_frames_remaining,
        .set_frequency = set_frequency,
    };

    ultramodern::input::callbacks_t input_callbacks{
        .poll_input = snp::handheld::poll_input,
        .get_input = snp::handheld::get_input,
        .set_rumble = snp::handheld::set_rumble,
        .get_connected_device_info = snp::handheld::get_connected_device_info,
    };

    ultramodern::events::callbacks_t events_callbacks{
        .vi_callback = nullptr,
        .gfx_init_callback = nullptr,
    };

    ultramodern::error_handling::callbacks_t error_handling_callbacks{
        .message_box = [](const char* msg) {
            fprintf(stderr, "%s\n", msg);
        },
    };

    ultramodern::threads::callbacks_t threads_callbacks{
        .get_game_thread_name = get_game_thread_name,
    };

    start_quit_watchdog();

    recomp::start({
        .argc = final_argc,
        .argv = final_argv.data(),
        .project_version = project_version,
        .window_handle = {},
        .rsp_callbacks = rsp_callbacks,
        .renderer_callbacks = renderer_callbacks,
        .audio_callbacks = audio_callbacks,
        .input_callbacks = input_callbacks,
        .gfx_callbacks = gfx_callbacks,
        .events_callbacks = events_callbacks,
        .error_handling_callbacks = error_handling_callbacks,
        .threads_callbacks = threads_callbacks,
        .message_queue_control = {},
    });

    snp::handheld::shutdown_overlay();
    snp::handheld::shutdown_input();
    fflush(stdout);
    fflush(stderr);
    std::_Exit(EXIT_SUCCESS);
}
