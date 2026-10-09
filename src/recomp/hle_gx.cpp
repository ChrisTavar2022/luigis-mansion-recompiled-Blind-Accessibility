// HLE: GX, forwarded to Aurora. Guest arguments are converted to host form
// (big-endian matrices and structs read out of guest memory; guest pointers to
// textures, arrays and display lists translated to host pointers into MEM1).
//
// Raw FIFO writes from game code (vertex data after GXBegin, inlined register
// writes) arrive through the write-gather pipe and are appended to Aurora's
// FIFO unchanged. The console needs no GXEnd; Aurora does, so an open draw is
// closed before the next GX call.
#include "recomp/efb_copy.h"
#include "recomp/frame.h"
#include "recomp/gx_bridge.h"
#include "recomp/hle.h"
#include "recomp/interp.h"
#include "recomp/runtime.h"
#include "recomp/widescreen.h"

#include <dolphin/gx.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

namespace recomp {
namespace {

bool sInBegin = false;
bool sInDisplayList = false;  // GXBeginDisplayList..GXEndDisplayList: everything is recorded

// Fast-forward (skip.cpp): a frame that is not shown sends no draws or copies.
bool dropDraw() { return gx::g_discardDraws && !sInDisplayList; }

GXColor color(u32 p) { return GXColor{u8(rd8(p)), u8(rd8(p + 1)), u8(rd8(p + 2)), u8(rd8(p + 3))}; }
f32 farg(const Ctx& c, int n) { return f32(c.f[n]); }

template <int R, int C>
struct Mtx {
    f32 m[R][C];
    explicit Mtx(u32 p) {
        for (int i = 0; i < R; i++)
            for (int j = 0; j < C; j++) m[i][j] = rdf32(p + 4 * (i * C + j));
    }
};

// Guest GXTexObj/GXTlutObj are smaller than Aurora's, so the host objects live
// in tables and the guest object holds a tag + index (copies of it still work).
constexpr u32 kTexTag = 0x41555458;   // 'AUTX'
constexpr u32 kTlutTag = 0x4155544C;  // 'AUTL'

template <typename T>
struct ObjTable {
    std::deque<T> objs;
    std::deque<u32> owners;

