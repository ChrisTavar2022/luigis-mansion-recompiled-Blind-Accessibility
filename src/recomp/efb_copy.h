#pragma once
// EFB copies to RAM, runtime side (efb_copy.cpp, docs/RENDERING.md).
#include "recomp/ctx.h"

namespace recomp::efbcopy {
void onFrame(u32 frameNo);         // guest frame boundary: settings, writebacks, stats
void cpuRead(u32 addr, u32 size);  // DCInvalidateRange: the CPU will read guest memory
void* dumpTarget(u32 frameNo);     // LM_FRAME_DUMP: host buffer to copy this frame's EFB into, or null
// Interpolation tests (LM_INTERP_TEST with LM_FRAME_DUMP): a buffer whose copy is written to
// <dump dir>/<name>.ppm when its readback lands; namedDumpWritten tells when it did.
void* namedDumpTarget(const char* name);
bool namedDumpWritten(const void* target);
void applyWritebacks();  // host thread, while the guest is held
}  // namespace recomp::efbcopy
