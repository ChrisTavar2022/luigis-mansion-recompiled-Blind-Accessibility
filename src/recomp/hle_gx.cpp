// HLE: GX, forwarded to Aurora. Guest arguments are converted to host form
// (big-endian matrices and structs read out of guest memory; guest pointers to
// textures, arrays and display lists translated to host pointers into MEM1).
//
// Raw FIFO writes from game code (vertex data after GXBegin, inlined register
// writes) arrive through the write-gather pipe and are appended to Aurora's
// FIFO unchanged. The console needs no GXEnd; Aurora does, so an open draw is
// closed before the next GX call.
#include "recomp/gx_bridge.h"
#include "recomp/hle.h"
#include "recomp/runtime.h"

#include <dolphin/gx.h>

#include <cmath>
#include <deque>
#include <mutex>

namespace recomp {
namespace {

bool sInBegin = false;

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

}  // namespace

void gx::endPendingDraw() {
    if (sInBegin) {
        sInBegin = false;
        GXEnd();
    }
}

void hw::fifoWrite(u32 v, int size) {
    switch (size) {
    case 1: GXCmd1u8(u8(v)); break;
    case 2: GXCmd1u16(u16(v)); break;
    default: GXCmd1u32(v); break;
    }
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

GXHLE(GXClearVtxDesc) { GXClearVtxDesc(); }
GXHLE(GXSetVtxDesc) { GXSetVtxDesc(GXAttr(c.r[3]), GXAttrType(c.r[4])); }
GXHLE(GXSetVtxDescv) {
    GXVtxDescList list[32];
    int n = 0;
    for (u32 p = c.r[3]; n < 31; p += 8) {
        list[n].attr = GXAttr(rd32(p));
        list[n].type = GXAttrType(rd32(p + 4));
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
    GXSetVtxAttrFmt(GXVtxFmt(c.r[3]), GXAttr(c.r[4]), GXCompCnt(c.r[5]), GXCompType(c.r[6]), u8(c.r[7]));
}
GXHLE(GXSetVtxAttrFmtv) {
    GXVtxAttrFmtList list[32];
    int n = 0;
    for (u32 p = c.r[4]; n < 31; p += 16) {
        list[n] = {GXAttr(rd32(p)), GXCompCnt(rd32(p + 4)), GXCompType(rd32(p + 8)), u8(rd8(p + 12))};
        if (list[n++].attr == GX_VA_NULL) break;
    }
    list[n - 1].attr = GX_VA_NULL;
    GXSetVtxAttrFmtv(GXVtxFmt(c.r[3]), list);
}
// Size 0: Aurora sizes the array from the indices each draw uses (patch 0002).
GXHLE(GXSetArray) { GXSetArray(GXAttr(c.r[3]), ptr(c.r[4]), 0, u8(c.r[5]), false); }

GXHLE(GXBegin) {
    GXBegin(GXPrimitive(c.r[3]), GXVtxFmt(c.r[4]), u16(c.r[5]));
    sInBegin = true;
}

GXHLE(GXCallDisplayList) { GXCallDisplayList(hostp(c.r[3]), c.r[4]); }
GXHLE(GXBeginDisplayList) { GXBeginDisplayList(hostp(c.r[3]), c.r[4]); }
GXHLE(GXEndDisplayList) { c.r[3] = GXEndDisplayList(); }

GXHLE(GXDrawSphere1) { GXDrawSphere1(u8(c.r[3])); }
GXHLE(GXDrawCube) { GXDrawCube(); }
GXHLE(GXDrawCylinder) { GXDrawCylinder(u8(c.r[3])); }

// --- texgen / transform ---------------------------------------------------------------

GXHLE(GXSetNumTexGens) { GXSetNumTexGens(u8(c.r[3])); }
GXHLE(GXSetTexCoordGen2) {
    GXSetTexCoordGen2(GXTexCoordID(c.r[3]), GXTexGenType(c.r[4]), GXTexGenSrc(c.r[5]), c.r[6], GXBool(c.r[7]),
                      c.r[8]);
}
GXHLE(GXSetProjection) {
    const Mtx<4, 4> m(c.r[3]);
    GXSetProjection(m.m, GXProjectionType(c.r[4]));
}
GXHLE(GXLoadPosMtxImm) {
    const Mtx<3, 4> m(c.r[3]);
    GXLoadPosMtxImm(m.m, c.r[4]);
}
GXHLE(GXLoadNrmMtxImm) {
    const Mtx<3, 4> m(c.r[3]);
    GXLoadNrmMtxImm(m.m, c.r[4]);
}
GXHLE(GXLoadNrmMtxImm3x3) {
    const Mtx<3, 3> m3(c.r[3]);
    f32 m[3][4] = {};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) m[i][j] = m3.m[i][j];
    GXLoadNrmMtxImm(m, c.r[4]);
}
GXHLE(GXLoadTexMtxImm) {
    const GXTexMtxType type = GXTexMtxType(c.r[5]);
    if (type == GX_MTX2x4) {
        const Mtx<2, 4> m(c.r[3]);
        GXLoadTexMtxImm(m.m, c.r[4], type);
    } else {
        const Mtx<3, 4> m(c.r[3]);
        GXLoadTexMtxImm(m.m, c.r[4], type);
    }
}
GXHLE(GXSetCurrentMtx) { GXSetCurrentMtx(c.r[3]); }
GXHLE(GXSetViewport) { GXSetViewport(farg(c, 1), farg(c, 2), farg(c, 3), farg(c, 4), farg(c, 5), farg(c, 6)); }
GXHLE(GXSetViewportJitter) {
    GXSetViewportJitter(farg(c, 1), farg(c, 2), farg(c, 3), farg(c, 4), farg(c, 5), farg(c, 6), c.r[3]);
}
GXHLE(GXSetScissor) { GXSetScissor(c.r[3], c.r[4], c.r[5], c.r[6]); }
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

GXHLE(GXInitTexObj) {
    GXInitTexObj(sTex.forInit(c.r[3], kTexTag), ptr(c.r[4]), u16(c.r[5]), u16(c.r[6]), GXTexFmt(c.r[7]),
                 GXTexWrapMode(c.r[8]), GXTexWrapMode(c.r[9]), GXBool(c.r[10]));
}
GXHLE(GXInitTexObjCI) {
    GXInitTexObjCI(sTex.forInit(c.r[3], kTexTag), ptr(c.r[4]), u16(c.r[5]), u16(c.r[6]), GXCITexFmt(c.r[7]),
                   GXTexWrapMode(c.r[8]), GXTexWrapMode(c.r[9]), GXBool(c.r[10]), stackArg(c, 0));
}
GXHLE(GXInitTexObjLOD) {
    if (GXTexObj* o = sTex.get(c.r[3], kTexTag))
        GXInitTexObjLOD(o, GXTexFilter(c.r[4]), GXTexFilter(c.r[5]), farg(c, 1), farg(c, 2), farg(c, 3),
                        GXBool(c.r[6]), GXBool(c.r[7]), GXAnisotropy(c.r[8]));
}
GXHLE(GXLoadTexObj) {
    if (GXTexObj* o = sTex.get(c.r[3], kTexTag)) GXLoadTexObj(o, GXTexMapID(c.r[4]));
    else log("[gx] GXLoadTexObj: unknown texture object %08X", c.r[3]);
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

GXHLE(GXSetNumTevStages) { GXSetNumTevStages(u8(c.r[3])); }
GXHLE(GXSetTevOp) { GXSetTevOp(GXTevStageID(c.r[3]), GXTevMode(c.r[4])); }
GXHLE(GXSetTevOrder) {
    GXSetTevOrder(GXTevStageID(c.r[3]), GXTexCoordID(c.r[4]), GXTexMapID(c.r[5]), GXChannelID(c.r[6]));
}
GXHLE(GXSetTevColorIn) {
    GXSetTevColorIn(GXTevStageID(c.r[3]), GXTevColorArg(c.r[4]), GXTevColorArg(c.r[5]), GXTevColorArg(c.r[6]),
                    GXTevColorArg(c.r[7]));
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
GXHLE(GXCopyDisp) { GXCopyDisp(hostp(c.r[3]), GXBool(c.r[4])); }
GXHLE(GXSetTexCopySrc) { GXSetTexCopySrc(u16(c.r[3]), u16(c.r[4]), u16(c.r[5]), u16(c.r[6])); }
GXHLE(GXSetTexCopyDst) { GXSetTexCopyDst(u16(c.r[3]), u16(c.r[4]), GXTexFmt(c.r[5]), GXBool(c.r[6])); }
GXHLE(GXCopyTex) { GXCopyTex(hostp(c.r[3]), GXBool(c.r[4])); }

}  // namespace recomp
