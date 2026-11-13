// Busy-wait loops (tools/find_spin_loops.py lists them). The game polls a
// few flags in loops with no OS call:
//   func_80025CA4 0x80025D30  the idle thread, waiting for a callback
//   func_80025E44 0x80025FB0  the main thread, waiting on D_80068A95
//   func_8004D7B0 0x8004D7B0  waiting for the job thread to finish its jobs
// On the N64, interrupts preempt them whenever an event wakes another
// thread. The runtime's scheduler only switches threads inside OS calls, and
// only delivers VI and other events when a game thread waits for one, so a
// spinning thread kept every other thread waiting forever. The hooks in
// snp.jp.toml call snp_spin_yield on each pass instead.
#include <chrono>
#include <cstdint>

#include "ultramodern/ultramodern.hpp"
#include "snp_recomp_hooks.h"

// ultramodern/src/mesgqueue.cpp, not declared in a header.
void dequeue_external_messages(uint8_t* rdram);

extern "C" int32_t osGetThreadPri(uint8_t* rdram, int32_t t);
extern "C" void osSetThreadPri(uint8_t* rdram, int32_t t, int32_t pri);

namespace {
constexpr int64_t ZERO_LATENCY_WINDOW_MS = 2;
constexpr uint32_t SLEEP_TIMEOUT_MS = 1;
} // namespace

extern "C" void snp_spin_yield(uint8_t* rdram) {
    int32_t pri = osGetThreadPri(rdram, 0);
    if (pri == 0) {
        // Idle thread waits up to 1 ms so it does not burn CPU unnecessarily.
        ultramodern::wait_for_external_message_timed(rdram, SLEEP_TIMEOUT_MS);
        dequeue_external_messages(rdram);
        ultramodern::check_running_queue(rdram);
        return;
    }

    // For non-idle threads, skip sleeping if >= 2 ms has elapsed since the last
    // yield on this thread. This avoids injecting artificial 1 ms delays into
    // quick spin checks while still throttling prolonged lockups.
    thread_local auto s_last_yield = std::chrono::steady_clock::time_point{};
    auto now = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - s_last_yield).count();

    if (elapsed_ms < ZERO_LATENCY_WINDOW_MS) {
        ultramodern::wait_for_external_message_timed(rdram, SLEEP_TIMEOUT_MS);
    }
    s_last_yield = std::chrono::steady_clock::now();

    dequeue_external_messages(rdram);
    ultramodern::check_running_queue(rdram);

    // Dropping priority to 0 for a moment allows lower-priority worker threads
    // to run before restoring the original priority.
    osSetThreadPri(rdram, 0, 0);
    osSetThreadPri(rdram, 0, pri);
}