    T* forInit(u32 guest, u32 tag) {
        const u32 idx = rd32(guest + 4);
        if (rd32(guest) == tag && idx < objs.size() && owners[idx] == guest) return &objs[idx];
        objs.emplace_back();
        owners.push_back(guest);
        wr32(guest, tag);
        wr32(guest + 4, u32(objs.size() - 1));
        return &objs.back();
    }
    T* get(u32 guest, u32 tag) {
        const u32 idx = rd32(guest + 4);
        if (rd32(guest) != tag || idx >= objs.size()) return nullptr;
        return &objs[idx];
    }
};
ObjTable<GXTexObj> sTex;
ObjTable<GXTlutObj> sTlut;

const void* ptr(u32 a) { return a ? hostp(a) : nullptr; }

// --- widescreen (see widescreen.h) -------------------------------------------------
// The guest's projection, viewport and scissor are tracked here, and the host
// projection/scissor are chosen per draw: unchanged ("plain", i.e. stretched to
// the window), with the projection's x row scaled by ws::factor() ("scaled":
// Hor+ for perspective, centered 4:3 for orthographic), or with the y row scaled
// by 1/factor ("zoom": a 4:3 picture enlarged to fill the width, top and bottom
// cropped).

constexpr f32 kLogicalWidth = 640.0f;

enum class ProjMode : u8 { None, Plain, Scaled, Zoom };

struct {
    f32 m[4][4] = {};
    GXProjectionType type = GX_PERSPECTIVE;
} sProj;
bool sProjSet = false;              // the guest has set a projection
bool sProjDirty = false;            // guest projection not yet sent to Aurora
ProjMode sProjEmitted = ProjMode::None;
int sProjEmittedAnchor = 0;         // HUD anchor the emitted projection was built for

struct {
    f32 left = 0, wd = kLogicalWidth;
} sVp;
// A partial viewport set by a HUD element (ws::AnchorScope: the GBH's live 3D view) is sent
// to Aurora moved and narrowed like the 2D around it, with this anchor; scissors set while
// it is current follow it too.
bool sVpFollow = false;
int sVpAnchor = 0;
struct {
    u32 x = 0, y = 0, w = 640, h = 480;
} sScissor;
bool sScissorPillar = false;  // Aurora currently has the centered-4:3 version of the guest scissor
int sScissorAnchor = 0;
// Draws written straight into the FIFO by game code (J3D inlines its display-list
// calls and matrix loads) bypass the HLE draw entry points, so the projection for
// them is brought up to date before the first raw FIFO write after any change.
bool sRawCheck = true;

// HUD anchor for the draw being prepared: a held draw keeps the one from its GXBegin (it
// is only sent at the next GX call, which may come after the HUD element's scope ended).
int sHeldAnchor = 0;
bool sInHeldPrepare = false;
// A by-side scope is resolved per held draw (classifyHeld); anything else in it stays centered.
int scopeAnchor() {
    const int a = ws::hudAnchor();
    return a == ws::kAnchorBySide ? 0 : a;
}
int hudAnchor() { return sInHeldPrepare ? sHeldAnchor : scopeAnchor(); }

// x of the centered (and anchored) 4:3 frame for logical x: what the scaled orthographic
// projection does to 2D, in pixels.
f32 pillarX(f32 x, int anchor) {
    const f32 s = ws::factor(), mid = kLogicalWidth * 0.5f;
    return mid + (x - mid) * s + f32(anchor) * (1.0f - s) * mid;
}

bool viewportFull() { return sVp.left <= 1.0f && sVp.left + sVp.wd >= kLogicalWidth - 1.0f; }

// Frame interpolation blends the matrices of 3D draws only: 2D (under an
// orthographic projection) is often drawn with one scratch matrix reused per
// glyph or element, so its loads don't line up between frames.
u32 mark3D() { return sProj.type == GX_PERSPECTIVE ? interp::mark() : ~0u; }

void emitProjection(ProjMode mode) {
    f32 m[4][4];
    std::memcpy(m, sProj.m, sizeof(m));
    int anchor = 0;
    if (mode == ProjMode::Scaled && sProj.type == GX_PERSPECTIVE) {
        // Field of view: the vertical FOV times fovScale, then Hor+ for the width.
        const f32 r = ws::fovRatio(m[1][1]);
        const f32 s = ws::factor();
        for (int j = 0; j < 4; j++) {
            m[0][j] *= s * r;
            m[1][j] *= r;
        }
    } else if (mode == ProjMode::Scaled) {
        // 2D at 4:3, centered; HUD elements anchored to a side move to that edge.
        const f32 s = ws::factor();
        anchor = hudAnchor();
        for (int j = 0; j < 4; j++) m[0][j] *= s;
        m[0][3] += f32(anchor) * (1.0f - s);
    } else if (mode == ProjMode::Zoom) {
        const f32 s = ws::factor();
        for (int j = 0; j < 4; j++) m[1][j] /= s;
    }
    const u32 at = mark3D();
    GXSetProjection(m, sProj.type);
    interp::note(at, interp::Kind::Projection, 0, 0, 6);
    sProjEmitted = mode;
    sProjEmittedAnchor = anchor;
    sProjDirty = false;
}

void emitScissor(bool pillar) {
    sScissorAnchor = 0;
    if (pillar) {
        sScissorAnchor = sVpFollow ? sVpAnchor : hudAnchor();
        const f32 l = std::max(0.0f, pillarX(f32(sScissor.x), sScissorAnchor));
        const f32 r = std::min(kLogicalWidth, pillarX(f32(sScissor.x + sScissor.w), sScissorAnchor));
        const u32 li = u32(std::max(0.0f, std::floor(l)));
        GXSetScissor(li, sScissor.y, u32(std::ceil(r)) - li, sScissor.h);
    } else {
        GXSetScissor(sScissor.x, sScissor.y, sScissor.w, sScissor.h);
    }
    sScissorPillar = pillar;
}

// Projection mode for a draw. `ortho2D` is what an orthographic draw into the
// full-screen viewport gets: Scaled (centered 4:3) or Plain (stretched).
ProjMode drawMode(ProjMode ortho2D) {
    if (!viewportFull()) return ProjMode::Plain;
    if (sProj.type == GX_PERSPECTIVE) return ws::adjusting() ? ProjMode::Scaled : ProjMode::Plain;
    return ws::active() ? ortho2D : ProjMode::Plain;
}

// LM_WS_TRACE=1: per frame, a summary of the widescreen decisions (draw path,
// projection type, viewport, result; consecutive repeats counted), logged
// whenever it differs from the previous frame's.
const bool sTrace = std::getenv("LM_WS_TRACE") != nullptr;
using TraceFrame = std::vector<std::pair<std::string, u32>>;
TraceFrame sTraceFrame, sTraceLast;
void traceNote(const char* fmt, ...) {
    if (!sTrace) return;
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (!sTraceFrame.empty() && sTraceFrame.back().first == buf) sTraceFrame.back().second++;
    else sTraceFrame.emplace_back(buf, 1);
}

void prepareDraw(ProjMode mode, const char* what = "draw") {
    traceNote("%s %s vp(%.0f,+%.0f) -> %s", what, sProj.type == GX_PERSPECTIVE ? "persp" : "ortho", sVp.left,
              sVp.wd, mode == ProjMode::Scaled ? "scaled" : mode == ProjMode::Zoom ? "zoom" : "plain");
    const bool hud = mode == ProjMode::Scaled && sProj.type == GX_ORTHOGRAPHIC;
    if (sProjSet && (sProjDirty || mode != sProjEmitted || (hud && hudAnchor() != sProjEmittedAnchor)))
        emitProjection(mode);
    const bool pillar = sVpFollow || (hud && (sScissor.x > 0 || sScissor.x + sScissor.w < 640));
    const int anchor = sVpFollow ? sVpAnchor : hudAnchor();
    if (pillar != sScissorPillar || (pillar && anchor != sScissorAnchor)) emitScissor(pillar);
}

// Mirror of the vertex layout state that immediate-mode 2D draws use, so a
// buffered draw's positions can be decoded (see coversScreen).
struct VatEntry {
    u8 cnt = 0, type = 0, frac = 0;
};
u8 sDesc[GX_VA_MAX_ATTR] = {};           // GXAttrType per attribute
VatEntry sVat[GX_MAX_VTXFMT][GX_VA_MAX_ATTR];
u32 sPosArray = 0, sPosStride = 0;
f32 sPosMtx[10][3][4];
bool sPosMtxValid[10] = {};
u32 sCurPosMtx = 0;

int attrSlot(u32 attr) { return attr == GX_VA_NBT ? GX_VA_NRM : attr < GX_VA_MAX_ATTR ? int(attr) : -1; }

int compSize(u8 type) { return type <= GX_S8 ? 1 : type <= GX_S16 ? 2 : 4; }

// Bytes one vertex of attribute `a` takes in the stream, or -1 if unknown.
int attrSize(int fmt, int a) {
    const u8 d = sDesc[a];
    if (d == GX_NONE) return 0;
    if (d == GX_INDEX8 || d == GX_INDEX16) {
        const int idx = d == GX_INDEX8 ? 1 : 2;
        return a == GX_VA_NRM && sVat[fmt][a].cnt == GX_NRM_NBT3 ? idx * 3 : idx;
    }
    if (a <= GX_VA_TEX7MTXIDX) return 1;
    const VatEntry& v = sVat[fmt][a];
    switch (a) {
    case GX_VA_POS: return (v.cnt == GX_POS_XYZ ? 3 : 2) * compSize(v.type);
    case GX_VA_NRM: return (v.cnt == GX_NRM_XYZ ? 3 : 9) * compSize(v.type);
    case GX_VA_CLR0:
    case GX_VA_CLR1: {
        static constexpr int kClr[] = {2, 3, 4, 2, 3, 4};
        return v.type < 6 ? kClr[v.type] : -1;
    }
    default:
        if (a >= GX_VA_TEX0 && a <= GX_VA_TEX7) return (v.cnt == GX_TEX_ST ? 2 : 1) * compSize(v.type);
        return -1;
    }
}

f32 readComp(const u8* p, u8 type, u8 frac) {
    const f32 scale = 1.0f / f32(1u << frac);
    switch (type) {
    case GX_U8: return f32(p[0]) * scale;
    case GX_S8: return f32(s8(p[0])) * scale;
    case GX_U16: return f32(u16(p[0] << 8 | p[1])) * scale;
    case GX_S16: return f32(s16(p[0] << 8 | p[1])) * scale;
    default: return std::bit_cast<f32>(u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3]);
    }
}

// Immediate-mode draws under an orthographic projection are held back until the
// draw ends, so the widescreen mode can look at where the vertices land before
// choosing the projection (see classifyHeld).
struct {
    bool active = false;
    GXPrimitive prim;
    GXVtxFmt fmt;
    u16 verts;
    std::vector<u8> bytes;    // the stream as the console's FIFO would see it (big-endian)
    std::vector<u32> writes;  // per FIFO write: byte offset | (size - 1) << 30, for exact replay
} sHeld;

// What the TEV samples, to tell a full-screen picture (a 4:3 background image)
// from a full-screen fill or a blit of an EFB copy.
std::vector<u32> sTexImage;      // guest image address (physical) per host texture object
u32 sMapImage[8] = {};           // image bound to each GXTexMapID
u8 sTevMap[16] = {};             // GXSetTevOrder map per stage (0xFF: none)
u32 sNumTevStages = 1;
std::vector<u32> sCopyDests;     // EFB copy destinations (physical), most recent last

u32 phys(u32 a) { return a & 0x01FFFFFF; }
bool isCopyDest(u32 image) { return std::find(sCopyDests.begin(), sCopyDests.end(), image) != sCopyDests.end(); }

// True if any active TEV stage samples a texture that is not an EFB copy.
bool samplesPicture() {
    for (u32 s = 0; s < sNumTevStages && s < 16; s++) {
        const u32 map = sTevMap[s];
        if (map >= 8) continue;  // GX_TEXMAP_NULL, or GX_TEX_DISABLE set
        if (sMapImage[map] && !isCopyDest(sMapImage[map])) return true;
    }
    return false;
}

f32 sHeldLo = 0, sHeldHi = 0;  // NDC x extent of the last decoded held draw
bool sHeldDecoded = false;
std::string sHeldChain;  // LM_HUD_TRACE
bool coversScreen() {
    const int fmt = sHeld.fmt;
    const size_t n = sHeld.verts;
    if (n == 0 || fmt >= GX_MAX_VTXFMT || sHeld.bytes.size() % n) return false;
    const size_t stride = sHeld.bytes.size() / n;
    int expected = 0, posOff = -1;
    for (int a = 0; a <= GX_VA_TEX7; a++) {
        const int sz = attrSize(fmt, a);
        if (sz < 0) return false;
        if (a == GX_VA_POS) posOff = expected;
        expected += sz;
    }
    if (size_t(expected) != stride || sDesc[GX_VA_POS] == GX_NONE) return false;
    const VatEntry pv = sVat[fmt][GX_VA_POS];
    const int comps = pv.cnt == GX_POS_XYZ ? 3 : 2;
    const int cs = compSize(pv.type);
    f32 lo = 1e30f, hi = -1e30f;
    for (size_t i = 0; i < n; i++) {
        const u8* v = sHeld.bytes.data() + i * stride;
        const u32 mtxId = sDesc[GX_VA_PNMTXIDX] == GX_DIRECT ? v[0] : sCurPosMtx;
        if (mtxId % 3 || mtxId / 3 >= 10 || !sPosMtxValid[mtxId / 3]) return false;
        u8 tmp[12];
        const u8* p = v + posOff;
        if (sDesc[GX_VA_POS] != GX_DIRECT) {
            const u32 idx = sDesc[GX_VA_POS] == GX_INDEX8 ? p[0] : u32(p[0] << 8 | p[1]);
            if (!sPosArray) return false;
            for (int b = 0; b < comps * cs; b++) tmp[b] = u8(rd8(sPosArray + idx * sPosStride + b));
            p = tmp;
        }
        const f32 pos[4] = {readComp(p, pv.type, pv.frac), readComp(p + cs, pv.type, pv.frac),
                            comps == 3 ? readComp(p + 2 * cs, pv.type, pv.frac) : 0.0f, 1.0f};
        const auto& m = sPosMtx[mtxId / 3];
        f32 eye[4] = {0, 0, 0, 1};
        for (int r = 0; r < 3; r++) eye[r] = m[r][0] * pos[0] + m[r][1] * pos[1] + m[r][2] * pos[2] + m[r][3];
        const f32 ndc = sProj.m[0][0] * eye[0] + sProj.m[0][1] * eye[1] + sProj.m[0][2] * eye[2] + sProj.m[0][3];
        lo = std::min(lo, ndc);
        hi = std::max(hi, ndc);
    }
    sHeldLo = lo;
    sHeldHi = hi;
    sHeldDecoded = true;
    return lo <= -0.98f && hi >= 0.98f;
}

// A held 2D draw into the full-screen viewport:
//  - covers the logical width and is a fill or samples an EFB copy (fades,
//    screen-space effects that copy the frame and draw it back): stretched,
//    so it lines up with the stretched EFB;
//  - covers the width and samples an ordinary texture (a 4:3 background
//    picture): per --ws-backgrounds, centered by default;
//  - anything else is HUD: centered 4:3 (moved to an edge with its HUD element's anchor).
ProjMode classifyHeld() {
    sHeldDecoded = false;
    if (!coversScreen()) {
        if (sHeldAnchor == ws::kAnchorBySide) sHeldAnchor = !sHeldDecoded ? 0 : sHeldLo + sHeldHi < 0.0f ? -1 : 1;
        return ProjMode::Scaled;
    }
    if (sHeldAnchor == ws::kAnchorBySide) sHeldAnchor = 0;
    traceNote("full-screen 2D, %s", samplesPicture() ? "picture" : "fill/copy");
    if (!samplesPicture()) return ProjMode::Plain;
    switch (ws::backgroundMode()) {
    case ws::Background::Stretch: return ProjMode::Plain;
    case ws::Background::Zoom: return ProjMode::Zoom;
    default: return ProjMode::Scaled;
    }
}

void rawFifoWrite(u32 v, int size) {
    switch (size) {
    case 1: GXCmd1u8(u8(v)); break;
    case 2: GXCmd1u16(u16(v)); break;
    default: GXCmd1u32(v); break;
    }
}

}  // namespace

