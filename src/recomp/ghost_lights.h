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
void onCharacterEvent(u32 actor, u32 msg, u32 thought, u32 event);

// From hle_gx.cpp: the ghost pass (fn_80060E70 pass 0x30) set the ghost layer's destination alpha
// for `actor` right before drawing its model (GXSetDstAlpha(1, alpha) at 0x800611C0/0x800611D0).
// Only ghosts the game actually draws this way with a non-zero alpha give light.
void onGhostDrawn(u32 actor, u32 alpha);
void onVacuumTick(u32 actor);

// Test aid LM_GHOST_HUNT=1 (hle_pad.cpp, after the scripted input): steers Luigi to stun and
// vacuum the nearest ghost (face it with the flashlight off, switch the light on when it shows its
// heart, then hold R and pull away), so the stun/vacuum/capture lights can be checked.
void testPad(u16& buttons, s8& stickX, s8& stickY, s8& cY, u8& triggerR);
int huntPort();  // pad port testPad drives: 0, or 1 with LM_GHOST_HUNT_PLAYER=2 (2-player mode)
// Test aid LM_GHOST_HUNT_BEAM: the game's "still lit?" query (message 21, the light-bind count)
// answers yes for the ghost the test aid is lighting.
bool testForceLit(u32 actor);

}  // namespace recomp::ghostlights
