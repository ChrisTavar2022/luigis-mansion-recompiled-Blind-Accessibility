#pragma once
// F1 > Enhancements > Silly, Luigi options (silly.cpp; docs/ENHANCEMENTS.md "Silly"): head size,
// body size, wobbly head, paper Luigi, clothes and Poltergust colours. Visual only, off by default.
#include "recomp/ctx.h"

namespace recomp::silly {
// Host thread (F1 menu, Enhancements > Silly): the Luigi widgets.
void drawLuigiOptions();
// Guest thread, GXInitTexObj: a recoloured host copy of the texture at `image` when it is one of
// Luigi's clothing or Poltergust textures and a colour option is on; nullptr otherwise.
const void* texOverride(u32 image, u32 width, u32 height, u32 gxFormat);
}  // namespace recomp::silly