// LM_HUD_TRACE=<first>,<last>[,<step>]: for those frames (GXCopyDisp count), every 2D draw, viewport,
// scissor and texture copy with its guest call chain, to map HUD pieces to game functions.
static u32 sDiagFrame = 0;
static bool hudTraceOn() {
    static const auto range = [] {
        u32 a = ~0u, b = 0, step = 1;
        if (const char* v = std::getenv("LM_HUD_TRACE")) std::sscanf(v, "%u,%u,%u", &a, &b, &step);
        return std::tuple<u32, u32, u32>{a, b, step ? step : 1};
    }();
    const auto [first, last, step] = range;
    return sDiagFrame >= first && sDiagFrame <= last && (sDiagFrame - first) % step == 0;
}
static std::string guestChain(const Ctx& c) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08X", c.lr);
    std::string s = buf;
    u32 sp = c.r[1];
    for (int i = 0; i < 12; i++) {
        sp = rd32(sp);
        if (sp < 0x80000000u || sp >= 0x81800000u) break;
        std::snprintf(buf, sizeof(buf), "<%08X", rd32(sp + 4));
        s += buf;
    }
    return s;
}
#define HUD_TRACE(fmt, ...) \
    do { if (hudTraceOn()) log("[hud] f%u " fmt, sDiagFrame, __VA_ARGS__); } while (0)

static FILE* dlTraceFile() {
    static FILE* f = [] { const char* p = std::getenv("LM_DL_TRACE"); return p ? std::fopen(p, "w") : nullptr; }();
    return f;
}
static u32 sDlFrame = 0;

u32 copyDispCount();
void gx::endFrame() {
    endPendingDraw();
    efbcopy::onFrame(copyDispCount());
    if (FILE* f = dlTraceFile()) std::fprintf(f, "\nF%u", sDlFrame++), std::fflush(f);
    if (!sTrace) return;
    static u32 frameNo = 0;
    if (sTraceFrame != sTraceLast) {
        std::string text;
        for (const auto& [note, n] : sTraceFrame) text += "\n  " + note + (n > 1 ? " x" + std::to_string(n) : "");
        log("[ws] frame %u (factor %.3f):%s", frameNo, ws::factor(), text.c_str());
        std::fflush(stdout);
    }
    frameNo++;
    sTraceLast = std::move(sTraceFrame);
    sTraceFrame.clear();
}

