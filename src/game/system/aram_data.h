#pragma once
// Compressed resource archives parked in ARAM between uses (retail: LMAramData,
// fn_80066DB8 / fn_80066E24). The boot scene stages the Yay0 archives from the
// static data; screens pull them back into main RAM and mount them on demand.
#include <dolphin/types.h>

class JKRMemArchive;

namespace lm {

struct AramData {
    u32 address;    // ARAM address of the Yay0 data
    u32 size;       // compressed size
    void* staging;  // main-RAM copy of the Yay0 data while a load is in flight
    s32 _C;
    void* command;  // JKRAMCommand on console

    // Boot: copy compressed data into ARAM.
    void stage(const void* yay0, u32 yay0Size);
    // fn_80066DB8: start bringing the data back into main RAM (current heap).
    void beginLoad();
    // fn_80066E24: finish the load, decompress, and mount the result.
    JKRMemArchive* finishLoadAndMount();
};

extern AramData gGameboyAramData;   // sGameboyAramData
extern AramData gGuidemapAramData;  // sGuidemapAramData
extern AramData gResTitleAramData;  // sResTitleAramData
extern AramData gResPauseAramData;  // sResPauseAramData
extern AramData gResListAramData;   // sResListAramData

}  // namespace lm
