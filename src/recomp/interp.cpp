// Frame interpolation (interp.h, docs/INTERPOLATION.md): tapes, pairing, blended replay.
#include "recomp/interp.h"

#include "recomp/efb_copy.h"
#include "recomp/hle.h"

#include <dolphin/gx/GXAurora.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace recomp::interp {

u64 detail::mix(u64 h) {
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ull;
    h ^= h >> 33;
    return h;
}

namespace {

using detail::mix;
using Clock = std::chrono::steady_clock;
constexpr double kRetrace = 1001.0 / 60000.0;  // seconds per VI field (NTSC)
constexpr u32 kCaptureSize = 64u << 20;        // one guest frame's commands
constexpr u32 kKinds = u32(Kind::Count);

// A value in the tape that moves between frames.
struct Patch {
    u32 offset;   // first float, in tape bytes
    u16 count;    // floats (Verts: vertices)
    Kind kind;
    u64 group;    // what it belongs to: kind, slot and the draw that follows it
    u64 origin;   // kind, slot and source address (second-chance pairing)
    u32 source, slot;
    u16 stride = 0, posOffset = 0;  // Verts
};

struct Tape {
    std::vector<u8> bytes;
    std::vector<Patch> patches;
    std::vector<std::vector<u8>> arrays;  // vertex-array copies the tape's array loads point at
    u64 arrayBytes = 0;
    u32 retrace = 0;
    Clock::time_point done;
    // Pairing with the tape captured before this one (`pairedWith`, compared by identity).
    const Tape* pairedWith = nullptr;
    std::vector<s32> partner;  // per patch: index into pairedWith->patches, or -1
    bool cut = false;          // a camera cut since pairedWith: shown as is
    u32 paired = 0, unpaired = 0;
    u32 dumpOffset = ~0u;  // LM_INTERP_TEST: tape offset of the LM_FRAME_DUMP copy's target pointer
    u32 dumpFrame = 0;
    u32 guestUs = 0;  // guest-thread time spent closing the tape (array copies' bookkeeping, pairing)
};

// --- guest side (guest thread only) ---
std::vector<u8> sScratch;
bool sCapturing = false;
bool sSuspended = false;  // the game is recording its own display list
u32 sBase = 0;            // start of the current capture segment in sScratch
std::vector<Patch> sPatches;
u32 sUngrouped = 0;  // patches from here on wait for their draw
bool sOverflowLogged = false;
u32 sDumpAt = ~0u, sDumpFrame = 0;
const void* sDumpPtr = nullptr;
std::shared_ptr<Tape> sLastTape;  // the previous tape, for pairing

// LM_INTERP_TEST=<steps> (with LM_FRAME_DUMP): deterministic interpolation test. The guest
// waits after each frame N while the host renders, with guest memory as it is at the end of
// N: tape N as is (fN_fresh, the 30 Hz frame), then frames N-2 -> N-1 at alpha s/steps
// (fN-1_s<s>; s = steps is tape N-1 replayed one game frame late, as the real replay is).
const int sTestSteps = [] {
    const char* v = std::getenv("LM_INTERP_TEST");
    return v ? std::max(1, std::atoi(v)) : 0;
}();
std::mutex sTestM;
std::condition_variable sTestCv;
bool sTestPending = false;  // the guest waits for the host to finish its tape

// --- shared ---
std::mutex sM;
std::shared_ptr<const Tape> sNewest, sPrevious;
// Tapes the FIFO may still be reading (their vertex-array copies) after being replaced.
std::deque<std::shared_ptr<const Tape>> sRetired;

// --- host side ---
std::vector<u8> sReplay;
u32 sPresented = 0, sTapes = 0, sBlended = 0, sSnapped = 0, sCuts = 0;
Clock::time_point sStatsStart = Clock::now();
u64 sTapeBytes = 0, sArrayBytes = 0, sPairedSum = 0, sUnpairedSum = 0, sGuestUs = 0, sReplayUs = 0;
// LM_INTERP_TRACE=1: per present, alpha, time since the last present and what
// could not be blended (by kind).
const bool sTrace = std::getenv("LM_INTERP_TRACE") != nullptr;
u32 sSnapKinds[kKinds];
bool sLogSnaps = false;  // LM_INTERP_TEST + trace: log what one test replay snapped
u32 sLoggedSnaps[kKinds];
// LM_INTERP_PAIRING=source: the old pairing by (source address, slot, occurrence), for comparison.
const bool sPairBySource = [] {
    const char* v = std::getenv("LM_INTERP_PAIRING");
    return v && std::strcmp(v, "source") == 0;
}();

f32 loadBE(const u8* p) { return std::bit_cast<f32>(u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3]); }
void storeBE(u8* p, f32 f) {
    const u32 v = std::bit_cast<u32>(f);
    p[0] = u8(v >> 24), p[1] = u8(v >> 16), p[2] = u8(v >> 8), p[3] = u8(v);
}
f32 val(const Tape& t, const Patch& p, int i) { return loadBE(t.bytes.data() + p.offset + 4 * i); }
// Verts: coordinate c of vertex v.
f32 vtx(const Tape& t, const Patch& p, u32 v, int c) {
    return loadBE(t.bytes.data() + p.offset + v * p.stride + p.posOffset + 4 * c);
}

