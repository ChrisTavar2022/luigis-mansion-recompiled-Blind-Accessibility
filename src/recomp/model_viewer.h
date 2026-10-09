#pragma once
// In-game model viewer (F1 > Enhancements > Debug, or F4). Lists every model on the disc
// (.mdl character/object models, .bin room/furniture/door models, .bmd J3D effect models,
// inside any archive) and whole maps (rooms with their furniture), and draws them with the
// game's own code: S3DModel for .mdl (relocation, .key joint animation, materials, lighting,
// the ghost layer for ghosts), the room/furniture renderer for .bin, J3D for .bmd.
// Orbit camera for models, fly camera for maps; no culling of any kind; wireframe via
// Aurora patch 0011. Details: docs/UNUSED_CONTENT.md ("Model viewer").
#include "recomp/ctx.h"

namespace recomp::viewer {
bool isOpen();
void setOpen(bool open);
void window();             // host thread: ImGui window (catalogue, camera, animation)
void guestFrame(Ctx& c);   // guest thread, end of each frame: loads and draws the model
}  // namespace recomp::viewer
