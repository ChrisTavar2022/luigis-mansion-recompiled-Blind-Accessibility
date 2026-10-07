#pragma once
// Unused maps 7 and 8 (docs/UNUSED_CONTENT.md, "Unused areas"). Their map archives as shipped
// cannot be loaded by the retail game: neither has the j3d_effect/k03-elem1_eyes model every
// map setup loads, and map 8's JMP tables are in an older field layout with five tables
// missing. The port serves rebuilt copies of /Map/map7.szp and /Map/map8.szp, made at load
// time from the user's disc: the missing model is taken from map 5 (map 7's twin) and map 8's
// tables are converted field by field (by name hash) to the layout of map 5's tables.
#include "recomp/ctx.h"

namespace recomp::unused_maps {

// Disc overlay used by hle_dvd.cpp (virtual entries distinct from the other overlays').
s32 overlayEntry(const char* guestPath);  // -1 when not redirected
bool isOverlayEntry(s32 entry);
u32 overlayLength(s32 entry);
s32 overlayRead(s32 entry, void* dst, s32 length, s32 offset);

}  // namespace recomp::unused_maps
