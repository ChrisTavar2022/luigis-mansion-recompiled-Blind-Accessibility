#pragma once
// Ghost lights (F1 > Visuals > Enhanced lighting > Ghost lights; docs/RENDERING.md,
// "Enhanced lighting > Ghost lights"): every visible ghost becomes a coloured point light in
// the renderer's extra-light list (aurora/lights.h, via recomp::submitExtraLights), coloured from the ghost's own glow colour.
#include "recomp/ctx.h"

namespace recomp::ghostlights {

// The main view fn_8000BB98(0) is about to be drawn (enhanced_lighting.cpp): build this frame's
// ghost lights and submit them (recomp::submitExtraLights).
void onMainView(Ctx& c);

// From gameplay_hooks.cpp: a CharacterEvent message delivered to a ghost's AI (EnThought,
// fn_800C0F08; Koga::CharacterEvent::Message: 2 in the flashlight beam, 3 capture begin,
// 4 capture end, 9 fishing (vacuum) begin, 12 collided with Luigi, 13-18 element hits), and
// the Poltergust capture update (fn_80081BCC) holding `actor` this frame.
void onCharacterEvent(u32 actor, u32 msg);
void onVacuumTick(u32 actor);

}  // namespace recomp::ghostlights
