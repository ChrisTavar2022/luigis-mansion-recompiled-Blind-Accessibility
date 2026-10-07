#pragma once
// Gameplay / difficulty options (F1 > Enhancements > Gameplay; port::GameplaySettings).
// Implemented as hooks on game functions, keyed by symbol name (tools/recomp/hooks.txt), in
// src/recomp/gameplay_hooks.cpp. Description and verification: docs/GAMEPLAY_OPTIONS.md.
//
// Version note: the hooks name US (GLME01) symbols, and a few use US data addresses (room
// table, struct offsets) that are marked "US" where used. A PAL-base install needs the PAL
// equivalents (tools/pal_diff.py var/field map them).
#include "port/settings.h"
#include "recomp/ctx.h"

namespace recomp::gameplay {

// Effective settings: the saved ones, with LM_GAMEPLAY test overrides applied, e.g.
// LM_GAMEPLAY="damage=2,health=0.5,ghosthp=3,vacuum=0.5,hearts=0,dark=1,ambient=0.4".
port::GameplaySettings current();

// Room ambient multiplier: 0.2 while the PAL Hidden Mansion rules apply or "PAL darkness
// everywhere" is on, times the ambient slider (the slider composes with PAL darkness).
float ambientFactor();

// "Always ride the Poltergust": whether the riding movement mode may be on for this Luigi
// right now (it is suspended where it would break the game; see docs/GAMEPLAY_OPTIONS.md).
bool rideAllowed(u32 luigi);

}  // namespace recomp::gameplay