void gx::endPendingDraw() {
    if (!sInBegin) return;
    sInBegin = false;
    if (sHeld.active) {
        sHeld.active = false;
        sInHeldPrepare = true;
        const ProjMode mode = classifyHeld();
        prepareDraw(mode, "held");
        sInHeldPrepare = false;
        HUD_TRACE("held %s x[%.3f,%.3f] mode=%d anchor=%d %s", sHeldDecoded ? "dec" : "nodec", sHeldLo, sHeldHi,
                  int(mode), sHeldAnchor, sHeldChain.c_str());
        sRawCheck = true;
        GXBegin(sHeld.prim, sHeld.fmt, sHeld.verts);
        for (u32 w : sHeld.writes) {
            const int size = int(w >> 30) + 1;  // 1, 2 or 4 (encoded 0, 1, 3)
            const u32 off = w & 0x3FFFFFFF;
            u32 v = 0;
            for (int b = 0; b < size; b++) v = v << 8 | sHeld.bytes[off + b];
            rawFifoWrite(v, size);
        }
    }
    GXEnd();
}

void hw::fifoWrite(u32 v, int size) {
    if (dropDraw()) [[unlikely]] return;
    if (sHeld.active) [[unlikely]] {
        const u32 off = u32(sHeld.bytes.size());
        for (int b = size - 1; b >= 0; b--) sHeld.bytes.push_back(u8(v >> (8 * b)));
        sHeld.writes.push_back(off | u32(size - 1) << 30);
        return;
    }
    if (sRawCheck && !sInBegin) [[unlikely]] {
        sRawCheck = false;
        prepareDraw(drawMode(ProjMode::Scaled), "fifo");
    }
    rawFifoWrite(v, size);
}

#define GXHLE(name)                                       \
    static void gxImpl_##name(::recomp::Ctx& c);          \
    HLE(name) {                                           \
        gx::endPendingDraw();                             \
        gxImpl_##name(c);                                 \
    }                                                     \
    static void gxImpl_##name([[maybe_unused]] ::recomp::Ctx& c)

// --- setup / sync ---------------------------------------------------------------

GXHLE(GXInit) {
    static u32 fifoObj = 0;
    if (!fifoObj) fifoObj = hleAlloc(128);
    GXInit(hostp(c.r[3]), c.r[4]);
    c.r[3] = fifoObj;
}
GXHLE(GXFlush) { GXFlush(); }
GXHLE(GXDrawDone) { GXDrawDone(); }
GXHLE(GXSetDrawDone) {}
GXHLE(GXWaitDrawDone) {}
GXHLE(GXPixModeSync) { GXPixModeSync(); }
GXHLE(GXSetMisc) {}
GXHLE(GXInvalidateVtxCache) { GXInvalidateVtxCache(); }
GXHLE(GXInvalidateTexAll) { GXInvalidateTexAll(); }

// --- vertex format ----------------------------------------------------------------

GXHLE(GXClearVtxDesc) {
    std::memset(sDesc, 0, sizeof(sDesc));
    GXClearVtxDesc();
}
GXHLE(GXSetVtxDesc) {
    if (const int a = attrSlot(c.r[3]); a >= 0) sDesc[a] = u8(c.r[4]);
    GXSetVtxDesc(GXAttr(c.r[3]), GXAttrType(c.r[4]));
}
GXHLE(GXSetVtxDescv) {
    GXVtxDescList list[32];
    int n = 0;
    for (u32 p = c.r[3]; n < 31; p += 8) {
        list[n].attr = GXAttr(rd32(p));
        list[n].type = GXAttrType(rd32(p + 4));
        if (const int a = attrSlot(list[n].attr); a >= 0) sDesc[a] = u8(list[n].type);
        if (list[n++].attr == GX_VA_NULL) break;
    }
    list[n - 1].attr = GX_VA_NULL;
    GXSetVtxDescv(list);
}
GXHLE(GXGetVtxDesc) {
    GXAttrType t;
    GXGetVtxDesc(GXAttr(c.r[3]), &t);
    wr32(c.r[4], u32(t));
}
GXHLE(GXSetVtxAttrFmt) {
    if (const int a = attrSlot(c.r[4]); a >= 0 && c.r[3] < GX_MAX_VTXFMT)
        sVat[c.r[3]][a] = {u8(c.r[5]), u8(c.r[6]), u8(c.r[7])};
    GXSetVtxAttrFmt(GXVtxFmt(c.r[3]), GXAttr(c.r[4]), GXCompCnt(c.r[5]), GXCompType(c.r[6]), u8(c.r[7]));
}
GXHLE(GXSetVtxAttrFmtv) {
    GXVtxAttrFmtList list[32];
    int n = 0;
    for (u32 p = c.r[4]; n < 31; p += 16) {
        list[n] = {GXAttr(rd32(p)), GXCompCnt(rd32(p + 4)), GXCompType(rd32(p + 8)), u8(rd8(p + 12))};
        if (const int a = attrSlot(list[n].attr); a >= 0 && c.r[3] < GX_MAX_VTXFMT)
            sVat[c.r[3]][a] = {u8(list[n].cnt), u8(list[n].type), list[n].frac};
        if (list[n++].attr == GX_VA_NULL) break;
    }
    list[n - 1].attr = GX_VA_NULL;
    GXSetVtxAttrFmtv(GXVtxFmt(c.r[3]), list);
}
// Size 0: Aurora sizes the array from the indices each draw uses (patch 0002).
GXHLE(GXSetArray) {
    if (c.r[3] == GX_VA_POS) {
        sPosArray = c.r[4];
        sPosStride = u8(c.r[5]);
    }
    GXSetArray(GXAttr(c.r[3]), ptr(c.r[4]), 0, u8(c.r[5]), false);
}

GXHLE(GXBegin) {
    if (dropDraw()) return;
    sInBegin = true;
    if (ws::active() && sProj.type == GX_ORTHOGRAPHIC && viewportFull()) {
        sHeld.active = true;
        sHeldAnchor = ws::hudAnchor();
        sHeld.prim = GXPrimitive(c.r[3]);
        sHeld.fmt = GXVtxFmt(c.r[4]);
        sHeld.verts = u16(c.r[5]);
        if (hudTraceOn()) sHeldChain = guestChain(c);
        sHeld.bytes.clear();
        sHeld.writes.clear();
        return;
    }
    prepareDraw(drawMode(ProjMode::Scaled), "begin");
    GXBegin(GXPrimitive(c.r[3]), GXVtxFmt(c.r[4]), u16(c.r[5]));
}

