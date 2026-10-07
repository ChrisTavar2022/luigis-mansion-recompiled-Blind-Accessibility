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
void stop();
bool gamePaused();

void present(Ctx& c, u32 xfb, bool black);  // guest side

}  // namespace recomp::frame
