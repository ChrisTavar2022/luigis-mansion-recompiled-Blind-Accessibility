// Stand-in for the audio DSP microcode that JAudio (jaudio/dspdriver.c,
// dspinterface.c) drives. The DSP itself is never emulated: we take the
// command packets JAudio sends through DSPSendCommands, read the per-voice
// parameter blocks the CPU writes into main memory, and mix the voices on the
// host the way the microcode would: sample decoding from ARAM (AFC ADPCM, PCM8,
// PCM16), 4-tap polyphase resampling, per-bus volume ramps, and the per-voice
// status JAudio reads back (end-of-sample flag, play position). See docs/AUDIO.md
// for the block layout and the frame protocol.
#include "recomp/audio.h"
#include "recomp/hle.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>

namespace recomp::audio {
bool g_log = [] {
    const char* e = std::getenv("LM_AUDIO_LOG");
    return e && *e && *e != '0';
}();

namespace {

constexpr u32 kSubframe = 80;      // samples per subframe (fixed by the microcode)
constexpr u32 kBlockSize = 0x180;  // bytes per voice parameter block
constexpr u32 kMaxVoices = 64;
constexpr int kMixers = 6;
constexpr u32 kNoSample = 0xFFFFFFFF;

// Voice parameter block offsets (bytes, big-endian fields).
enum : u32 {
    kEnabled = 0x000,
    kDone = 0x002,       // set by the DSP when a one-shot sample ends / a stop finishes
    kPitch = 0x004,      // 0x1000 = one source sample per output sample
    kReset = 0x008,      // set by DSP_PlayStart; the DSP restarts the voice and clears it
    kPause = 0x00C,
    kMixer = 0x010,      // 6 x { u16 bus, s16 target volume, s16 current volume, u16 ramp }
    kPosFrac = 0x060,
    kBlockSamples = 0x064,
    kPosition = 0x068,   // u32, written back
    kRemaining = 0x06C,  // u16, written back (read by the AFC stream player)
    kRamRemaining = 0x074,  // ring voices: total samples left, counted down by the DSP
    kSourceType = 0x100,
    kLoopFlag = 0x102,
    kLoopYn1 = 0x104,
    kLoopYn2 = 0x106,
    kFilterMode = 0x108,
    kStopReq = 0x10A,    // set by ForceStopDSPchannel: fade out, then flag done
    kLoopStart = 0x110,
    kEnd = 0x114,
    kAramAddr = 0x118,
    kSampleCount = 0x11C,
};

// Source types (block +0x100).
enum : u16 { kSrcAfc5 = 0x05, kSrcPcm8 = 0x08, kSrcAfc9 = 0x09, kSrcPcm16 = 0x10, kSrcRamPcm16 = 0x21 };

// Output buses: DSP-memory buffer addresses from JAudio's bus table.
constexpr u16 kBusLeft = 0x0D00, kBusRight = 0x0D60;

// AFC coefficient pairs, used until JAudio's setup command hands us its own table.
constexpr s16 kAfcCoefDefault[16][2] = {
    {0, 0}, {2048, 0}, {0, 2048}, {1024, 1024}, {4096, -2048}, {3584, -1536}, {3072, -1024}, {4608, -2560},
    {4200, -2248}, {4800, -2300}, {5120, -3072}, {2048, -2048}, {1024, -1024}, {-1024, 1024}, {-1024, 0}, {-2048, 0},
};

struct Voice {
    bool playing = false;
    bool lastEnabled = false;
    u32 cur = 0;     // next source sample to fetch
    u32 winIdx[4]{};  // source index of each window sample (kNoSample past the end)
    u32 frac = 0;    // 16.16 fraction between win[1] and win[2]
    s16 win[4]{};
    s16 hist1 = 0, hist2 = 0;
    u32 lastIdx = 0;  // index of the sample fetch() returned last
    u32 ramStartOff = 0, ramTotal = 0;  // type 0x21 ring voices
    s32 frameIdx = -1;
    s16 frame[16]{};
    bool sourceEnded = false;
    bool warned = false;
    bool loudLogged = false;
};

struct State {
    u32 chanBase = 0, numVoices = 0, fxBase = 0;
    s16 afcCoef[16][2]{};
    s16 interp[64][4]{};
    bool haveInterp = false;
    // Current frame.
    bool frameActive = false;
    u32 subframes = 0, sub = 0, bufL = 0, bufR = 0;
    u16 level = 0x4000;
    std::array<Voice, kMaxVoices> voices{};
    u32 busSeen[16]{};
    u16 busIds[16]{};
} S;

inline u16 bu16(u32 a) { return u16(rd16(a)); }
inline s16 bs16(u32 a) { return s16(rd16(a)); }

void setup(u32 count, u32 chanBase, u32 interpTable, u32 afcTable, u32 fxBase) {
    S.numVoices = std::min<u32>(count, kMaxVoices);
    S.chanBase = chanBase;
    S.fxBase = fxBase;
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 2; j++)
            S.afcCoef[i][j] = afcTable ? bs16(afcTable + (i * 2 + j) * 2) : kAfcCoefDefault[i][j];
    if (interpTable) {
        for (int p = 0; p < 64; p++)
            for (int t = 0; t < 4; t++) S.interp[p][t] = bs16(interpTable + (p * 4 + t) * 2);
        S.haveInterp = true;
    }
    S.voices = {};
    if (g_log) log("[audio] DSP setup: %u voices at %08X, interp %08X, afc %08X, fx %08X", count, chanBase, interpTable, afcTable, fxBase);
}

// Decodes AFC frame `f` of the voice's sample into v.frame, continuing from v.hist.
void decodeAfc(Voice& v, u32 aram, u16 type, u32 f) {
    const u32 frameBytes = type == kSrcAfc9 ? 9 : 5;
    const u8* p = aramRead(aram + f * frameBytes, frameBytes);
    if (!p) {
        std::fill(std::begin(v.frame), std::end(v.frame), s16(0));
        return;
    }
    const s32 scale = 1 << (p[0] >> 4);
    const s32 c1 = S.afcCoef[p[0] & 15][0], c2 = S.afcCoef[p[0] & 15][1];
    for (int i = 0; i < 16; i++) {
        s32 delta;
        if (type == kSrcAfc9) {
            const int nib = (p[1 + i / 2] >> (i & 1 ? 0 : 4)) & 15;
            delta = ((nib ^ 8) - 8) * scale << 11;
        } else {
            const int bits = (p[1 + i / 4] >> (6 - 2 * (i & 3))) & 3;
            delta = ((bits ^ 2) - 2) * scale << 13;
        }
        s32 s = (delta + c1 * v.hist1 + c2 * v.hist2) >> 11;
        s = std::clamp(s, -32768, 32767);
        v.frame[i] = s16(s);
        v.hist2 = v.hist1;
        v.hist1 = s16(s);
    }
    v.frameIdx = s32(f);
}

// Source type 0x21: PCM16 ring buffer in main memory, refilled by the CPU (AFC
// stream player, movie audio). +0x118 start address, +0x110 ring start, ring length
// in samples in the high half of +0x114, +0x74 total samples left to play (0 = no
// limit). Fetch ordinals count samples from the start of the voice.
s16 fetchRam(Voice& v, u32 blk) {
    const u32 total = v.ramTotal;
    if (total && v.cur >= total) {
        v.sourceEnded = true;
        return 0;
    }
    const u32 ring = rd32(blk + kLoopStart);
    const u32 len = std::max<u32>(rd32(blk + kEnd) >> 16, 1);
    const u32 n = v.cur++;
    v.lastIdx = n;
    const u32 off = (v.ramStartOff + n) % len;
    return s16(rd16(ring + off * 2));
}

s16 fetch(Voice& v, u32 blk) {
    v.lastIdx = kNoSample;
    if (v.sourceEnded) return 0;
    const u16 type = bu16(blk + kSourceType);
    if (type == kSrcRamPcm16) return fetchRam(v, blk);
    const u32 end = rd32(blk + kEnd);
    if (v.cur >= end) {
        if (bu16(blk + kLoopFlag)) {
            v.cur = rd32(blk + kLoopStart);
            v.hist1 = bs16(blk + kLoopYn1);
            v.hist2 = bs16(blk + kLoopYn2);
            v.frameIdx = -1;
            if (v.cur >= end) {
                v.sourceEnded = true;
                return 0;
            }
        } else {
            v.sourceEnded = true;
            return 0;
        }
    }
    const u32 aram = rd32(blk + kAramAddr);
    const u32 n = v.cur++;
    v.lastIdx = n;
    switch (type) {
    case kSrcPcm16: {
        const u8* p = aramRead(aram + n * 2, 2);
        return p ? s16(u16(p[0] << 8 | p[1])) : 0;
    }
    case kSrcPcm8: {
        const u8* p = aramRead(aram + n, 1);
        return p ? s16(s8(p[0]) * 256) : 0;
    }
    case kSrcAfc9:
    case kSrcAfc5: {
        const u32 f = n / 16;
        if (v.frameIdx != s32(f)) {
            // Frames decode in order; a jump (loop) restarts the history from the block.
            decodeAfc(v, aram, type, f);
        }
        return v.frame[n % 16];
    }
    default:
        if (!v.warned) {
            v.warned = true;
            if (g_log) log("[audio] voice uses unsupported source type %u (oscillator?)", type);
        }
        v.sourceEnded = true;
        return 0;
    }
}

void startVoice(Voice& v, u32 blk) {
    v = Voice{};
    v.playing = true;
    v.lastEnabled = true;
    if (bu16(blk + kSourceType) == kSrcRamPcm16) {
        const u32 len = std::max<u32>(rd32(blk + kEnd) >> 16, 1);
        v.ramStartOff = ((rd32(blk + kAramAddr) - rd32(blk + kLoopStart)) / 2) % len;
        v.ramTotal = rd32(blk + kRamRemaining);
    }
    v.win[0] = 0;
    v.winIdx[0] = kNoSample;
    for (int k = 1; k < 4; k++) {
        v.win[k] = fetch(v, blk);
        v.winIdx[k] = v.lastIdx;
    }
    v.frac = 0;
    if (g_log)
        log("[audio] voice %u start: type %u aram %06X loop %u [%u,%u) count %u pitch %04X", (blk - S.chanBase) / kBlockSize,
            bu16(blk + kSourceType), rd32(blk + kAramAddr), bu16(blk + kLoopFlag), rd32(blk + kLoopStart), rd32(blk + kEnd),
            rd32(blk + kSampleCount), bu16(blk + kPitch));
    if (g_log)
        for (int m = 0; m < kMixers; m++) {
            const u32 mx = blk + kMixer + m * 8;
            if (bu16(mx)) log("[audio]    mixer %d: bus %04X target %04X current %04X ramp %04X", m, bu16(mx), bu16(mx + 2), bu16(mx + 4), bu16(mx + 6));
        }
}

// Mixes one subframe of voice `i` into the bus accumulators.
void mixVoice(u32 i, s32* outL, s32* outR) {
    const u32 blk = S.chanBase + i * kBlockSize;
    Voice& v = S.voices[i];
    const bool enabled = bu16(blk + kEnabled) != 0;
    if (!enabled) {
        v.playing = false;
        v.lastEnabled = false;
        return;
    }
    if (bu16(blk + kReset) || !v.lastEnabled) {
        wr16(blk + kReset, 0);
        startVoice(v, blk);
    }
    if (!v.playing || bu16(blk + kPause)) return;
    if (g_log && bu16(blk + kFilterMode)) {
        static u32 filtLogged = 0;
        if (filtLogged++ < 20)
            log("[audio] voice %u filter mode %04X iir %04X %04X %04X %04X fir %04X %04X %04X %04X %04X %04X %04X %04X dist %04X", i,
                bu16(blk + kFilterMode), bu16(blk + 0x148), bu16(blk + 0x14A), bu16(blk + 0x14C), bu16(blk + 0x14E),
                bu16(blk + 0x120), bu16(blk + 0x122), bu16(blk + 0x124), bu16(blk + 0x126), bu16(blk + 0x128),
                bu16(blk + 0x12A), bu16(blk + 0x12C), bu16(blk + 0x12E), bu16(blk + 0x150));
    }

    const bool stopping = bu16(blk + kStopReq) != 0;
    s16 tmp[kSubframe];
    const u32 step = u32(std::min<u16>(bu16(blk + kPitch), 0x7FFF)) << 4;
    for (u32 n = 0; n < kSubframe; n++) {
        const s16* c = S.interp[v.frac >> 10];
        const s32 s = (v.win[0] * c[0] + v.win[1] * c[1] + v.win[2] * c[2] + v.win[3] * c[3]) >> 15;
        tmp[n] = s16(std::clamp(s, -32768, 32767));
        v.frac += step;
        while (v.frac >= 0x10000) {
            v.frac -= 0x10000;
            v.win[0] = v.win[1];
            v.win[1] = v.win[2];
            v.win[2] = v.win[3];
            v.win[3] = fetch(v, blk);
            v.winIdx[0] = v.winIdx[1];
            v.winIdx[1] = v.winIdx[2];
            v.winIdx[2] = v.winIdx[3];
            v.winIdx[3] = v.lastIdx;
        }
    }

    for (int m = 0; m < kMixers; m++) {
        const u32 mx = blk + kMixer + m * 8;
        const u16 bus = bu16(mx);
        if (!bus) continue;
        const s32 target = stopping ? 0 : bs16(mx + 2);
        const s32 from = bs16(mx + 4);
        wr16(mx + 4, u32(target));
        if (g_log) {
            for (int b = 0; b < 16; b++) {
                if (S.busIds[b] == bus || S.busIds[b] == 0) {
                    S.busIds[b] = bus;
                    S.busSeen[b]++;
                    break;
                }
            }
        }
        s32* out = bus == kBusLeft ? outL : bus == kBusRight ? outR : nullptr;
        if (g_log && !out && (from || target)) {
            static u32 auxLogged = 0;
            if (auxLogged++ < 20) log("[audio] voice %u sends to aux bus %04X vol %04X", i, bus, u16(target));
        }
        if (!out || (from == 0 && target == 0)) continue;
        for (u32 n = 0; n < kSubframe; n++) {
            const s32 vol = from + (target - from) * s32(n + 1) / s32(kSubframe);
            out[n] += (tmp[n] * vol) >> 15;
            if (g_log && std::abs((tmp[n] * vol) >> 15) > 3000 && !v.loudLogged) {
                v.loudLogged = true;
                log("[audio] voice %u loud: sample %d vol %04X->%04X bus %04X pitch %04X", i, tmp[n], u16(from), u16(target), bus, bu16(blk + kPitch));
            }
        }
    }

    // Status the CPU reads back.
    if (bu16(blk + kSourceType) == kSrcRamPcm16) {
        // Ring voices: samples left before the ring wraps (16.16, read by the stream
        // and movie players to know which part of the ring is free) and samples left
        // in total (A/V sync).
        const u32 len = std::max<u32>(rd32(blk + kEnd) >> 16, 1);
        const u32 played = v.winIdx[1] == kNoSample ? v.cur : v.winIdx[1];
        wr32(blk + kRemaining, (len - (v.ramStartOff + played) % len) << 16);
        if (v.ramTotal) wr32(blk + kRamRemaining, v.ramTotal > played ? v.ramTotal - played : 0);
        wr32(blk + kPosition, played);
    } else {
        const u32 end = rd32(blk + kEnd);
        const u32 pos = v.winIdx[1] == kNoSample ? end : v.winIdx[1];
        wr32(blk + kPosition, pos);
        wr16(blk + kRemaining, std::min<u32>(end > pos ? end - pos : 0, 0xFFFF));
    }
    wr16(blk + kPosFrac, v.frac);
    // A one-shot is finished once the window has moved past its last real sample.
    if (stopping || (v.sourceEnded && v.winIdx[1] == kNoSample)) {
        v.playing = false;
        wr16(blk + kDone, 1);
    }
}

void runSubframe() {
    s32 accL[kSubframe]{}, accR[kSubframe]{};
    if (S.chanBase && S.haveInterp)
        for (u32 i = 0; i < S.numVoices; i++) mixVoice(i, accL, accR);
    const u32 off = S.sub * kSubframe * 2;
    for (u32 n = 0; n < kSubframe; n++) {
        // The frame's mixer level is 4.12 fixed point (JAudio: DsetMixerLevel(f) = f * 4096).
        const s32 l = (accL[n] * s32(S.level)) >> 12;
        const s32 r = (accR[n] * s32(S.level)) >> 12;
        wr16(S.bufL + off + n * 2, u32(std::clamp(l, -32768, 32767)));
        wr16(S.bufR + off + n * 2, u32(std::clamp(r, -32768, 32767)));
    }
}

}  // namespace

