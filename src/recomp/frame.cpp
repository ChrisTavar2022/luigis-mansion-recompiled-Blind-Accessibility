#include "recomp/frame.h"

#include "port/settings.h"
#include "recomp/gx_bridge.h"
#include "recomp/hidden_mansion.h"
#include "recomp/hle.h"
#include "recomp/interp.h"
#include "recomp/sched.h"
#include "recomp/skip.h"

#include <aurora/aurora.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace recomp {
u32 viRetraceCount();
u32 frameStamp();  // hle_vi.cpp: the frame's time stamp in NTSC fields
}

namespace recomp::frame {
namespace {

enum class Turn { Host, Guest };
std::mutex sM;
std::condition_variable sCv;
Turn sTurn = Turn::Host;
bool sFinished = false;
bool sStopping = false;
std::thread sBoot;

// Interpolated presentation (interp.h). The guest is "decoupled": it no longer
// waits for the host in present() but captures each frame into a tape, while the
// host replays tapes at display rate. Entered and left at a guest present.
std::atomic<u32> sPresents{0};
bool sEnterDecoupled = false;   // host -> guest, under sM: continue decoupled after this wait
std::atomic<bool> sStayDecoupled{false};  // host -> guest: keep going decoupled at the next present
bool sGuestDecoupled = false;   // guest thread
bool sHostDecoupled = false;    // host thread
constexpr u32 kDecoupleAfter = 60;  // presents; keeps boot (GXInit, first loads) in lockstep

bool interpolationWanted() { return targetFps() > port::kMinFps && sPresents.load() >= kDecoupleAfter; }

}  // namespace

void start(Entry boot) {
    sBoot = std::thread([boot] {
        {
            std::unique_lock lk(sM);
            sCv.wait(lk, [] { return sTurn == Turn::Guest || sStopping; });
        }
        if (!sStopping) {
            try {
                boot();
            } catch (const sched::ThreadExit&) {
            }
        }
        std::lock_guard lk(sM);
        sFinished = true;
        sTurn = Turn::Host;
        sCv.notify_all();
    });
}

bool runUntilPresent() {
    std::unique_lock lk(sM);
    if (sFinished) return false;
    sTurn = Turn::Guest;
    sCv.notify_all();
    sCv.wait(lk, [] { return sTurn == Turn::Host; });
    return !sFinished;
}

bool hostFrame() {
    const bool want = interpolationWanted();
    if (!sHostDecoupled) {
        if (!runUntilPresent()) return false;  // the guest drew this frame directly
        if (want) {
            // Let the guest continue without waiting for the next host frame; from
            // its next frame on it records tapes.
            std::lock_guard lk(sM);
            sStayDecoupled = true;
            sEnterDecoupled = true;
            sTurn = Turn::Guest;
            sCv.notify_all();
            sHostDecoupled = true;
        }
        return true;
    }
    sStayDecoupled = want;
    bool guestBack;
    {
        std::lock_guard lk(sM);
        if (sFinished) return false;
        guestBack = sTurn == Turn::Host;  // left decoupled mode and waits in present()
    }
    if (guestBack) {
        interp::replay(true);  // its last tape, as is
        interp::reset();
        sHostDecoupled = false;
        return true;
    }
    interp::replay();
    return true;
}

float displayRefreshHz() {
    // Followed every half second, so moving the window to another display (or changing
    // its mode) takes effect without a restart.
    using Clock = std::chrono::steady_clock;
    static Clock::time_point checked{};
    static float hz = 0;
    const auto now = Clock::now();
    if (now - checked < std::chrono::milliseconds(500) && checked != Clock::time_point{}) return hz;
    checked = now;
    if (SDL_Window* w = aurora_get_window())
        if (const SDL_DisplayID d = SDL_GetDisplayForWindow(w))
            if (const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(d)) hz = m->refresh_rate;
    static float logged = -1;
    if (hz != logged) log("[frame] display refresh rate %.2f Hz", logged = hz);
    return hz;
}

int targetFps() {
    const auto& v = port::settings().video;
    int fps = v.targetFps;
    if (v.matchDisplay) {
        const float hz = displayRefreshHz();
        fps = hz > 0 ? int(std::lround(hz)) : 60;
    }
    return std::clamp(fps, port::kMinFps, port::kMaxFps);
}

void pace() {
    // Interpolated presentation: the host frame loop is capped at the target. With vsync the
    // swap chain already waits for the display, so a target at or above the refresh rate
    // runs at the refresh rate (the menu says so).
    using Clock = std::chrono::steady_clock;
    static Clock::time_point next = Clock::now();
    const int fps = targetFps();
    const float hz = displayRefreshHz();
    if (!sHostDecoupled || (port::settings().video.vsync && hz > 0 && float(fps) >= hz - 0.5f)) {
        next = Clock::now();
        return;
    }
    const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / fps));
    next += period;
    const auto now = Clock::now();
    if (next < now - period) {  // fell behind: don't try to catch up
        next = now;
        return;
    }
    if (next > now) SDL_DelayPrecise(u64(std::chrono::duration_cast<std::chrono::nanoseconds>(next - now).count()));
}

void stop() {
    {
        std::lock_guard lk(sM);
        sStopping = true;
        sCv.notify_all();
    }
    sched::shutdown();
    // Let the guest threads unwind (ThreadExit) before the caller tears down Aurora and audio:
    // the one running guest code only leaves at its next present/scheduling point and would
    // otherwise still be calling into GX while it is destroyed. Bounded, in case a thread
    // spins without reaching one.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    {
        std::unique_lock lk(sM);
        sCv.wait_until(lk, deadline, [] { return sFinished; });
    }
    const double left = std::chrono::duration<double>(deadline - std::chrono::steady_clock::now()).count();
    if (!sched::waitForThreads(std::max(left, 0.0))) log("[frame] guest threads still running at exit");
    if (sBoot.joinable()) sBoot.detach();
}

// While the host is paused (window minimized or unfocused) a decoupled guest
// would keep playing; stopping the VI retraces holds it like lockstep does.
std::atomic<bool> sPauseGuest{false};
void setHostPaused(bool paused) { sPauseGuest = paused && sHostDecoupled; }
bool gamePaused() { return sPauseGuest.load(std::memory_order_relaxed); }

void present(Ctx& c, u32, bool) {
    gx::endFrame();
    hm::onFrame();
    if (skip::onGuestFrame(c)) return;  // fast-forward: this frame is not shown
    const u32 presents = ++sPresents;
    if (presents % 30 == 0 || presents < 60) log("[frame] %u presents, %u retraces", presents, viRetraceCount());
    if (sGuestDecoupled) {
        interp::endTape(frameStamp());
        if (sStopping) throw sched::ThreadExit{};
        if (sStayDecoupled) {
            interp::beginTape();
            return;
        }
        sGuestDecoupled = false;  // back to lockstep: wait for the host below
    }
    std::unique_lock lk(sM);
    sTurn = Turn::Host;
    sCv.notify_all();
    sCv.wait(lk, [] { return sTurn == Turn::Guest || sStopping; });
    if (sStopping) {
        lk.unlock();
        throw sched::ThreadExit{};
    }
    if (sEnterDecoupled) {
        sEnterDecoupled = false;
        sGuestDecoupled = true;
        lk.unlock();
        interp::beginTape();
    }
}

}  // namespace recomp::frame
