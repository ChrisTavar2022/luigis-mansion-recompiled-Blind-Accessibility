// Host entry point: owns the window/renderer (Aurora) and drives the game
// thread one presented frame at a time.
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/main.h>

#include "port/frame_driver.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace lm {
void game_main();  // src/game/main.cpp
}

static void log_callback(AuroraLogLevel level, const char* module, const char* message, unsigned int) {
    static const char* names[] = {"DEBUG", "INFO", "WARNING", "ERROR", "FATAL"};
    std::fprintf(level >= LOG_ERROR ? stderr : stdout, "[%s: %s] %s\n", names[level], module, message);
    std::fflush(stdout);
    if (level == LOG_FATAL) {
        std::fflush(stderr);
        std::abort();
    }
}

int main(int argc, char* argv[]) {
    AuroraConfig config{};
    config.appName = "Luigi's Mansion PC";
    config.logCallback = &log_callback;
    aurora_initialize(argc, argv, &config);

    using clock = std::chrono::steady_clock;
    // The GameCube VI runs at 59.94 Hz; the game counts retraces to pace itself.
    constexpr auto kRetracePeriod = std::chrono::nanoseconds(16'683'350);

    port::frame_driver::start(&lm::game_main);

    auto nextDeadline = clock::time_point{};
    bool exiting = false;
    bool paused = false;
    while (!exiting) {
        for (const AuroraEvent* ev = aurora_update(); ev && ev->type != AURORA_NONE; ++ev) {
            if (ev->type == AURORA_EXIT) exiting = true;
            else if (ev->type == AURORA_PAUSED) paused = true;
            else if (ev->type == AURORA_UNPAUSED) paused = false;
        }
        if (exiting || paused || !aurora_begin_frame()) {
            continue;
        }
        const bool running = port::frame_driver::run_until_present();
        aurora_end_frame();
        if (!running) {
            break;
        }

        // Pace to the original speed: one host frame covers however many retraces
        // the game waited for. (Replaced by the interpolating game clock later.)
        if (nextDeadline == clock::time_point{}) {
            nextDeadline = clock::now();  // anchor pacing at the first presented frame
        }
        nextDeadline += kRetracePeriod * std::max<uint32_t>(port::frame_driver::last_retrace_count(), 1);
        const auto now = clock::now();
        if (nextDeadline > now) {
            std::this_thread::sleep_until(nextDeadline);
        } else if (now - nextDeadline > std::chrono::milliseconds(250)) {
            nextDeadline = now;  // fell far behind (e.g. loading hitch); don't fast-forward
        }
    }

    port::frame_driver::stop();
    aurora_shutdown();
    return 0;
}
