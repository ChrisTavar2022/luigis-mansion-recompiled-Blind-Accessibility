// HLE: VI. A host timer raises the retrace interrupt at 59.94 Hz (the PAL game: 50 Hz,
// or 59.94 Hz "PAL60" when chosen in the F1 menu); the game paces itself on it. VISetNextFrameBuffer marks a finished frame, which is
// handed to the host loop (see frame.h) before the game continues.
#include "port/install.h"
#include "port/settings.h"
#include "recomp/frame.h"
#include "recomp/hle.h"
#include "recomp/runtime.h"
#include "recomp/sched.h"
#include "recomp/skip.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace recomp {
namespace {

std::atomic<u32> sRetraceCount{0};
u32 sPreCallback = 0, sPostCallback = 0;
u32 sRetraceQueue = 0;  // guest-address key for VIWaitForRetrace sleepers
bool sBlack = true;
std::thread sTimer;
std::atomic<bool> sTimerRun{false};
std::atomic<bool> sRetracePending{false};  // fast-forward: one retrace in flight at a time

void retrace(Ctx& c) {
    sRetracePending = false;
    const u32 count = sRetraceCount.fetch_add(1) + 1;
    if (sPreCallback) guestCall(c, sPreCallback, count);
    if (sPostCallback) guestCall(c, sPostCallback, count);
    sched::wakeup(sRetraceQueue);
}

bool palGame() { return port::install().running == port::GameVersion::PAL; }

// The PAL release's logic is tuned for 50 Hz: at 60 Hz it runs 20% fast (like PAL60 on
// a console). Read every tick, so the menu option applies immediately.
double retraceHz() {
    if (palGame() && !port::settings().video.pal60) return 50.0;
    return 60000.0 / 1001.0;
}

}  // namespace

namespace vi {
void startTimer() {
    sRetraceQueue = hleAlloc(8);
    sTimerRun = true;
    sTimer = std::thread([] {
        using clock = std::chrono::steady_clock;
        auto period = std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(1.0 / retraceHz()));
        auto next = clock::now() + period;
        while (sTimerRun) {
            period = std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(1.0 / retraceHz()));
            if (const double speed = skip::retraceSpeed(); speed > 1.0) {
                // Fast-forward (skip.h): retraces as fast as the game takes them, up to `speed` x.
                const auto fast = std::chrono::duration_cast<clock::duration>(period / speed);
                while (clock::now() < next && sTimerRun) std::this_thread::yield();
                next = std::max(next + fast, clock::now());
                if (!frame::gamePaused() && !sRetracePending.exchange(true)) sched::postIrq(&retrace);
                continue;
            }
            std::this_thread::sleep_until(next);
            next += period;
            if (clock::now() > next + period * 4) next = clock::now() + period;  // don't spiral after a stall
            if (frame::gamePaused()) continue;
            sched::postIrq(&retrace);
        }
    });
}
void stopTimer() {
    sTimerRun = false;
    if (sTimer.joinable()) sTimer.join();
}
}  // namespace vi

u32 hw::viRead(u32 addr, int size) {
    switch (addr & 0xFFF) {
    case 0x2C:  // VI_DISP_POS: vertical beam position (high half)
        return size == 4 ? (1u << 16) : 1;
    case 0x6E:  // VI_CLK_SEL / DTV status
        return 0;
    default:
        return 0;
    }
}

HLE(VIInit) {}
HLE(VIConfigure) {}
HLE(VIConfigurePan) {}
HLE(VIFlush) {}
HLE(VISetBlack) { sBlack = c.r[3] != 0; }
HLE(VIGetRetraceCount) { c.r[3] = sRetraceCount.load(); }
HLE(VIGetNextField) { c.r[3] = 1; }
HLE(VIGetCurrentLine) { c.r[3] = 0; }
HLE(VIGetTvFormat) { c.r[3] = palGame() ? 1 : 0; }  // VI_PAL / VI_NTSC
HLE(VIGetDTVStatus) { c.r[3] = 0; }
HLE(VISetPreRetraceCallback) {
    const u32 old = sPreCallback;
    sPreCallback = c.r[3];
    c.r[3] = old;
}
HLE(VISetPostRetraceCallback) {
    const u32 old = sPostCallback;
    sPostCallback = c.r[3];
    c.r[3] = old;
}
HLE(VIWaitForRetrace) {
    const u32 start = sRetraceCount.load();
    while (sRetraceCount.load() == start) sched::sleepOn(sRetraceQueue);
}
HLE(VISetNextFrameBuffer) { frame::present(c, c.r[3], sBlack); }

}  // namespace recomp

namespace recomp {
u32 viRetraceCount() { return sRetraceCount.load(); }
}  // namespace recomp
