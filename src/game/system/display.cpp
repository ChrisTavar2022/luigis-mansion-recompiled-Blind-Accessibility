// Port of Unsorted/LMDisplayUtil. Based on lm-decomp's version of this TU, plus
// the wipe state machine (fn_800080DC..fn_800083B4) ported from the retail binary.
#include "game/system/display.h"
#include "game/system/gx_util.h"
#include "game/system/heap.h"
#include "game/system/pad.h"
#include "game/unported.h"

#include <dolphin/pad.h>
#include <dolphin/gx.h>
#include <dolphin/os.h>
#include <dolphin/vi.h>

#include <JSystem/JKernel/JKRExpHeap.h>
#include <JSystem/JUtility/JUTGamePad.h>

namespace lm {
namespace {

struct VIMessageQueue {
    u32 lastRetrace;  // retrace count when the last frame was released
    u32 interval;     // retraces per game frame
    OSMessageQueue queue;
    OSMessage msgs[1];
};

struct VISettings {
    u16 flags;
    bool doubleBuffered;
    u8 bufferIndex;
};

VIMessageQueue sVIQueue;
GXRenderModeObj sIntRenderMode;
GXRenderModeObj sProgRenderMode = {
    VI_TVMODE_NTSC_PROG, 640, 480, 480, 40, 0, 640, 480, VI_XFBMODE_SF, GX_FALSE, GX_FALSE,
    {{6, 6}, {6, 6}, {6, 6}, {6, 6}, {6, 6}, {6, 6}, {6, 6}, {6, 6}, {6, 6}, {6, 6}, {6, 6}, {6, 6}},
    {0, 0, 26, 12, 26, 0, 0},
};
GXRenderModeObj* sRenderModes[2] = {&sIntRenderMode, &sProgRenderMode};

u8 sRenderMode;
VISettings sVISettings;
void* sFrameBuffers[2];
void* sMainFrameBuffer;
void* sPrevMainFrameBuffer;
GXRenderModeObj* sCurRenderMode;
s32 sProgressiveState;
s32 sProgressiveMode;

constexpr u32 kFrameBufferSize = 0x96000;  // 640x480 YUY2

void postRetraceCallback(u32 retraceCount) {
    if (retraceCount - sVIQueue.lastRetrace >= sVIQueue.interval) {
        OSSendMessage(&sVIQueue.queue, reinterpret_cast<OSMessage>(uintptr_t(0x444F4E45)), OS_MESSAGE_NOBLOCK);
    }
}

void resetRenderDisplay() {
    GXSetCopyClear(GXColor{0, 0, 0, 0}, GX_MAX_Z24);
    GXSetColorUpdate(GX_TRUE);
    GXSetAlphaUpdate(GX_TRUE);
    GXSetZMode(GX_TRUE, GX_LESS, GX_TRUE);
    GXSetDispCopySrc(0, 0, sCurRenderMode->fbWidth, sCurRenderMode->efbHeight);
    GXSetDispCopyDst(sCurRenderMode->fbWidth, sCurRenderMode->efbHeight);
    GXSetDispCopyYScale(f32(sCurRenderMode->xfbHeight) / f32(sCurRenderMode->efbHeight));
    GXSetCopyFilter(sCurRenderMode->aa, sCurRenderMode->sample_pattern, GX_TRUE, sCurRenderMode->vfilter);
    GXSetCopyClamp(GXFBClamp(GX_CLAMP_TOP | GX_CLAMP_BOTTOM));
    GXSetDispCopyGamma(GX_GM_1_0);
    if (sCurRenderMode->aa) {
        GXSetPixelFmt(GX_PF_RGB565_Z16, GX_ZC_LINEAR);
        GXSetDither(GX_FALSE);
    } else {
        GXSetPixelFmt(GX_PF_RGBA6_Z24, GX_ZC_LINEAR);
        GXSetDither(GX_TRUE);
    }
}

void configureRenderDisplay() {
    VIConfigure(sCurRenderMode);
    resetRenderDisplay();
    GXCopyDisp(sMainFrameBuffer, GX_TRUE);
    GXCopyDisp(sMainFrameBuffer, GX_TRUE);
    GXFlush();
    GXDrawDone();
    VIFlush();
    VIWaitForRetrace();
    VIWaitForRetrace();
}

void setNextVIFrameBuffer(VISettings* settings) {
    VISetBlack((settings->flags & 1) ? GX_FALSE : GX_TRUE);
    VISetNextFrameBuffer(sMainFrameBuffer);
    VIFlush();
    if (settings->doubleBuffered) {
        settings->bufferIndex ^= 1;
        sMainFrameBuffer = sFrameBuffers[settings->bufferIndex];
    }
}

// Single-buffered present path (fn_8000783C).
void presentSingleBuffered() {
    GXDrawDone();
    OSReceiveMessage(&sVIQueue.queue, nullptr, OS_MESSAGE_BLOCK);
    sVIQueue.lastRetrace = VIGetRetraceCount();
    VIWaitForRetrace();
    GXSetColorUpdate(GX_TRUE);
    GXSetAlphaUpdate(GX_TRUE);
    if (sMainFrameBuffer != nullptr) {
        GXCopyDisp(sMainFrameBuffer, GX_TRUE);
    }
    setNextVIFrameBuffer(&sVISettings);
    GXFlush();
}

}  // namespace

void initDisplay() {
    VISetBlack(GX_TRUE);
    sIntRenderMode = GXNtsc480IntDf;
    sIntRenderMode.vfilter[0] = 6;
    sIntRenderMode.vfilter[1] = 0;
    sIntRenderMode.vfilter[2] = 20;
    sIntRenderMode.vfilter[3] = 12;
    sIntRenderMode.vfilter[4] = 20;
    sIntRenderMode.vfilter[5] = 0;
    sIntRenderMode.vfilter[6] = 6;

    OSInitMessageQueue(&sVIQueue.queue, sVIQueue.msgs, 1);
    sVIQueue.lastRetrace = VIGetRetraceCount();
    sVIQueue.interval = 1;

    sVISettings.flags = 0;
    sVISettings.doubleBuffered = true;
    sVISettings.bufferIndex = 0;
    gGameHeap->changeGroupID(2);

    // PORT: the XFBs are only handles on PC (Aurora presents the EFB copy itself),
    // but they are kept so frame-buffer identity logic behaves as on console.
    for (void*& fb : sFrameBuffers) {
        if (fb == nullptr) {
            fb = JKRAllocFromHeap(nullptr, kFrameBufferSize, 0x20);
        }
    }
    sMainFrameBuffer = sFrameBuffers[0];

    VISetNextFrameBuffer(sMainFrameBuffer);
    sCurRenderMode = &sIntRenderMode;
    configureRenderDisplay();
    VISetPostRetraceCallback(postRetraceCallback);
    gGameHeap->changeGroupID(0x1A);
}

void beginFrame() {
    GXInvalidateTexAll();
    GXInvalidateVtxCache();
    GXSetScissor(0, 0, sCurRenderMode->fbWidth, sCurRenderMode->xfbHeight);
}

void presentFrame() {
    resetRenderDisplay();
    if (sVISettings.doubleBuffered) {
        GXDrawDone();
        GXSetColorUpdate(GX_TRUE);
        GXSetAlphaUpdate(GX_TRUE);
        GXCopyDisp(sMainFrameBuffer, GX_TRUE);
        GXFlush();
        setNextVIFrameBuffer(&sVISettings);
        OSReceiveMessage(&sVIQueue.queue, nullptr, OS_MESSAGE_BLOCK);
        sVIQueue.lastRetrace = VIGetRetraceCount();
    } else {
        presentSingleBuffered();
    }
    // (Retail also retargets JUTDirectPrint's crash-screen output to the new XFB.)
}

void checkRenderMode() {
    if (sCurRenderMode != sRenderModes[sRenderMode]) {
        sCurRenderMode = sRenderModes[sRenderMode];
        configureRenderDisplay();
    }
}

void defaultOrthoView() {
    GXSetScissor(0, 0, sCurRenderMode->fbWidth, sCurRenderMode->xfbHeight);
    GXSetViewportJitter(0.0f, 0.0f, sCurRenderMode->fbWidth, sCurRenderMode->xfbHeight, 0.0f, 1.875f, 1);
    Mtx44 ortho;
    MTXOrtho(ortho, -0.0f, 3.96875f, -0.0f, sCurRenderMode->fbWidth, -1.875f, 4.4765625f);
    GXSetProjection(ortho, GX_ORTHOGRAPHIC);
    GXSetCullMode(GX_CULL_NONE);
    GXLoadPosMtxImm(gIdentityMtx, GX_PNMTX0);
    GXLoadNrmMtxImm(gIdentityMtx, GX_PNMTX0);
    GXLoadTexMtxImm(gIdentityMtx, GX_IDENTITY, GX_MTX2x4);
    GXSetCurrentMtx(GX_PNMTX0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_CLR_RGBA, GX_RGBA4, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_CLR_RGBA, GX_RGBX8, 15);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX1, GX_CLR_RGBA, GX_RGBX8, 15);
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_CLAMP, GX_AF_NONE);
    GXSetChanCtrl(GX_COLOR1A1, GX_FALSE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_CLAMP, GX_AF_NONE);
    constexpr GXColor kWhite = {0xFF, 0xFF, 0xFF, 0xFF};
    GXSetChanMatColor(GX_COLOR0A0, kWhite);
    GXSetChanAmbColor(GX_COLOR0A0, kWhite);
    GXSetChanMatColor(GX_COLOR1A1, kWhite);
    GXSetChanAmbColor(GX_COLOR1A1, kWhite);
    GXSetTevKColor(GX_KCOLOR0, kWhite);
    GXSetTevKColorSel(GX_TEVSTAGE0, GX_TEV_KCSEL_K0);
    GXSetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_K0_A);
    GXSetNumTexGens(1);
    GXSetNumTevStages(1);
    GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY, GX_FALSE, GX_PTIDENTITY);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetZCompLoc(GX_FALSE);
    GXSetAlphaCompare(GX_GREATER, 0, GX_AOP_AND, GX_GREATER, 0);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_NOOP);
    GXSetZMode(GX_FALSE, GX_LEQUAL, GX_FALSE);
    GXSetDstAlpha(GX_FALSE, 0xFF);
}

