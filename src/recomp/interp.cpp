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
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace recomp {
u32 copyDispCount();  // hle_gx.cpp
}

namespace recomp::interp {

// LM_INTERP_DRAWS=<frame> (with LM_FRAME_DUMP numbering): log every draw and noted value of
// that game frame's tape, with the caller chain and what each value was paired with.
// LM_INTERP_DRAWS=<first>[,<last>].
static u32 sDrawLogLast = 0;
static const u32 sDrawLogFrame = [] {
    const char* v = std::getenv("LM_INTERP_DRAWS");
    if (!v) return ~0u;
    char* end = nullptr;
    const u32 first = u32(std::strtoul(v, &end, 10));
    sDrawLogLast = *end == ',' ? u32(std::strtoul(end + 1, nullptr, 10)) : first;
    return first;
}();
static bool drawLogFrame(u32 f) { return sDrawLogFrame != ~0u && f >= sDrawLogFrame && f <= sDrawLogLast; }
bool drawLogActive() { return capturing() && drawLogFrame(copyDispCount() + 1); }
void drawLogCaller(const std::string& chain) {
    if (drawLogActive()) log("[draws] caller %s", chain.c_str());
}
void detail::drawLog(const char* fmt, ...) {
    if (!drawLogActive()) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log("[draws] %s", buf);
}

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
    u64 group;    // unused (kept for the draw log)
    u64 origin;   // kind and source address (second-chance pairing)
    u32 source, slot;
    u16 stride = 0, posOffset = 0;  // Verts
    u32 bundle = ~0u;  // the draw it belongs to (index into Tape::bundleIds)
};

struct Tape {
    std::vector<u8> bytes;
    std::vector<Patch> patches;
    std::vector<u64> bundleIds;  // per draw: what is drawn (display list, vertex layout)
    std::vector<std::vector<u8>> arrays;  // vertex-array copies the tape's array loads point at
    u64 arrayBytes = 0;
    u32 retrace = 0;
    Clock::time_point done;
    // Pairing with the tape captured before this one (`pairedWith`, compared by identity).
    const Tape* pairedWith = nullptr;
    u64 seq = 0;  // capture order (consecutive tapes differ by 1); replay state slots
    std::vector<s32> partner;  // per patch: index into pairedWith->patches, or -1
    bool cut = false;          // a camera cut since pairedWith: shown as is
    u32 paired = 0, unpaired = 0;
    std::vector<std::pair<u32, u32>> lights;  // extra-light commands: offset of the first light, count
    u32 dumpOffset = ~0u;  // LM_INTERP_TEST: tape offset of the LM_FRAME_DUMP copy's target pointer
    u32 dumpFrame = 0;
    u32 guestUs = 0;  // guest-thread time spent closing the tape (array copies' bookkeeping, pairing)
    // Breakdown of guestUs: array bookkeeping, tape copy, draw pairing, leftover pairing, cut check.
    u32 phaseUs[5] = {};
};

u32 usSince(std::chrono::steady_clock::time_point& t) {
    const auto now = std::chrono::steady_clock::now();
    const u32 us = u32(std::chrono::duration_cast<std::chrono::microseconds>(now - t).count());
    t = now;
    return us;
}

// --- guest side (guest thread only) ---
std::vector<u8> sScratch;
u64 sSeq = 0;  // tapes captured so far
bool sCapturing = false;
bool sSuspended = false;  // the game is recording its own display list
u32 sBase = 0;            // start of the current capture segment in sScratch
std::vector<Patch> sPatches;
u32 sUngrouped = 0;  // patches from here on wait for their draw
std::vector<u64> sBundleIds;
std::vector<std::pair<u32, u32>> sLights;  // extra-light commands of the tape being captured
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
u64 sPhaseUs[5] = {};
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
        // Small texture matrices (a sprite's spin) tell particles of one effect apart well.
        d += (a.kind == Kind::PosMtx && (i & 3) != 3) ? diff * 100.0f
             : (a.kind == Kind::TexMtx && a.count == 8)  ? diff * 30.0f
                                                           : diff;
    }
    return d;
}

bool compatible(const Patch& a, const Patch& b) {
    return a.kind == b.kind && a.count == b.count && a.stride == b.stride && a.posOffset == b.posOffset;
}

