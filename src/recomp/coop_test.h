#pragma once
// Test aids for the 2-player mode across the whole game (coop_test.cpp). Testing only: each
// part runs only when its LM_ environment variable is set.
#include "recomp/ctx.h"

struct PADStatus;

namespace recomp::cooptest {

void onMapSetup();                       // coop::onMapSetupFixes
void onMessage(u32 thought, u32 msg);    // EnThought::receiveMessage (fn_800C0F08), before the original
void frame(Ctx& c);                      // once per game-scene frame (fn_800112F8 hook)
void pad(PADStatus* st);                 // scripted-input runs: after the input script (hle_pad.cpp)

}  // namespace recomp::cooptest