void setViewportJitter(f32 left, f32 top, f32 width, f32 height, f32 nearZ, f32 farZ) {
    const f32 widthScale = sCurRenderMode->fbWidth / 640.0f;
    const u32 field = sCurRenderMode->field_rendering ? VIGetNextField() : 1;
    // Retail passes top*-1 / height*-1 here; GX's viewport takes them as-is on console.
    GXSetViewportJitter(left * widthScale, top * -1.0f, width * widthScale, height * -1.0f, nearZ, farZ, field);
}

void setDisplayFlags(u16 flags) { sVISettings.flags |= flags; }
void setRetraceInterval(s32 retraces) { sVIQueue.interval = retraces; }

void setDoubleBuffered(bool enable) {
    sVISettings.doubleBuffered = enable;
    if (enable) {
        sVISettings.bufferIndex ^= 1;
        sVISettings.bufferIndex &= sVISettings.flags;
        sMainFrameBuffer = sFrameBuffers[sVISettings.bufferIndex];
        return;
    }
    sPrevMainFrameBuffer = sMainFrameBuffer;
    sMainFrameBuffer = (sPrevMainFrameBuffer == sFrameBuffers[0]) ? sFrameBuffers[1] : sFrameBuffers[0];
}

void* getMainFrameBuffer() { return sMainFrameBuffer; }
void* getPrevMainFrameBuffer() { return sPrevMainFrameBuffer; }

