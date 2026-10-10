#pragma once
// LM_LIGHTNING test aid (lightning_test.cpp): forced lightning strikes in the room update.
namespace recomp::lightning {
bool beginRoomUpdate();           // before fn_800168EC: true if this call forces a strike
void endRoomUpdate(bool strike);  // after it
}  // namespace recomp::lightning