GXHLE(GXCallDisplayList) {
    if (FILE* f = dlTraceFile()) {
        std::fprintf(f, " %08X:%X@%08X", c.r[3], c.r[4], c.lr);
        u32 sp = c.r[1];
        for (int i = 0; i < 10 && sp >= 0x80000000u && sp < 0x81800000u; i++) {
            sp = rd32(sp);
            if (sp < 0x80000000u || sp >= 0x81800000u) break;
            std::fprintf(f, "<%08X", rd32(sp + 4));
        }
    }
    if (dropDraw()) return;
    if (sProj.type == GX_ORTHOGRAPHIC || !viewportFull())
        HUD_TRACE("dlist %s vp(%.0f,+%.0f) %s", sProj.type == GX_ORTHOGRAPHIC ? "ortho" : "persp", sVp.left, sVp.wd,
                  guestChain(c).c_str());
    prepareDraw(drawMode(ProjMode::Scaled), "dlist");
    GXCallDisplayList(hostp(c.r[3]), c.r[4]);
}
GXHLE(GXBeginDisplayList) {
    sInDisplayList = true;
    interp::suspendForDisplayList();
    GXBeginDisplayList(hostp(c.r[3]), c.r[4]);
}
GXHLE(GXEndDisplayList) {
    sInDisplayList = false;
    c.r[3] = GXEndDisplayList();
    interp::resumeAfterDisplayList();
}

GXHLE(GXDrawSphere1) {
    if (dropDraw()) return;
    prepareDraw(drawMode(ProjMode::Scaled));
    GXDrawSphere1(u8(c.r[3]));
}
GXHLE(GXDrawCube) {
    if (dropDraw()) return;
    prepareDraw(drawMode(ProjMode::Scaled));
    GXDrawCube();
}
GXHLE(GXDrawCylinder) {
    if (dropDraw()) return;
    prepareDraw(drawMode(ProjMode::Scaled));
    GXDrawCylinder(u8(c.r[3]));
}

// --- texgen / transform ---------------------------------------------------------------

GXHLE(GXSetNumTexGens) { GXSetNumTexGens(u8(c.r[3])); }
GXHLE(GXSetTexCoordGen2) {
    GXSetTexCoordGen2(GXTexCoordID(c.r[3]), GXTexGenType(c.r[4]), GXTexGenSrc(c.r[5]), c.r[6], GXBool(c.r[7]),
                      c.r[8]);
}
// Sent to Aurora lazily, at the next draw, once the viewport and (for 2D) the
// vertices tell which widescreen treatment it gets.
GXHLE(GXSetProjection) {
    const Mtx<4, 4> m(c.r[3]);
    std::memcpy(sProj.m, m.m, sizeof(sProj.m));
    sProj.type = GXProjectionType(c.r[4]);
    sProjDirty = sProjSet = sRawCheck = true;
    traceNote("projection %s from %08X", c.r[4] ? "ortho" : "persp", c.lr);
    if (!ws::adjusting()) emitProjection(ProjMode::Plain);
}
GXHLE(GXLoadPosMtxImm) {
    const Mtx<3, 4> m(c.r[3]);
    if (const u32 id = c.r[4]; id % 3 == 0 && id / 3 < 10) {
        std::memcpy(sPosMtx[id / 3], m.m, sizeof(m.m));
        sPosMtxValid[id / 3] = true;
    }
    const u32 at = mark3D();
    GXLoadPosMtxImm(m.m, c.r[4]);
    interp::note(at, interp::Kind::PosMtx, c.r[3], c.r[4], 12);
}
GXHLE(GXLoadNrmMtxImm) {
    const Mtx<3, 4> m(c.r[3]);
    const u32 at = mark3D();
    GXLoadNrmMtxImm(m.m, c.r[4]);
    interp::note(at, interp::Kind::NrmMtx, c.r[3], c.r[4], 9);
}
GXHLE(GXLoadNrmMtxImm3x3) {
    const Mtx<3, 3> m3(c.r[3]);
    f32 m[3][4] = {};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) m[i][j] = m3.m[i][j];
    const u32 at = mark3D();
    GXLoadNrmMtxImm(m, c.r[4]);
    interp::note(at, interp::Kind::NrmMtx, c.r[3], c.r[4], 9);
}
GXHLE(GXLoadTexMtxImm) {
    const GXTexMtxType type = GXTexMtxType(c.r[5]);
    if (type == GX_MTX2x4) {
        const Mtx<2, 4> m(c.r[3]);
        const u32 at = mark3D();
        GXLoadTexMtxImm(m.m, c.r[4], type);
        interp::note(at, interp::Kind::TexMtx, c.r[3], c.r[4], sizeof(m.m) / 4);
    } else {
        const Mtx<3, 4> m(c.r[3]);
        const u32 at = mark3D();
        GXLoadTexMtxImm(m.m, c.r[4], type);
        interp::note(at, interp::Kind::TexMtx, c.r[3], c.r[4], sizeof(m.m) / 4);
    }
}
GXHLE(GXSetCurrentMtx) {
    sCurPosMtx = c.r[3];
    GXSetCurrentMtx(c.r[3]);
}
// Records the guest viewport; returns the x/width to send (moved and narrowed like the 2D
// around it for a partial viewport set by a HUD element, see sVpFollow).
static std::pair<f32, f32> setViewport(const Ctx& c) {
    sRawCheck = true;
    sVp.left = farg(c, 1);
    sVp.wd = farg(c, 3);
    sVpFollow = !viewportFull() && ws::inHudScope();
    sVpAnchor = sVpFollow ? scopeAnchor() : 0;
    HUD_TRACE("viewport %.1f %.1f %.1f %.1f%s %s", farg(c, 1), farg(c, 2), farg(c, 3), farg(c, 4),
              sVpFollow ? " follow" : "", guestChain(c).c_str());
    if (!sVpFollow) return {sVp.left, sVp.wd};
    return {pillarX(sVp.left, sVpAnchor), sVp.wd * ws::factor()};
}
GXHLE(GXSetViewport) {
    const auto [x, w] = setViewport(c);
    GXSetViewport(x, farg(c, 2), w, farg(c, 4), farg(c, 5), farg(c, 6));
}
GXHLE(GXSetViewportJitter) {
    const auto [x, w] = setViewport(c);
    GXSetViewportJitter(x, farg(c, 2), w, farg(c, 4), farg(c, 5), farg(c, 6), c.r[3]);
}
GXHLE(GXSetScissor) {
    HUD_TRACE("scissor %u %u %u %u %s", c.r[3], c.r[4], c.r[5], c.r[6], guestChain(c).c_str());
    sScissor = {c.r[3], c.r[4], c.r[5], c.r[6]};
    emitScissor(sVpFollow);
}
GXHLE(GXSetClipMode) { GXSetClipMode(GXClipMode(c.r[3])); }

// --- geometry state ------------------------------------------------------------------