void checkProgressiveModeAtBoot() {
    // No component-cable/progressive-scan prompt on PC (VIGetDTVStatus() == 0).
    OSSetProgressiveMode(0);
}

bool isProgressivePromptActive() { return sProgressiveState == 1; }

void updateProgressiveSelect() {
    if (sProgressiveState != 1) {
        return;
    }
    if (gGamePad->getRepeat() & (PAD_BUTTON_LEFT | PAD_BUTTON_RIGHT)) {
        sProgressiveMode ^= 1;
    }
    unported::fn_80047444_showProgressiveMessage(sProgressiveMode);
}

void updateProgressiveMode() {
    if (sProgressiveState != 1) {
        return;
    }
    sProgressiveState = 0;  // PC: the mode never needs a confirmation countdown
    OSSetProgressiveMode(sProgressiveMode);
    sRenderMode = u8(sProgressiveMode);
}

u32 getRetraceInterval() { return sVIQueue.interval; }

// --- Wipes ---
WipeControl gWipe;
static u8 sWipeObject[0x1A8];  // lbl_803A3668 (wipe effect object; +0xC is its draw state)
static void* sCapturedFrame;    // lbl_804D8070
extern "C" void* lbl_804D86BC;  // texture used for the wipe-in (owned by unported code)
void* lbl_804D86BC;

