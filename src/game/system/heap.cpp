// Port of Unsorted/80005EB8 (heap setup). Based on lm-decomp's version of this TU.
#include "game/system/heap.h"

#include <dolphin/gx.h>

#include <JSystem/JKernel/JKRExpHeap.h>

namespace lm {

JKRExpHeap* gGameHeap;
JKRExpHeap* gSceneHeap;

static JKRExpHeap* sRootHeap;
static void* sGXBaseBuffer;
static u8 sCurHeapGroupId = 1;

// PORT: retail reserves 0x6AC000 bytes. 64-bit pointers and host-side objects
// (e.g. JUTTexture/GXTexObj wrappers) are larger, so give the game heap double.
static constexpr u32 kGameHeapSize = 0x6AC000 * 2;

void initializeGX() {
    gGameHeap->changeGroupID(1);
    sGXBaseBuffer = JKRAllocFromSysHeap(0x80000, 0x20);
    GXInit(sGXBaseBuffer, 0x80000);
}

void initHeaps() {
    sRootHeap = JKRExpHeap::createRoot(1, false);
    // (Retail allocates and frees the whole arena here to flush it from the
    // data cache with DCStoreRange; meaningless on PC.)
    gGameHeap = JKRExpHeap::create(kGameHeapSize, sRootHeap, false);
    gSceneHeap = JKRExpHeap::create(sRootHeap->getFreeSize(), nullptr, false);

    gGameHeap->becomeSystemHeap();
    gGameHeap->becomeCurrentHeap();
    setHeapGroup(0x1A);
}

void* allocFromCurrentHeap(u32 size, int align) {
    return JKRGetCurrentHeap()->alloc(size, align);
}

void freeToHeap(void* ptr) {
    JKRHeap::free(ptr, nullptr);
}

void freeSceneGroups(u8 lastGroup) {
    for (s32 i = 1; i <= lastGroup; i++) {
        gSceneHeap->freeGroup(i);
    }
}

u8 setHeapGroup(u8 group) {
    sCurHeapGroupId = group;
    gGameHeap->changeGroupID(group);
    return gSceneHeap->changeGroupID(group);
}

u8 getHeapGroup() {
    return sCurHeapGroupId;
}

void useSceneHeap() {
    gSceneHeap->becomeCurrentHeap();
}

s32 sceneHeapFreeSize() {
    return gSceneHeap->getFreeSize();
}

}  // namespace lm