GXHLE(GXSetCullMode) { GXSetCullMode(GXCullMode(c.r[3])); }
GXHLE(GXSetCoPlanar) { GXSetCoPlanar(GXBool(c.r[3])); }
GXHLE(GXSetLineWidth) { GXSetLineWidth(u8(c.r[3]), GXTexOffset(c.r[4])); }
GXHLE(GXSetPointSize) { GXSetPointSize(u8(c.r[3]), GXTexOffset(c.r[4])); }
GXHLE(GXEnableTexOffsets) { GXEnableTexOffsets(GXTexCoordID(c.r[3]), GXBool(c.r[4]), GXBool(c.r[5])); }

// --- lighting ----------------------------------------------------------------------------
// Guest GXLightObj (64 bytes) is the same size as Aurora's, so Aurora fills it in place.

static GXLightObj* light(u32 p) { return reinterpret_cast<GXLightObj*>(hostp(p)); }

GXHLE(GXSetNumChans) { GXSetNumChans(u8(c.r[3])); }
GXHLE(GXSetChanCtrl) {
    GXSetChanCtrl(GXChannelID(c.r[3]), GXBool(c.r[4]), GXColorSrc(c.r[5]), GXColorSrc(c.r[6]), c.r[7],
                  GXDiffuseFn(c.r[8]), GXAttnFn(c.r[9]));
}
GXHLE(GXSetChanMatColor) { GXSetChanMatColor(GXChannelID(c.r[3]), color(c.r[4])); }
GXHLE(GXSetChanAmbColor) { GXSetChanAmbColor(GXChannelID(c.r[3]), color(c.r[4])); }
GXHLE(GXInitLightPos) { GXInitLightPos(light(c.r[3]), farg(c, 1), farg(c, 2), farg(c, 3)); }
GXHLE(GXInitLightDir) { GXInitLightDir(light(c.r[3]), farg(c, 1), farg(c, 2), farg(c, 3)); }
GXHLE(GXInitSpecularDir) { GXInitSpecularDir(light(c.r[3]), farg(c, 1), farg(c, 2), farg(c, 3)); }
GXHLE(GXInitLightColor) { GXInitLightColor(light(c.r[3]), color(c.r[4])); }
GXHLE(GXInitLightSpot) { GXInitLightSpot(light(c.r[3]), farg(c, 1), GXSpotFn(c.r[4])); }
GXHLE(GXInitLightDistAttn) {
    GXInitLightDistAttn(light(c.r[3]), farg(c, 1), farg(c, 2), GXDistAttnFn(c.r[4]));
}
GXHLE(GXInitLightAttn) {
    GXInitLightAttn(light(c.r[3]), farg(c, 1), farg(c, 2), farg(c, 3), farg(c, 4), farg(c, 5), farg(c, 6));
}
GXHLE(GXLoadLightObjImm) { GXLoadLightObjImm(light(c.r[3]), GXLightID(c.r[4])); }

// --- textures ------------------------------------------------------------------------------

void diagTexInit(const char* kind, u32 obj, u32 image, u32 w, u32 h, u32 fmt, u32 lr);
static void noteTexImage(u32 obj, u32 image) {
    const u32 idx = rd32(obj + 4);
    if (idx >= sTexImage.size()) sTexImage.resize(idx + 1);
    sTexImage[idx] = phys(image);
}
GXHLE(GXInitTexObj) {
    GXInitTexObj(sTex.forInit(c.r[3], kTexTag), ptr(c.r[4]), u16(c.r[5]), u16(c.r[6]), GXTexFmt(c.r[7]),
                 GXTexWrapMode(c.r[8]), GXTexWrapMode(c.r[9]), GXBool(c.r[10]));
    noteTexImage(c.r[3], c.r[4]);
    diagTexInit("INIT", c.r[3], c.r[4], c.r[5], c.r[6], c.r[7], c.lr);
}
GXHLE(GXInitTexObjCI) {
    GXInitTexObjCI(sTex.forInit(c.r[3], kTexTag), ptr(c.r[4]), u16(c.r[5]), u16(c.r[6]), GXCITexFmt(c.r[7]),
                   GXTexWrapMode(c.r[8]), GXTexWrapMode(c.r[9]), GXBool(c.r[10]), stackArg(c, 0));
    noteTexImage(c.r[3], c.r[4]);
    diagTexInit("INITCI", c.r[3], c.r[4], c.r[5], c.r[6], c.r[7], c.lr);
}
GXHLE(GXInitTexObjLOD) {
    if (GXTexObj* o = sTex.get(c.r[3], kTexTag))
        GXInitTexObjLOD(o, GXTexFilter(c.r[4]), GXTexFilter(c.r[5]), farg(c, 1), farg(c, 2), farg(c, 3),
                        GXBool(c.r[6]), GXBool(c.r[7]), GXAnisotropy(c.r[8]));
}
GXHLE(GXLoadTexObj) {
    if (GXTexObj* o = sTex.get(c.r[3], kTexTag)) {
        GXLoadTexObj(o, GXTexMapID(c.r[4]));
        const u32 idx = rd32(c.r[3] + 4);
        if (c.r[4] < 8) sMapImage[c.r[4]] = idx < sTexImage.size() ? sTexImage[idx] : 0;
    } else {
        log("[gx] GXLoadTexObj: unknown texture object %08X", c.r[3]);
    }
}
GXHLE(GXGetTexBufferSize) {
    c.r[3] = GXGetTexBufferSize(u16(c.r[3]), u16(c.r[4]), c.r[5], GXBool(c.r[6]), u8(c.r[7]));
}
GXHLE(GXInitTexCacheRegion) {}
GXHLE(GXInitTlutObj) {
    GXInitTlutObj(sTlut.forInit(c.r[3], kTlutTag), ptr(c.r[4]), GXTlutFmt(c.r[5]), u16(c.r[6]));
}
GXHLE(GXLoadTlut) {
    if (GXTlutObj* o = sTlut.get(c.r[3], kTlutTag)) GXLoadTlut(o, c.r[4]);
}

// --- TEV -------------------------------------------------------------------------------------

GXHLE(GXSetNumTevStages) {
    sNumTevStages = u8(c.r[3]);
    GXSetNumTevStages(u8(c.r[3]));
}
// Model viewer "Textures off": texture colour reads as white and texture blend modes pass
// the lit vertex colour through, so models show lighting/vertex colours only. Alpha stays
// textured so cut-out edges survive.
static bool sUntextured = false;
void gx::setUntextured(bool on) { sUntextured = on; }
static GXTevColorArg untex(u32 a) {
    return GXTevColorArg(sUntextured && (a == GX_CC_TEXC || a == GX_CC_TEXA) ? GX_CC_ONE : a);
}

