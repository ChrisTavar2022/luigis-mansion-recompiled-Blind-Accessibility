#pragma once
// In-game model viewer (F1 > Enhancements > Debug, or F4): lists the .mdl models in the disc's
// archives and draws the selected one with the game's own model code (S3DModel: relocation,
// joint animation from .key files, materials, lighting), full screen with an orbit camera.
#include "recomp/ctx.h"

namespace recomp::viewer {
bool isOpen();
void setOpen(bool open);
void window();             // host thread: ImGui window (catalogue, camera, animation)
void guestFrame(Ctx& c);   // guest thread, end of each frame: loads and draws the model
}  // namespace recomp::viewer
