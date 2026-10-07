#pragma once
// Runtime services shared by the HLE modules.
#include "recomp/ctx.h"

namespace recomp {

constexpr double kTimerClock = 40500000.0;  // OS timer: bus clock / 4

// The recompiled game: set once at startup, before any guest code runs (module_loader.cpp).
extern const GameModule* g_module;

GuestFn lookupFunc(u32 addr);
[[noreturn]] void callMissing(Ctx& c, u32 addr);  // indirect call to an address with no function
void initTimeBase();
void initTrace();
void initWatch();
void watchRange(u32 addr, u32 len, const char* what);  // HLE bulk writes
u64 timeBase();

namespace hw {
u32 viRead(u32 addr, int size);        // hle_vi.cpp
void fifoWrite(u32 value, int size);  // hle_gx.cpp
// Optional rewrite of values the game writes to the GX write-gather pipe (set only for the
// duration of a hook, e.g. hidden_mansion_hooks.cpp's mirrored screen-copy quad).
extern u32 (*fifoFilter)(u32 value, int size);
}  // namespace hw

}  // namespace recomp
