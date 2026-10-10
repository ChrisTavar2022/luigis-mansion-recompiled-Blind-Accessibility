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
#include "port/settings.h"
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

// The console's real rates (audio.h): the game's audio clock runs at these, so music
// tempo and pitch match the console (0.09% above the nominal 32000 Hz).
double rateHz() { return sDspRate.load() ? audio::kConsoleRate48 : audio::kConsoleRate32; }

void dmaThread() {
    using clock = std::chrono::steady_clock;
    auto next = clock::now();
    for (;;) {
        if (audio::outPaused()) {  // game paused by the host: no audio interrupts
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            next = clock::now();
            continue;
        }
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
// ---- The game's sound mode (docs/AUDIO.md, "Surround") ----
// The options menu, and the code that applies saved options (fn_8005582C, fn_800558C8,
// options struct byte 0: 0 mono, 1 stereo, 2 surround), all go through fn_8018A70C(mode),
// which stores the mode in JAIBasic +0x41 (sequences then take their Dolby surround
// send from their own table in mode 2, 0 otherwise), sets JAudio's output mode (0 mono:
// centred pan, no surround send; 1 otherwise) and the AFC streamer's mono flag. The port's
// F1 > Sound > Output is the same setting: saved options are overridden with it, the
// game's own menu changes it, and changing it in F1 applies it to the game.
namespace {
int sGameMode = -1;           // last mode applied through fn_8018A70C
bool sApplyingSaved = false;  // inside fn_8005582C / fn_800558C8
bool sPortApplying = false;   // our own call (syncSoundMode)
u32 sOptions = 0;             // the game's options struct (byte 0 = sound mode)

int modeFor(port::AudioOutput o) {
    switch (o) {
    case port::AudioOutput::Mono: return 0;
    case port::AudioOutput::Stereo: return 1;
    default: return 2;
    }
}

// The game asked for `mode` from its own options menu: F1 follows (keeping 5.1 / 7.1 when
// the game chose surround and one of those was already selected).
void adoptGameMode(int mode) {
    auto& a = port::settings().audio;
    if (modeFor(a.output) == mode) return;
    a.output = mode == 0 ? port::AudioOutput::Mono : mode == 1 ? port::AudioOutput::Stereo : port::AudioOutput::Surround;
    port::saveSettings();
    if (audio::g_log) log("[audio] game options: sound mode %d -> F1 output %d", mode, int(a.output));
}

void applySavedOptions(Ctx& c, const char* name) {
    sOptions = c.r[3];
    const int want = modeFor(port::settings().audio.output);
    wr8(sOptions, u32(want));
    sApplyingSaved = true;
    callOriginal(c, name);
    sApplyingSaved = false;
    wr8(sOptions, u32(want));  // the original forces mono when the console's SRAM says mono
}

// F1 changed the output: apply it to the game (from JAudio's audio thread, where the
// mode is read). Only once the game has set a mode itself (JAudio is up).
void syncSoundMode(Ctx& c) {
    if (sGameMode < 0) return;
    const int want = modeFor(port::settings().audio.output);
    if (want == sGameMode) return;
    static const u32 fn = guestSymbol("fn_8018A70C");
    if (!fn) return;
    sPortApplying = true;
    guestCall(c, fn, u32(want));
    sPortApplying = false;
    if (sOptions) wr8(sOptions, u32(want));  // the options menu shows it, saves keep it
}
}  // namespace

int audio::gameSoundMode() { return sGameMode; }

HLE(fn_8005582C) { applySavedOptions(c, "fn_8005582C"); }
HLE(fn_800558C8) { applySavedOptions(c, "fn_800558C8"); }
HLE(fn_8018A70C) {
    const int req = int(c.r[3]);
    if (sApplyingSaved || sPortApplying) {
        c.r[3] = u32(modeFor(port::settings().audio.output));
    } else {
        adoptGameMode(req);
    }
    if (audio::g_log && int(c.r[3]) != sGameMode)
        log("[audio] sound mode %d (game asked %d%s)", int(c.r[3]), req,
            sApplyingSaved ? ", saved options" : sPortApplying ? ", F1" : ", options menu");
    sGameMode = int(c.r[3]);
    callOriginal(c, "fn_8018A70C");
}

// Jac_imixcopy(bufR, bufL, dac, frames): Jac_VframeWork interleaves the finished DSP
// buffer pair into the next AI block. The output keeps that buffer's side data (centre /
// surround buses) for when the block plays (discrete 5.1 / 7.1).
HLE(Jac_imixcopy__FPsPsPsl) {
    const u32 bufL = c.r[4], dac = c.r[5], frames = c.r[6];
    callOriginal(c, "Jac_imixcopy__FPsPsPsl");
    audio::outTagBlock(dac, bufL, frames);
}

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
    syncSoundMode(c);
    if (!audio::dspRunSubframe()) return;
    static const u32 dspSync = guestSymbol("DspSync__Fv");
    sched::postIrq([](Ctx& ic) { guestCall(ic, dspSync); });
}
HLE(DSPWaitFinish__Fv) {}
HLE(DiplSec__FUl) {}
}  // namespace recomp
