#pragma once
// Leftover room models visited for one warp (F1 > Enhancements > Unused content, "Unused rooms").
// A visit warps to the retail room the model belongs to and, only for that visit, serves the
// room's archive (or .bin) with the leftover model in place of the retail one; collision,
// doors and objects stay the retail room's. The swap ends by itself when Luigi leaves the
// room or the map, or another warp is made. docs/UNUSED_CONTENT.md ("Unused rooms").
#include "recomp/ctx.h"

namespace recomp::rooms {

struct LeftoverRoom {
    const char* name;
    const char* note;
    int map;            // map the visit warps to
    int room;           // room the swap is tied to (-1: the whole map)
    float spawn[3];     // Luigi's start position (all 0: the map's default appear point)
    const char* target; // disc path the game loads for the retail room
    const char* targetMember;  // member of `target` replaced (nullptr: the whole file)
    const char* source; // disc path holding the leftover model
    const char* sourceMember;  // member inside `source` (nullptr: the whole file)
};
const LeftoverRoom* list(int* count);

void visit(int index);  // swap + warp
void cancel();          // another warp was requested
int active();           // index of the swap in effect, -1 if none
void onFrame(bool inGame, int map, int room);  // ends the visit when Luigi leaves

// Disc overlay used by hle_dvd.cpp (virtual entries distinct from the Hidden Mansion's).
s32 overlayEntry(const char* guestPath);  // -1 when not redirected
bool isOverlayEntry(s32 entry);
u32 overlayLength(s32 entry);
s32 overlayRead(s32 entry, void* dst, s32 length, s32 offset);

// Pending Luigi start position for the next map setup (consumed by the appear-point hook).
bool takeSpawn(float out[3]);

}  // namespace recomp::rooms
