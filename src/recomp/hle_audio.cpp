// HLE: audio interface and DSP. Output is silent for now, but the AI DMA
// interrupt is raised at the real rate so JAudio's frame processing (sequence
// playback, wave loading) keeps running and the game never waits on it.
#include "recomp/hle.h"
#include "recomp/sched.h"

#include <atomic>
#include <chrono>
#include <thread>

namespace recomp {
namespace {
std::atomic<u32> sDmaCallback{0};
u32 sStreamCallback = 0;
std::atomic<u32> sDmaLength{0};
std::atomic<bool> sDmaRunning{false};
std::atomic<u32> sDspRate{0};  // 0 = 32 kHz, 1 = 48 kHz
std::thread sDmaThread;

void dmaThread() {
    using clock = std::chrono::steady_clock;
    auto next = clock::now();
    for (;;) {
        const u32 len = sDmaLength.load();
        const double rate = sDspRate.load() ? 48000.0 : 32000.0;
        const double secs = len ? (len / 4.0) / rate : 0.005;  // 16-bit stereo frames
        next += std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(secs));
        if (clock::now() > next + std::chrono::milliseconds(100)) next = clock::now();
        std::this_thread::sleep_until(next);
        const u32 cb = sDmaCallback.load();
        if (sDmaRunning.load() && cb) sched::postIrq([cb](Ctx& ic) { guestCall(ic, cb); });
    }
}
}  // namespace

HLE(AIInit) {}
HLE(AIInitDMA) { sDmaLength = c.r[4]; }
HLE(AIStartDMA) {
    sDmaRunning = true;
    if (!sDmaThread.joinable()) {
        sDmaThread = std::thread(dmaThread);
        sDmaThread.detach();
    }
}
HLE(AIStopDMA) { sDmaRunning = false; }
HLE(AIGetDMAStartAddr) { c.r[3] = 0; }
HLE(AISetDSPSampleRate) { sDspRate = c.r[3]; }
HLE(AIGetDSPSampleRate) { c.r[3] = sDspRate; }
HLE(AIRegisterDMACallback) {
    const u32 old = sDmaCallback;
    sDmaCallback = c.r[3];
    c.r[3] = old;
}
HLE(AIRegisterStreamCallback) {
    const u32 old = sStreamCallback;
    sStreamCallback = c.r[3];
    c.r[3] = old;
}
HLE(AISetStreamSampleRate) {}
HLE(AISetStreamVolLeft) {}
HLE(AISetStreamVolRight) {}
HLE(AIResetStreamSampleCount) {}
HLE(AISetStreamPlayState) {}
HLE(AISetStreamTrigger) {}
HLE(AIGetStreamSampleCount) { c.r[3] = 0; }
HLE(AIGetStreamTrigger) { c.r[3] = 0; }

HLE(DSPInit) {}
HLE(DSPCheckMailToDSP) { c.r[3] = 0; }
HLE(DSPCheckMailFromDSP) { c.r[3] = 0; }
HLE(DSPReadMailFromDSP) { c.r[3] = 0; }
HLE(DSPSendMailToDSP) {}
HLE(DSPAssertInt) {}

}  // namespace recomp

namespace recomp {
// JAudio DSP driver hooks (tools/recomp/hooks.txt): no DSP, so nothing to boot or talk to.
HLE(DspBoot__Fv) {}
HLE(DSPSendCommands__FPUlUl) {}
HLE(DSPReleaseHalt__Fv) {}
HLE(DSPWaitFinish__Fv) {}
HLE(DsyncFrame__FUlUlUl) {}
HLE(DwaitFrame__Fv) {}
HLE(DiplSec__FUl) {}
}  // namespace recomp
