#pragma once
// The title screen's 2D layout and its animation (retail: Unsorted/TitleScreenPane).
#include <dolphin/types.h>

namespace lm::TitleScreen {

void create(bool showPressStart);  // fn_8003ADE0
bool update();                     // fn_8003AE54: true once the title is fully shown
void draw();                       // fn_8003AE7C
void destroy();                    // fn_8003AEA0

}  // namespace lm::TitleScreen
