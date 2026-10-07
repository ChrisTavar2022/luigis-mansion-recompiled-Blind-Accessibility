// Port of Unsorted/bootScene. Based on lm-decomp's version of this TU.
//
// Shows the Nintendo logo while the loader thread brings up audio and loads
// the always-resident game archive (/Game/game_usa.szp).
#include "game/scene/boot_scene.h"
#include "game/system/aram_data.h"
#include "game/system/camera.h"
#include "game/system/display.h"
#include "game/system/dvd_file.h"
#include "game/system/gx_util.h"
#include "game/system/heap.h"
#include "game/unported.h"

#include "port/dol.h"

#include <dolphin/gx.h>

#include <JSystem/J2DGraph/J2DOrthoGraph.h>
#include <JSystem/J2DGraph/J2DPicture.h>
#include <JSystem/JKernel/JKRExpHeap.h>
#include <JSystem/JKernel/JKRMemArchive.h>

#include <cstring>

namespace lm::BootScene {

namespace {

enum State { STATE_START_LOADING, STATE_LOADING, STATE_DONE };

// staticdata_usa_arc: a RARC embedded in the US executable's .data section
// (logo, title-screen resources, boot sound). Read from the user's own DOL.
constexpr u32 kStaticDataAddr = 0x8021E000;
constexpr u32 kStaticDataSize = 0xDD380;

State sState;
u16 sLogoRotation;   // lbl_804D8118
f32 sLogoOffsetZ;    // lbl_804D811C
J2DPicture* sNintendoLogo;
GXColor sLogoMatColor = {255, 255, 255, 32};  // lbl_804CFFB0
void* sStaticData;

JKRMemArchive* sGameArchive;  // lbl_804D8738 (fn_80066FAC)

void onGameArchiveLoaded(void*, DvdFileInfo* info) {
    void* block = JKRAllocFromHeap(nullptr, info->data->decompressedSize(), 32);
    info->data->decompressYay0(static_cast<DvdFileBlock*>(block));
    freeToHeap(info->data);
    sGameArchive = JKR_NEW JKRMemArchive(block, 0, JKRMEMBREAK_FLAG_UNKNOWN0);
}

void loadStaticArchives() {
    setHeapGroup(16);
    useSceneHeap();
    dvdFileOpenAsync("/Game/game_usa.szp", onGameArchiveLoaded, nullptr, 0);
    gGameHeap->becomeCurrentHeap();
    setHeapGroup(3);
    // Park the compressed screen archives in ARAM until their screens need them.
    const struct {
        const char* name;
        AramData* dst;
    } kAramResources[] = {
        {"/res_titl.szp", &gResTitleAramData},
        {"/gameboy.szp", &gGameboyAramData},
        {"/guidemap.szp", &gGuidemapAramData},
        {"/res_paus.szp", &gResPauseAramData},
        {"/res_list.szp", &gResListAramData},
    };
    for (const auto& res : kAramResources) {
        void* data = sStaticDataArchive->getResource(res.name);
        res.dst->stage(data, sStaticDataArchive->getResSize(data));
    }
    while (dvdFilesPending()) {
        OSYieldThread();
    }
}

// Runs on the loader thread while the logo is shown.
void loadStaticData() {
    gGameHeap->changeGroupID(4);
    unported::fn_80188594_initAudioHeap(JKRAllocFromHeap(gGameHeap, 0xC0000, 0x20), 0xC0000, 0xF00000);
    unported::fn_8004D4C8_loadSoundTables();
    unported::fn_80188714_setBootSound(sStaticDataArchive->getResource("/ninlogo.pcm"));
    loadStaticArchives();
    unported::fn_80188638_startAudioLoad();
    while (unported::fn_80188740_audioLoading()) {
    }
}

}  // namespace

s32 sAudioReady;
JKRMemArchive* sStaticDataArchive;

static void init() {
    sState = STATE_START_LOADING;
    sAudioReady = 0;
    gGameHeap->becomeCurrentHeap();
    initDefaultCamera();
    unported::fn_8005FD80_fillRandomTable();
    setHeapGroup(8);
    unported::fn_8003EB44_initSystemFont();
    setHeapGroup(26);
    sLogoRotation = 0;
    sLogoOffsetZ = 0.0f;
    sLogoMatColor.a = 32;

    // PORT: retail mounts the archive in place inside .data; copy it out of the DOL.
    const auto staticData = port::dol::bytes(kStaticDataAddr, kStaticDataSize);
    sStaticData = JKRAllocFromHeap(nullptr, kStaticDataSize, 32);
    std::memcpy(sStaticData, staticData.data(), staticData.size());
    sStaticDataArchive = JKR_NEW JKRMemArchive(sStaticData, 0, JKRMEMBREAK_FLAG_UNKNOWN0);

    auto* logoYay0 = static_cast<DvdFileBlock*>(sStaticDataArchive->getResource("/nintendo.szp"));
    const s32 size = logoYay0->decompressedSize();
    useSceneHeap();
    auto* logoTex = static_cast<DvdFileBlock*>(allocFromCurrentHeap(size, 0x20));
    gGameHeap->becomeCurrentHeap();
    logoYay0->decompressYay0(logoTex);
    sNintendoLogo = JKR_NEW J2DPicture(reinterpret_cast<const ResTIMG*>(logoTex));
}

static void destroy() {
    JKR_DELETE(sNintendoLogo);
    sStaticDataArchive->unmount();
    useSceneHeap();
}

static s32 update() {
    switch (sState) {
    case STATE_START_LOADING:
        runOnLoaderThread(loadStaticData);
        checkProgressiveModeAtBoot();
        sState = STATE_LOADING;
        return SCENE_BOOT;
    case STATE_LOADING:
        if (!loaderThreadDone()) {
            return SCENE_BOOT;
        }
        sAudioReady = 1;
        sState = STATE_DONE;
        OSReport("[boot] static data loaded (scene frame %llu)\n", (unsigned long long)gSceneFrameCounter);
        return SCENE_BOOT;
    default:
        return SCENE_TITLE;
    }
}

static void draw() {
    static s32 sDoneFrames;  // lbl_804D8128

    resetGXState();
    applyCamera(&gCamera);
    GXSetChanMatColor(GX_COLOR0A0, sLogoMatColor);
    setViewportJitter(0.0f, 0.0f, 640.0f, 480.0f, 0.0f, 1.0f);
    GXSetScissor(0, 0, 640, 480);
    GXSetCurrentMtx(GX_PNMTX0);
    GXSetNumChans(1);
    GXSetNumTevStages(1);

    Mtx mtx;
    makeSRTMtx(0.2f, 0.2f, 0.2f, 0.0f, 0.0f, 0.0f, mtx, 0, 0, sLogoRotation);
    MTXConcat(gCamera.viewMtx, mtx, mtx);
    GXLoadNrmMtxImm(mtx, GX_PNMTX0);
    makeSRTMtx(0.2f, 0.2f, 0.2f, 0.0f, 0.0f, sLogoOffsetZ, mtx, 0, 0, sLogoRotation);
    MTXConcat(gCamera.viewMtx, mtx, mtx);
    GXLoadNrmMtxImm(mtx, GX_PNMTX0);

    J2DOrthoGraph graph(0, 0, 640, 480, -1.0f, 1.0f);
    graph.setPort();
    sNintendoLogo->setBlackWhite(JUtility::TColor(0, 0, 0, 255), JUtility::TColor(220, 0, 0, 255));
    sNintendoLogo->draw(0.0f, 0.0f, 640.0f, 480.0f, false, false, false);

    // Unblank video once the scene has been running for two frames.
    if (gSceneFrameCounter >= 2) {
        setDisplayFlags(1);
    }
    if (sState == STATE_DONE) {
        sDoneFrames++;
    }
}

Scene sScene = {"BOOT", 1, init, update, draw, destroy};

}  // namespace lm::BootScene
