// Port of Unsorted/TitleScene (0x8000EC98..0x8000EF58).
#include "game/scene/title_scene.h"
#include "game/scene/title_screen.h"
#include "game/system/display.h"
#include "game/system/dvd_file.h"
#include "game/system/heap.h"
#include "game/system/pad.h"
#include "game/system/reset.h"
#include "game/unported.h"

#include <dolphin/pad.h>
#include <JSystem/JUtility/JUTGamePad.h>

namespace lm::TitleScene {
namespace {

enum State { STATE_START_LOADING, STATE_LOADING, STATE_READY, STATE_LEAVING };

s32 sState;           // lbl_804D8130 (persists: returning to the title skips loading)
s32 sIdleTimer;       // lbl_804D8134: frames until the attract demo starts
s32 sInputLockout;    // lbl_804D8138: frames before START is accepted
s32 sStartPressed;    // lbl_804D813C

constexpr s32 kAttractTimeout = 1800;  // 60 s at 30 Hz

// fn_8000EC98 -> fn_80005C00 (runs on the loader thread).
void loadTitle() {
    resetResetState();
    // (Retail also starts an idle priority-1 thread here, fn_80005C8C; it only
    // ever blocks on an empty queue, so it is not needed on PC.)
}

}  // namespace

s32 gNextFrontEndMode;  // lbl_804CFFAC: 1 = attract demo, 2 = file select

static void init() {
    sInputLockout = 60;
    sStartPressed = 0;
    // (Retail zero-fills the whole free scene heap here; allocations on PC are
    // already zeroed where it matters, and the scene heap is far larger.)
    unported::setLuigiAppearPoint(0);
    unported::fn_80188760_playStream(0x80000021, 0);  // title music
    sIdleTimer = kAttractTimeout;
    if (sState != STATE_START_LOADING) {
        sState = STATE_READY;
    }
    setHeapGroup(2);
    // No progressive-scan prompt on PC, so "PRESS START" is always shown.
    TitleScreen::create(!isProgressivePromptActive());
    setHeapGroup(0x1A);
}

static s32 update() {
    switch (sState) {
    case STATE_START_LOADING:
        runOnLoaderThread(loadTitle);
        sState = STATE_LOADING;
        break;
    case STATE_LOADING:
        TitleScreen::update();
        if (loaderThreadDone()) {
            sState = STATE_READY;
        }
        break;
    case STATE_READY:
        if (gGamePad->getTrigger() & (PAD_BUTTON_A | PAD_BUTTON_START)) {
            sStartPressed = 1;
        }
        if (sStartPressed && sInputLockout <= 0) {
            sState = STATE_LEAVING;
            gNextFrontEndMode = 2;
            unported::fn_8018A2A0_playSystemSound(0x3802);
            requestScene(SCENE_SELECT);
            updateProgressiveMode();
        } else if (TitleScreen::update() && sInputLockout != 0) {
            sInputLockout--;
        }
        break;
    default:
        break;
    }

    if (--sIdleTimer == 0) {
        gNextFrontEndMode = 1;
        requestScene(SCENE_OPENING);
    }
    return SCENE_TITLE;
}

static void draw() {
    TitleScreen::draw();
    updateProgressiveSelect();
}

static void destroy() {
    unported::fn_80188D0C_stopStream(30);
    TitleScreen::destroy();
}

Scene sScene = {"TITLE", 1, init, update, draw, destroy};

}  // namespace lm::TitleScene
