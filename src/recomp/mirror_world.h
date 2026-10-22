#pragma once
// The left/right mirror of the European (PAL) Hidden Mansion, and "Mirror world" (F1 >
// Enhancements > Gameplay), which applies it in every mode. The switch itself is
// hm::mirrorOn() (hidden_mansion.h); docs/ENHANCEMENTS.md "Mirror world" and
// docs/HIDDEN_MANSION.md "Mirror" list where the game mirrors and where it does not.
//
// This file: the Game Boy Horror map mirror (the USA game's equivalent of the European code,
// and the European functions driven through hm::callMirror), the stereo-swap flag on the
// European game, and the 2-player screen copy (both players' views mirrored in place).
#include "recomp/ctx.h"

namespace recomp::mirror {

// fn_8005F8B8 hook (hidden_mansion_hooks.cpp), before anything else: true = skip the scene
// copy now. With two player views the copy (and its mirrored redraw) is made after the last
// view instead of after view 0, so both views are mirrored (beforeViewHud runs it).
bool deferSceneCopy();
// fn_8003E7B0(view) hook (coop_fixes.cpp), before the original: runs a deferred scene copy
// before the last view's HUD.
void beforeViewHud(Ctx& c, u32 view);
// Side-by-side split (co-op improvements): player 1's view is laid out in the right half and
// player 2's in the left, so the mirrored copy of the whole screen puts each back on its side.
bool swapSplitHalves();

// fn_80015BDC hook (tags.cpp), before the original: inside the GBH map draw of the USA game
// the map's view matrix argument is replaced by view x Scale(-1, 1, 1) (European fn_80029DD4 /
// fn_8002A01C).
void mapViewMatrix(Ctx& c);
// After the original fn_80015BDC: the European map draw turns on the mirrored cull-mode row
// from here to the end of the map draw.
void afterMapViewMatrix();

// Per guest frame (hm::onFrame): keeps the European game's map sign and stereo-swap flag in
// step with mirror world when it is switched while a map is loaded.
void onFrame();

}  // namespace recomp::mirror
