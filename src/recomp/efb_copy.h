#pragma once
// EFB copies to RAM, runtime side (efb_copy.cpp, docs/RENDERING.md).
#include "recomp/ctx.h"

#include <functional>
#include <string>

namespace recomp::efbcopy {
void onFrame(u32 frameNo);         // guest frame boundary: settings, writebacks, stats
void cpuRead(u32 addr, u32 size);  // DCInvalidateRange: the CPU will read guest memory
void* dumpTarget(u32 frameNo);     // LM_FRAME_DUMP: host buffer to copy this frame's EFB into, or null
// One-off dump of the next finished guest frame (model viewer sweep): `done` gets the 640x480
// RGB pixels; `path` (optional) receives a .ppm downscaled by `shrink`.
void requestNamedDump(const std::string& path, int shrink, std::function<void(const u8* rgb, u32 w, u32 h)> done);
}  // namespace recomp::efbcopy
