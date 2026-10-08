#pragma once
// EFB copies to RAM, runtime side (efb_copy.cpp, docs/RENDERING.md).
#include "recomp/ctx.h"

namespace recomp::efbcopy {
void onFrame(u32 frameNo);         // guest frame boundary: settings, writebacks, stats
void cpuRead(u32 addr, u32 size);  // DCInvalidateRange: the CPU will read guest memory
void* dumpTarget(u32 frameNo);     // LM_FRAME_DUMP: host buffer to copy this frame's EFB into, or null
}  // namespace recomp::efbcopy
