#pragma once
// Audio back end shared by hle_audio.cpp (AI/DSP SDK + JAudio driver hooks),
// audio_dsp.cpp (our stand-in for the JAudio DSP microcode) and audio_out.cpp
// (host output through SDL3, WAV capture). See docs/AUDIO.md.
#include "recomp/ctx.h"

namespace recomp::audio {

// ---- DSP (audio_dsp.cpp) ----
// One command packet from JAudio's DSPSendCommands(cmds, count).
void dspCommand(const u32* words, u32 count);
// DSPReleaseHalt: the CPU finished its per-subframe channel updates; mix the next
// subframe. Returns true when a subframe was mixed (the DSP interrupt is due).
bool dspRunSubframe();

// ---- ARAM (hle_ar.cpp) ----
// Host view of `len` bytes of ARAM at `addr`, or nullptr when out of range.
const u8* aramRead(u32 addr, u32 len);

// ---- Output (audio_out.cpp) ----
// Starts the host device (once). `rate` is the AI DSP sample rate in Hz.
void outStart(u32 rate);
void outSetRate(u32 rate);
// Queues one AI DMA block: big-endian s16 stereo frames read from guest memory.
void outPushGuestBlock(u32 addr, u32 bytes);
// Seconds of audio queued on the host device (0 when there is no device).
double outQueuedSeconds();
bool outHasDevice();

// Debug logging (LM_AUDIO_LOG=1).
extern bool g_log;

}  // namespace recomp::audio
