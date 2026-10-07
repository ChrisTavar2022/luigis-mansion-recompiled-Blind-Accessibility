#include "recomp/interp.h"

#include "recomp/hle.h"

#include <dolphin/gx/GXAurora.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace recomp::interp {
namespace {

using Clock = std::chrono::steady_clock;
constexpr double kRetrace = 1001.0 / 60000.0;  // seconds per VI field (NTSC)
constexpr u32 kCaptureSize = 64u << 20;        // one guest frame's commands

struct Patch {
    u32 offset;  // first float, in tape bytes
    u8 count;    // floats
    Kind kind;
    u64 key;
};

struct Tape {
    std::vector<u8> bytes;
    std::vector<Patch> patches;
    std::unordered_map<u64, u32> byKey;  // key -> index into patches
    u32 retrace = 0;
    Clock::time_point done;
};

// --- guest side (guest thread only) ---
std::vector<u8> sScratch;
bool sCapturing = false;
u32 sBase = 0;  // start of the current capture segment in sScratch
std::vector<Patch> sPatches;
std::unordered_map<u64, u32> sOccurrences;
bool sOverflowLogged = false;

// --- shared ---
std::mutex sM;
std::shared_ptr<const Tape> sNewest, sPrevious;

// --- host side ---
std::vector<u8> sReplay;
u32 sPresented = 0, sTapes = 0, sBlended = 0, sSnapped = 0;
Clock::time_point sStatsStart = Clock::now();
u64 sTapeBytes = 0;
// LM_INTERP_TRACE=1: per present, alpha, time since the last present and what
// could not be blended (by kind).
const bool sTrace = std::getenv("LM_INTERP_TRACE") != nullptr;
u32 sSnapKinds[4], sMissing;

f32 loadBE(const u8* p) { return std::bit_cast<f32>(u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3]); }
void storeBE(u8* p, f32 f) {
    const u32 v = std::bit_cast<u32>(f);
    p[0] = u8(v >> 24), p[1] = u8(v >> 16), p[2] = u8(v >> 8), p[3] = u8(v);
}

u64 mix(u64 h) {
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    return h;
}

// Whether a and b are close enough to blend; a jump (camera cut, teleport,
// texture scroll wrapping, a different object reusing the slot) shows b as is.
bool blendable(Kind kind, const f32* a, const f32* b, int n) {
    switch (kind) {
    case Kind::PosMtx:
        for (int i = 0; i < n; i++) {
            const bool translation = (i & 3) == 3;
            if (std::fabs(a[i] - b[i]) > (translation ? 400.0f : 0.35f)) return false;
        }
        return true;
    case Kind::NrmMtx:
        for (int i = 0; i < n; i++)
            if (std::fabs(a[i] - b[i]) > 0.35f) return false;
        return true;
    case Kind::TexMtx:  // scrolling coordinates wrap; projection-style matrices have large entries
        for (int i = 0; i < n; i++)
            if (std::fabs(a[i] - b[i]) > 0.2f * std::max({std::fabs(a[i]), std::fabs(b[i]), 1.0f})) return false;
        return true;
    case Kind::Projection:
        for (int i = 0; i < n; i++)
            if (std::fabs(a[i] - b[i]) > 0.25f * std::max({std::fabs(a[i]), std::fabs(b[i]), 1e-3f})) return false;
        return true;
    }
    return false;
}

}  // namespace

// --- guest ---------------------------------------------------------------------------

bool capturing() { return sCapturing; }

void beginTape() {
    if (sScratch.empty()) sScratch.resize(kCaptureSize);
    sBase = 0;
    sPatches.clear();
    sOccurrences.clear();
    AuroraCaptureBegin(sScratch.data(), kCaptureSize);
    sCapturing = true;
}

void endTape(u32 retraceCount) {
    if (!sCapturing) return;
    sCapturing = false;
    const u32 size = sBase + AuroraCaptureEnd();
    if (size + 4096 >= kCaptureSize) {  // the capture ran out of room: drop this frame
        if (!sOverflowLogged) log("[interp] frame larger than %u MiB; dropped", kCaptureSize >> 20);
        sOverflowLogged = true;
        return;
    }
    auto tape = std::make_shared<Tape>();
    tape->bytes.assign(sScratch.begin(), sScratch.begin() + size);
    tape->patches = std::move(sPatches);
    tape->byKey.reserve(tape->patches.size());
    for (u32 i = 0; i < tape->patches.size(); i++) tape->byKey.emplace(tape->patches[i].key, i);
    tape->retrace = retraceCount;
    tape->done = Clock::now();
    sPatches = {};
    std::lock_guard lk(sM);
    sPrevious = std::move(sNewest);
    sNewest = std::move(tape);
}

