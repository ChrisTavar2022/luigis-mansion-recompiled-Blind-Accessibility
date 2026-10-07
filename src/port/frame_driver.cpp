#include "port/frame_driver.h"
#include "port/main.h"
#include "port/os.h"

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
bool sFramePresented = false;
uint32_t sRetraces = 0;
uint32_t sLastRetraces = 0;
std::thread sGameThread;

// Game thread: give control back to the host and wait to be resumed.
void yield_to_host(std::unique_lock<std::mutex>& lock) {
    sTurn = Turn::Host;
    sCv.notify_all();
    sCv.wait(lock, [] { return sTurn == Turn::Game || g_shuttingDown; });
}

}  // namespace

void start(GameEntry entry) {
    sGameThread = std::thread([entry] {
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
    sRetraces = 0;
    sTurn = Turn::Game;
    sCv.notify_all();
    sCv.wait(lock, [] { return sTurn == Turn::Host; });
    sLastRetraces = sRetraces;
    return !sGameFinished;
}

uint32_t last_retrace_count() { return sLastRetraces; }

void stop() {
    g_shuttingDown = true;
    sCv.notify_all();
    if (sGameThread.joinable()) {
        // The game thread may be blocked inside game code (e.g. a message queue);
        // primitives observe g_shuttingDown, but don't hang exit waiting on it.
        sGameThread.detach();
    }
}

void notify_frame_presented() {
    std::lock_guard lock(sMutex);
    sFramePresented = true;
}

void on_retrace() {
    std::unique_lock lock(sMutex);
    ++sRetraces;
    if (sFramePresented) {
        sFramePresented = false;
        yield_to_host(lock);
    }
}

}  // namespace port::frame_driver