GXHLE(GXSetTevOp) {
    u32 mode = c.r[4];
    if (sUntextured && (mode == GX_MODULATE || mode == GX_DECAL || mode == GX_REPLACE)) mode = GX_PASSCLR;
    GXSetTevOp(GXTevStageID(c.r[3]), GXTevMode(mode));
}
GXHLE(GXSetTevOrder) {
    if (c.r[3] < 16) sTevMap[c.r[3]] = c.r[5] & 0x100 ? 0xFF : u8(c.r[5]);
    GXSetTevOrder(GXTevStageID(c.r[3]), GXTexCoordID(c.r[4]), GXTexMapID(c.r[5]), GXChannelID(c.r[6]));
}
GXHLE(GXSetTevColorIn) {
    GXSetTevColorIn(GXTevStageID(c.r[3]), untex(c.r[4]), untex(c.r[5]), untex(c.r[6]), untex(c.r[7]));
}
GXHLE(GXSetTevAlphaIn) {
    GXSetTevAlphaIn(GXTevStageID(c.r[3]), GXTevAlphaArg(c.r[4]), GXTevAlphaArg(c.r[5]), GXTevAlphaArg(c.r[6]),
                    GXTevAlphaArg(c.r[7]));
}
GXHLE(GXSetTevColorOp) {
    GXSetTevColorOp(GXTevStageID(c.r[3]), GXTevOp(c.r[4]), GXTevBias(c.r[5]), GXTevScale(c.r[6]), GXBool(c.r[7]),
                    GXTevRegID(c.r[8]));
}
GXHLE(GXSetTevAlphaOp) {
    GXSetTevAlphaOp(GXTevStageID(c.r[3]), GXTevOp(c.r[4]), GXTevBias(c.r[5]), GXTevScale(c.r[6]), GXBool(c.r[7]),
                    GXTevRegID(c.r[8]));
}
GXHLE(GXSetTevColor) { GXSetTevColor(GXTevRegID(c.r[3]), color(c.r[4])); }
GXHLE(GXSetTevColorS10) {
    const u32 p = c.r[4];
    GXSetTevColorS10(GXTevRegID(c.r[3]), GXColorS10{s16(rd16(p)), s16(rd16(p + 2)), s16(rd16(p + 4)), s16(rd16(p + 6))});
}
GXHLE(GXSetTevKColor) { GXSetTevKColor(GXTevKColorID(c.r[3]), color(c.r[4])); }
GXHLE(GXSetTevKColorSel) { GXSetTevKColorSel(GXTevStageID(c.r[3]), GXTevKColorSel(c.r[4])); }
GXHLE(GXSetTevKAlphaSel) { GXSetTevKAlphaSel(GXTevStageID(c.r[3]), GXTevKAlphaSel(c.r[4])); }
GXHLE(GXSetTevSwapMode) { GXSetTevSwapMode(GXTevStageID(c.r[3]), GXTevSwapSel(c.r[4]), GXTevSwapSel(c.r[5])); }
GXHLE(GXSetTevSwapModeTable) {
    GXSetTevSwapModeTable(GXTevSwapSel(c.r[3]), GXTevColorChan(c.r[4]), GXTevColorChan(c.r[5]),
                          GXTevColorChan(c.r[6]), GXTevColorChan(c.r[7]));
}
GXHLE(GXSetAlphaCompare) {
    GXSetAlphaCompare(GXCompare(c.r[3]), u8(c.r[4]), GXAlphaOp(c.r[5]), GXCompare(c.r[6]), u8(c.r[7]));
}
GXHLE(GXSetZTexture) { GXSetZTexture(GXZTexOp(c.r[3]), GXTexFmt(c.r[4]), c.r[5]); }

// --- indirect -----------------------------------------------------------------------------------

GXHLE(GXSetNumIndStages) { GXSetNumIndStages(u8(c.r[3])); }
GXHLE(GXSetIndTexOrder) {
    GXSetIndTexOrder(GXIndTexStageID(c.r[3]), GXTexCoordID(c.r[4]), GXTexMapID(c.r[5]));
}
GXHLE(GXSetIndTexCoordScale) {
    GXSetIndTexCoordScale(GXIndTexStageID(c.r[3]), GXIndTexScale(c.r[4]), GXIndTexScale(c.r[5]));
}
GXHLE(GXSetIndTexMtx) {
    const Mtx<2, 3> m(c.r[4]);
    GXSetIndTexMtx(GXIndTexMtxID(c.r[3]), m.m, s8(c.r[5]));
}
GXHLE(GXSetTevDirect) { GXSetTevDirect(GXTevStageID(c.r[3])); }
GXHLE(GXSetTevIndWarp) {
    GXSetTevIndWarp(GXTevStageID(c.r[3]), GXIndTexStageID(c.r[4]), GXBool(c.r[5]), GXBool(c.r[6]),
                    GXIndTexMtxID(c.r[7]));
}
GXHLE(GXSetTevIndirect) {
    GXSetTevIndirect(GXTevStageID(c.r[3]), GXIndTexStageID(c.r[4]), GXIndTexFormat(c.r[5]),
                     GXIndTexBiasSel(c.r[6]), GXIndTexMtxID(c.r[7]), GXIndTexWrap(c.r[8]), GXIndTexWrap(c.r[9]),
                     GXBool(c.r[10]), GXBool(stackArg(c, 0)), GXIndTexAlphaSel(stackArg(c, 1)));
}

// --- pixel engine ---------------------------------------------------------------------------------

GXHLE(GXSetColorUpdate) { GXSetColorUpdate(GXBool(c.r[3])); }
GXHLE(GXSetAlphaUpdate) { GXSetAlphaUpdate(GXBool(c.r[3])); }
GXHLE(GXSetZMode) { GXSetZMode(GXBool(c.r[3]), GXCompare(c.r[4]), GXBool(c.r[5])); }
GXHLE(GXSetPixelFmt) { GXSetPixelFmt(GXPixelFmt(c.r[3]), GXZFmt16(c.r[4])); }
GXHLE(GXSetDither) { GXSetDither(GXBool(c.r[3])); }
GXHLE(GXSetZCompLoc) { GXSetZCompLoc(GXBool(c.r[3])); }
GXHLE(GXSetBlendMode) {
    GXSetBlendMode(GXBlendMode(c.r[3]), GXBlendFactor(c.r[4]), GXBlendFactor(c.r[5]), GXLogicOp(c.r[6]));
}
GXHLE(GXSetDstAlpha) { GXSetDstAlpha(GXBool(c.r[3]), u8(c.r[4])); }
GXHLE(GXSetFog) {
    GXSetFog(GXFogType(c.r[3]), farg(c, 1), farg(c, 2), farg(c, 3), farg(c, 4), color(c.r[4]));
}
GXHLE(GXSetFogRangeAdj) {
    GXFogAdjTable t{};
    if (c.r[5])
        for (int i = 0; i < 10; i++) t.r[i] = u16(rd16(c.r[5] + 2 * i));
    GXSetFogRangeAdj(GXBool(c.r[3]), u16(c.r[4]), &t);
}
GXHLE(GXInitFogAdjTable) {
    const u32 table = c.r[3];
    const u16 width = u16(c.r[4]);
    const Mtx<4, 4> p(c.r[5]);
    f32 nearZ, sideX;
    if (p.m[3][3] == 0.0f) {
        nearZ = p.m[2][3] / (p.m[2][2] - 1.0f);
        sideX = nearZ / p.m[0][0];
    } else {
        sideX = 1.0f / p.m[0][0];
        nearZ = 1.73205f * sideX;
    }
    const f32 iw = 2.0f / width;
    for (u32 i = 0; i < 10; i++) {
        const f32 xi = f32((i + 1) << 5) * iw * sideX;
        const f32 range = std::sqrt(1.0f + (xi * xi) / (nearZ * nearZ));
        wr16(table + 2 * i, u32(range * 256.0f) & 0xFFF);
    }
}

