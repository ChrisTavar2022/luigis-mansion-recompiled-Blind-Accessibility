#pragma once
// Host <-> guest frame handoff. Guest code only runs while the host has an
// Aurora frame open: the host calls runUntilPresent() between
// aurora_begin_frame() and aurora_end_frame(), and the guest gives control
// back when it presents (VISetNextFrameBuffer).
#include "recomp/ctx.h"

namespace recomp::frame {

using Entry = void (*)();

void start(Entry boot);       // boot runs on a new host thread once the first frame opens
bool runUntilPresent();       // false once the guest has exited
// One host frame (between aurora_begin_frame and aurora_end_frame): runs the
// guest until it presents, or with frame interpolation on (settings), replays
// the guest's recorded frames instead while the guest runs on its own.
bool hostFrame();             // false once the guest has exited
void pace();                  // after aurora_end_frame: frame-rate cap for "Interpolated 60"
void stop();
bool gamePaused();
void setHostPaused(bool paused);  // host: window paused (no frames)

void present(Ctx& c, u32 xfb, bool black);  // guest side

}  // namespace recomp::frame
