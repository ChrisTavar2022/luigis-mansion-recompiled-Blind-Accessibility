#pragma once
// Leftover room models visited for one warp (F1 > Enhancements > Unused content, "Unused rooms").
// A visit warps to the retail room the model belongs to, with Luigi standing inside it, and
// shows the leftover model for that visit only: either the room's archive (or .bin) is
// served with the leftover model in place of the retail one, or (room01A.bin) the game's own
// alternate-model slot for the room is selected. Collision, doors and objects stay the retail
// room's. The swap ends by itself when Luigi leaves the room or the map, or another warp is
// made. docs/UNUSED_CONTENT.md ("Unused rooms").
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
    int alternate;      // > 0: no file swap; select the room's alternate model n (fn_800129FC)
};
const LeftoverRoom* list(int* count);

void visit(int index);  // swap + warp
void cancel();          // another warp was requested
int active();           // index of the swap in effect, -1 if none
void onMapSetup();  // a map is being set up (guest thread): a pending visit has arrived
// Every frame (guest thread): applies an alternate model, ends the visit when Luigi leaves.
void onFrame(Ctx& c, bool inGame, bool luigi, int map, int room);

// Disc overlay used by hle_dvd.cpp (virtual entries distinct from the Hidden Mansion's).
s32 overlayEntry(const char* guestPath);  // -1 when not redirected
bool isOverlayEntry(s32 entry);
u32 overlayLength(s32 entry);
s32 overlayRead(s32 entry, void* dst, s32 length, s32 offset);

}  // namespace recomp::rooms