// --- pairing ----------------------------------------------------------------------------

// How different two values of the same kind are (for choosing among candidates).
float distance(const Tape& ta, const Patch& a, const Tape& tb, const Patch& b) {
    float d = 0;
    if (a.kind == Kind::Verts) {
        const u32 n = std::min<u32>(a.count, 4);
        for (u32 v = 0; v < n; v++)
            for (int c = 0; c < 3; c++) d += std::fabs(vtx(ta, a, v, c) - vtx(tb, b, v, c));
        return d;
    }
    for (int i = 0; i < a.count; i++) {
        const float diff = std::fabs(val(ta, a, i) - val(tb, b, i));
        // Matrices: rotation/scale entries are ~1, translations are in world units.
        d += (a.kind == Kind::PosMtx && (i & 3) != 3) ? diff * 100.0f : diff;
    }
    return d;
}

bool compatible(const Patch& a, const Patch& b) {
    return a.kind == b.kind && a.count == b.count && a.stride == b.stride && a.posOffset == b.posOffset;
}

// Pairs the members of one group: nearest values first, so two instances of the same
// mesh (or two bones of one skeleton loaded into the same slot) don't swap.
void pairGroup(const Tape& a, const std::vector<u32>& A, const Tape& b, const std::vector<u32>& B,
               std::vector<s32>& partner, std::vector<u8>& usedA) {
    if (A.size() == 1 && B.size() == 1) {
        if (!usedA[A[0]] && partner[B[0]] < 0 && compatible(a.patches[A[0]], b.patches[B[0]])) {
            partner[B[0]] = s32(A[0]);
            usedA[A[0]] = 1;
        }
        return;
    }
    if (A.size() * B.size() > 4096) {  // large groups: in order
        for (size_t k = 0; k < std::min(A.size(), B.size()); k++)
            if (!usedA[A[k]] && partner[B[k]] < 0 && compatible(a.patches[A[k]], b.patches[B[k]])) {
                partner[B[k]] = s32(A[k]);
                usedA[A[k]] = 1;
            }
        return;
    }
    static std::vector<std::tuple<float, u32, u32>> cand;
    cand.clear();
    for (u32 i : A) {
        if (usedA[i]) continue;
        for (u32 j : B)
            if (partner[j] < 0 && compatible(a.patches[i], b.patches[j]))
                cand.emplace_back(distance(a, a.patches[i], b, b.patches[j]), i, j);
    }
    std::sort(cand.begin(), cand.end());
    for (const auto& [d, i, j] : cand)
        if (!usedA[i] && partner[j] < 0) {
            partner[j] = s32(i);
            usedA[i] = 1;
        }
}

// A 3x3 (cols 3) or 3x4 (cols 4, row-major) matrix's 3x3 part as M = Q U: a rotation Q (as
// a quaternion) times an upper-triangular scale/shear U (Gram-Schmidt on the columns; a
// mirrored matrix gets a negative U[2][2]). Model-view and normal matrices blend well this
// way, including the sheared ones a non-uniformly scaled parent produces.
struct Rigid {
    float q[4];     // x, y, z, w
    float u[3][3];  // upper triangular
};

