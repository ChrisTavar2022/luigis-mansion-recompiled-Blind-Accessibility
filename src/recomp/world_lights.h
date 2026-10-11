#pragma once
// World lights (F1 > Visuals > Enhanced lighting > World lights; docs/RENDERING.md,
// "Enhanced lighting > World lights"): candles, fireplaces, lamps, chandeliers, element sprays,
// fire ghosts, sparkles... become coloured extra lights, like the ghost lights.
#include "recomp/ctx.h"

namespace recomp::worldlights {

// The main view fn_8000BB98(0) is about to be drawn (enhanced_lighting.cpp): build this frame's
// world lights and submit them (recomp::submitExtraLights, after the ghost lights).
void onMainView(Ctx& c);

}  // namespace recomp::worldlights