void dspCommand(const u32* w, u32 count) {
    if (!count) return;
    switch (w[0] >> 24) {
    case 0x81:  // setup: voice count | 0x81000000, voice blocks, interpolation table, AFC table, FX lines
        if (count >= 5) setup(w[0] & 0xFFFF, w[1], w[2], w[3], w[4]);
        break;
    case 0x82:  // sync frame: subframes << 16 | mixer level, left buffer, right buffer
        if (count >= 3) {
            S.subframes = (w[0] >> 16) & 0xFF;
            S.level = u16(w[0]);
            S.bufL = w[1];
            S.bufR = w[2];
            S.sub = 0;
            S.frameActive = S.subframes != 0;
        }
        break;
    case 0x80:  // wait frame: nothing to wait for
        break;
    default:
        if (g_log) log("[audio] unknown DSP command %08X (%u words)", w[0], count);
        break;
    }
}

bool dspRunSubframe() {
    if (!S.frameActive) return false;
    runSubframe();
    if (++S.sub >= S.subframes) S.frameActive = false;
    if (g_log) {
        static u32 frames = 0;
        if (!S.frameActive && ++frames % 600 == 0) {
            u32 active = 0;
            for (auto& v : S.voices) active += v.playing;
            log("[audio] %u DSP frames, %u voices playing, level %04X", frames, active, S.level);
            for (int b = 0; b < 16 && S.busIds[b]; b++) log("[audio]   bus %04X used %u", S.busIds[b], S.busSeen[b]);
            for (u32 l = 0; S.fxBase && l < 4; l++) {
                const u32 f = S.fxBase + l * 0x20;
                log("[audio]   fx %u: %04X %04X %08X %04X %04X %04X %04X | %04X %04X %04X %04X %04X %04X %04X %04X", l, bu16(f),
                    bu16(f + 2), rd32(f + 4), bu16(f + 8), bu16(f + 10), bu16(f + 12), bu16(f + 14), bu16(f + 16), bu16(f + 18),
                    bu16(f + 20), bu16(f + 22), bu16(f + 24), bu16(f + 26), bu16(f + 28), bu16(f + 30));
            }
        }
    }
    return true;
}

}  // namespace recomp::audio
