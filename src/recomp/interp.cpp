// Frame interpolation (interp.h, docs/INTERPOLATION.md): tapes, pairing, blended replay.
#include "recomp/interp.h"

#include "recomp/efb_copy.h"
#include "recomp/hle.h"

#include <dolphin/gx/GXAurora.h>

#include <algorithm>
#include <array>
#include <atomic>
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
    u16 view = 0xFFFF;  // index into Tape::views: the camera it was noted under
    u8 role = 0;        // 1: a shadow receiver's texture matrix (noteShadowLight), 2: 2D (orthographic)
};

using M34 = std::array<f32, 12>;
struct ShadowLight {
    bool valid = false;
    M34 lv{}, kp{}, kr{};
};

struct Tape {
    std::vector<u8> bytes;
    std::vector<Patch> patches;
    std::vector<u64> bundleIds;  // per draw: what is drawn (display list, vertex layout)
    std::vector<std::array<u32, 2>> bundleRange;  // per draw: its tape bytes (~0u: unknown)
    std::vector<u8> bundlePass;                    // per draw: the pass (0 main, 1 shadow casters)
    // Fades (pairTapes): draws of this tape without a partner fade in; draws of pairedWith
    // without a partner fade out, drawn from pairedWith's commands at `.second` (an offset
    // in this tape: after the draw paired with the closest earlier paired draw).
    std::vector<u32> spawns;
    std::vector<std::pair<u32, u32>> dying;
    std::vector<M34> views;      // camera view matrices the values were noted under
    ShadowLight shadow;
    std::vector<std::vector<u8>> arrays;  // vertex-array copies the tape's array loads point at
    u64 arrayBytes = 0;
    std::vector<std::shared_ptr<const std::vector<u8>>> textures;  // texture/TLUT copies its loads point at
    u64 textureBytes = 0;  // newly copied for this tape
    mutable std::atomic<bool> replayed{false};  // host: replayed at least once
    u32 retrace = 0;
    Clock::time_point done;
    // Pairing with the tape captured before this one (`pairedWith`, compared by identity).
    const Tape* pairedWith = nullptr;
    u64 seq = 0;  // capture order (consecutive tapes differ by 1); replay state slots
    std::vector<s32> partner;  // per patch: index into pairedWith->patches, or -1
    bool cut = false;          // a camera cut since pairedWith: shown as is
    float camStep = 0;         // median relative move of the paired position matrices (cut check)
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
std::vector<std::array<u32, 2>> sBundleRange;
std::vector<u8> sBundlePass;
s32 sOpenDraw = -1;  // immediate-mode draw whose end is not known yet
std::vector<M34> sViews;
ShadowLight sShadow;
int sShadowRecv = 0;
u32 sPass = 0;
bool sOrtho = false;

// The camera the game is drawing with now (see viewMatrixSource), as an index into sViews.
u16 sampleView() {
    const u32 src = viewMatrixSource();
    if (!src) return 0xFFFF;
    M34 m;
    for (int i = 0; i < 12; i++) m[i] = std::bit_cast<f32>(rd32(src + 4 * i));
    if (!sViews.empty() && sViews.back() == m) return u16(sViews.size() - 1);
    for (size_t i = sViews.size() > 8 ? sViews.size() - 8 : 0; i < sViews.size(); i++)
        if (sViews[i] == m) return u16(i);
    if (sViews.size() >= 0xFFF0) return 0xFFFF;
    sViews.push_back(m);
    return u16(sViews.size() - 1);
}
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
std::vector<s32> sViewToA, sViewToB;  // per replay: view of b -> view of a and back (paired values)
u32 sPresented = 0, sTapes = 0, sBlended = 0, sSnapped = 0, sCuts = 0;
Clock::time_point sStatsStart = Clock::now();
u64 sTapeBytes = 0, sArrayBytes = 0, sTexBytes = 0, sPairedSum = 0, sUnpairedSum = 0, sGuestUs = 0, sReplayUs = 0;
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

bool rigidView(const f32* v);

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
            // In world space when the camera is known (camera motion is then not a draw's own
            // motion, and the motion limit below means what it says).
            if (p.view < t.views.size()) {
                const M34& v = t.views[p.view];
                float d[3], w[3];
                for (int r = 0; r < 3; r++) d[r] = out[r] - v[4 * r + 3];
                for (int r = 0; r < 3; r++) w[r] = v[r] * d[0] + v[4 + r] * d[1] + v[8 + r] * d[2];
                for (int r = 0; r < 3; r++) out[r] = w[r];
            }
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
    // Colors are not part of a draw's identity (a material may set them only when they change);
    // they are paired after the draws (pairTapes).
    for (const Patch& p : t.patches)
        if (p.bundle < n && p.kind != Kind::Color) start[p.bundle + 1]++;
    for (size_t k = 0; k < n; k++) start[k + 1] += start[k];
    out.flat.resize(start[n]);
    std::vector<u32> fill(start.begin(), start.end() - 1);
    for (u32 i = 0; i < t.patches.size(); i++)
        if (t.patches[i].bundle < n && t.patches[i].kind != Kind::Color) out.flat[fill[t.patches[i].bundle]++] = i;
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

// Draws that appear (no partner in a) fade in over the in-between frames, draws that are gone
// (no partner in b) fade out. Only 3D draws of the main pass with a model-view matrix or
// CPU-built vertices count; their tape bytes must be known.
void findFades(const Tape& a, Tape& b) {
    b.spawns.clear();
    b.dying.clear();
    static const bool sOff = std::getenv("LM_INTERP_FADES") && std::getenv("LM_INTERP_FADES")[0] == '0';
    if (sOff || b.cut) return;
    const auto eligible = [](const Tape& t, std::vector<u8>& out) {
        out.assign(t.bundleIds.size(), 0);
        for (const Patch& p : t.patches)
            if (p.bundle < out.size() && p.role == 0 && (p.kind == Kind::PosMtx || p.kind == Kind::Verts)) out[p.bundle] = 1;
        for (u32 k = 0; k < out.size(); k++)
            if (k >= t.bundleRange.size() || t.bundleRange[k][0] == ~0u || t.bundleRange[k][1] == ~0u ||
                t.bundleRange[k][1] <= t.bundleRange[k][0] || (k < t.bundlePass.size() && t.bundlePass[k] != 0))
                out[k] = 0;
    };
    static std::vector<u8> eA, eB;
    eligible(a, eA);
    eligible(b, eB);
    std::vector<s32> aToB(a.bundleIds.size(), -1);
    std::vector<u8> bPaired(b.bundleIds.size(), 0);
    for (u32 i = 0; i < b.patches.size(); i++) {
        const s32 j = b.partner[i];
        const u32 kb = b.patches[i].bundle;
        if (j < 0 || kb >= bPaired.size()) continue;
        bPaired[kb] = 1;
        const u32 ka = a.patches[j].bundle;
        if (ka < aToB.size() && aToB[ka] < 0) aToB[ka] = s32(kb);
    }
    for (u32 k = 0; k < eB.size(); k++)
        if (eB[k] && !bPaired[k]) b.spawns.push_back(k);
    s32 lastB = -1;  // b draw paired with the closest earlier paired draw of a
    for (u32 k = 0; k < eA.size(); k++) {
        if (aToB[k] >= 0) {
            lastB = aToB[k];
            continue;
        }
        if (!eA[k] || lastB < 0 || u32(lastB) >= b.bundleRange.size()) continue;
        const u32 at = b.bundleRange[lastB][1];
        if (at == ~0u) continue;
        b.dying.emplace_back(k, at);
    }
    // Inserted in b's stream in order of position.
    std::stable_sort(b.dying.begin(), b.dying.end(), [](const auto& x, const auto& y) { return x.second < y.second; });
}

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
            // Values in draw order; a skinned model may load its joints in another order the next
            // frame: if a position matrix would then jump, its draw's values of each kind are
            // paired nearest first instead.
            bool reorder = false;
            for (size_t k = 0; k < pb.size() && !reorder; k++) {
                const Patch& p = b.patches[pb[k]];
                if (p.kind != Kind::PosMtx || p.count != 12) continue;
                f32 va[12], vb[12];
                bool close = true;  // cheap test first: nearly the same matrix needs no decomposition
                for (int e = 0; e < 12; e++) {
                    va[e] = val(a, a.patches[pa[k]], e), vb[e] = val(b, p, e);
                    close = close && std::fabs(va[e] - vb[e]) < ((e & 3) == 3 ? 100.0f : 0.25f);
                }
                reorder = !close && !blendable(Kind::PosMtx, va, vb, 12);
            }
            if (!reorder) {
                for (size_t k = 0; k < pb.size(); k++) {
                    b.partner[pb[k]] = s32(pa[k]);
                    usedA[pa[k]] = 1;
                }
                continue;
            }
            static std::vector<std::tuple<float, u32, u32>> cand;
            cand.clear();
            for (size_t x = 0; x < pb.size(); x++)
                for (size_t y = 0; y < pa.size(); y++)
                    if (compatible(a.patches[pa[y]], b.patches[pb[x]]))
                        cand.emplace_back(distance(a, a.patches[pa[y]], b, b.patches[pb[x]]), pa[y], pb[x]);
            std::sort(cand.begin(), cand.end());
            for (const auto& [d, ia, ib] : cand)
                if (!usedA[ia] && b.partner[ib] < 0) {
                    b.partner[ib] = s32(ia);
                    usedA[ia] = 1;
                }
        }
        // Colors follow their draw: the n-th write of a register before (or in) a draw pairs
        // with the n-th write of that register for the draw it was paired with.
        {
            std::unordered_map<u64, u32> colA, occ;
            colA.reserve(64);
            for (u32 i = 0; i < a.patches.size(); i++) {
                const Patch& p = a.patches[i];
                if (p.kind != Kind::Color || p.bundle >= ba.key.size()) continue;
                const u64 k0 = u64(p.bundle) << 32 | p.slot;
                colA.emplace(mix(k0 ^ u64(occ[k0]++) << 56), i);
            }
            occ.clear();
            for (u32 j = 0; j < b.patches.size(); j++) {
                const Patch& p = b.patches[j];
                if (p.kind != Kind::Color || p.bundle >= bb.key.size()) continue;
                const u64 k0 = u64(p.bundle) << 32 | p.slot;
                const u32 n = occ[k0]++;
                if (drawPartner[p.bundle] < 0) continue;
                const auto it = colA.find(mix((u64(u32(drawPartner[p.bundle])) << 32 | p.slot) ^ u64(n) << 56));
                if (it == colA.end() || usedA[it->second] || a.patches[it->second].posOffset != p.posOffset) continue;
                b.partner[j] = s32(it->second);
                usedA[it->second] = 1;
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
    static std::vector<float> rel;
    rel.clear();
    b.paired = b.unpaired = 0;
    for (u32 i = 0; i < b.patches.size(); i++) {
        const Patch& p = b.patches[i];
        if (b.partner[i] < 0) {
            b.unpaired++;
            continue;
        }
        b.paired++;
        if (p.kind != Kind::PosMtx || p.role == 2) continue;
        const Patch& q = a.patches[b.partner[i]];
        f32 va[12], vb[12];
        for (int k = 0; k < 12; k++) va[k] = val(a, q, k), vb[k] = val(b, p, k);
        pos++;
        if (!blendable(Kind::PosMtx, va, vb, 12)) jumps++;
        float dd = 0, db = 0;
        for (int r = 0; r < 3; r++) {
            const float d = vb[4 * r + 3] - va[4 * r + 3];
            dd += d * d, db += vb[4 * r + 3] * vb[4 * r + 3];
        }
        if (std::isfinite(dd) && std::isfinite(db)) rel.push_back(std::sqrt(dd) / std::max(std::sqrt(db), 300.0f));
    }
    b.cut = pos >= 8 && jumps * 2 > pos;
    // The game's camera itself: the view most position matrices were noted under. A jump of its
    // eye by more than 250 units or a turn of more than 30 degrees in one game frame is a cut.
    {
        auto mainView = [](const Tape& t) -> const M34* {
            std::unordered_map<u16, u32> n;
            for (const Patch& p : t.patches)
                if (p.kind == Kind::PosMtx && p.role == 0 && p.view < t.views.size()) n[p.view]++;
            u16 best = 0xFFFF;
            u32 cnt = 0;
            for (const auto& [v, c] : n)
                if (c > cnt) best = v, cnt = c;
            return cnt >= 8 ? &t.views[best] : nullptr;
        };
        const M34 *va = mainView(a), *vb = mainView(b);
        if (va && vb && rigidView(va->data()) && rigidView(vb->data())) {
            float ea[3], eb[3], dot = 0;
            for (int c = 0; c < 3; c++) {
                ea[c] = -((*va)[c] * (*va)[3] + (*va)[4 + c] * (*va)[7] + (*va)[8 + c] * (*va)[11]);
                eb[c] = -((*vb)[c] * (*vb)[3] + (*vb)[4 + c] * (*vb)[7] + (*vb)[8 + c] * (*vb)[11]);
            }
            for (int r = 0; r < 3; r++) dot += (*va)[8 + r] * (*vb)[8 + r];  // view directions
            const float de = std::sqrt((ea[0] - eb[0]) * (ea[0] - eb[0]) + (ea[1] - eb[1]) * (ea[1] - eb[1]) +
                                       (ea[2] - eb[2]) * (ea[2] - eb[2]));
            if (de > 250.0f || dot < 0.866f) {
                if (sTrace && !b.cut) log("[interp] tape %llu: camera cut (eye moved %.0f, turned %.0f degrees)",
                                         (unsigned long long)b.seq, de, std::acos(std::clamp(dot, -1.0f, 1.0f)) * 57.3f);
                b.cut = true;
            }
        }
    }
    // A camera cut to a nearby viewpoint (the camera jumps a few hundred units) keeps far objects
    // within the blend limits, so the test above misses it and the in-between frames would slide
    // from one shot to the other. Steady camera motion moves the scene by about the same relative
    // amount every game frame; a cut moves it by far more than the frame before did (SMS port).
    if (rel.size() >= 8) {
        std::nth_element(rel.begin(), rel.begin() + rel.size() / 2, rel.end());
        b.camStep = rel[rel.size() / 2];
        if (b.camStep > 0.08f && b.camStep > 4.0f * a.camStep) b.cut = true;
    }
    if (sTrace && (b.cut || b.camStep > 0.04f))
        log("[interp] tape %llu: scene step %.3f (previous %.3f), %u of %u position matrices jump%s",
            (unsigned long long)b.seq, b.camStep, a.camStep, jumps, pos, b.cut ? ": CUT" : "");
    findFades(a, b);
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
            // Scales blend freely (particles grow, eyelids blink to nothing); a wrong pair would
            // also have to agree in rotation and position.
            if (std::max(sa, sb) > 0.05f && (sb > 16.0f * sa + 1e-4f || sa > 16.0f * sb + 1e-4f)) return false;
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
        if (big <= 4.0f) {
            // Texture-coordinate scale/scroll: a wrap makes the translation jump by about 1.
            // Otherwise (rows turning: environment/reflection mapping from normals) the rows are
            // compared like a rotation below.
            const int cols = n == 8 ? 4 : n / 3;
            if (cols == 4)
                for (int r = 0; r * 4 < n; r++)
                    if (std::fabs(a[4 * r + 3] - b[4 * r + 3]) > 0.5f) return false;
            if (cols != 4)
                for (int i = 0; i < n; i++)
                    if (std::fabs(a[i] - b[i]) > 0.5f) return false;
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
    case Kind::Light: {  // position xyz, direction xyz (view space)
        // A jump larger than half its distance (at least 400 units) snaps: distant lights move by
        // hundreds of units per frame as the camera turns (from the SMS port).
        float da = 0, db = 0, dd = 0;
        for (int i = 0; i < 3; i++) da += a[i] * a[i], db += b[i] * b[i], dd += (a[i] - b[i]) * (a[i] - b[i]);
        if (!std::isfinite(dd)) return false;
        return std::sqrt(dd) <= std::max(400.0f, 0.5f * std::sqrt(std::max(da, db)));
    }
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
    sBundleRange.clear();
    sBundlePass.clear();
    sOpenDraw = -1;
    sViews.clear();
    sShadow = {};
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
    std::vector<std::shared_ptr<const std::vector<u8>>> textures;
    u64 textureBytes = 0;
    detail::texEndTape(textures, textureBytes);
    detail::drawEnd(mark());
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
    tape->bundleRange = std::move(sBundleRange);
    sBundleRange = {};
    tape->bundlePass = std::move(sBundlePass);
    sBundlePass = {};
    tape->views = std::move(sViews);
    sViews = {};
    tape->shadow = sShadow;
    tape->lights = std::move(sLights);
    sLights = {};
    tape->arrays = std::move(arrays);
    tape->arrayBytes = arrayBytes;
    tape->textures = std::move(textures);
    tape->textureBytes = textureBytes;
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

u8* detail::captureBytes() { return sScratch.data(); }

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
    if (kind == Kind::TexMtx && sShadowRecv > 0) p.role = 1;
    else if (sOrtho && kind != Kind::Projection) p.role = 2;
    if (p.role != 2 && (kind == Kind::PosMtx || kind == Kind::NrmMtx || kind == Kind::Light || p.role == 1)) p.view = sampleView();
    detail::drawLog("  #%u kind %u slot %u src %08X first %.2f %.2f %.2f %.2f", u32(sPatches.size()), u32(kind), slot,
                    source, loadBE(&sScratch[p.offset]), nFloats > 1 ? loadBE(&sScratch[p.offset + 4]) : 0.0f,
                    nFloats > 2 ? loadBE(&sScratch[p.offset + 8]) : 0.0f, nFloats > 3 ? loadBE(&sScratch[p.offset + 12]) : 0.0f);
    sPatches.push_back(p);
}

namespace {
void pushColor(u32 at, u32 slot, u8 fmt) {
    Patch p{at, 1, Kind::Color, 0, 0, 0, slot};
    p.posOffset = fmt;
    p.origin = mix(u64(Kind::Color) << 56 ^ u64(slot) << 8 ^ fmt);
    sPatches.push_back(p);
}
}  // namespace

void noteColors(u32 at) {
    if (at == ~0u || !capturing() || sNoBlend > 0) return;
    const u32 end = sBase + AuroraCapturePos();
    const u8* s = sScratch.data();
    const auto rd = [&](u32 o) { return u32(s[o]) << 24 | u32(s[o + 1]) << 16 | u32(s[o + 2]) << 8 | s[o + 3]; };
    for (u32 i = at; i < end;) {
        if (s[i] == 0x61 && i + 5 <= end) {  // BP: TEV color/konst registers, fog color
            const u32 reg = s[i + 1];
            if (reg >= 0xE0 && reg <= 0xE7) pushColor(i + 1, reg, 1);
            else if (reg == 0xF2) pushColor(i + 1, reg, 2);
            i += 5;
        } else if (s[i] == 0x10 && i + 5 <= end) {  // XF: channel colors, light blocks
            const u32 hdr = rd(i + 1), n = ((hdr >> 16) & 0xFFFF) + 1, addr = hdr & 0xFFFF;
            for (u32 k = 0; k < n && i + 5 + 4 * k + 4 <= end; k++) {
                const u32 r = addr + k;
                if ((r >= 0x100A && r <= 0x100D) || (r >= 0x600 && r < 0x680 && (r & 15) == 3)) pushColor(i + 5 + 4 * k, r, 0);
            }
            i += 5 + 4 * n;
        } else {
            i++;
        }
    }
}

void noteShadowLight(const float lightView[12], const float kProj[12], const float kRamp[12]) {
    if (!capturing()) return;
    sShadow.valid = true;
    std::copy(lightView, lightView + 12, sShadow.lv.begin());
    std::copy(kProj, kProj + 12, sShadow.kp.begin());
    std::copy(kRamp, kRamp + 12, sShadow.kr.begin());
}
void pushShadowReceivers() { sShadowRecv++; }
void setOrthographic(bool ortho) { sOrtho = ortho; }
void popShadowReceivers() { sShadowRecv = sShadowRecv > 0 ? sShadowRecv - 1 : 0; }

void detail::noteDrawColor(u32 at, u32 reg, u8 fmt) {
    if (!capturing() || sNoBlend > 0 || sBundleIds.empty()) return;
    pushColor(at, reg, fmt);
    sPatches.back().bundle = u32(sBundleIds.size() - 1);
    sPatches.back().group = sBundleIds.back();
    sPatches.back().stride = 1;  // bound to its draw already
    if (sUngrouped == sPatches.size() - 1) sUngrouped = u32(sPatches.size());
}

void setPass(u32 pass) { sPass = pass; }

void detail::noteDraw(u64 id) {
    id = mix(id ^ u64(sPass) << 60);  // the same model drawn in another pass is another draw
    const u32 bundle = u32(sBundleIds.size());
    sBundleIds.push_back(id);
    sBundleRange.push_back({~0u, ~0u});
    sBundlePass.push_back(u8(sPass));
    sOpenDraw = -1;
    for (u32 i = sUngrouped; i < sPatches.size(); i++) {
        Patch& p = sPatches[i];
        if (p.kind == Kind::Verts || (p.kind == Kind::Color && p.stride == 1)) continue;  // already belongs to its own draw
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
    if (sBundleIds.empty()) {
        sBundleIds.push_back(id);
        sBundleRange.push_back({~0u, ~0u});
        sBundlePass.push_back(u8(sPass));
    }
    p.bundle = u32(sBundleIds.size() - 1);
    const bool tail = sUngrouped == sPatches.size();
    sPatches.push_back(p);
    if (tail) sUngrouped = u32(sPatches.size());
}

void detail::drawRange(u32 start, u32 end) {
    if (!capturing() || sBundleRange.empty() || start == ~0u || end == ~0u) return;
    sBundleRange.back() = {start, end};
    sOpenDraw = -1;
}
void detail::drawStart(u32 start) {
    if (!capturing() || sBundleRange.empty() || start == ~0u) return;
    sBundleRange.back() = {start, ~0u};
    sOpenDraw = s32(sBundleRange.size() - 1);
}
void detail::drawEnd(u32 end) {
    if (sOpenDraw < 0 || end == ~0u || u32(sOpenDraw) >= sBundleRange.size()) return;
    sBundleRange[sOpenDraw][1] = end;
    sOpenDraw = -1;
}
void vtxEnd() { detail::drawEnd(mark()); }

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

// --- camera factoring (3x4 row-major matrices as the affine 4x4 [M; 0 0 0 1]) ---
// out = a x b (a may be any 3x4, e.g. a projective texture matrix's rows).
void compose(const f32* a, const f32* b, f32* out) {
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 4; c++)
            out[4 * r + c] = a[4 * r] * b[c] + a[4 * r + 1] * b[4 + c] + a[4 * r + 2] * b[8 + c] + (c == 3 ? a[4 * r + 3] : 0.0f);
}
// Inverse of the 3x3 part (adjugate); false if singular.
bool invert33(const f32* m, int cols, f32 out[9]) {
    const f32 a = m[0], b = m[1], c = m[2], d = m[cols], e = m[cols + 1], f = m[cols + 2], g = m[2 * cols],
              h = m[2 * cols + 1], i = m[2 * cols + 2];
    const f32 A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    const f32 det = a * A + b * B + c * C;
    if (!(std::fabs(det) > 1e-12f) || !std::isfinite(det)) return false;
    const f32 k = 1.0f / det;
    out[0] = A * k, out[1] = -(b * i - c * h) * k, out[2] = (b * f - c * e) * k;
    out[3] = B * k, out[4] = (a * i - c * g) * k, out[5] = -(a * f - c * d) * k;
    out[6] = C * k, out[7] = -(a * h - b * g) * k, out[8] = (a * e - b * d) * k;
    return true;
}
// out = K^-1 x T for affine-like 3x4 K (solves T = K x W for W).
bool solveAffine(const f32* k, const f32* t, f32* out) {
    f32 inv[9];
    if (!invert33(k, 4, inv)) return false;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 4; c++) {
            f32 s = 0;
            for (int j = 0; j < 3; j++) s += inv[3 * r + j] * (t[4 * j + c] - (c == 3 ? k[4 * j + 3] : 0.0f));
            out[4 * r + c] = s;
        }
    return true;
}
bool invertAffine(const f32* m, f32* out) {
    static const f32 kId[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    return solveAffine(m, kId, out);
}
// A camera view matrix: rotation (orthonormal rows) and translation.
bool rigidView(const f32* v) {
    for (int r = 0; r < 3; r++)
        for (int s = r; s < 3; s++) {
            const f32 d = v[4 * r] * v[4 * s] + v[4 * r + 1] * v[4 * s + 1] + v[4 * r + 2] * v[4 * s + 2];
            if (!(std::fabs(d - (r == s ? 1.0f : 0.0f)) < 2e-3f)) return false;
        }
    return true;
}
// Blended camera for a pair of views (cached per replay: a tape has few views).
struct ViewPair {
    u32 key;
    bool ok;
    bool cut;  // the camera jumped (another light took over the shadow pass, a cut): show b as is
    M34 va, vb, vt;  // views and blended view
    M34 ia, ib, it;  // inverses
};
std::vector<ViewPair> sViewPairs;
std::vector<u64> sLightsBlended;  // (draw << 8 | light index) of the lights blended in this replay
const ViewPair* viewPair(const Tape& a, u16 qa, const Tape& b, u16 qb, float alpha) {
    if (qa >= a.views.size() || qb >= b.views.size()) return nullptr;
    const u32 key = u32(qa) << 16 | qb;
    for (const ViewPair& vp : sViewPairs)
        if (vp.key == key) return vp.ok || vp.cut ? &vp : nullptr;
    ViewPair vp{key, false, false};
    vp.va = a.views[qa], vp.vb = b.views[qb];
    {
        float ea[3], eb[3], dot = 0, de = 0;
        for (int c = 0; c < 3; c++) {
            ea[c] = -(vp.va[c] * vp.va[3] + vp.va[4 + c] * vp.va[7] + vp.va[8 + c] * vp.va[11]);
            eb[c] = -(vp.vb[c] * vp.vb[3] + vp.vb[4 + c] * vp.vb[7] + vp.vb[8 + c] * vp.vb[11]);
            dot += vp.va[8 + c] * vp.vb[8 + c];
            de += (ea[c] - eb[c]) * (ea[c] - eb[c]);
        }
        vp.cut = std::sqrt(de) > 250.0f || dot < 0.866f;
    }
    vp.ok = rigidView(vp.va.data()) && rigidView(vp.vb.data()) && blendRigid(vp.va.data(), vp.vb.data(), 4, alpha, vp.vt.data()) &&
            invertAffine(vp.va.data(), vp.ia.data()) && invertAffine(vp.vb.data(), vp.ib.data()) &&
            invertAffine(vp.vt.data(), vp.it.data());
    sViewPairs.push_back(vp);
    return sViewPairs.back().ok || sViewPairs.back().cut ? &sViewPairs.back() : nullptr;
}
// The blended light of a pair of tapes with shadow maps: LV(t) and P(t) for both K matrices.
struct LightPair {
    bool ok = false;
    M34 lvt, kpt, krt;  // K(t) = P(t) x LV(t)
};
LightPair sLightPair;
void makeLightPair(const Tape& a, const Tape& b, float alpha) {
    sLightPair = {};
    if (!a.shadow.valid || !b.shadow.valid) return;
    {  // another light took over (flashlight <-> room light, lightning): no blend
        float ea[3], eb[3], dot = 0, de = 0;
        const M34 &la = a.shadow.lv, &lb = b.shadow.lv;
        for (int c = 0; c < 3; c++) {
            ea[c] = -(la[c] * la[3] + la[4 + c] * la[7] + la[8 + c] * la[11]);
            eb[c] = -(lb[c] * lb[3] + lb[4 + c] * lb[7] + lb[8 + c] * lb[11]);
            dot += la[8 + c] * lb[8 + c];
            de += (ea[c] - eb[c]) * (ea[c] - eb[c]);
        }
        if (std::sqrt(de) > 250.0f || dot < 0.866f) return;
    }
    M34 ia, ib;
    if (!rigidView(a.shadow.lv.data()) || !rigidView(b.shadow.lv.data()) || !invertAffine(a.shadow.lv.data(), ia.data()) ||
        !invertAffine(b.shadow.lv.data(), ib.data()) ||
        !blendRigid(a.shadow.lv.data(), b.shadow.lv.data(), 4, alpha, sLightPair.lvt.data()))
        return;
    for (int which = 0; which < 2; which++) {
        const M34& ka = which ? a.shadow.kr : a.shadow.kp;
        const M34& kb = which ? b.shadow.kr : b.shadow.kp;
        M34 pa, pb, pt;
        compose(ka.data(), ia.data(), pa.data());
        compose(kb.data(), ib.data(), pb.data());
        for (int k = 0; k < 12; k++) pt[k] = pa[k] + (pb[k] - pa[k]) * alpha;
        compose(pt.data(), sLightPair.lvt.data(), (which ? sLightPair.krt : sLightPair.kpt).data());
    }
    sLightPair.ok = true;
}
float diff12(const f32* x, const f32* y) {
    float d = 0;
    for (int k = 0; k < 12; k++) d += std::fabs(x[k] - y[k]) / (1.0f + std::fabs(y[k]));
    return d;
}
// A shadow receiver's texture matrix rebuilt from the blended light (see noteShadowLight).
// Per draw of a tape, the G of its shadow receivers' matrices T = K x G. G is the receiver's
// world matrix (TEXMTX on model-space positions) or the inverse view the game built the actors'
// post-transform matrices with (PT18/PT19, on view-space positions); the game uses the same G with
// the projection K and the depth-ramp K, but the ramp's K is singular (its s and t rows are both
// depth), so G is solved from the projection matrix of the same draw.
std::unordered_map<u32, M34> sRecvA, sRecvB;
void receiverGs(const Tape& t, std::unordered_map<u32, M34>& out) {
    out.clear();
    if (!t.shadow.valid) return;
    for (const Patch& p : t.patches) {
        const u32 key = p.bundle * 2 + (p.slot >= 64 ? 1 : 0);  // post-transform and TEXMTX receivers apart
        if (p.role != 1 || p.count != 12 || out.count(key)) continue;
        f32 v[12];
        for (int k = 0; k < 12; k++) v[k] = val(t, p, k);
        M34 g;
        if (!solveAffine(t.shadow.kp.data(), v, g.data())) continue;
        // The projection's G has orthogonal columns (a world matrix or an inverse view).
        float skew = 0;
        for (int i = 0; i < 3; i++)
            for (int j = i + 1; j < 3; j++) skew += std::fabs(g[i] * g[j] + g[4 + i] * g[4 + j] + g[8 + i] * g[8 + j]);
        if (skew < 0.05f) out.emplace(key, g);
    }
}
bool blendShadowReceiver(const Tape& a, const Patch& q, const f32* ta, const Tape& b, const Patch& p, const f32* tb,
                         float alpha, f32* out) {
    if (!sLightPair.ok || p.count != 12) return false;
    const auto ga = sRecvA.find(q.bundle * 2 + (q.slot >= 64 ? 1 : 0)), gb = sRecvB.find(p.bundle * 2 + (p.slot >= 64 ? 1 : 0));
    if (ga == sRecvA.end() || gb == sRecvB.end()) return false;
    // Which K: the one that rebuilds b's value from its G.
    M34 xp, xr;
    compose(b.shadow.kp.data(), gb->second.data(), xp.data());
    compose(b.shadow.kr.data(), gb->second.data(), xr.data());
    const bool ramp = diff12(xr.data(), tb) < diff12(xp.data(), tb);
    M34 gt;
    if (!blendRigid(ga->second.data(), gb->second.data(), 4, alpha, gt.data()))
        for (int k = 0; k < 12; k++) gt[k] = ga->second[k] + (gb->second[k] - ga->second[k]) * alpha;
    compose((ramp ? sLightPair.krt : sLightPair.kpt).data(), gt.data(), out);
    (void)ta;
    return true;
}
// view x world values blended as blended view x blended world.
bool blendFactored(const Tape& a, const Patch& q, const f32* va, const Tape& b, const Patch& p, const f32* vb, float alpha,
                   f32* out) {
    if (p.view == 0xFFFF || q.view == 0xFFFF) return false;
    const ViewPair* vp = viewPair(a, q.view, b, p.view, alpha);
    if (!vp) return false;
    if (vp->cut) {  // drawn from a camera that jumped: as captured
        std::copy(vb, vb + p.count, out);
        return true;
    }
    if (p.kind == Kind::PosMtx && p.count == 12) {
        M34 wa, wb, wt;
        compose(vp->ia.data(), va, wa.data());
        compose(vp->ib.data(), vb, wb.data());
        if (!blendRigid(wa.data(), wb.data(), 4, alpha, wt.data())) return false;
        compose(vp->vt.data(), wt.data(), out);
        return true;
    }
    // Rotation parts (rows of a rigid view): R^T x n (world), R(t) x n (view).
    const auto rotT = [](const M34& v, const f32* n, f32* o) {
        for (int r = 0; r < 3; r++) o[r] = v[r] * n[0] + v[4 + r] * n[1] + v[8 + r] * n[2];
    };
    const auto rot = [](const M34& v, const f32* n, f32* o) {
        for (int r = 0; r < 3; r++) o[r] = v[4 * r] * n[0] + v[4 * r + 1] * n[1] + v[4 * r + 2] * n[2];
    };
    if (p.kind == Kind::NrmMtx && p.count == 9) {
        f32 na[9], nb[9], nt[9];
        for (int c = 0; c < 3; c++) {  // columns: n_world = R^T n_view
            const f32 ca[3] = {va[c], va[3 + c], va[6 + c]}, cb[3] = {vb[c], vb[3 + c], vb[6 + c]};
            f32 wa[3], wb[3];
            rotT(vp->va, ca, wa);
            rotT(vp->vb, cb, wb);
            for (int r = 0; r < 3; r++) na[3 * r + c] = wa[r], nb[3 * r + c] = wb[r];
        }
        if (!blendRigid(na, nb, 3, alpha, nt)) return false;
        for (int c = 0; c < 3; c++) {
            const f32 cw[3] = {nt[c], nt[3 + c], nt[6 + c]};
            f32 cv[3];
            rot(vp->vt, cw, cv);
            for (int r = 0; r < 3; r++) out[3 * r + c] = cv[r];
        }
        return true;
    }
    if (p.kind == Kind::Light && p.count == 6) {
        f32 pa[3], pb[3], da[3], db[3], ra[3], rb[3];
        for (int k = 0; k < 3; k++) ra[k] = va[k] - vp->va[4 * k + 3], rb[k] = vb[k] - vp->vb[4 * k + 3];
        rotT(vp->va, ra, pa);
        rotT(vp->vb, rb, pb);
        rotT(vp->va, va + 3, da);
        rotT(vp->vb, vb + 3, db);
        f32 pw[3], dw[3];
        for (int k = 0; k < 3; k++) pw[k] = pa[k] + (pb[k] - pa[k]) * alpha, dw[k] = da[k] + (db[k] - da[k]) * alpha;
        rot(vp->vt, pw, out);
        for (int k = 0; k < 3; k++) out[k] += vp->vt[4 * k + 3];
        rot(vp->vt, dw, out + 3);
        return true;
    }
    return false;
}

// --- spawn/despawn fades ---------------------------------------------------------------
// A value without partner, re-expressed under the blended camera: its world placement (from
// the frame it belongs to) seen from the in-between camera, so it moves with the scene
// instead of with the camera of its own frame.
bool reexpress(const ViewPair& vp, bool fromA, const Patch& p, const f32* v, f32* out) {
    if (!vp.ok || vp.cut) return false;
    const M34& V = fromA ? vp.va : vp.vb;
    const M34& inv = fromA ? vp.ia : vp.ib;
    const auto rotT = [&](const f32* n, f32* o) {
        for (int r = 0; r < 3; r++) o[r] = V[r] * n[0] + V[4 + r] * n[1] + V[8 + r] * n[2];
    };
    const auto rot = [&](const f32* n, f32* o) {
        for (int r = 0; r < 3; r++) o[r] = vp.vt[4 * r] * n[0] + vp.vt[4 * r + 1] * n[1] + vp.vt[4 * r + 2] * n[2];
    };
    if (p.kind == Kind::PosMtx && p.count == 12) {
        M34 w;
        compose(inv.data(), v, w.data());
        compose(vp.vt.data(), w.data(), out);
        return true;
    }
    if (p.kind == Kind::NrmMtx && p.count == 9) {
        for (int c = 0; c < 3; c++) {
            const f32 n[3] = {v[c], v[3 + c], v[6 + c]};
            f32 w[3], o[3];
            rotT(n, w);
            rot(w, o);
            for (int r = 0; r < 3; r++) out[3 * r + c] = o[r];
        }
        return true;
    }
    if (p.kind == Kind::Light && p.count == 6) {
        f32 r[3], w[3];
        for (int k = 0; k < 3; k++) r[k] = v[k] - V[4 * k + 3];
        rotT(r, w);
        rot(w, out);
        for (int k = 0; k < 3; k++) out[k] += vp.vt[4 * k + 3];
        rotT(v + 3, w);
        rot(w, out + 3);
        return true;
    }
    return false;
}

std::vector<u8> sReplayA, sFaded;
u32 sFadeIns = 0, sFadeOuts = 0;  // per replay (trace)

void auroraCmd(std::vector<u8>& out, u16 sub, u8 arg) {
    out.push_back(0x50);
    out.push_back(u8(sub >> 8));
    out.push_back(u8(sub));
    out.push_back(arg);
}

// sReplay (b, values blended) with fades: b's draws without partner fade in; a's draws without
// partner are drawn from a's commands (state slot 5, masked except those draws) at their place in
// b, fading out. Returns false if there is nothing to fade (sReplay is used as is).
bool buildFades(const Tape& a, const Tape& b, float alpha, bool haveAStart, const std::vector<s32>& viewToA,
                const std::vector<s32>& viewToB) {
    sFadeIns = sFadeOuts = 0;
    if (b.spawns.empty() && (b.dying.empty() || !haveAStart)) return false;
    const u8 qIn = u8(std::lround((1.0f - alpha) * 255.0f)), qOut = u8(std::lround(alpha * 255.0f));
    struct Ev {
        u32 at;
        u8 order;  // at the same place: a fade-in ends, then fade-outs, then a fade-in starts
        u32 index;
    };
    static std::vector<Ev> evs;
    evs.clear();
    if (qIn != 0)
        for (u32 k : b.spawns) {
            evs.push_back({b.bundleRange[k][0], 2, k});
            evs.push_back({b.bundleRange[k][1], 0, k});
        }
    if (haveAStart && qOut != 255) {
        // a's values of the dying draws, seen from the blended camera.
        sReplayA.assign(a.bytes.begin(), a.bytes.end());
        static std::vector<u8> dyingA;
        dyingA.assign(a.bundleIds.size(), 0);
        for (const auto& [k, at] : b.dying) dyingA[k] = 1;
        for (const Patch& q : a.patches) {
            if (q.bundle >= dyingA.size() || !dyingA[q.bundle] || q.view >= a.views.size() || q.view >= viewToB.size()) continue;
            if (viewToB[q.view] < 0 || q.count > 16) continue;
            const ViewPair* vp = viewPair(a, q.view, b, u16(viewToB[q.view]), alpha);
            if (!vp) continue;
            f32 v[16], out[16];
            for (int k = 0; k < q.count; k++) v[k] = val(a, q, k);
            if (reexpress(*vp, true, q, v, out))
                for (int k = 0; k < q.count; k++) storeBE(sReplayA.data() + q.offset + 4 * k, out[k]);
        }
        // Group the dying draws by their place in b (at most 8 passes over a per replay).
        u32 groups = 0;
        for (u32 i = 0; i < b.dying.size(); i++)
            if (i == 0 || b.dying[i].second != b.dying[i - 1].second) groups++;
        const u32 every = (groups + 7) / 8;
        u32 g = 0;
        for (u32 i = 0; i < b.dying.size(); i++) {
            const bool first = i == 0 || b.dying[i].second != b.dying[i - 1].second;
            if (first) g++;
            const bool last = i + 1 == b.dying.size() || (b.dying[i + 1].second != b.dying[i].second && g % every == 0);
            if (last) evs.push_back({b.dying[i].second, 1, i});  // the pass ends with dying[i]
        }
    }
    if (evs.empty()) return false;
    std::sort(evs.begin(), evs.end(), [](const Ev& x, const Ev& y) { return x.at != y.at ? x.at < y.at : x.order < y.order; });
    sFaded.clear();
    sFaded.reserve(sReplay.size() + evs.size() * 16 + (b.dying.empty() ? 0 : a.bytes.size()));
    u32 cursor = 0;      // in sReplay (b)
    u32 aCursor = 0;     // in sReplayA: a's commands processed so far (state in slot 5)
    u32 nextDying = 0;   // b.dying index
    for (const Ev& e : evs) {
        if (e.at > sReplay.size() || e.at < cursor) continue;
        sFaded.insert(sFaded.end(), sReplay.begin() + cursor, sReplay.begin() + e.at);
        cursor = e.at;
        if (e.order == 2) {
            auroraCmd(sFaded, GX_AURORA_SET_FADE, qIn);
            sFadeIns++;
        } else if (e.order == 0) {
            auroraCmd(sFaded, GX_AURORA_SET_FADE, 0);
        } else {
            auroraCmd(sFaded, GX_AURORA_STATE_SAVE, 4);
            auroraCmd(sFaded, GX_AURORA_STATE_RESTORE, 5);
            auroraCmd(sFaded, GX_AURORA_SET_REPLAY_MASK, 1);
            for (; nextDying <= e.index; nextDying++) {
                const auto& r = a.bundleRange[b.dying[nextDying].first];
                if (r[0] < aCursor || r[1] > sReplayA.size()) continue;  // out of order: skipped
                sFaded.insert(sFaded.end(), sReplayA.begin() + aCursor, sReplayA.begin() + r[0]);
                auroraCmd(sFaded, GX_AURORA_SET_REPLAY_MASK, 0);
                auroraCmd(sFaded, GX_AURORA_SET_FADE, qOut);
                sFaded.insert(sFaded.end(), sReplayA.begin() + r[0], sReplayA.begin() + r[1]);
                auroraCmd(sFaded, GX_AURORA_SET_FADE, 0);
                auroraCmd(sFaded, GX_AURORA_SET_REPLAY_MASK, 1);
                aCursor = r[1];
                sFadeOuts++;
            }
            auroraCmd(sFaded, GX_AURORA_SET_REPLAY_MASK, 0);
            auroraCmd(sFaded, GX_AURORA_STATE_SAVE, 5);
            auroraCmd(sFaded, GX_AURORA_STATE_RESTORE, 4);
        }
    }
    sFaded.insert(sFaded.end(), sReplay.begin() + cursor, sReplay.end());
    (void)viewToA;
    return true;
}

// Builds tape b with its values blended from a toward b (alpha 1 = b as captured) into
// sReplay and submits it. `dump` redirects the LM_FRAME_DUMP copy (LM_INTERP_TEST).
void replayPair(const Tape* a, const Tape& b, float alpha, void* dump) {
    sReplay.assign(b.bytes.begin(), b.bytes.end());
    if (a && b.pairedWith != a) a = nullptr;  // not consecutive (a tape was dropped): no pairing
    if (a && b.cut) {
        sCuts++;
        a = nullptr;
    }
    if (a && alpha < 1.0f && sLogSnaps) {  // self-check: the rebuilt values at alpha 1 must equal b's
        sViewPairs.clear();
        makeLightPair(*a, b, 1.0f);
        receiverGs(*a, sRecvA);
        receiverGs(b, sRecvB);
        u32 bad = 0, checked = 0;
        for (u32 i = 0; i < b.patches.size(); i++) {
            const s32 j = b.partner[i];
            if (j < 0) continue;
            const Patch& p = b.patches[i];
            const Patch& q = a->patches[j];
            if (p.kind == Kind::Verts || p.kind == Kind::Color || p.count > 16) continue;
            f32 va[16], vb[16], out[16];
            for (int k = 0; k < p.count; k++) va[k] = val(*a, q, k), vb[k] = val(b, p, k);
            bool ok;
            if (p.role == 1) ok = blendShadowReceiver(*a, q, va, b, p, vb, 1.0f, out);
            else ok = blendFactored(*a, q, va, b, p, vb, 1.0f, out);
            if (!ok) continue;
            checked++;
            float e = 0;
            for (int k = 0; k < p.count; k++) e = std::max(e, std::fabs(out[k] - vb[k]) / (1.0f + std::fabs(vb[k])));
            if (e > 5e-3f && bad++ < 6) {  // float noise: values near 0 under a far camera
                char buf[600];
                int n = std::snprintf(buf, sizeof(buf), "[interp]   self-check kind %u role %u slot %u err %.4f:", u32(p.kind), p.role, p.slot, e);
                for (int k = 0; k < p.count && n < 560; k++) n += std::snprintf(buf + n, sizeof(buf) - n, " %.2f/%.2f", out[k], vb[k]);
                log("%s", buf);
                if (p.role == 1 && bad <= 2) {
                    auto m = [](const char* n, const f32* x) {
                        log("[interp]     %s %.3f %.3f %.3f %.2f | %.3f %.3f %.3f %.2f | %.3f %.3f %.3f %.2f", n, x[0], x[1], x[2], x[3], x[4],
                            x[5], x[6], x[7], x[8], x[9], x[10], x[11]);
                    };
                    if (p.view < b.views.size()) m("V ", b.views[p.view].data());
                    m("LV", b.shadow.lv.data());
                    m("KP", b.shadow.kp.data());
                    m("KR", b.shadow.kr.data());
                    if (p.view < b.views.size()) {
                        M34 kb;
                        compose(vb, b.views[p.view].data(), kb.data());
                        m("TV", kb.data());
                    }
                }
            }
        }
        log("[interp]   self-check: %u of %u rebuilt values differ at alpha 1", bad, checked);
    }
    if (a && alpha < 1.0f) {
        sViewPairs.clear();
        sLightsBlended.clear();
        makeLightPair(*a, b, alpha);
        receiverGs(*a, sRecvA);
        receiverGs(b, sRecvB);
        // LM_INTERP_NOBLEND=<mask> (test aid): value kinds (bit = Kind) shown as captured.
        static const u32 sNoBlendKinds = [] {
            const char* v = std::getenv("LM_INTERP_NOBLEND");
            return v ? u32(std::strtoul(v, nullptr, 0)) : 0u;
        }();
        // Which camera of a goes with which of b (for values without partner).
        sViewToA.assign(b.views.size(), -1);
        sViewToB.assign(a->views.size(), -1);
        for (u32 i = 0; i < b.patches.size(); i++) {
            const s32 j = b.partner[i];
            if (j < 0) continue;
            const u16 vb = b.patches[i].view, va = a->patches[j].view;
            if (vb < sViewToA.size() && va < sViewToB.size()) {
                if (sViewToA[vb] < 0) sViewToA[vb] = va;
                if (sViewToB[va] < 0) sViewToB[va] = vb;
            }
        }
        for (u32 i = 0; i < b.patches.size(); i++) {
            const s32 j = b.partner[i];
            if (j < 0) {
                // No partner (it appeared): b's placement in the world, seen from the blended camera.
                const Patch& p = b.patches[i];
                if (p.role != 0 || p.view >= sViewToA.size() || sViewToA[p.view] < 0 || p.count > 16) continue;
                const ViewPair* vp = viewPair(*a, u16(sViewToA[p.view]), b, p.view, alpha);
                f32 v[16], out[16];
                for (int k = 0; k < p.count; k++) v[k] = val(b, p, k);
                if (vp && reexpress(*vp, false, p, v, out))
                    for (int k = 0; k < p.count; k++) storeBE(sReplay.data() + p.offset + 4 * k, out[k]);
                continue;
            }
            const Patch& p = b.patches[i];
            const Patch& q = a->patches[j];
            if (sNoBlendKinds & (1u << u32(p.kind))) continue;
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
            // A light's color goes with its position: if the light was not blended (another light in
            // the slot, a jump), its color is not either (a's color at b's place is neither frame).
            if (p.kind == Kind::Color && p.slot >= 0x600 && p.slot < 0x680) {
                const u64 key = u64(p.bundle) << 8 | ((p.slot - 0x600) >> 4);
                if (std::find(sLightsBlended.begin(), sLightsBlended.end(), key) == sLightsBlended.end()) continue;
            }
            if (p.kind == Kind::Color) {  // material/light/TEV/fog colors: per channel
                            const auto rd = [](const u8* s) { return u32(s[0]) << 24 | u32(s[1]) << 16 | u32(s[2]) << 8 | s[3]; };
                const u32 ca = rd(a->bytes.data() + q.offset), cb = rd(b.bytes.data() + p.offset);
                u32 out = cb;
                const auto lerp = [&](int shift, int bits, bool sign) {
                    const u32 mask = (1u << bits) - 1;
                    int x = int((ca >> shift) & mask), y = int((cb >> shift) & mask);
                    if (sign) {
                        if (x & (1 << (bits - 1))) x -= 1 << bits;
                        if (y & (1 << (bits - 1))) y -= 1 << bits;
                    }
                    const int v = int(std::lround(float(x) + float(y - x) * alpha));
                    out = (out & ~(mask << shift)) | (u32(v) & mask) << shift;
                };
                if (p.posOffset == 0) {
                    for (int s = 0; s < 32; s += 8) lerp(s, 8, false);
                } else if (p.posOffset == 1) {
                    const bool konst = (cb >> 23) & 1;
                    if (((ca >> 23) & 1) == u32(konst)) {
                        lerp(0, konst ? 8 : 11, !konst);
                        lerp(12, konst ? 8 : 11, !konst);
                    }
                } else {
                    for (int s = 0; s < 24; s += 8) lerp(s, 8, false);
                }
                u8* o = sReplay.data() + p.offset;
                o[0] = u8(out >> 24), o[1] = u8(out >> 16), o[2] = u8(out >> 8), o[3] = u8(out);
                sBlended++;
                continue;
            }
            f32 va[16], vb[16], out[16];
            for (int k = 0; k < p.count; k++) va[k] = val(*a, q, k), vb[k] = val(b, p, k);
            // A projection is only comparable to one of the same type (the u32 after it).
            if (p.kind == Kind::Projection &&
                std::memcmp(a->bytes.data() + q.offset + 24, b.bytes.data() + p.offset + 24, 4) != 0)
                continue;
            // A GX light slot holds whichever light the game put there for this draw: blend only the
            // same light object (its colour and range differ otherwise).
            // ...and only while its attenuation stays (a spot switched to another shape, or off by
            // its attenuation, is another light: a's colour with b's attenuation is neither frame).
            const auto attnChanged = [&] {
                if (p.kind != Kind::Light || p.offset < 24 || q.offset < 24) return false;
                for (int k = 0; k < 6; k++) {
                    const f32 x = loadBE(a->bytes.data() + q.offset - 24 + 4 * k), y = loadBE(b.bytes.data() + p.offset - 24 + 4 * k);
                    if (!(std::fabs(x - y) <= 0.02f * std::max({std::fabs(x), std::fabs(y), 1e-3f}))) return true;
                }
                return false;
            };
            if (p.kind == Kind::Light && (p.source != q.source || attnChanged())) {
                sSnapped++;
                sSnapKinds[u32(p.kind)]++;
                continue;
            }
            const bool jump2D = p.role == 2 && p.kind == Kind::PosMtx && p.count == 12 &&
                                std::fabs(va[3] - vb[3]) + std::fabs(va[7] - vb[7]) + std::fabs(va[11] - vb[11]) > 96.0f;
            if (jump2D || !blendable(p.kind, va, vb, p.count)) {
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
            if (p.role == 1) {  // a shadow receiver: rebuilt from the blended light, or as captured
                if (!blendShadowReceiver(*a, q, va, b, p, vb, alpha, out)) std::copy(vb, vb + p.count, out);
            } else if (!(blendFactored(*a, q, va, b, p, vb, alpha, out) ||
                  (p.kind == Kind::PosMtx && p.count == 12 && blendRigid(va, vb, 4, alpha, out)) ||
                  (p.kind == Kind::NrmMtx && p.count == 9 && blendRigid(va, vb, 3, alpha, out)) ||
                  (p.kind == Kind::TexMtx && p.count == 8 && blendSpin(va, vb, alpha, out))))
                for (int k = 0; k < p.count; k++)
                    out[k] = std::isfinite(va[k]) && std::isfinite(vb[k]) ? va[k] + (vb[k] - va[k]) * alpha : vb[k];
            if (p.kind == Kind::Light) {  // keep the direction's length (spot cone falloff)
                const float la = std::sqrt(va[3] * va[3] + va[4] * va[4] + va[5] * va[5]);
                const float lb = std::sqrt(vb[3] * vb[3] + vb[4] * vb[4] + vb[5] * vb[5]);
                const float lo = std::sqrt(out[3] * out[3] + out[4] * out[4] + out[5] * out[5]);
                // (Directions of lights that have none can hold any bits: only rescale sane ones.)
                if (lo > 1e-6f && la > 1e-6f && lb > 1e-6f && std::isfinite(la) && std::isfinite(lb) && std::isfinite(lo)) {
                    const float s = (la + (lb - la) * alpha) / lo;
                    for (int k = 3; k < 6; k++) out[k] *= s;
                }
            }
            for (int k = 0; k < p.count; k++) storeBE(sReplay.data() + p.offset + 4 * k, out[k]);
            if (p.kind == Kind::Light) sLightsBlended.push_back(u64(p.bundle) << 8 | (p.slot & 0xFF));
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
    const bool haveBStart = b.seq > 1 && sEndSeq[(b.seq - 1) & 3] == b.seq - 1;
    // Fades need a's start state (in slot 5) for drawing a's vanished draws.
    const bool haveAStart = a && haveBStart && a->seq > 1 && sEndSeq[(a->seq - 1) & 3] == a->seq - 1;
    const bool faded = a && alpha < 1.0f && buildFades(*a, b, alpha, haveAStart, sViewToA, sViewToB);
    if (faded && haveAStart && !b.dying.empty()) {
        GXRestoreStateAurora(u8((a->seq - 1) & 3));
        GXSaveStateAurora(5);
    }
    if (haveBStart) GXRestoreStateAurora(u8((b.seq - 1) & 3));
    // EFB copies made by this replay go into this tape's copy generation, so they don't replace the
    // previous frame's copies that this frame's early draws may still read (Aurora patch 0028).
    // Only the first replay of a tape reads its copies back into memory (bit 6 on the others).
    GXSetCopyGenerationAurora(u8((b.seq & 3) | 0x80 | (b.replayed.exchange(true) ? 0x40 : 0)));
    if (faded) AuroraReplay(sFaded.data(), u32(sFaded.size()));
    else AuroraReplay(sReplay.data(), u32(sReplay.size()));
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
        if (p.kind != Kind::PosMtx || p.role == 2) continue;
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
    sLogSnaps = sTrace && it.frames == 0 && it.alpha < 1.0f && it.name.size() > 3 && it.name.compare(it.name.size() - 3, 3, "_s1") == 0;
    std::fill(std::begin(sLoggedSnaps), std::end(sLoggedSnaps), 0u);
    std::fill(std::begin(sSnapKinds), std::end(sSnapKinds), 0u);
    // LM_INTERP_POISON=<item>,<phys hex>,<size hex> (test aid): fill that guest memory with garbage during
    // that LM_INTERP_TEST replay, to find memory a replay still reads live (bisect the range).
    static const char* sPoison = std::getenv("LM_INTERP_POISON");
    std::vector<u8> saved;
    u32 pphys = 0, psize = 0;
    if (sPoison && it.frames == 0) {
        char nm[64] = {};
        if (std::sscanf(sPoison, "%63[^,],%x,%x", nm, &pphys, &psize) == 3 && it.name == nm) {
            saved.assign(g_mem + pphys, g_mem + pphys + psize);
            std::memset(g_mem + pphys, 0xA5, psize);
            log("[dbg] poisoned %08X+%X for %s", pphys, psize, nm);
        }
    }
    replayPair(it.a.get(), *it.b, it.alpha, it.target);
    if (!saved.empty()) {
        AuroraGXSync();
        std::memcpy(g_mem + pphys, saved.data(), psize);
    }
    if (sLogSnaps) {
        u32 unpaired[kKinds] = {}, listed = 0;
        for (u32 i = 0; i < it.b->patches.size(); i++)
            if (it.b->partner.empty() || it.b->partner[i] < 0) {
                const Patch& p = it.b->patches[i];
                unpaired[u32(p.kind)]++;
                if (p.kind == Kind::PosMtx && listed++ < 6) {
                    const u64 id = p.bundle < it.b->bundleIds.size() ? it.b->bundleIds[p.bundle] : 0;
                    u32 same = 0;
                    if (it.a)
                        for (u64 x : it.a->bundleIds) same += x == id;
                    log("[interp]   unpaired pos src %08X slot %u role %u draw %016llX (%u such draws before) t %.0f %.0f %.0f", p.source,
                        p.slot, p.role, (unsigned long long)id, same, val(*it.b, p, 3), val(*it.b, p, 7), val(*it.b, p, 11));
                }
            }
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
        sTexBytes += b->textureBytes;
        sPairedSum += b->paired;
        sGuestUs += b->guestUs;
        for (int k = 0; k < 5; k++) sPhaseUs[k] += b->phaseUs[k];
        sUnpairedSum += b->unpaired;
    }
    const double elapsed = std::chrono::duration<double>(Clock::now() - sStatsStart).count();
    if (elapsed >= 5.0) {
        log("[interp] presented %.1f fps, game %.1f fps, tape %.0f KiB + arrays %.0f KiB + new texture copies %.0f KiB, "
            "per tape %.0f paired / %.0f "
            "unpaired; per present %u blended / %u snapped; %u cut presents; tape close %.2f ms (guest: arrays %.2f, "
            "copy %.2f, draws %.2f, rest %.2f, cut %.2f), replay %.2f ms (host)",
            sPresented / elapsed, sTapes / elapsed, sTapes ? double(sTapeBytes) / sTapes / 1024.0 : 0.0,
            sTapes ? double(sArrayBytes) / sTapes / 1024.0 : 0.0, sTapes ? double(sTexBytes) / sTapes / 1024.0 : 0.0,
            sTapes ? double(sPairedSum) / sTapes : 0.0,
            sTapes ? double(sUnpairedSum) / sTapes : 0.0, sPresented ? sBlended / sPresented : 0,
            sPresented ? sSnapped / sPresented : 0, sCuts, sTapes ? double(sGuestUs) / sTapes / 1000.0 : 0.0,
            sTapes ? double(sPhaseUs[0]) / sTapes / 1000.0 : 0.0, sTapes ? double(sPhaseUs[1]) / sTapes / 1000.0 : 0.0,
            sTapes ? double(sPhaseUs[2]) / sTapes / 1000.0 : 0.0, sTapes ? double(sPhaseUs[3]) / sTapes / 1000.0 : 0.0,
            sTapes ? double(sPhaseUs[4]) / sTapes / 1000.0 : 0.0,
            sPresented ? double(sReplayUs) / sPresented / 1000.0 : 0.0);
        sPresented = sTapes = sBlended = sSnapped = sCuts = 0;
        sTapeBytes = sArrayBytes = sTexBytes = sPairedSum = sUnpairedSum = sGuestUs = sReplayUs = 0;
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
