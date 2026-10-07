// Host entry point: owns the window/renderer (Aurora) and drives the game
// thread one presented frame at a time.
#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <aurora/event.h>
#include <aurora/main.h>

#include "port/dol.h"
#include "port/frame_driver.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
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

// Finds the disc image: --disc <path>, else orig/GLME01.iso searched upward
// from the working directory (the repo layout produced by tools/make_iso.py).
static std::string find_disc(int argc, char* argv[]) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--disc") == 0) {
            return argv[i + 1];
        }
    }
    for (auto dir = std::filesystem::current_path(); ; dir = dir.parent_path()) {
        const auto candidate = dir / "orig" / "GLME01.iso";
        if (std::filesystem::exists(candidate)) {
            return candidate.string();
        }
        if (dir == dir.parent_path()) {
            return {};
        }
    }
}

int main(int argc, char* argv[]) {
    AuroraConfig config{};
    config.appName = "Luigi's Mansion PC";
    config.logCallback = &log_callback;
    // Emulated MEM1 backing the game's JKR heaps. Retail has 24 MiB; 64-bit pointers
    // and host-side structures need more headroom (Dusklight uses the same size).
    config.mem1Size = 256 * 1024 * 1024;
    config.mem2Size = 16 * 1024 * 1024;  // ARAM, same as retail
    aurora_initialize(argc, argv, &config);

    const std::string disc = find_disc(argc, argv);
    if (disc.empty() || !aurora_dvd_open(disc.c_str())) {
        std::fprintf(stderr, "Could not open a Luigi's Mansion disc image. Pass --disc <path to .iso/.rvz>.\n");
        aurora_shutdown();
        return 1;
    }
    if (port::dol::load() != port::dol::Version::GLME01) {
        std::fprintf(stderr, "Unsupported disc: the port is based on Luigi's Mansion USA (GLME01) rev 0.\n");
        aurora_shutdown();
        return 1;
    }
    std::printf("Opened %s\n", disc.c_str());

    using host_clock = std::chrono::steady_clock;
    // The GameCube VI runs at 59.94 Hz; the game counts retraces to pace itself.
    constexpr auto kRetracePeriod = std::chrono::nanoseconds(16'683'350);

    port::frame_driver::start(&lm::game_main);

    auto nextDeadline = host_clock::time_point{};
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
        if (nextDeadline == host_clock::time_point{}) {
            nextDeadline = host_clock::now();  // anchor pacing at the first presented frame
        }
        nextDeadline += kRetracePeriod * std::max<uint32_t>(port::frame_driver::last_retrace_count(), 1);
        const auto now = host_clock::now();
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
