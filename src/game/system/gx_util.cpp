#include "game/system/gx_util.h"
#include "game/system/camera.h"
#include "game/system/display.h"

#include "port/dol.h"

#include <array>

namespace lm {

Mtx gIdentityMtx = {
    {1.0f, 0.0f, 0.0f, 0.0f},
    {0.0f, 1.0f, 0.0f, 0.0f},
    {0.0f, 0.0f, 1.0f, 0.0f},
};

static constexpr u32 kSinTableAddr = 0x80218238;  // lbl_80218238 (followed by lbl_80219238)
static constexpr int kSinTableEntries = 1024 + 4096;
static std::array<f32, kSinTableEntries> sSinTable;

void loadTrigTables() {
    for (int i = 0; i < kSinTableEntries; i++) {
        sSinTable[i] = port::dol::bef32(kSinTableAddr + i * 4);
    }
}

f32 sinIdx(u16 angle) { return sSinTable[angle >> 4]; }
f32 cosIdx(u16 angle) { return sSinTable[1024 + (angle >> 4)]; }

void makeSRTMtx(f32 sx, f32 sy, f32 sz, f32 tx, f32 ty, f32 tz, Mtx dst, s32 rx, s32 ry, u32 rz) {
    const f32 sinX = sinIdx(u16(rx)), cosX = cosIdx(u16(rx));
    const f32 sinY = sinIdx(u16(ry)), cosY = cosIdx(u16(ry));
    const f32 sinZ = sinIdx(u16(rz)), cosZ = cosIdx(u16(rz));

    const f32 cxcz = cosX * cosZ;
    const f32 sxcz = sinX * cosZ;
    const f32 sxsz = sinX * sinZ;
    const f32 cxsz = cosX * sinZ;

    dst[0][0] = sx * (cosY * cosZ);
    dst[0][1] = sy * (sinY * sxcz - cxsz);
    dst[0][2] = sz * (sinY * cxcz + sxsz);
    dst[0][3] = tx;
    dst[1][0] = sx * (cosY * sinZ);
    dst[1][1] = sy * (cxcz + sinY * sxsz);
    dst[1][2] = sz * (sinY * cxsz - sxcz);
    dst[1][3] = ty;
    dst[2][0] = -sinY * sx;
    dst[2][1] = sy * (cosY * sinX);
    dst[2][2] = sz * (cosY * cosX);
    dst[2][3] = tz;
}

void resetGXState() {
    static constexpr GXColor kBlack = {0, 0, 0, 255};   // lbl_804CFF88
    static constexpr GXColor kWhite = {255, 255, 255, 255};  // lbl_804CFF84

    setViewportJitter(0.0f, 0.0f, 640.0f, 480.0f, 0.0f, 1.0f);
    GXSetScissor(0, 0, 640, 480);
    GXSetCurrentMtx(GX_PNMTX0);
    GXSetCullMode(GX_CULL_BACK);

    GXLoadPosMtxImm(gCamera.viewMtx, GX_PNMTX0);
    for (u32 i = GX_PNMTX1; i <= GX_PNMTX9; i += 3) {
        GXLoadPosMtxImm(gIdentityMtx, i);
    }
    GXLoadNrmMtxImm(gCamera.viewMtx, GX_PNMTX0);
    for (u32 i = GX_PNMTX1; i <= GX_PNMTX9; i += 3) {
        GXLoadNrmMtxImm(gIdentityMtx, i);
    }

    GXSetNumChans(1);
    for (GXChannelID chan : {GX_COLOR0A0, GX_COLOR1A1}) {
        GXSetChanAmbColor(chan, kBlack);
        GXSetChanMatColor(chan, kWhite);
        GXSetChanCtrl(chan, GX_FALSE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_CLAMP, GX_AF_NONE);
    }

    for (u32 i = GX_TEXMTX0; i <= GX_IDENTITY; i += 3) {
        GXLoadTexMtxImm(gIdentityMtx, i, GX_MTX3x4);
    }
    GXSetNumTexGens(1);
    GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY, GX_FALSE, GX_PTIDENTITY);
    GXEnableTexOffsets(GX_TEXCOORD0, GX_FALSE, GX_FALSE);

    GXSetNumTevStages(1);
    for (int stage = GX_TEVSTAGE0; stage <= GX_TEVSTAGE15; stage++) {
        GXSetTevOrder(GXTevStageID(stage), GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR_NULL);
    }
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevSwapMode(GX_TEVSTAGE0, GX_TEV_SWAP0, GX_TEV_SWAP0);
    GXSetTevSwapMode(GX_TEVSTAGE1, GX_TEV_SWAP0, GX_TEV_SWAP0);
    GXSetTevKColor(GX_KCOLOR0, kWhite);
    GXSetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_K0_A);
    GXSetTevKAlphaSel(GX_TEVSTAGE1, GX_TEV_KASEL_K0_A);

    GXSetZMode(GX_TRUE, GX_LESS, GX_TRUE);
    GXSetColorUpdate(GX_TRUE);
    GXSetDstAlpha(GX_FALSE, 0xFF);
    GXSetAlphaUpdate(GX_TRUE);
    GXSetAlphaCompare(GX_GREATER, 0, GX_AOP_AND, GX_GREATER, 0);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_NOOP);
    GXSetZCompLoc(GX_FALSE);
    GXSetDither(GX_TRUE);
}

}  // namespace lm
