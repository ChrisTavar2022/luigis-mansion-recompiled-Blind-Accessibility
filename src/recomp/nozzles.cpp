// Unused air and water nozzles (F1 > Enhancements > Unused content, off by default).
//
// The Poltergust's nozzle is one actor (Luigi +0x7A4) that the game re-types on the fly:
// Luigi's mode selector fn_800B36B8 picks the mode (+0x1180: 1 vacuum, 2-4 fire/water/ice,
// 5 flashlight, 6 air) and, on a change, calls fn_800B9064(luigi, type), which turns the
// nozzle into character type 6 (`vhead`) for every mode except the flashlight, which uses
// type 7 (`wplight`). Types 8 (`wpwater`) and 9 (`wpair`) are registered right after them,
// with their own models and their own attach tables in fn_80078008, but the switch never
// selects them. With the option on, the mode selector runs unchanged and the nozzle is then
// re-typed through the same function: water spray -> wpwater, air -> wpair. No map ever
// requests those two models, so they are loaded first with the game's own model loader.
// Fire, ice and the vacuum keep `vhead`, the flashlight `wplight`. Docs: docs/UNUSED_CONTENT.md.
#include "port/install.h"
#include "port/settings.h"
#include "recomp/hle.h"
#include "recomp/runtime.h"
#include "recomp/version.h"

#include <cstdlib>

namespace recomp {
namespace {

constexpr u32 kNozzle = 0x7A4;   // nozzle actor (MoveObj); character type at +0x3C
constexpr u32 kMode = 0x1180;    // Poltergust mode chosen by fn_800B36B8
constexpr u32 kCharType = 0x3C;
constexpr u32 kModeWater = 3, kModeAir = 6, kModeLight = 5;
constexpr u32 kTypeHose = 6, kTypeLight = 7, kTypeWater = 8, kTypeAir = 9;

bool enabled() {
    static const bool sEnv = [] {
        const char* v = std::getenv("LM_NOZZLES");
        return v && *v && *v != '0';
    }();
    return sEnv || port::settings().enhancements.airWaterNozzles;
}

bool inMem1(u32 a) { return a >= 0x80000000u && a < 0x81800000u; }

// Character models: type table lbl_8033B590 (0x1C per type, +0 = model index) and model
// registry lbl_8034E46C (0x34 per model: +0 file name, +8 loaded .mdl, +0x30 load state).
// The game loads a model when a map needs it (fn_8006227C at map setup; per-event models via
// fn_800620F4 from the event code, JmpMessage). wpwater/wpair are never needed, so their
// entries stay empty and re-typing the nozzle to them would leave it without a model.
// Both releases (European fn_800B36B8/fn_800B9064 are identical; VA() maps the tables).
#define kTypeTable VA(0x8033B590)
#define kModelRegistry VA(0x8034E46C)

u32 modelEntry(u32 type) { return kModelRegistry + 0x34 * rd32(kTypeTable + 0x1C * type); }

// True once the type's model is resident; otherwise starts the game's own async load
// (fn_800620F4: disc read into the map's model heap, then the same setup as the map loader).
// The registry is cleared at every map setup, so this runs again after each map change.
bool modelReady(Ctx& c, u32 type) {
    const u32 e = modelEntry(type);
    if (rd32(e + 8)) return true;
    if (rd32(e + 0x30) == 0) {
        static const u32 sLoad = guestSymbol("fn_800620F4__Fi");
        const Ctx saved = c;
        guestCall(c, sLoad, rd32(kTypeTable + 0x1C * type));
        c = saved;
        log("[nozzle] loading %s", gstr(rd32(e)));
    }
    return false;
}

}  // namespace

// Luigi's Poltergust mode selector (once per Luigi update).
HLE(fn_800B36B8) {
    const u32 luigi = c.r[3];
    const u32 nozzle = inMem1(luigi) ? rd32(luigi + kNozzle) : 0;
    // Luigi's own type 1/0xB3 uses its 0xB5.. nozzle set, which has no air/water models.
    const u32 self = inMem1(luigi) ? rd32(luigi + kCharType) : 0;
    if (!inMem1(nozzle) || self == 1 || self == 0xB3) {
        callOriginal(c, "fn_800B36B8");
        return;
    }
    const bool on = enabled();
    callOriginal(c, "fn_800B36B8");

    const u32 mode = rd32(luigi + kMode);
    const u32 type = rd32(nozzle + kCharType);
    u32 want = type;
    // Load both as soon as the option is on, so the first spray already has its model.
    const bool waterReady = on && modelReady(c, kTypeWater);
    const bool airReady = on && modelReady(c, kTypeAir);
    if (waterReady && mode == kModeWater) want = kTypeWater;
    else if (airReady && mode == kModeAir) want = kTypeAir;
    else if (type == kTypeWater || type == kTypeAir) want = mode == kModeLight ? kTypeLight : kTypeHose;  // back to retail
    if (want != type) {
        static const u32 sRetype = guestSymbol("fn_800B9064");
        const Ctx saved = c;
        guestCall(c, sRetype, luigi, want);
        c = saved;
        static int sLogged = 0;
        if (sLogged++ < 40) log("[nozzle] mode %u: nozzle type %u -> %u", mode, type, want);
    }
}

}  // namespace recomp
