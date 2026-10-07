// HLE: audio interface (AI) and the JAudio DSP driver.
//
// AI: the game hands finished 16-bit stereo blocks to AIInitDMA; a host thread
// plays them through SDL3 (audio_out.cpp) and raises the AI DMA interrupt each
// time a block starts, paced by the host device so the game's audio clock
// follows the sound card. Without a device the interrupt runs off a timer.
//
// DSP: JAudio's DSP driver (jaudio/dspboot.c, dspproc.c) talks to its microcode
// through a mailbox. Its entry points are hooked (tools/recomp/hooks.txt):
// command packets go to our microcode stand-in (audio_dsp.cpp), and every
// DSPReleaseHalt mixes one subframe and then raises the DSP interrupt that
// JAudio's audio thread waits for. See docs/AUDIO.md.
#include "recomp/audio.h"
#include "recomp/hle.h"
#include "recomp/sched.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

namespace recomp {
namespace {
std::atomic<u32> sDmaCallback{0};
u32 sStreamCallback = 0;
std::atomic<u32> sDmaAddr{0};
std::atomic<u32> sDmaLength{0};
std::atomic<bool> sDmaRunning{false};
std::atomic<u32> sDspRate{0};  // 0 = 32 kHz, 1 = 48 kHz
std::thread sDmaThread;

u32 rateHz() { return sDspRate.load() ? 48000 : 32000; }

void dmaThread() {
    using clock = std::chrono::steady_clock;
    auto next = clock::now();
    for (;;) {
        const u32 len = sDmaLength.load();
        const double secs = len ? (len / 4.0) / rateHz() : 0.005;  // 16-bit stereo frames
        if (audio::outHasDevice()) {
            // Keep about three blocks queued on the device: its clock paces the game.
            const double target = std::max(3 * secs, 0.040);
            while (audio::outQueuedSeconds() > target) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            next = clock::now();
        } else {
            next += std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(secs));
            if (clock::now() > next + std::chrono::milliseconds(100)) next = clock::now();
            std::this_thread::sleep_until(next);
        }
        const u32 cb = sDmaCallback.load();
        if (!sDmaRunning.load()) {
            if (audio::outHasDevice()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        // The block programmed by the last AIInitDMA starts playing now (the AI repeats
        // the previous block if the game did not supply a new one in time).
        if (const u32 addr = sDmaAddr.load(); addr && len) audio::outPushGuestBlock(addr, len);
        if (cb) sched::postIrq([cb](Ctx& ic) { guestCall(ic, cb); });
    }
}
}  // namespace

HLE(AIInit) {}
HLE(AIInitDMA) {
    sDmaAddr = c.r[3];
    sDmaLength = c.r[4];
}
HLE(AIStartDMA) {
    sDmaRunning = true;
    audio::outStart(rateHz());
    if (!sDmaThread.joinable()) {
        sDmaThread = std::thread(dmaThread);
        sDmaThread.detach();
    }
}
HLE(AIStopDMA) { sDmaRunning = false; }
HLE(AIGetDMAStartAddr) { c.r[3] = sDmaAddr; }
HLE(AISetDSPSampleRate) {
    sDspRate = c.r[3];
    audio::outSetRate(rateHz());
}
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
// DVD audio streaming (DTK). JAudio's StreamMain (jaudio/streamctrl.c) supports it,
// but the disc has no DTK tracks: LM's streamed music is AFC played through the DSP.
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
// JAudio DSP driver hooks (tools/recomp/hooks.txt).
HLE(DspBoot__Fv) {}  // nothing to upload: the "microcode" is audio_dsp.cpp

// DSPSendCommands(u32* cmds, u32 count): one mailbox packet.
HLE(DSPSendCommands__FPUlUl) {
    u32 words[16];
    const u32 n = std::min<u32>(c.r[4], 16);
    for (u32 i = 0; i < n; i++) words[i] = rd32(c.r[3] + i * 4);
    audio::dspCommand(words, n);
    c.r[3] = 0;
}

// The DSP halts after every subframe until the CPU has updated the voice blocks.
// Releasing it mixes the next subframe; its completion interrupt (__DspSync ->
// DspSync) wakes JAudio's audio thread for the next update.
HLE(DSPReleaseHalt__Fv) {
    if (!audio::dspRunSubframe()) return;
    static const u32 dspSync = guestSymbol("DspSync__Fv");
    sched::postIrq([](Ctx& ic) { guestCall(ic, dspSync); });
}
HLE(DSPWaitFinish__Fv) {}
HLE(DiplSec__FUl) {}
}  // namespace recomp
