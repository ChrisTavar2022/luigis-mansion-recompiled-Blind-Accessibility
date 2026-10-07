#pragma once
#include <dolphin/types.h>

class JUTGamePad;

namespace lm {

extern JUTGamePad* gGamePad;  // sGamePad

void initGamePad();                  // fn_80005830
void setButtonRepeatRate(s32 div);   // fn_80009540: repeat delay/rate scaled for 1/div speed

}  // namespace lm
