// Port of the scene machine in auto_03_8000B014 (fn_8000B334..fn_8000B880) and
// the per-frame scratch buffer in auto_03_80009540 (fn_8000AD48..fn_8000ADCC).
#include "game/scene/scene.h"
#include "game/scene/boot_scene.h"
#include "game/scene/title_scene.h"
#include "game/system/display.h"
#include "game/system/dvd_file.h"
#include "game/system/gx_util.h"
#include "game/system/heap.h"
#include "game/system/pad.h"
#include "game/system/reset.h"
#include "game/unported.h"

#include <dolphin/dvd.h>
#include <dolphin/gx.h>
#include <dolphin/os.h>

#include <JSystem/JUtility/JUTGamePad.h>

namespace lm {

Scene* gSceneTable[SCENE_MAX] = {
    &BootScene::sScene,
    &TitleScene::sScene,
    // Not ported yet: MAIN_GAME, SELECT, OPENING, STAFF_ROLL, ENDING, MOVIE.
};
Scene* gCurScene = &BootScene::sScene;
s32 gCurSceneId = SCENE_BOOT;

u64 gFrameCounter;
u64 gSceneFrameCounter;

namespace {

enum Phase : s32 {
    PHASE_LOADING = 0,    // scene init() running on the loader thread
    PHASE_WAIT_WIPE = 1,  // waiting for the previous scene's wipe-out to finish
    PHASE_RUNNING = 2,
};

// lbl_803A3AE0
struct SceneControl {
    s32 phase;
    s32 nextScene;
    OSMessageQueue resetQueue;
    OSMessage resetMsg;
};
SceneControl sControl;

s32 sSceneChanging;       // lbl_804D8088
s32 sExitSceneLoop;       // lbl_804D80A8
s32 sFrameSkipped;        // lbl_804D80B4
s32 sAltAudioFrame;       // lbl_804D80B8
s32 sSceneLoopActive;     // lbl_804CFF78

// Per-frame double-buffered scratch memory (lbl_803A3A80).
struct FrameScratch {
    u8* halves[2];
    u8* cur;
    u32 halfSize;
    u32 index;
};
FrameScratch sScratch;

void allocFrameScratch() {  // fn_8000AD48
    sScratch.index = 0;
    sScratch.halfSize = 0x200;
    sScratch.halves[0] = static_cast<u8*>(allocFromCurrentHeap(0x400, 0x20));
    sScratch.halves[1] = sScratch.halves[0] + 0x200;
}

void flipFrameScratch() {  // fn_8000ADCC
    sScratch.index ^= 1;
    sScratch.cur = sScratch.halves[sScratch.index] + sScratch.halfSize;
}

void freeFrameScratch() {  // fn_8000ADA0
    freeToHeap(sScratch.halves[0]);
}

void initSceneControl() {  // fn_8000B880 (static constructor)
    sControl.phase = PHASE_LOADING;
    OSInitMessageQueue(&sControl.resetQueue, &sControl.resetMsg, 1);
    OSReceiveMessage(&sControl.resetQueue, nullptr, OS_MESSAGE_NOBLOCK);
}

// fn_8000B370: draw the running scene with default matrices loaded.
void drawScene() {
    for (u32 slot = GX_PNMTX0; slot <= GX_PNMTX9; slot += 3) {
        GXLoadPosMtxImm(gIdentityMtx, slot);
        GXLoadNrmMtxImm(gIdentityMtx, slot);
    }
    gCurScene->draw();
    defaultOrthoView();
}

[[noreturn]] void hardReset() {
    GXDrawDone();
    while (unported::fn_80185938_cardManagerState() == 7) {
    }
    OSResetSystem(OS_RESET_HOTRESET, 0, FALSE);
    while (true) {
    }
}

// fn_8000B4A0: scene transitions and reset/disc-error handling.
void checkSceneChange() {
    if (gCurSceneId != sControl.nextScene) {
        gCurSceneId = sControl.nextScene;
        sSceneChanging = 1;
        sExitSceneLoop = 1;
        requestWipeOut();
    }

    if (unported::fn_8018A8B4_resetState() == 2) {
        if (!unported::fn_8018A860_resetReady()) {
            return;
        }
        if (DVDCheckDisk() == FALSE) {
            hardReset();
        }
    }

    if (dvdFilesPending() || sControl.phase != PHASE_RUNNING) {
        return;
    }
    if (unported::fn_8018A8B4_resetState() == 2) {
        if (!unported::fn_8018A860_resetReady() || DVDGetDriveStatus() != DVD_STATE_END) {
            return;
        }
    }

    OSMessage requested;
    if (!OSReceiveMessage(&sControl.resetQueue, &requested, OS_MESSAGE_NOBLOCK)) {
        return;
    }
    if (gResetState == 3) {
        gResetState = 0;
        if (DVDCheckDisk() == FALSE) {
            hardReset();
        }
    }
    if (unported::fn_8018A8B4_resetState() == 3) {
        unported::fn_8018A8BC_setResetState(0);
    }
    gCurSceneId = s32(reinterpret_cast<intptr_t>(requested));
    requestWipeOut();
    sExitSceneLoop = 1;
}

// fn_8000B610: run the current scene until it requests another one.
void runCurrentScene() {
    sControl.phase = PHASE_LOADING;
    sControl.nextScene = gCurSceneId;

    while (true) {
        sFrameSkipped = 0;
        beginFrame();
        if (sControl.phase == PHASE_RUNNING) {
            drawScene();
        }
        wipeDraw(&gWipe);
        checkDvdStatus();

        switch (sControl.phase) {
        case PHASE_LOADING:
            if (loaderThreadDone()) {
                sControl.phase = PHASE_WAIT_WIPE;
                setButtonRepeatRate(gCurScene->retraceInterval);
                setRetraceInterval(gCurScene->retraceInterval);
            }
            break;
        case PHASE_WAIT_WIPE:
            if (gWipe.state != WIPE_OUT) {
                setWipeType(2);
                sControl.phase = PHASE_RUNNING;
                setHeapGroup(0x16);
            }
            break;
        default:
            break;
        }

        unported::fn_80185910_probeCards();
        checkRenderMode();
        JUTGamePad::read();

        if (sControl.phase == PHASE_RUNNING && gDvdErrorShown == 0 && sExitSceneLoop == 0) {
            flipFrameScratch();
            sControl.nextScene = gCurScene->update();
        }

        wipeUpdate(&gWipe);
        if (BootScene::sAudioReady) {
            unported::fn_8018865C_audioFrame();
        }
        if (sAltAudioFrame) {
            unported::fn_80188710_audioFrameAlt();
        }
        presentFrame();
        if (sControl.phase != PHASE_LOADING) {
            wipeAfterPresent(&gWipe);
        }

        if (sExitSceneLoop) {
            sExitSceneLoop = 0;
            return;
        }
        checkSceneChange();
        pollResetButton();
        gFrameCounter++;
        gSceneFrameCounter++;
    }
}

}  // namespace

void requestScene(s32 scene) {  // fn_8000B334
    sSceneChanging = 1;
    OSSendMessage(&sControl.resetQueue, reinterpret_cast<OSMessage>(intptr_t(scene)), OS_MESSAGE_NOBLOCK);
}

void runScenes() {
    initSceneControl();
    initLoaderThread();
    while (true) {
        gSceneFrameCounter = 0;
        sSceneLoopActive = 1;

        setHeapGroup(0xC);
        allocFrameScratch();
        setHeapGroup(0x1A);

        OSReport("[scene] starting %s\n", gCurScene->name);
        runOnLoaderThread(gCurScene->init);
        setWipeFrames(30);
        runCurrentScene();
        gCurScene->destroy();
        freeFrameScratch();

        Scene* next = gCurSceneId >= 0 && gCurSceneId < SCENE_MAX ? gSceneTable[gCurSceneId] : nullptr;
        if (next == nullptr) {
            // PORT: the remaining scenes are not ported yet; stop here rather than crash.
            OSReport("[scene] scene %d is not ported yet; halting the scene machine\n", gCurSceneId);
            while (true) {
                beginFrame();
                presentFrame();
            }
        }
        gCurScene = next;
        freeSceneGroups(0x1A);
    }
}

}  // namespace lm
