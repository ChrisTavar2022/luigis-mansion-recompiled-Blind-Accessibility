#include "port/frame_driver.h"
#include "port/main.h"
#include "port/os.h"
#include "port/vi.h"

#include <condition_variable>
#include <mutex>
#include <thread>

namespace port::frame_driver {
namespace {

enum class Turn { Host, Game };

std::mutex sMutex;
std::condition_variable sCv;
Turn sTurn = Turn::Host;
bool sGameFinished = false;
std::thread sGameThread;
thread_local bool tIsGameThread = false;

}  // namespace

void start(GameEntry entry) {
    vi::start_retrace_timer();
    sGameThread = std::thread([entry] {
        tIsGameThread = true;
        OSSetCurrentThreadName("Game");
        {
            std::unique_lock lock(sMutex);
            sCv.wait(lock, [] { return sTurn == Turn::Game || g_shuttingDown; });
        }
        if (!g_shuttingDown) {
            entry();
        }
        std::lock_guard lock(sMutex);
        sGameFinished = true;
        sTurn = Turn::Host;
        sCv.notify_all();
    });
}

bool run_until_present() {
    std::unique_lock lock(sMutex);
    if (sGameFinished) {
        return false;
    }
    sTurn = Turn::Game;
    sCv.notify_all();
    sCv.wait(lock, [] { return sTurn == Turn::Host; });
    return !sGameFinished;
}

void stop() {
    g_shuttingDown = true;
    vi::stop_retrace_timer();
    sCv.notify_all();
    if (sGameThread.joinable()) {
        // The game thread may be blocked inside game code (e.g. a message queue);
        // primitives observe g_shuttingDown, but don't hang exit waiting on it.
        sGameThread.detach();
    }
}

void present() {
    // Other game-side threads (e.g. the DVD loader) never present; only the
    // main game thread hands frames to the host.
    if (!tIsGameThread) {
        return;
    }
    std::unique_lock lock(sMutex);
    sTurn = Turn::Host;
    sCv.notify_all();
    sCv.wait(lock, [] { return sTurn == Turn::Game || g_shuttingDown; });
}

}  // namespace port::frame_driver