// --- copies ---------------------------------------------------------------------------------------

// Current texture-copy setup (LM_COPY_LOG and the LM_FRAME_DUMP restore), copies by frame for the log.
static struct { u32 x, y, w, h, dw, dh, fmt, mip; } sCopyDiag;
static const bool sCopyLog = std::getenv("LM_COPY_LOG") != nullptr;
u32 copyDispCount() { return sDiagFrame; }
struct DiagCopy { u32 dest, size, fmt, w, h, frame; };
static std::vector<DiagCopy> sDiagCopies;
void diagTexInit(const char* kind, u32 obj, u32 image, u32 w, u32 h, u32 fmt, u32 lr) {
    if (!sCopyLog) return;
    const u32 im = phys(image);
    const u32 sz = GXGetTexBufferSize(u16(w), u16(h), fmt, GX_FALSE, 0);
    for (const auto& d : sDiagCopies) {
        if (im < d.dest + d.size && d.dest < im + sz) {
            log("[copydiag] f%u %s obj=%08X img=%08X %ux%u fmt=%u size=%X lr=%08X  overlaps copy %08X %ux%u fmt=%u (f%u)%s",
                sDiagFrame, kind, obj, image, w, h, fmt, sz, lr, d.dest, d.w, d.h, d.fmt, d.frame,
                (im == d.dest && w == d.w && h == d.h && fmt == d.fmt) ? " [same]" : " [DIFFERENT]");
        }
    }
}
GXHLE(GXSetCopyClear) { GXSetCopyClear(color(c.r[3]), c.r[4]); }
GXHLE(GXSetDispCopySrc) { GXSetDispCopySrc(u16(c.r[3]), u16(c.r[4]), u16(c.r[5]), u16(c.r[6])); }
GXHLE(GXSetDispCopyDst) { GXSetDispCopyDst(u16(c.r[3]), u16(c.r[4])); }
GXHLE(GXSetDispCopyYScale) { c.r[3] = GXSetDispCopyYScale(farg(c, 1)); }
GXHLE(GXSetDispCopyGamma) { GXSetDispCopyGamma(GXGamma(c.r[3])); }
GXHLE(GXSetCopyClamp) {}
GXHLE(GXSetCopyFilter) {
    u8 pattern[12][2] = {};
    u8 vfilter[7] = {};
    if (c.r[4]) std::memcpy(pattern, hostp(c.r[4]), sizeof(pattern));
    if (c.r[6]) std::memcpy(vfilter, hostp(c.r[6]), sizeof(vfilter));
    GXSetCopyFilter(GXBool(c.r[3]), pattern, GXBool(c.r[5]), vfilter);
}
GXHLE(GXCopyDisp) {
    sDiagFrame++;
    if (void* dump = dropDraw() ? nullptr : efbcopy::dumpTarget(sDiagFrame)) {
        // LM_FRAME_DUMP: copy the finished EFB through the copy-to-RAM path, then restore
        // the game's texture-copy setup.
        GXSetTexCopySrc(0, 0, 640, 480);
        GXSetTexCopyDst(640, 480, GX_TF_RGBA8, GX_FALSE);
        GXCopyTex(dump, GX_FALSE);
        GXSetTexCopySrc(u16(sCopyDiag.x), u16(sCopyDiag.y), u16(sCopyDiag.w), u16(sCopyDiag.h));
        GXSetTexCopyDst(u16(sCopyDiag.dw), u16(sCopyDiag.dh), GXTexFmt(sCopyDiag.fmt), GXBool(sCopyDiag.mip));
    }
    if (!dropDraw()) GXCopyDisp(hostp(c.r[3]), GXBool(c.r[4]));
    if (frame::presentOnCopy()) frame::present(c, c.r[3], frame::viBlack());  // PAL release (frame.h)
}
GXHLE(GXSetTexCopySrc) {
    sCopyDiag.x = c.r[3]; sCopyDiag.y = c.r[4]; sCopyDiag.w = c.r[5]; sCopyDiag.h = c.r[6];
    traceNote("copy src (%u,%u %ux%u)", c.r[3], c.r[4], c.r[5], c.r[6]);
    GXSetTexCopySrc(u16(c.r[3]), u16(c.r[4]), u16(c.r[5]), u16(c.r[6]));
}
GXHLE(GXSetTexCopyDst) {
    sCopyDiag.dw = c.r[3]; sCopyDiag.dh = c.r[4]; sCopyDiag.fmt = c.r[5]; sCopyDiag.mip = c.r[6];
    GXSetTexCopyDst(u16(c.r[3]), u16(c.r[4]), GXTexFmt(c.r[5]), GXBool(c.r[6]));
}
GXHLE(GXCopyTex) {
    HUD_TRACE("copytex src(%u,%u %ux%u) dst %ux%u fmt %u -> %08X %s", sCopyDiag.x, sCopyDiag.y, sCopyDiag.w, sCopyDiag.h,
              sCopyDiag.dw, sCopyDiag.dh, sCopyDiag.fmt, c.r[3], guestChain(c).c_str());
    if (sCopyLog) {
        const u32 sz = GXGetTexBufferSize(u16(sCopyDiag.dw), u16(sCopyDiag.dh), sCopyDiag.fmt, GX_FALSE, 0);
        log("[copydiag] f%u COPY dest=%08X src=(%u,%u %ux%u) dst=%ux%u fmt=%u mip=%u clear=%u size=%X lr=%08X%s",
            sDiagFrame, c.r[3], sCopyDiag.x, sCopyDiag.y, sCopyDiag.w, sCopyDiag.h, sCopyDiag.dw, sCopyDiag.dh,
            sCopyDiag.fmt, sCopyDiag.mip, c.r[4], sz, c.lr, dropDraw() ? " DROPPED" : "");
        bool found = false;
        for (auto& d : sDiagCopies)
            if (d.dest == phys(c.r[3])) { d = {phys(c.r[3]), sz, sCopyDiag.fmt, sCopyDiag.dw, sCopyDiag.dh, sDiagFrame}; found = true; }
        if (!found) sDiagCopies.push_back({phys(c.r[3]), sz, sCopyDiag.fmt, sCopyDiag.dw, sCopyDiag.dh, sDiagFrame});
    }
    if (dropDraw()) return;
    if (!isCopyDest(phys(c.r[3]))) {
        if (sCopyDests.size() >= 64) sCopyDests.erase(sCopyDests.begin());
        sCopyDests.push_back(phys(c.r[3]));
    }
    GXCopyTex(hostp(c.r[3]), GXBool(c.r[4]));
}

}  // namespace recomp