bool decompose(const f32* m, int cols, Rigid& out) {
    float c[3][3];  // columns
    for (int j = 0; j < 3; j++)
        for (int r = 0; r < 3; r++) c[j][r] = m[cols * r + j];
    auto dot = [](const float* x, const float* y) { return x[0] * y[0] + x[1] * y[1] + x[2] * y[2]; };
    float e[3][3];  // orthonormal columns
    std::memset(out.u, 0, sizeof(out.u));
    for (int j = 0; j < 3; j++) {
        float v[3] = {c[j][0], c[j][1], c[j][2]};
        for (int i = 0; i < j; i++) {
            const float d = dot(e[i], c[j]);
            out.u[i][j] = d;
            for (int r = 0; r < 3; r++) v[r] -= d * e[i][r];
        }
        const float len = std::sqrt(dot(v, v));
        if (!(len > 1e-7f) || !std::isfinite(len)) return false;
        out.u[j][j] = len;
        for (int r = 0; r < 3; r++) e[j][r] = v[r] / len;
    }
    const float cr[3] = {e[0][1] * e[1][2] - e[0][2] * e[1][1], e[0][2] * e[1][0] - e[0][0] * e[1][2],
                         e[0][0] * e[1][1] - e[0][1] * e[1][0]};
    if (dot(cr, e[2]) < 0) {  // mirrored: keep Q a rotation
        for (int r = 0; r < 3; r++) e[2][r] = -e[2][r];
        out.u[2][2] = -out.u[2][2];
    }
    // Rotation matrix R[r][j] = e[j][r] to quaternion.
    const float m00 = e[0][0], m01 = e[1][0], m02 = e[2][0], m10 = e[0][1], m11 = e[1][1], m12 = e[2][1], m20 = e[0][2],
                m21 = e[1][2], m22 = e[2][2];
    const float tr = m00 + m11 + m22;
    float* q = out.q;
    if (tr > 0) {
        const float k = 0.5f / std::sqrt(tr + 1.0f);
        q[3] = 0.25f / k, q[0] = (m21 - m12) * k, q[1] = (m02 - m20) * k, q[2] = (m10 - m01) * k;
    } else if (m00 > m11 && m00 > m22) {
        const float k = 2.0f * std::sqrt(1.0f + m00 - m11 - m22);
        q[3] = (m21 - m12) / k, q[0] = 0.25f * k, q[1] = (m01 + m10) / k, q[2] = (m02 + m20) / k;
    } else if (m11 > m22) {
        const float k = 2.0f * std::sqrt(1.0f + m11 - m00 - m22);
        q[3] = (m02 - m20) / k, q[0] = (m01 + m10) / k, q[1] = 0.25f * k, q[2] = (m12 + m21) / k;
    } else {
        const float k = 2.0f * std::sqrt(1.0f + m22 - m00 - m11);
        q[3] = (m10 - m01) / k, q[0] = (m02 + m20) / k, q[1] = (m12 + m21) / k, q[2] = 0.25f * k;
    }
    return true;
}

