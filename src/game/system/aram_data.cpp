#include "game/system/aram_data.h"
#include "game/system/dvd_file.h"
#include "game/system/heap.h"

#include <JSystem/JKernel/JKRAram.h>
#include <JSystem/JKernel/JKRHeap.h>
#include <JSystem/JKernel/JKRMemArchive.h>

namespace lm {

AramData gGameboyAramData;
AramData gGuidemapAramData;
AramData gResTitleAramData;
AramData gResPauseAramData;
AramData gResListAramData;

void AramData::stage(const void* yay0, u32 yay0Size) {
    JKRAramBlock* block = JKRMainRamToAram(static_cast<u8*>(const_cast<void*>(yay0)), 0, yay0Size,
                                           EXPAND_SWITCH_UNKNOWN0, 0, nullptr, -1, nullptr);
    address = block->getAddress();
    size = block->getSize();
}

void AramData::beginLoad() {
    // Retail allocates from the tail of the current heap (alignment -0x20).
    staging = allocFromCurrentHeap(size, -0x20);
    // PORT: retail starts an asynchronous ARAM DMA here and waits for it in
    // finishLoadAndMount(); emulated ARAM is plain memory, so copy now.
    JKRAramToMainRam(address, static_cast<u8*>(staging), size, EXPAND_SWITCH_UNKNOWN0, 0, nullptr, -1, nullptr);
}

JKRMemArchive* AramData::finishLoadAndMount() {
    auto* yay0 = static_cast<DvdFileBlock*>(staging);
    auto* archiveData = static_cast<DvdFileBlock*>(allocFromCurrentHeap(yay0->decompressedSize(), 0x20));
    yay0->decompressYay0(archiveData);
    freeToHeap(staging);
    staging = nullptr;
    return JKR_NEW JKRMemArchive(archiveData, 0, JKRMEMBREAK_FLAG_UNKNOWN1);
}

}  // namespace lm
