#pragma once
// Runtime services shared by the HLE modules.
#include "recomp/ctx.h"

namespace recomp {

constexpr double kTimerClock = 40500000.0;  // OS timer: bus clock / 4

GuestFn lookupFunc(u32 addr);
void initTimeBase();
u64 timeBase();

namespace hw {
u32 viRead(u32 addr, int size);        // hle_vi.cpp
void fifoWrite(u32 value, int size);  // hle_gx.cpp
}  // namespace hw

}  // namespace recomp