static void initWipeTexture(GXTexObj* tex, void* image) {
    GXInitTexObj(tex, image, 640, 480, GX_TF_RGB565, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GXInitTexObjLOD(tex, GX_NEAR, GX_NEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
}

void wipeDraw(WipeControl* wipe) {
    resetGXState();
    GXTexObj tex;
    switch (wipe->state) {
    case WIPE_REQUEST_OUT:
        unported::fn_800395BC_wipeStart(sWipeObject + 0xC, wipe->type, s16(wipe->frames));
        setDoubleBuffered(false);
        // Capture the last presented frame so it can be wiped away.
        sCapturedFrame = getPrevMainFrameBuffer();
        GXSetTexCopySrc(0, 0, 640, 480);
        GXSetTexCopyDst(640, 480, GX_TF_RGB565, GX_FALSE);
        GXSetCopyClamp(GXFBClamp(GX_CLAMP_TOP | GX_CLAMP_BOTTOM));
        GXCopyTex(sCapturedFrame, GX_FALSE);
        GXPixModeSync();
        initWipeTexture(&tex, sCapturedFrame);
        wipe->state = WIPE_OUT;
        unported::fn_80039C74_wipeDraw(sWipeObject + 0xC, &tex);
        break;
    case WIPE_OUT:
        initWipeTexture(&tex, sCapturedFrame);
        unported::fn_80039C74_wipeDraw(sWipeObject + 0xC, &tex);
        break;
    case WIPE_IN:
    case WIPE_OUT_DONE:
        initWipeTexture(&tex, lbl_804D86BC);
        unported::fn_80039C74_wipeDraw(sWipeObject + 0xC, &tex);
        break;
    default:
        break;
    }
}

void wipeUpdate(WipeControl* wipe) {
    if (!unported::fn_80039770_wipeUpdate(sWipeObject + 0xC)) {
        return;
    }
    switch (wipe->state) {
    case WIPE_OUT:
        wipe->state = WIPE_OUT_DONE;
        break;
    case WIPE_IN:
        wipe->state = WIPE_IDLE;
        break;
    default:
        break;
    }
}

void wipeAfterPresent(WipeControl* wipe) {
    if (wipe->state == WIPE_OUT_DONE) {
        setDoubleBuffered(true);
        unported::fn_80039628_wipeReverse(sWipeObject + 0xC);
        wipe->state = WIPE_IN;
    }
}

void requestWipeOut() { gWipe.state = WIPE_REQUEST_OUT; }
void setWipeType(s32 type) { gWipe.type = type; }
void setWipeFrames(s32 frames) { gWipe.frames = frames; }

}  // namespace lm
