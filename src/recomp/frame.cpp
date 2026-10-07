#include "recomp/frame.h"

#include "port/settings.h"
#include "recomp/gx_bridge.h"
#include "recomp/hidden_mansion.h"
#include "recomp/hle.h"
#include "recomp/interp.h"
#include "recomp/sched.h"
#include "recomp/skip.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace recomp {
u32 viRetraceCount();
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

bool interpolationWanted() {
    return port::settings().video.frameRate != port::FrameRateMode::Original30 && sPresents.load() >= kDecoupleAfter;
}

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

void pace() {
    using Clock = std::chrono::steady_clock;
    static Clock::time_point next = Clock::now();
    const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1001.0 / 60000.0));
    if (!sHostDecoupled || port::settings().video.frameRate != port::FrameRateMode::Interpolated60) {
        next = Clock::now();
        return;
    }
    next += period;
    const auto now = Clock::now();
    if (next < now - period) next = now;  // fell behind: don't try to catch up
    else std::this_thread::sleep_until(next);
}

void stop() {
    {
        std::lock_guard lk(sM);
        sStopping = true;
        sCv.notify_all();
    }
    sched::shutdown();
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
        interp::endTape(viRetraceCount());
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