u32 mark() { return sCapturing ? sBase + AuroraCapturePos() : ~0u; }

void note(u32 at, Kind kind, u32 source, u32 slot, int nFloats) {
    if (at == ~0u || !sCapturing) return;
    // Aurora writes XF loads as 0x10, a u32 header, then the floats.
    if (at + 5 + 4 * u32(nFloats) > sBase + AuroraCapturePos() || sScratch[at] != 0x10) return;
    const u64 base = mix(u64(kind) << 56 ^ u64(slot) << 40 ^ source);
    const u32 occurrence = sOccurrences[base]++;
    sPatches.push_back({at + 5, u8(nFloats), kind, mix(base + occurrence)});
}

void suspendForDisplayList() {
    if (!sCapturing) return;
    sBase += AuroraCaptureEnd();  // padded with NOPs; the stream stays valid
}

void resumeAfterDisplayList() {
    if (!sCapturing) return;
    AuroraCaptureBegin(sScratch.data() + sBase, kCaptureSize - sBase);
}

// --- host ----------------------------------------------------------------------------

bool replay(bool final) {
    std::shared_ptr<const Tape> a, b;
    {
        std::lock_guard lk(sM);
        a = sPrevious;
        b = sNewest;
    }
    if (!b) return false;

    // One game frame behind: blend from a (alpha 0) to b (alpha 1) over the time
    // between the two game frames, starting when b was finished.
    float alpha = 1.0f;
    if (a && !final) {
        const double interval = std::clamp((b->retrace - a->retrace) * kRetrace, 0.008, 0.1);
        const double since = std::chrono::duration<double>(Clock::now() - b->done).count();
        alpha = float(std::clamp(since / interval, 0.0, 1.0));
    }

    sReplay.assign(b->bytes.begin(), b->bytes.end());
    if (alpha < 1.0f) {
        for (const Patch& p : b->patches) {
            const auto it = a->byKey.find(p.key);
            if (it == a->byKey.end()) {
                sMissing++;
                continue;
            }
            const Patch& q = a->patches[it->second];
            if (q.count != p.count) continue;
            f32 va[16], vb[16];
            for (int i = 0; i < p.count; i++) {
                va[i] = loadBE(a->bytes.data() + q.offset + 4 * i);
                vb[i] = loadBE(b->bytes.data() + p.offset + 4 * i);
            }
            // A projection is only comparable to one of the same type (the u32 after it).
            if (p.kind == Kind::Projection &&
                std::memcmp(a->bytes.data() + q.offset + 24, b->bytes.data() + p.offset + 24, 4) != 0)
                continue;
            if (!blendable(p.kind, va, vb, p.count)) {
                sSnapped++;
                sSnapKinds[u32(p.kind)]++;
                continue;
            }
            for (int i = 0; i < p.count; i++)
                storeBE(sReplay.data() + p.offset + 4 * i, va[i] + (vb[i] - va[i]) * alpha);
            sBlended++;
        }
    }
    AuroraReplay(sReplay.data(), u32(sReplay.size()));
    if (sTrace) {
        static Clock::time_point last = Clock::now();
        const auto now = Clock::now();
        log("[interp] alpha %.3f dt %.1f ms tape %u snapped pos %u nrm %u tex %u proj %u, unmatched %u", alpha,
            std::chrono::duration<double, std::milli>(now - last).count(), b->retrace, sSnapKinds[0], sSnapKinds[1],
            sSnapKinds[2], sSnapKinds[3], sMissing);
        last = now;
        sSnapKinds[0] = sSnapKinds[1] = sSnapKinds[2] = sSnapKinds[3] = sMissing = 0;
    }

    // Stats, every 5 s.
    sPresented++;
    static u32 lastRetrace = 0;
    if (b->retrace != lastRetrace) {
        lastRetrace = b->retrace;
        sTapes++;
        sTapeBytes += b->bytes.size();
    }
    const double elapsed = std::chrono::duration<double>(Clock::now() - sStatsStart).count();
    if (elapsed >= 5.0) {
        log("[interp] presented %.1f fps, game %.1f fps, tape %.0f KiB, %u matrices blended / %u snapped per present",
            sPresented / elapsed, sTapes / elapsed, sTapes ? double(sTapeBytes) / sTapes / 1024.0 : 0.0,
            sPresented ? sBlended / sPresented : 0, sPresented ? sSnapped / sPresented : 0);
        sPresented = sTapes = sBlended = sSnapped = 0;
        sTapeBytes = 0;
        sStatsStart = Clock::now();
    }
    return true;
}

void reset() {
    std::lock_guard lk(sM);
    sNewest.reset();
    sPrevious.reset();
}

}  // namespace recomp::interp
