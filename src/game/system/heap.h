#pragma once
// Game heap layout (retail: Unsorted/80005EB8).
//
//   root (whole MEM1 arena)
//   +-- gGameHeap   lbl_804D8014: long-lived data; system + current heap at boot
//   +-- gSceneHeap  lbl_804D8018: the rest of MEM1; freed group-by-group between scenes
//
// Allocations are tagged with a "group id" so a scene can free everything it
// allocated in one call (freeSceneGroups).

#include <dolphin/types.h>

class JKRExpHeap;

namespace lm {

extern JKRExpHeap* gGameHeap;   // lbl_804D8014
extern JKRExpHeap* gSceneHeap;  // lbl_804D8018

void initHeaps();                          // fn_80005F10
void initializeGX();                       // initializeGX
void* allocFromCurrentHeap(u32 size, int align);  // fn_80006080
void freeToHeap(void* ptr);                // fn_80006138
void freeSceneGroups(u8 lastGroup);        // fn_8000615C
u8 setHeapGroup(u8 group);                 // fn_800061B8 (returns previous scene-heap group)
u8 getHeapGroup();                         // getCurHeapGroupId
void useSceneHeap();                       // fn_80006204
s32 sceneHeapFreeSize();                   // fn_80006228

}  // namespace lm
