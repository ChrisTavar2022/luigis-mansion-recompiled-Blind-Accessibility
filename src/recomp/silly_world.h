#pragma once
// Silly options, world part (F1 > Enhancements > Silly; docs/ENHANCEMENTS.md "Silly options:
// world"): Big Head Mode for ghosts, ghost colour shuffle, disco mansion, Luigi voice pitch,
// moon gravity for spilled treasure, confetti coins. All default to the retail game.
//
// Wiring: hooks of its own in silly_world.cpp (tools/recomp/hooks.txt) plus small calls from
// existing hooks (listed with each function below). Guest-thread functions run on the guest
// thread; drawWorldOptions() runs on the host thread while the guest is parked (as menu.cpp).
#include "recomp/ctx.h"

namespace recomp::sillyworld {

// End of every guest frame (dev_tools.cpp LMChangeFrameBuffer hook): test aids, disco clock.
void onFrame(Ctx& c);
// A JAI sound effect was started (hle_audio.cpp fn_8019754C hook, after the original):
// id = sound id, handle = the JAISound** the start filled in.
void onSeStarted(Ctx& c, u32 id, u32 handle);
// Ghost layer lighting (tags.cpp fn_800581E8 hook, before the original): r3 points to a stack
// copy of the ghost's material colour (actor +0xE9); the ghost shuffle tints it.
void ghostMaterialColor(u32 actor, u32 colorPtr);
// Enhanced lighting ghost lights (ghost_lights.cpp): the light colour of a ghost (sRGB bytes);
// true when the colour shuffle replaced it.
bool ghostGlowColor(u32 actor, u32 rgb[3]);
// Room ambient update (hidden_mansion_hooks.cpp fn_800168EC wrapper): the room record's ambient
// RGB bytes for the call; disco tints them.
void discoAmbient(u8 rgb[3]);
// Enhanced lighting world lights (world_lights.cpp): tints a lamp light's linear colour.
void discoLight(u32 id, float color[3]);
bool discoOn();

}  // namespace recomp::sillyworld

namespace recomp::silly {
// F1 > Enhancements > Silly: the world options' widgets (host thread).
void drawWorldOptions();
}  // namespace recomp::silly
