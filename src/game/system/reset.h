#pragma once
// Soft reset (START+B+X held) and reset-button handling (retail: auto_03_80005760).
#include <dolphin/types.h>

namespace lm {

extern s32 gResetState;  // lbl_804D7FF0: 0 idle, 1 button held, 2 reset requested, 3 resetting

void resetResetState();   // part of fn_80005C00
bool pollResetCombo();    // fn_80005CF8: true once START+B+X has been held for ~1 s
void pollResetButton();   // fn_80005E04

}  // namespace lm