// Blends two matrices by rotation (slerp), scale/shear and translation, so a turning
// object keeps its shape at in-between frames (an entrywise blend shrinks it).
bool blendRigid(const f32* a, const f32* b, int cols, float t, f32* out) {
    Rigid ra, rb;
    if (!decompose(a, cols, ra) || !decompose(b, cols, rb) || (ra.u[2][2] < 0) != (rb.u[2][2] < 0)) return false;
    float d = ra.q[0] * rb.q[0] + ra.q[1] * rb.q[1] + ra.q[2] * rb.q[2] + ra.q[3] * rb.q[3];
    float qb[4] = {rb.q[0], rb.q[1], rb.q[2], rb.q[3]};
    if (d < 0) {
        d = -d;
        for (float& v : qb) v = -v;
    }
    float wa = 1.0f - t, wb = t;
    if (d < 0.9995f) {
        const float th = std::acos(std::min(d, 1.0f)), sn = std::sin(th);
        wa = std::sin((1.0f - t) * th) / sn;
        wb = std::sin(t * th) / sn;
    }
    float q[4];
    float len = 0;
    for (int k = 0; k < 4; k++) q[k] = wa * ra.q[k] + wb * qb[k], len += q[k] * q[k];
    len = std::sqrt(len);
    for (float& v : q) v /= len;
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float R[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
                           {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
                           {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
    float U[3][3];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) U[i][j] = ra.u[i][j] + (rb.u[i][j] - ra.u[i][j]) * t;
    for (int r = 0; r < 3; r++)
        for (int j = 0; j < 3; j++) out[cols * r + j] = R[r][0] * U[0][j] + R[r][1] * U[1][j] + R[r][2] * U[2][j];
    if (cols == 4)
        for (int r = 0; r < 3; r++) out[4 * r + 3] = a[4 * r + 3] + (b[4 * r + 3] - a[4 * r + 3]) * t;
    return true;
}

bool blendable(Kind kind, const f32* a, const f32* b, int n);

void pairTapes(const Tape& a, Tape& b) {
    b.pairedWith = &a;
    b.partner.assign(b.patches.size(), -1);
    std::vector<u8> usedA(a.patches.size(), 0);
    auto groups = [](const Tape& t, bool origin) {
        std::unordered_map<u64, std::vector<u32>> g;
        g.reserve(t.patches.size());
        for (u32 i = 0; i < t.patches.size(); i++) g[origin ? t.patches[i].origin : t.patches[i].group].push_back(i);
        return g;
    };
    // By draw first, then what is left by source address and slot.
    for (int pass = sPairBySource ? 1 : 0; pass < 2; pass++) {
        const auto ga = groups(a, pass == 1), gb = groups(b, pass == 1);
        for (const auto& [key, B] : gb) {
            const auto it = ga.find(key);
            if (it != ga.end()) pairGroup(a, it->second, b, B, b.partner, usedA);
        }
    }
    // A camera cut (or a teleport) moves nearly every object at once: show the new frame as is.
    u32 pos = 0, jumps = 0;
    b.paired = b.unpaired = 0;
    for (u32 i = 0; i < b.patches.size(); i++) {
        const Patch& p = b.patches[i];
        if (b.partner[i] < 0) {
            b.unpaired++;
            continue;
        }
        b.paired++;
        if (p.kind != Kind::PosMtx) continue;
        const Patch& q = a.patches[b.partner[i]];
        f32 va[12], vb[12];
        for (int k = 0; k < 12; k++) va[k] = val(a, q, k), vb[k] = val(b, p, k);
        pos++;
        if (!blendable(Kind::PosMtx, va, vb, 12)) jumps++;
    }
    b.cut = pos >= 8 && jumps * 2 > pos;
}

// Whether a and b are close enough to blend; a jump (teleport, texture scroll wrapping,
// a spawn reusing a slot) shows b as is.
bool blendable(Kind kind, const f32* a, const f32* b, int n) {
    switch (kind) {
    case Kind::PosMtx:
    case Kind::NrmMtx: {
        // Rotation by up to ~150 degrees per game frame (Luigi turns around within a frame or
        // two; a swinging limb, or
        // the camera turning, which also swings far objects around in view space); a
        // translation jump larger than the distance (teleport, cut, respawn) snaps.
        const int cols = kind == Kind::PosMtx ? 4 : 3;
        Rigid ra, rb;
        if (!decompose(a, cols, ra) || !decompose(b, cols, rb)) {  // not a rotation and scale: entrywise
            for (int i = 0; i < n; i++)
                if (std::fabs(a[i] - b[i]) > ((cols == 4 && (i & 3) == 3) ? 400.0f : 0.35f)) return false;
            return true;
        }
        if ((ra.u[2][2] < 0) != (rb.u[2][2] < 0)) return false;
        if (std::fabs(ra.q[0] * rb.q[0] + ra.q[1] * rb.q[1] + ra.q[2] * rb.q[2] + ra.q[3] * rb.q[3]) < 0.26f)
            return false;  // cos(angle / 2) < 0.26: more than ~150 degrees, no telling which way
        for (int k = 0; k < 3; k++) {
            const float sa = std::fabs(ra.u[k][k]), sb = std::fabs(rb.u[k][k]);
            if (sb > 2.0f * sa + 1e-4f || sa > 2.0f * sb + 1e-4f) return false;
        }
        if (cols == 4) {
            float da = 0, db = 0, dd = 0;
            for (int r = 0; r < 3; r++) {
                da += a[4 * r + 3] * a[4 * r + 3];
                db += b[4 * r + 3] * b[4 * r + 3];
                dd += (a[4 * r + 3] - b[4 * r + 3]) * (a[4 * r + 3] - b[4 * r + 3]);
            }
            if (std::sqrt(dd) > std::max(200.0f, 0.5f * std::sqrt(std::max(da, db)))) return false;
        }
        return true;
    }
    case Kind::TexMtx: {
        // Some effects alternate a projection-style matrix with a zero matrix between
        // frames: never blend toward or away from zero. Scrolling coordinates wrap.
        bool za = true, zb = true;
        float big = 0;
        for (int i = 0; i < n; i++) za &= a[i] == 0.0f, zb &= b[i] == 0.0f, big = std::max(big, std::fabs(b[i]));
        if (za || zb) return za && zb;
        if (big <= 4.0f) {  // texture-coordinate scale/scroll: a wrap jumps by about 1
            for (int i = 0; i < n; i++)
                if (std::fabs(a[i] - b[i]) > 0.5f) return false;
            return true;
        }
        // Projection-style (projected flashlight, shadows; rows of a scaled view matrix):
        // follows the camera and the light. Translations pass through zero as things turn,
        // so only the rotation/scale part is compared, per row.
        const int cols = n == 8 ? 4 : n / 3;
        for (int r = 0; r * cols < n; r++) {
            float na = 0, nb = 0, dd = 0;
            for (int c = 0; c < 3 && c < cols; c++) {
                const float x = a[r * cols + c], y = b[r * cols + c];
                na += x * x, nb += y * y, dd += (x - y) * (x - y);
            }
            if (std::sqrt(dd) > 1.9f * std::sqrt(std::max(na, nb)) + 1e-6f) return false;  // ~145 degrees
        }
        return true;
    }
    case Kind::Projection:
        for (int i = 0; i < n; i++)
            if (std::fabs(a[i] - b[i]) > 0.25f * std::max({std::fabs(a[i]), std::fabs(b[i]), 1e-3f})) return false;
        return true;
    case Kind::Light:  // position xyz, direction xyz
        for (int i = 0; i < 3; i++)
            if (std::fabs(a[i] - b[i]) > 400.0f) return false;
        return true;
    default:
        return false;
    }
}

}  // namespace

// --- guest ---------------------------------------------------------------------------

bool capturing() { return sCapturing && !sSuspended; }

void beginTape() {
    if (sScratch.empty()) sScratch.resize(kCaptureSize);
    sBase = 0;
    sPatches.clear();
    sUngrouped = 0;
    AuroraCaptureBegin(sScratch.data(), kCaptureSize);
    sCapturing = true;
    sSuspended = false;
    detail::vtxBeginTape();
}

void endTape(u32 retraceCount) {
    if (!sCapturing) return;
    const auto t0 = Clock::now();
    const u32 size = sBase + AuroraCaptureEnd();
    std::vector<std::vector<u8>> arrays;
    u64 arrayBytes = 0;
    detail::vtxEndTape(sScratch.data(), size, arrays, arrayBytes);
    detail::noteDraw(0);
    sCapturing = false;
    if (size + 4096 >= kCaptureSize) {  // the capture ran out of room: drop this frame
        if (!sOverflowLogged) log("[interp] frame larger than %u MiB; dropped", kCaptureSize >> 20);
        sOverflowLogged = true;
        return;
    }
    auto tape = std::make_shared<Tape>();
    tape->bytes.assign(sScratch.begin(), sScratch.begin() + size);
    tape->patches = std::move(sPatches);
    sPatches = {};
    tape->arrays = std::move(arrays);
    tape->arrayBytes = arrayBytes;
    tape->retrace = retraceCount;
    if (sDumpAt != ~0u) {
        const u64 ptr = u64(reinterpret_cast<uintptr_t>(sDumpPtr));
        for (u32 i = sDumpAt; i + 8 <= std::min(size, sDumpAt + 64); i++) {
            u64 v = 0;
            for (int k = 0; k < 8; k++) v = v << 8 | tape->bytes[i + k];
            if (v == ptr) {
                tape->dumpOffset = i;
                tape->dumpFrame = sDumpFrame;
                break;
            }
        }
        sDumpAt = ~0u;
    }
    if (sLastTape) pairTapes(*sLastTape, *tape);
    tape->done = Clock::now();
    tape->guestUs = u32(std::chrono::duration_cast<std::chrono::microseconds>(tape->done - t0).count());
    sLastTape = tape;
    {
        std::lock_guard lk(sM);
        if (sPrevious) sRetired.push_back(std::move(sPrevious));
        while (sRetired.size() > 6) sRetired.pop_front();
        sPrevious = std::move(sNewest);
        sNewest = tape;
    }
    if (sTestSteps) {
        std::unique_lock lk(sTestM);
        sTestPending = true;
        sTestCv.wait_for(lk, std::chrono::seconds(20), [] { return !sTestPending; });
        sTestPending = false;
    }
}

void noteDump(u32 at, const void* target, u32 frameNo) {
    if (at == ~0u || !capturing()) return;
    sDumpAt = at;
    sDumpPtr = target;
    sDumpFrame = frameNo;
}

u32 mark() { return capturing() ? sBase + AuroraCapturePos() : ~0u; }

void note(u32 at, Kind kind, u32 source, u32 slot, int nFloats, int first) {
    if (at == ~0u || !capturing()) return;
    // Aurora writes XF loads as 0x10, a u32 header, then the values.
    if (at + 5 + 4 * u32(first + nFloats) > sBase + AuroraCapturePos() || sScratch[at] != 0x10) return;
    Patch p{at + 5 + 4 * u32(first), u16(nFloats), kind, 0, 0, source, slot};
    p.origin = mix(u64(kind) << 56 ^ u64(slot) << 40 ^ source);
    sPatches.push_back(p);
}

void detail::noteDraw(u64 id) {
    for (u32 i = sUngrouped; i < sPatches.size(); i++) {
        Patch& p = sPatches[i];
        if (p.kind != Kind::Verts) p.group = mix(u64(p.kind) << 56 ^ u64(p.slot) << 40 ^ id);
    }
    sUngrouped = u32(sPatches.size());
}

void detail::noteVertices(u32 at, u32 count, u32 stride, u32 posOffset, u64 id) {
    if (!capturing() || count == 0 || count > 0xFFFF) return;
    Patch p{at, u16(count), Kind::Verts, mix(u64(Kind::Verts) << 56 ^ id), 0, 0, 0, u16(stride), u16(posOffset)};
    p.origin = p.group;
    // Keep it out of the matrix grouping of the next draw.
    const bool tail = sUngrouped == sPatches.size();
    sPatches.push_back(p);
    if (tail) sUngrouped = u32(sPatches.size());
}

void suspendForDisplayList() {
    if (!sCapturing) return;
    sBase += AuroraCaptureEnd();  // padded with NOPs; the stream stays valid
    sSuspended = true;
}

void resumeAfterDisplayList() {
    if (!sCapturing) return;
    AuroraCaptureBegin(sScratch.data() + sBase, kCaptureSize - sBase);
    sSuspended = false;
}

// --- host ----------------------------------------------------------------------------

namespace {

// Builds tape b with its values blended from a toward b (alpha 1 = b as captured) into
// sReplay and submits it. `dump` redirects the LM_FRAME_DUMP copy (LM_INTERP_TEST).
void replayPair(const Tape* a, const Tape& b, float alpha, void* dump) {
    sReplay.assign(b.bytes.begin(), b.bytes.end());
    if (a && b.pairedWith != a) a = nullptr;  // not consecutive (a tape was dropped): no pairing
    if (a && b.cut) {
        sCuts++;
        a = nullptr;
    }
    if (a && alpha < 1.0f) {
        for (u32 i = 0; i < b.patches.size(); i++) {
            const s32 j = b.partner[i];
            if (j < 0) continue;
            const Patch& p = b.patches[i];
            const Patch& q = a->patches[j];
            if (p.kind == Kind::Verts) {
                // CPU-built geometry (particles, effects): positions move as a whole.
                float jump = 0;
                for (u32 v = 0; v < p.count; v++)
                    for (int c = 0; c < 3; c++) jump = std::max(jump, std::fabs(vtx(*a, q, v, c) - vtx(b, p, v, c)));
                if (jump > 200.0f) {
                    sSnapped++;
                    sSnapKinds[u32(p.kind)]++;
                    continue;
                }
                for (u32 v = 0; v < p.count; v++)
                    for (int c = 0; c < 3; c++) {
                        const f32 va = vtx(*a, q, v, c), vb = vtx(b, p, v, c);
                        storeBE(sReplay.data() + p.offset + v * p.stride + p.posOffset + 4 * c, va + (vb - va) * alpha);
                    }
                sBlended++;
                continue;
            }
            f32 va[16], vb[16], out[16];
            for (int k = 0; k < p.count; k++) va[k] = val(*a, q, k), vb[k] = val(b, p, k);
            // A projection is only comparable to one of the same type (the u32 after it).
            if (p.kind == Kind::Projection &&
                std::memcmp(a->bytes.data() + q.offset + 24, b.bytes.data() + p.offset + 24, 4) != 0)
                continue;
            if (!blendable(p.kind, va, vb, p.count)) {
                sSnapped++;
                sSnapKinds[u32(p.kind)]++;
                if (sLogSnaps && sLoggedSnaps[u32(p.kind)]++ < 12) {
                    char buf[512];
                    int n = std::snprintf(buf, sizeof(buf), "[interp]   snap kind %u src %08X slot %u:", u32(p.kind), p.source, p.slot);
                    for (int k = 0; k < p.count && n < 480; k++) n += std::snprintf(buf + n, sizeof(buf) - n, " %.2f>%.2f", va[k], vb[k]);
                    log("%s", buf);
                }
                continue;
            }
            if (!((p.kind == Kind::PosMtx && p.count == 12 && blendRigid(va, vb, 4, alpha, out)) ||
                  (p.kind == Kind::NrmMtx && p.count == 9 && blendRigid(va, vb, 3, alpha, out))))
                for (int k = 0; k < p.count; k++) out[k] = va[k] + (vb[k] - va[k]) * alpha;
            if (p.kind == Kind::Light) {  // keep the direction's length (spot cone falloff)
                const float la = std::sqrt(va[3] * va[3] + va[4] * va[4] + va[5] * va[5]);
                const float lb = std::sqrt(vb[3] * vb[3] + vb[4] * vb[4] + vb[5] * vb[5]);
                const float lo = std::sqrt(out[3] * out[3] + out[4] * out[4] + out[5] * out[5]);
                if (lo > 1e-6f && la > 1e-6f && lb > 1e-6f) {
                    const float s = (la + (lb - la) * alpha) / lo;
                    for (int k = 3; k < 6; k++) out[k] *= s;
                }
            }
            for (int k = 0; k < p.count; k++) storeBE(sReplay.data() + p.offset + 4 * k, out[k]);
            sBlended++;
        }
    }
    if (dump && b.dumpOffset != ~0u) {
        const u64 ptr = u64(reinterpret_cast<uintptr_t>(dump));
        for (int k = 0; k < 8; k++) sReplay[b.dumpOffset + k] = u8(ptr >> (56 - 8 * k));
    }
    AuroraReplay(sReplay.data(), u32(sReplay.size()));
}

// --- LM_INTERP_TEST host side ---
struct TestItem {
    std::shared_ptr<const Tape> a, b;
    float alpha;
    std::string name;
    void* target = nullptr;
    u32 frames = 0;
};
std::deque<TestItem> sTestItems;
std::deque<std::shared_ptr<const Tape>> sTestTapes;  // newest last

// LM_INTERP_TEST + LM_INTERP_TRACE: how well matrices are paired over three consecutive
// tapes. A correctly paired object moves smoothly: its translation's second difference is
// small next to its per-frame motion; a wrong pair (another object's matrix) jumps.
void diagnoseMatching(const Tape& t0, const Tape& t1, const Tape& t2) {
    if (t1.pairedWith != &t0 || t2.pairedWith != &t1) return;
    std::vector<s32> next(t1.patches.size(), -1);
    for (u32 k = 0; k < t2.patches.size(); k++)
        if (t2.partner[k] >= 0) next[t2.partner[k]] = s32(k);
    u32 total = 0, unmatched01 = 0, unmatched12 = 0, jerky = 0, logged = 0;
    for (u32 i = 0; i < t1.patches.size(); i++) {
        const Patch& p = t1.patches[i];
        if (p.kind != Kind::PosMtx) continue;
        total++;
        const s32 j0 = t1.partner[i], j2 = next[i];
        if (j0 < 0) unmatched01++;
        if (j2 < 0) unmatched12++;
        if (j0 < 0 || j2 < 0) continue;
        const Patch& q0 = t0.patches[j0];
        const Patch& q2 = t2.patches[j2];
        float jerk = 0, step = 0, rot = 0;
        for (int r = 0; r < 3; r++) {
            const int k = 4 * r + 3;
            const float a = val(t0, q0, k), b = val(t1, p, k), c = val(t2, q2, k);
            jerk = std::max(jerk, std::fabs(a - 2 * b + c));
            step = std::max({step, std::fabs(b - a), std::fabs(c - b)});
            for (int col = 0; col < 3; col++) {
                const int m = 4 * r + col;
                rot = std::max(rot, std::fabs(val(t0, q0, m) - 2 * val(t1, p, m) + val(t2, q2, m)));
            }
        }
        if (jerk > 0.5f * step + 5.0f || rot > 0.2f) {
            jerky++;
            if (logged++ < 4)
                log("[interp]   jerk %.1f (step %.1f, rot %.3f) src %08X slot %u", jerk, step, rot, p.source, p.slot);
        }
    }
    u32 kinds[kKinds] = {}, verts = 0;
    for (const Patch& p : t1.patches) kinds[u32(p.kind)]++;
    for (u32 i = 0; i < t1.patches.size(); i++)
        if (t1.patches[i].kind == Kind::Verts && t1.partner[i] >= 0) verts++;
    log("[interp] match f%u: %u pos matrices, %u not in previous, %u not in next, %u jerky; cut %d; nrm %u tex %u "
        "proj %u light %u verts %u (%u paired)",
        t1.dumpFrame, total, unmatched01, unmatched12, jerky, int(t1.cut), kinds[1], kinds[2], kinds[3], kinds[4],
        kinds[5], verts);
}

bool testReplay() {
    if (sTestItems.empty()) {
        bool pending;
        {
            std::lock_guard lk(sTestM);
            pending = sTestPending;
        }
        std::shared_ptr<const Tape> newest;
        {
            std::lock_guard lk(sM);
            newest = sNewest;
        }
        if (!newest) return false;
        if (!pending) {  // nothing new: keep showing the newest frame
            replayPair(nullptr, *newest, 1.0f, nullptr);
            return true;
        }
        if (sTestTapes.empty() || sTestTapes.back() != newest) sTestTapes.push_back(newest);
        while (sTestTapes.size() > 3) sTestTapes.pop_front();
        const auto& n = sTestTapes.back();
        if (sTrace && sTestTapes.size() == 3 && sTestTapes[1]->dumpOffset != ~0u)
            diagnoseMatching(*sTestTapes[0], *sTestTapes[1], *sTestTapes[2]);
        if (n->dumpOffset != ~0u) {
            sTestItems.push_back({nullptr, n, 1.0f, "f" + std::to_string(n->dumpFrame) + "_fresh"});
            if (sTestTapes.size() == 3 && sTestTapes[1]->dumpOffset != ~0u)
                for (int st = 1; st <= sTestSteps; st++)
                    sTestItems.push_back({sTestTapes[0], sTestTapes[1], float(st) / float(sTestSteps),
                                          "f" + std::to_string(sTestTapes[1]->dumpFrame) + "_s" + std::to_string(st)});
        }
        if (sTestItems.empty()) {
            std::lock_guard lk(sTestM);
            sTestPending = false;
            sTestCv.notify_all();
            replayPair(nullptr, *newest, 1.0f, nullptr);
            return true;
        }
    }
    TestItem& it = sTestItems.front();
    if (!it.target) it.target = efbcopy::namedDumpTarget(it.name.c_str());
    sLogSnaps = sTrace && it.frames == 0 && it.alpha > 0.4f && it.alpha < 0.6f;
    std::fill(std::begin(sLoggedSnaps), std::end(sLoggedSnaps), 0u);
    std::fill(std::begin(sSnapKinds), std::end(sSnapKinds), 0u);
    replayPair(it.a.get(), *it.b, it.alpha, it.target);
    if (sLogSnaps) {
        u32 unpaired[kKinds] = {};
        for (u32 i = 0; i < it.b->patches.size(); i++)
            if (it.b->partner.empty() || it.b->partner[i] < 0) unpaired[u32(it.b->patches[i].kind)]++;
        log("[interp] %s: snapped pos %u nrm %u tex %u proj %u light %u verts %u; unpaired pos %u nrm %u tex %u proj %u "
            "light %u verts %u%s",
            it.name.c_str(), sSnapKinds[0], sSnapKinds[1], sSnapKinds[2], sSnapKinds[3], sSnapKinds[4], sSnapKinds[5],
            unpaired[0], unpaired[1], unpaired[2], unpaired[3], unpaired[4], unpaired[5], it.b->cut ? " CUT" : "");
    }
    sLogSnaps = false;
    efbcopy::applyWritebacks();
    if (efbcopy::namedDumpWritten(it.target) || ++it.frames > 120) {
        if (it.frames > 120) log("[interp] test dump %s timed out", it.name.c_str());
        sTestItems.pop_front();
        if (sTestItems.empty()) {
            std::lock_guard lk(sTestM);
            sTestPending = false;
            sTestCv.notify_all();
        }
    }
    return true;
}

}  // namespace

bool replay(bool final) {
    if (sTestSteps && !final) return testReplay();
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
    const auto r0 = Clock::now();
    replayPair(final ? nullptr : a.get(), *b, alpha, nullptr);
    sReplayUs += u64(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - r0).count());
    if (sTrace) {
        static Clock::time_point last = Clock::now();
        const auto now = Clock::now();
        log("[interp] alpha %.3f dt %.1f ms tape %u%s, snapped pos %u nrm %u tex %u proj %u light %u verts %u, "
            "unpaired %u",
            alpha, std::chrono::duration<double, std::milli>(now - last).count(), b->retrace, b->cut ? " (cut)" : "",
            sSnapKinds[0], sSnapKinds[1], sSnapKinds[2], sSnapKinds[3], sSnapKinds[4], sSnapKinds[5], b->unpaired);
        last = now;
        std::fill(std::begin(sSnapKinds), std::end(sSnapKinds), 0u);
    }

    // Stats, every 5 s.
    sPresented++;
    static u32 lastRetrace = 0;
    if (b->retrace != lastRetrace) {
        lastRetrace = b->retrace;
        sTapes++;
        sTapeBytes += b->bytes.size();
        sArrayBytes += b->arrayBytes;
        sPairedSum += b->paired;
        sGuestUs += b->guestUs;
        sUnpairedSum += b->unpaired;
    }
    const double elapsed = std::chrono::duration<double>(Clock::now() - sStatsStart).count();
    if (elapsed >= 5.0) {
        log("[interp] presented %.1f fps, game %.1f fps, tape %.0f KiB + arrays %.0f KiB, per tape %.0f paired / %.0f "
            "unpaired; per present %u blended / %u snapped; %u cut presents; tape close %.2f ms (guest), replay "
            "%.2f ms (host)",
            sPresented / elapsed, sTapes / elapsed, sTapes ? double(sTapeBytes) / sTapes / 1024.0 : 0.0,
            sTapes ? double(sArrayBytes) / sTapes / 1024.0 : 0.0, sTapes ? double(sPairedSum) / sTapes : 0.0,
            sTapes ? double(sUnpairedSum) / sTapes : 0.0, sPresented ? sBlended / sPresented : 0,
            sPresented ? sSnapped / sPresented : 0, sCuts, sTapes ? double(sGuestUs) / sTapes / 1000.0 : 0.0,
            sPresented ? double(sReplayUs) / sPresented / 1000.0 : 0.0);
        sPresented = sTapes = sBlended = sSnapped = sCuts = 0;
        sTapeBytes = sArrayBytes = sPairedSum = sUnpairedSum = sGuestUs = sReplayUs = 0;
        sStatsStart = Clock::now();
    }
    return true;
}

void reset() {
    std::lock_guard lk(sM);
    // The FIFO may still be reading the last replay's array copies.
    if (sPrevious) sRetired.push_back(std::move(sPrevious));
    if (sNewest) sRetired.push_back(std::move(sNewest));
    while (sRetired.size() > 6) sRetired.pop_front();
}

}  // namespace recomp::interp
