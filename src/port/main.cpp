// Platform entry point. For now this only brings up Aurora and clears the
// screen; the game's own main loop is wired in during Phase 1.
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/main.h>
#include <dolphin/gx.h>

#include <cstdio>
#include <cstdlib>

static void log_callback(AuroraLogLevel level, const char* module, const char* message, unsigned int) {
    static const char* names[] = {"DEBUG", "INFO", "WARNING", "ERROR", "FATAL"};
    std::fprintf(level >= LOG_ERROR ? stderr : stdout, "[%s: %s] %s\n", names[level], module, message);
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
        GXSetCopyClear(GXColor{0, 40, 30, 255}, GX_MAX_Z24);
        aurora_end_frame();
    }

    aurora_shutdown();
    return 0;
}