// Pairs members of one group (draws, or leftover values): nearest first, so two instances of
// the same mesh or two particles of one effect don't swap. `dist(i, j)` compares a[i], b[j].
template <class Dist>
void pairNearest(const std::vector<u32>& A, const std::vector<u32>& B, std::vector<s32>& partnerB,
                 std::vector<u8>& usedA, Dist dist) {
    if (A.size() == 1 && B.size() == 1) {
        if (!usedA[A[0]] && partnerB[B[0]] < 0) {
            partnerB[B[0]] = s32(A[0]);
            usedA[A[0]] = 1;
        }
        return;
    }
    if (A.size() * B.size() > 250000) {  // huge groups (hundreds of particles): in order
        for (size_t k = 0; k < std::min(A.size(), B.size()); k++)
            if (!usedA[A[k]] && partnerB[B[k]] < 0) {
                partnerB[B[k]] = s32(A[k]);
                usedA[A[k]] = 1;
            }
        return;
    }
    static std::vector<std::tuple<float, u32, u32>> cand;
    cand.clear();
    for (u32 i : A) {
        if (usedA[i]) continue;
        for (u32 j : B)
            if (partnerB[j] < 0)
                if (const float d = dist(i, j); d < 1e29f) cand.emplace_back(d, i, j);  // too far: never a pair
    }
    std::sort(cand.begin(), cand.end());
    for (const auto& [d, i, j] : cand)
        if (!usedA[i] && partnerB[j] < 0) {
            partnerB[j] = s32(i);
            usedA[i] = 1;
        }
}

// The values of one draw: a view into Bundles::flat.
struct Span {
    const u32* p = nullptr;
    u32 n = 0;
    const u32* begin() const { return p; }
    const u32* end() const { return p + n; }
    size_t size() const { return n; }
    bool empty() const { return n == 0; }
    u32 operator[](size_t k) const { return p[k]; }
};

// Where a draw is (view space): the translation of its first position matrix, or its first
// CPU-built vertex. False if it has neither.
bool bundlePos(const Tape& t, Span patches, float out[3]) {
    for (u32 i : patches) {
        const Patch& p = t.patches[i];
        if (p.kind == Kind::PosMtx) {
            for (int r = 0; r < 3; r++) out[r] = val(t, p, 4 * r + 3);
            return true;
        }
        if (p.kind == Kind::Verts) {
            for (int c = 0; c < 3; c++) out[c] = vtx(t, p, 0, c);
            return true;
        }
    }
    return false;
}

