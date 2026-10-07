#include "game/system/reset.h"
#include "game/scene/scene.h"
#include "game/system/display.h"
#include "game/system/pad.h"
#include "game/unported.h"

#include <dolphin/pad.h>
#include <dolphin/os.h>

#include <JSystem/JUtility/JUTGamePad.h>

namespace lm {

s32 gResetState;

static u32 sComboFrames;   // lbl_804D7FF4
static s32 sComboLatched;  // lbl_804D7FFC

void resetResetState() { gResetState = 0; }

bool pollResetCombo() {
    const u32 held = gGamePad->getButton();
    static constexpr u32 kCombo[3] = {PAD_BUTTON_START, PAD_BUTTON_B, PAD_BUTTON_X};
    for (u32 button : kCombo) {
        if (!(held & button)) {
            sComboLatched = 0;
            sComboFrames = 0;
            return false;
        }
    }
    if (sComboLatched) {
        return false;
    }
    // About one second regardless of the scene's frame rate (30 retraces / interval).
    if (++sComboFrames < 30u / getRetraceInterval()) {
        return false;
    }
    sComboLatched = 1;
    sComboFrames = 0;
    gResetState = 2;
    return true;
}

void pollResetButton() {
    switch (gResetState) {
    case 0:
        if (OSGetResetSwitchState()) {
            gResetState = 1;
        }
        pollResetCombo();
        break;
    case 1:
        if (!pollResetCombo() && OSGetResetSwitchState() != 1) {
            gResetState = 2;
        }
        break;
    case 2:
        JUTGamePad::recalibrate(0xF0000000);
        JUTGamePad::CRumble::stopMotor(gGamePad->getPortNum());
        setWipeType(2);
        unported::fn_8018A78C_beginReset(0xF);
        requestScene(SCENE_BOOT);
        gResetState = 3;
        break;
    default:
        break;
    }
}

}  // namespace lm
