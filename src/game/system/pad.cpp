// Gamepad setup (retail: fn_80005830 in auto_03_80005760, fn_80009540).
#include "game/system/pad.h"
#include "game/system/heap.h"

#include <dolphin/pad.h>

#include <JSystem/JKernel/JKRHeap.h>
#include <JSystem/JUtility/JUTGamePad.h>

namespace lm {

JUTGamePad* gGamePad;

void initGamePad() {
    setHeapGroup(5);
    PADSetSpec(PAD_SPEC_5);
    JUTGamePad::init();
    gGamePad = JKR_NEW JUTGamePad(JUTGamePad::EPort1);
    setButtonRepeatRate(1);
    setHeapGroup(0x1A);
}

void setButtonRepeatRate(s32 div) {
    // Repeat applies to the D-pad and C-stick directions (mask 0x0F0F000F).
    gGamePad->setButtonRepeat(0x0F0F000F, 0x20 / div, 4 / div);
}

}  // namespace lm