// The values noted for each draw of a tape, keyed by what is drawn and by the kinds and
// sizes of the values (so two draws are only compared value by value when they line up).
struct Bundles {
    std::vector<u32> flat;     // patch indices, grouped by draw
    std::vector<Span> patches;  // per draw
    std::vector<u64> key;
};
Bundles bundlesOf(const Tape& t) {
    Bundles out;
    const size_t n = t.bundleIds.size();
    // Counting sort by draw (one allocation instead of one per draw; patches of a draw are
    // usually consecutive, and the order within a draw is kept).
    std::vector<u32> start(n + 1, 0);
    for (const Patch& p : t.patches)
        if (p.bundle < n) start[p.bundle + 1]++;
    for (size_t k = 0; k < n; k++) start[k + 1] += start[k];
    out.flat.resize(start[n]);
    std::vector<u32> fill(start.begin(), start.end() - 1);
    for (u32 i = 0; i < t.patches.size(); i++)
        if (t.patches[i].bundle < n) out.flat[fill[t.patches[i].bundle]++] = i;
    out.patches.resize(n);
    out.key.resize(n);
    for (size_t k = 0; k < n; k++) {
        out.patches[k] = {out.flat.data() + start[k], start[k + 1] - start[k]};
        u64 h = t.bundleIds[k];
        for (u32 i : out.patches[k]) {
            const Patch& p = t.patches[i];
            h = mix(h ^ (u64(p.kind) << 48 | u64(p.count) << 32 | u64(p.stride) << 16 | p.posOffset));
        }
        out.key[k] = h;
    }
    return out;
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

// A 2x4 texture matrix that turns texture coordinates in their plane (a spinning
// particle sprite): [c -s 0 tx; s c 0 ty] times a uniform scale.
struct Spin {
    float angle, scale;
};
bool spin2D(const f32* m, Spin& out) {
    const float sc = std::sqrt(m[0] * m[0] + m[4] * m[4]);
    if (!(sc > 1e-6f) || std::fabs(m[2]) > 1e-4f || std::fabs(m[6]) > 1e-4f) return false;
    if (std::fabs(m[0] - m[5]) > 0.02f * sc || std::fabs(m[1] + m[4]) > 0.02f * sc) return false;
    out = {std::atan2(m[4], m[0]), sc};
    return true;
}
float angleDelta(float from, float to) {
    float d = std::remainder(to - from, 6.2831853f);
    return d;
}

// Blends two spinning texture matrices by angle and scale about their common pivot (the
// sprite's centre), so the texture turns instead of shrinking through an entrywise blend.
bool blendSpin(const f32* a, const f32* b, float t, f32* out) {
    Spin sa, sb;
    if (!spin2D(a, sa) || !spin2D(b, sb)) return false;
    const float d = angleDelta(sa.angle, sb.angle);
    if (std::fabs(d) > 2.6f || sb.scale > 2.0f * sa.scale || sa.scale > 2.0f * sb.scale) return false;
    // Pivot c with t = c - M c, i.e. (I - M) c = t; from whichever matrix turns more.
    auto pivot = [](const f32* m, float& cx, float& cy) {
        const float a00 = 1 - m[0], a01 = -m[1], a10 = -m[4], a11 = 1 - m[5];
        const float det = a00 * a11 - a01 * a10;
        if (std::fabs(det) < 1e-3f) return false;
        cx = (m[3] * a11 - a01 * m[7]) / det;
        cy = (a00 * m[7] - a10 * m[3]) / det;
        return true;
    };
    float cx, cy, cx2, cy2;
    const bool pa = pivot(a, cx, cy), pb = pivot(b, cx2, cy2);
    const float ang = sa.angle + d * t, sc = sa.scale + (sb.scale - sa.scale) * t;
    const float c = std::cos(ang) * sc, s = std::sin(ang) * sc;
    out[0] = c, out[1] = -s, out[2] = 0, out[4] = s, out[5] = c, out[6] = 0;
    if (pa && pb && std::fabs(cx - cx2) < 0.05f && std::fabs(cy - cy2) < 0.05f) {
        out[3] = cx - (c * cx - s * cy);
        out[7] = cy - (s * cx + c * cy);
    } else {
        out[3] = a[3] + (b[3] - a[3]) * t;
        out[7] = a[7] + (b[7] - a[7]) * t;
    }
    return true;
}

bool blendable(Kind kind, const f32* a, const f32* b, int n);

void pairTapes(const Tape& a, Tape& b) {
    auto tp = Clock::now();
    b.pairedWith = &a;
    b.partner.assign(b.patches.size(), -1);
    std::vector<u8> usedA(a.patches.size(), 0);
    // 1. Draws: pair the draws of the same thing (display list, vertex layout) that carry the
    //    same kinds of values, nearest values first, then their values one by one. Slots are
    //    not part of it: J3D hands out matrix slots per frame.
    if (!sPairBySource) {
        const Bundles ba = bundlesOf(a), bb = bundlesOf(b);
        std::unordered_map<u64, std::vector<u32>> ga, gb;
        for (u32 k = 0; k < ba.key.size(); k++)
            if (!ba.patches[k].empty()) ga[ba.key[k]].push_back(k);
        for (u32 k = 0; k < bb.key.size(); k++)
            if (!bb.patches[k].empty()) gb[bb.key[k]].push_back(k);
        // Where each draw is, once (the pairing below compares every draw of a group with every
        // other: hundreds of particles or decorations of one kind).
        struct Where {
            float p[3];
            bool ok;
        };
        auto where = [](const Tape& t, const Bundles& bs) {
            std::vector<Where> w(bs.patches.size());
            for (size_t k = 0; k < w.size(); k++) w[k].ok = bundlePos(t, bs.patches[k], w[k].p);
            return w;
        };
        const std::vector<Where> wa = where(a, ba), wb = where(b, bb);
        std::vector<s32> drawPartner(bb.key.size(), -1);
        std::vector<u8> drawUsed(ba.key.size(), 0);
        for (const auto& [key, B] : gb) {
            const auto it = ga.find(key);
            if (it == ga.end()) continue;
            // Many draws of one thing (particles of an effect): a particle that died and one
            // that was born elsewhere are not the same one, even if nothing closer is left.
            // The camera moves all of them alike (view space): take the common motion out, as
            // the median of each one's move to its nearest predecessor, then allow 12 units
            // plus 1% of the distance from the camera of motion of its own per game frame.
            const auto& A = it->second;
            const bool many = A.size() > 1 || B.size() > 1;
            float common[3] = {0, 0, 0};
            if (many && A.size() * B.size() <= 250000) {
                // From a sample of at most 64 of them: the median of a few dozen moves is as
                // good, and big groups (hundreds of draws) stay linear.
                std::vector<float> moves[3];
                const size_t sample = std::max<size_t>(1, B.size() / 64);
                for (size_t n = 0; n < B.size(); n += sample) {
                    const u32 j = B[n];
                    float best = 1e30f, bm[3] = {};
                    if (!wb[j].ok) continue;
                    const float* pb = wb[j].p;
                    for (u32 i : A) {
                        if (!wa[i].ok) continue;
                        const float* pa = wa[i].p;
                        const float d = (pa[0] - pb[0]) * (pa[0] - pb[0]) + (pa[1] - pb[1]) * (pa[1] - pb[1]) +
                                        (pa[2] - pb[2]) * (pa[2] - pb[2]);
                        if (d < best) best = d, bm[0] = pb[0] - pa[0], bm[1] = pb[1] - pa[1], bm[2] = pb[2] - pa[2];
                    }
                    if (best < 1e30f)
                        for (int c = 0; c < 3; c++) moves[c].push_back(bm[c]);
                }
                for (int c = 0; c < 3; c++)
                    if (!moves[c].empty()) {
                        std::nth_element(moves[c].begin(), moves[c].begin() + moves[c].size() / 2, moves[c].end());
                        common[c] = moves[c][moves[c].size() / 2];
                    }
            }
            auto fullDistance = [&](u32 i, u32 j) {
                float d = 0;
                const auto& pa = ba.patches[i];
                const auto& pb = bb.patches[j];
                for (size_t k = 0; k < pa.size(); k++) d += distance(a, a.patches[pa[k]], b, b.patches[pb[k]]);
                return d;
            };
            if (many && A.size() * B.size() > 1024 && A.size() * B.size() <= 250000) {
                // Big groups: only draws within the motion limit can pair, so find them through
                // the predecessors sorted by x (moved by the common motion) instead of comparing
                // every draw with every other.
                static std::vector<std::pair<float, u32>> byX;
                byX.clear();
                std::vector<u32> nowhereA;  // no position: compared with everything
                // Along the axis they are most spread out on.
                float lo3[3] = {1e30f, 1e30f, 1e30f}, hi3[3] = {-1e30f, -1e30f, -1e30f};
                for (u32 i : A)
                    if (wa[i].ok)
                        for (int c = 0; c < 3; c++) lo3[c] = std::min(lo3[c], wa[i].p[c]), hi3[c] = std::max(hi3[c], wa[i].p[c]);
                int ax = 0;
                for (int c = 1; c < 3; c++)
                    if (hi3[c] - lo3[c] > hi3[ax] - lo3[ax]) ax = c;
                for (u32 i : A) {
                    if (drawUsed[i]) continue;
                    if (wa[i].ok) byX.emplace_back(wa[i].p[ax] + common[ax], i);
                    else nowhereA.push_back(i);
                }
                std::sort(byX.begin(), byX.end());
                // Draws of one CPU-built vertex block each (particles): their first vertices
                // decoded once, not once per candidate pair.
                const Patch& p0 = b.patches[bb.patches[B[0]][0]];
                const bool vertsOnly = bb.patches[B[0]].size() == 1 && p0.kind == Kind::Verts;
                const u32 nv = std::min<u32>(p0.count, 4);
                static std::vector<float> decA, decB;
                if (vertsOnly) {
                    decA.resize(ba.patches.size() * 12);
                    decB.resize(bb.patches.size() * 12);
                    for (u32 i : A)
                        for (u32 v = 0; v < nv; v++)
                            for (int c = 0; c < 3; c++) decA[i * 12 + v * 3 + c] = vtx(a, a.patches[ba.patches[i][0]], v, c);
                    for (u32 j : B)
                        for (u32 v = 0; v < nv; v++)
                            for (int c = 0; c < 3; c++) decB[j * 12 + v * 3 + c] = vtx(b, b.patches[bb.patches[j][0]], v, c);
                }
                auto groupDistance = [&](u32 i, u32 j) {
                    if (!vertsOnly) return fullDistance(i, j);
                    float d = 0;  // as distance() for Verts
                    for (u32 k = 0; k < nv * 3; k++) d += std::fabs(decA[i * 12 + k] - decB[j * 12 + k]);
                    return d;
                };
                // Candidates as (distance bits << 32 | index) keys: sorting plain integers is several
                // times faster than tuples (distances are never negative).
                static std::vector<u64> keys;
                static std::vector<std::pair<u32, u32>> cand;
                keys.clear();
                cand.clear();
                auto add = [&](float d, u32 i, u32 j) {
                    keys.push_back(u64(std::bit_cast<u32>(d)) << 32 | cand.size());
                    cand.emplace_back(i, j);
                };
                for (u32 j : B) {
                    if (drawPartner[j] >= 0) continue;
                    if (!wb[j].ok) {  // no position: compared with everything
                        for (u32 i : A)
                            if (!drawUsed[i]) add(groupDistance(i, j), i, j);
                        continue;
                    }
                    const float* pb = wb[j].p;
                    const float lim = 12.0f + 0.01f * std::sqrt(pb[0] * pb[0] + pb[1] * pb[1] + pb[2] * pb[2]);
                    auto it = std::lower_bound(byX.begin(), byX.end(), std::pair<float, u32>(pb[ax] - lim, 0));
                    for (; it != byX.end() && it->first <= pb[ax] + lim; ++it) {
                        const u32 i = it->second;
                        float d2 = 0;
                        for (int c = 0; c < 3; c++) {
                            const float e = pb[c] - wa[i].p[c] - common[c];
                            d2 += e * e;
                        }
                        if (d2 <= lim * lim) add(groupDistance(i, j), i, j);
                    }
                    for (u32 i : nowhereA) add(groupDistance(i, j), i, j);
                }
                std::sort(keys.begin(), keys.end());
                for (const u64 k : keys)
                    if (const auto [i, j] = cand[u32(k)]; !drawUsed[i] && drawPartner[j] < 0) {
                        drawPartner[j] = s32(i);
                        drawUsed[i] = 1;
                    }
                continue;
            }
            pairNearest(A, B, drawPartner, drawUsed, [&](u32 i, u32 j) {
                if (many) {
                    if (wa[i].ok && wb[j].ok) {
                        const float *pa = wa[i].p, *pb = wb[j].p;
                        float d2 = 0, r2 = 0;
                        for (int c = 0; c < 3; c++) {
                            const float e = pb[c] - pa[c] - common[c];
                            d2 += e * e, r2 += pb[c] * pb[c];
                        }
                        const float lim = 12.0f + 0.01f * std::sqrt(r2);
                        if (d2 > lim * lim) return 1e30f;
                    }
                }
                return fullDistance(i, j);
            });
        }
        for (u32 j = 0; j < bb.key.size(); j++) {
            if (drawPartner[j] < 0) continue;
            const auto& pa = ba.patches[drawPartner[j]];
            const auto& pb = bb.patches[j];
            for (size_t k = 0; k < pb.size(); k++) {
                b.partner[pb[k]] = s32(pa[k]);
                usedA[pa[k]] = 1;
            }
        }
    }
    b.phaseUs[2] = usSince(tp);
    // 2. What is left: by kind and source address, only where that is unambiguous (pool and
    //    scratch matrices are shared by many objects).
    {
        std::unordered_map<u64, std::vector<u32>> ga, gb;
        for (u32 i = 0; i < a.patches.size(); i++)
            if (!usedA[i]) ga[a.patches[i].origin].push_back(i);
        for (u32 j = 0; j < b.patches.size(); j++)
            if (b.partner[j] < 0) gb[b.patches[j].origin].push_back(j);
        for (const auto& [key, B] : gb) {
            const auto it = ga.find(key);
            if (it == ga.end()) continue;
            if (!sPairBySource && (B.size() != 1 || it->second.size() != 1)) continue;
            const auto& A = it->second;
            pairNearest(A, B, b.partner, usedA, [&](u32 i, u32 j) {
                return compatible(a.patches[i], b.patches[j]) ? distance(a, a.patches[i], b, b.patches[j]) : 1e30f;
            });
            for (u32 j : B)  // never pair values of different shapes
                if (b.partner[j] >= 0 && !compatible(a.patches[b.partner[j]], b.patches[j])) {
                    usedA[b.partner[j]] = 0;
                    b.partner[j] = -1;
                }
        }
    }
    b.phaseUs[3] = usSince(tp);
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
    b.phaseUs[4] = usSince(tp);
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
        if (n == 8) {  // a spinning sprite: up to ~150 degrees per game frame
            Spin sa, sb;
            if (spin2D(a, sa) && spin2D(b, sb))
                return std::fabs(angleDelta(sa.angle, sb.angle)) <= 2.6f && sb.scale <= 2.0f * sa.scale &&
                       sa.scale <= 2.0f * sb.scale;
        }
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
    sBundleIds.clear();
    sLights.clear();
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
    auto tp = t0;
    const u32 vtxUs = usSince(tp);
    if (size + 4096 >= kCaptureSize) {  // the capture ran out of room: drop this frame
        if (!sOverflowLogged) log("[interp] frame larger than %u MiB; dropped", kCaptureSize >> 20);
        sOverflowLogged = true;
        return;
    }
    auto tape = std::make_shared<Tape>();
    tape->seq = ++sSeq;
    tape->bytes.assign(sScratch.begin(), sScratch.begin() + size);
    tape->patches = std::move(sPatches);
    sPatches = {};
    tape->bundleIds = std::move(sBundleIds);
    sBundleIds = {};
    tape->lights = std::move(sLights);
    sLights = {};
    tape->arrays = std::move(arrays);
    tape->arrayBytes = arrayBytes;
    tape->retrace = retraceCount;
    tape->phaseUs[0] = vtxUs;
    tape->phaseUs[1] = usSince(tp);
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
    if (drawLogFrame(sDumpFrame) && tape->dumpOffset != ~0u)
        for (u32 i = 0; i < tape->patches.size(); i++) {
            const Patch& p = tape->patches[i];
            const s32 j = tape->partner.empty() ? -1 : tape->partner[i];
            log("[draws] f%u pair #%u kind %u slot %u src %08X -> %d%s", sDumpFrame, i, u32(p.kind), p.slot, p.source, j,
                j >= 0 && sLastTape->patches[j].group == p.group ? "" : j >= 0 ? " (by source)" : "");
        }
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

void noteExtraLights(u32 at, u32 count) {
    if (at == ~0u || !capturing() || count == 0) return;
    // GX_AURORA (0x50), u16 GX_AURORA_SET_EXTRA_LIGHTS, u8 count, then the lights.
    const u32 end = at + 4 + count * GX_AURORA_EXTRA_LIGHT_WORDS * 4;
    if (end > sBase + AuroraCapturePos() || sScratch[at] != 0x50 || sScratch[at + 3] != count) return;
    sLights.emplace_back(at + 4, count);
}

namespace {
int sNoBlend = 0;
int sLinearPos = 0;
}  // namespace
void pushNoBlend() { sNoBlend++; }
void popNoBlend() { sNoBlend = sNoBlend > 0 ? sNoBlend - 1 : 0; }
void pushLinearPosMtx() { sLinearPos++; }
void popLinearPosMtx() { sLinearPos = sLinearPos > 0 ? sLinearPos - 1 : 0; }

void note(u32 at, Kind kind, u32 source, u32 slot, int nFloats, int first) {
    if (at == ~0u || !capturing() || sNoBlend > 0) return;
    if (sLinearPos > 0 && kind == Kind::PosMtx) kind = Kind::TexMtx;  // element-wise, own pairing key
    // Aurora writes XF loads as 0x10, a u32 header, then the values.
    if (at + 5 + 4 * u32(first + nFloats) > sBase + AuroraCapturePos() || sScratch[at] != 0x10) return;
    Patch p{at + 5 + 4 * u32(first), u16(nFloats), kind, 0, 0, source, slot};
    p.origin = mix(u64(kind) << 56 ^ source);
    detail::drawLog("  #%u kind %u slot %u src %08X first %.2f %.2f %.2f %.2f", u32(sPatches.size()), u32(kind), slot,
                    source, loadBE(&sScratch[p.offset]), nFloats > 1 ? loadBE(&sScratch[p.offset + 4]) : 0.0f,
                    nFloats > 2 ? loadBE(&sScratch[p.offset + 8]) : 0.0f, nFloats > 3 ? loadBE(&sScratch[p.offset + 12]) : 0.0f);
    sPatches.push_back(p);
}

void detail::noteDraw(u64 id) {
    const u32 bundle = u32(sBundleIds.size());
    sBundleIds.push_back(id);
    for (u32 i = sUngrouped; i < sPatches.size(); i++) {
        Patch& p = sPatches[i];
        if (p.kind == Kind::Verts) continue;  // already belongs to its own draw
        p.group = id;
        p.bundle = bundle;
    }
    sUngrouped = u32(sPatches.size());
}

void detail::noteVertices(u32 at, u32 count, u32 stride, u32 posOffset, u64 id) {
    if (!capturing() || count == 0 || count > 0xFFFF) return;
    Patch p{at, u16(count), Kind::Verts, mix(u64(Kind::Verts) << 56 ^ id), 0, 0, 0, u16(stride), u16(posOffset)};
    detail::drawLog("  #%u verts %u stride %u pos@%u", u32(sPatches.size()), count, stride, posOffset);
    p.origin = p.group;
    // It belongs to the draw just begun (vtxBegin noted it), not to the next one.
    if (sBundleIds.empty()) sBundleIds.push_back(id);
    p.bundle = u32(sBundleIds.size() - 1);
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

u64 sEndSeq[4] = {};  // per replay state slot: the tape whose end state it holds (host side)

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
                    if (sLogSnaps && sLoggedSnaps[u32(p.kind)]++ < 12)
                        log("[interp]   snap verts %u stride %u jump %.1f: v0 (%.1f %.1f %.1f) > (%.1f %.1f %.1f)", p.count,
                            p.stride, jump, vtx(*a, q, 0, 0), vtx(*a, q, 0, 1), vtx(*a, q, 0, 2), vtx(b, p, 0, 0),
                            vtx(b, p, 0, 1), vtx(b, p, 0, 2));
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
                  (p.kind == Kind::NrmMtx && p.count == 9 && blendRigid(va, vb, 3, alpha, out)) ||
                  (p.kind == Kind::TexMtx && p.count == 8 && blendSpin(va, vb, alpha, out))))
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
    // Extra lights (enhanced lighting): position, color, radius and intensity blended by id;
    // a light without a partner is shown as captured.
    if (a && alpha < 1.0f && !a->lights.empty()) {
        constexpr u32 kWords = GX_AURORA_EXTRA_LIGHT_WORDS;
        auto u32At = [](const u8* p) { return u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3]; };
        for (const auto& [offB, countB] : b.lights) {
            for (u32 i = 0; i < countB; i++) {
                const u8* lb = b.bytes.data() + offB + i * kWords * 4;
                const u32 id = u32At(lb + 36);
                if (id == 0) continue;
                const u8* la = nullptr;
                for (const auto& [offA, countA] : a->lights)
                    for (u32 j = 0; j < countA && !la; j++) {
                        const u8* cand = a->bytes.data() + offA + j * kWords * 4;
                        if (u32At(cand + 36) == id) la = cand;
                    }
                if (!la) continue;
                const float jump = std::fabs(loadBE(la) - loadBE(lb)) + std::fabs(loadBE(la + 4) - loadBE(lb + 4)) +
                                   std::fabs(loadBE(la + 8) - loadBE(lb + 8));
                if (jump > 600.0f) continue;  // teleport / camera cut
                u8* out = sReplay.data() + (lb - b.bytes.data());
                for (int k = 0; k < 8; k++) {
                    const f32 va = loadBE(la + 4 * k), vb = loadBE(lb + 4 * k);
                    storeBE(out + 4 * k, va + (vb - va) * alpha);
                }
                // Shadow weight (flags bits 8..15, 0 = full when casting; bit 0 = casts a shadow)
                // blended too, so a fading shadow fades smoothly at high frame rates.
                const auto weight = [&](const u8* l) {
                    const u32 f = u32At(l + 32);
                    if ((f & 1u) == 0) return 0.0f;
                    const u32 w = (f >> 8) & 0xFFu;
                    return w ? float(w) / 255.0f : 1.0f;
                };
                const float wa = weight(la), wb = weight(lb), w = wa + (wb - wa) * alpha;
                u32 fl = u32At(lb + 32) & ~0xFF01u;
                if (w > 0.002f) fl |= 1u | (std::clamp(u32(w * 255.0f + 0.5f), 1u, 255u) << 8);
                for (int k = 0; k < 4; k++) out[32 + k] = u8(fl >> (24 - 8 * k));
            }
        }
    }
    if (dump && b.dumpOffset != ~0u) {
        const u64 ptr = u64(reinterpret_cast<uintptr_t>(dump));
        for (int k = 0; k < 8; k++) sReplay[b.dumpOffset + k] = u8(ptr >> (56 - 8 * k));
    }
    // Each replay starts from the GX state the tape started from on the console: the state after
    // the previous tape (Aurora replay state slots, patch 0021; from the SMS port). A tape is
    // replayed several times (once per presented frame); without this every replay after the
    // first started from its own end state, so a frame relying on state left by the previous one
    // (vertex layout, arrays, TEV setup) drew differently in its fresh and late replays. The end
    // state of each tape is saved after it is replayed (four slots: LM_INTERP_TEST replays tape N
    // before the late replays of N-1); a tape whose predecessor was never replayed (dropped, or
    // drawn directly since reset()) starts from the current state.
    if (b.seq > 1 && sEndSeq[(b.seq - 1) & 3] == b.seq - 1) GXRestoreStateAurora(u8((b.seq - 1) & 3));
    AuroraReplay(sReplay.data(), u32(sReplay.size()));
    GXSaveStateAurora(u8(b.seq & 3));
    sEndSeq[b.seq & 3] = b.seq;
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
    u32 total = 0, unmatched01 = 0, unmatched12 = 0, jerky = 0, logged = 0, bbTotal = 0, bbTracked = 0, bbJerky = 0;
    for (u32 i = 0; i < t1.patches.size(); i++) {
        const Patch& p = t1.patches[i];
        if (p.kind != Kind::PosMtx) continue;
        total++;
        // Camera-facing sprites (particles, billboards): no rotation in view space.
        const bool billboard = std::fabs(val(t1, p, 1)) + std::fabs(val(t1, p, 2)) + std::fabs(val(t1, p, 4)) +
                                   std::fabs(val(t1, p, 6)) + std::fabs(val(t1, p, 8)) + std::fabs(val(t1, p, 9)) <
                               1e-3f;
        bbTotal += billboard;
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
        if (billboard) {
            bbTracked++;
            if (jerk > 0.5f * step + 3.0f && bbJerky++ < 6)
                log("[interp]   billboard bundle %u: (%.1f %.1f %.1f s%.2f) (%.1f %.1f %.1f s%.2f) (%.1f %.1f %.1f s%.2f)", p.bundle,
                    val(t0, q0, 3), val(t0, q0, 7), val(t0, q0, 11), val(t0, q0, 0), val(t1, p, 3), val(t1, p, 7),
                    val(t1, p, 11), val(t1, p, 0), val(t2, q2, 3), val(t2, q2, 7), val(t2, q2, 11), val(t2, q2, 0));
            else if (jerk > 0.5f * step + 3.0f) bbJerky++;
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
        "proj %u light %u verts %u (%u paired); billboards %u, %u tracked over 3 frames, %u jerky",
        t1.dumpFrame, total, unmatched01, unmatched12, jerky, int(t1.cut), kinds[1], kinds[2], kinds[3], kinds[4],
        kinds[5], verts, bbTotal, bbTracked, bbJerky);
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
        for (int k = 0; k < 5; k++) sPhaseUs[k] += b->phaseUs[k];
        sUnpairedSum += b->unpaired;
    }
    const double elapsed = std::chrono::duration<double>(Clock::now() - sStatsStart).count();
    if (elapsed >= 5.0) {
        log("[interp] presented %.1f fps, game %.1f fps, tape %.0f KiB + arrays %.0f KiB, per tape %.0f paired / %.0f "
            "unpaired; per present %u blended / %u snapped; %u cut presents; tape close %.2f ms (guest: arrays %.2f, "
            "copy %.2f, draws %.2f, rest %.2f, cut %.2f), replay %.2f ms (host)",
            sPresented / elapsed, sTapes / elapsed, sTapes ? double(sTapeBytes) / sTapes / 1024.0 : 0.0,
            sTapes ? double(sArrayBytes) / sTapes / 1024.0 : 0.0, sTapes ? double(sPairedSum) / sTapes : 0.0,
            sTapes ? double(sUnpairedSum) / sTapes : 0.0, sPresented ? sBlended / sPresented : 0,
            sPresented ? sSnapped / sPresented : 0, sCuts, sTapes ? double(sGuestUs) / sTapes / 1000.0 : 0.0,
            sTapes ? double(sPhaseUs[0]) / sTapes / 1000.0 : 0.0, sTapes ? double(sPhaseUs[1]) / sTapes / 1000.0 : 0.0,
            sTapes ? double(sPhaseUs[2]) / sTapes / 1000.0 : 0.0, sTapes ? double(sPhaseUs[3]) / sTapes / 1000.0 : 0.0,
            sTapes ? double(sPhaseUs[4]) / sTapes / 1000.0 : 0.0,
            sPresented ? double(sReplayUs) / sPresented / 1000.0 : 0.0);
        sPresented = sTapes = sBlended = sSnapped = sCuts = 0;
        sTapeBytes = sArrayBytes = sPairedSum = sUnpairedSum = sGuestUs = sReplayUs = 0;
        std::fill(std::begin(sPhaseUs), std::end(sPhaseUs), 0ull);
        sStatsStart = Clock::now();
    }
    return true;
}

void reset() {
    std::lock_guard lk(sM);
    // Frames are drawn directly from now on: the saved replay states are out of date.
    std::fill(std::begin(sEndSeq), std::end(sEndSeq), 0);
    // The FIFO may still be reading the last replay's array copies.
    if (sPrevious) sRetired.push_back(std::move(sPrevious));
    if (sNewest) sRetired.push_back(std::move(sNewest));
    while (sRetired.size() > 6) sRetired.pop_front();
}

}  // namespace recomp::interp
