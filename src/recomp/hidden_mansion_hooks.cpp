// PAL Hidden Mansion tuning on the US build: hooks (tools/recomp/hooks.txt) that run the
// original US function and adjust its inputs or results while hm::palActive().
// What each change is, and where PAL makes it: docs/HIDDEN_MANSION.md ("Difficulty and tuning").
#include "recomp/hidden_mansion.h"
#include "recomp/hle.h"

#include <bit>

namespace recomp {
namespace {

float rdf(u32 a) { return std::bit_cast<float>(rd32(a)); }
void wrf(u32 a, float v) { wr32(a, std::bit_cast<u32>(v)); }
bool inMem1(u32 a) { return a >= 0x80000000u && a < 0x81800000u; }

// One log line the first time each hook changes something (verification aid).
#define HM_FIRST(what)                                      \
    do {                                                    \
        static bool sLogged = false;                        \
        if (!sLogged) {                                     \
            sLogged = true;                                 \
            log("[hm] PAL rule applied: %s", what);         \
        }                                                   \
    } while (0)

// Runs the original with some .sdata2 literals temporarily replaced (they are only read by
// that function in the call's dynamic extent; see the per-hook notes).
struct ScopedLiteral {
    u32 addr, saved;
    ScopedLiteral(u32 a, float v) : addr(a), saved(rd32(a)) { wrf(a, v); }
    ~ScopedLiteral() { wr32(addr, saved); }
};

// The player actor (fn_800AD39C(0)); the caller's registers are left untouched.
u32 player(Ctx& c) {
    static const u32 fn = guestSymbol("fn_800AD39C");
    const Ctx saved = c;
    const u32 p = guestCall(c, fn, 0);
    c = saved;
    return p;
}

constexpr u32 kHeartCounter = 0x6C8;  // player field compared by the heart-item logic
constexpr s32 kUsHiddenThreshold = 400, kPalHiddenThreshold = 1000;

}  // namespace

// Heart-item check: true when the player's counter reaches the threshold
// (normal 200, US hidden 400, PAL hidden 1000).
HLE(fn_800E70BC) {
    callOriginal(c, "fn_800E70BC");
    if (hm::palActive()) {
        const u32 r3 = c.r[3];
        const u32 p = player(c);
        c.r[3] = inMem1(p) ? u32(s32(rd32(p + kHeartCounter)) >= kPalHiddenThreshold) : r3;
        HM_FIRST("fn_800E70BC heart threshold 1000");
    }
}

// Item selection switch; one case picks between two items on the same threshold. That case
// reads the counter once, so while PAL rules apply a value in [400, 1000) is presented as 399
// for the duration of the call (PAL: below 1000 takes the "under" branch).
HLE(fn_800E7698) {
    if (!hm::palActive()) return callOriginal(c, "fn_800E7698");
    const u32 p = player(c);
    const s32 v = inMem1(p) ? s32(rd32(p + kHeartCounter)) : 0;
    const bool adjust = v >= kUsHiddenThreshold && v < kPalHiddenThreshold;
    if (adjust) {
        wr32(p + kHeartCounter, u32(kUsHiddenThreshold - 1));
        HM_FIRST("fn_800E7698 heart threshold 1000");
    }
    callOriginal(c, "fn_800E7698");
    if (adjust && rd32(p + kHeartCounter) == u32(kUsHiddenThreshold - 1)) wr32(p + kHeartCounter, u32(v));
}

// Chauncey (KtEnBaby) approach state: PAL hidden uses 40 for both the distance test (US 15,
// literal 0x804DB240, only this function) and the move speed (US 20, literal 0x804DB234, also
// read by fn_8009F4AC, which this function does not call).
HLE(fn_8009FD54) {
    if (!hm::palActive()) return callOriginal(c, "fn_8009FD54");
    ScopedLiteral dist(0x804DB240, 40.0f), speed(0x804DB234, 40.0f);
    HM_FIRST("fn_8009FD54 Chauncey distance/speed 40");
    callOriginal(c, "fn_8009FD54");
}

// Chauncey "POYON" state entry: PAL hidden also sets the model's +0xD8 to 2.0.
HLE(fn_8009FCA8) {
    const u32 self = c.r[3];
    callOriginal(c, "fn_8009FCA8");
    if (!hm::palActive()) return;
    const u32 actor = rd32(self + 4);
    const u32 model = inMem1(actor) ? rd32(actor + 0x808) : 0;
    if (inMem1(model)) {
        wrf(model + 0xD8, 2.0f);
        HM_FIRST("fn_8009FCA8 Chauncey +0xD8 = 2.0");
    }
}

// Enemy strategy (state table 0x8034BB48) state entry: timer 400 -> 250 in the PAL hidden mansion.
HLE(fn_8006D1B8) {
    const u32 self = c.r[3];
    callOriginal(c, "fn_8006D1B8");
    if (hm::palActive() && rd32(self + 0x1CC) == 400) {
        wr32(self + 0x1CC, 250);
        HM_FIRST("fn_8006D1B8 state timer 250");
    }
}

// Room ambient colour: PAL multiplies the room's ambient colour by 0.2 in the hidden mansion
// (both the lit-room x0.6 path and the unlit path). The current room record's RGB bytes are
// scaled for the duration of the call.
HLE(fn_800168EC) {
    if (!hm::palActive()) return callOriginal(c, "fn_800168EC");
    const u32 roomNo = rd32(0x803A3CA0 + 0xC) >> 24;
    const u32 rooms = rd32(0x804D8178);
    const u32 rec = inMem1(rooms) ? rd32(rooms + roomNo * 0x14C + 0x18) : 0;
    if (!inMem1(rec)) return callOriginal(c, "fn_800168EC");
    u8 saved[3];
    for (int i = 0; i < 3; i++) {
        saved[i] = u8(rd8(rec + 0x2C + i));
        hostp(rec + 0x2C + i)[0] = u8(float(saved[i]) * 0.2f);
    }
    HM_FIRST("fn_800168EC room ambient x0.2");
    callOriginal(c, "fn_800168EC");
    for (int i = 0; i < 3; i++) hostp(rec + 0x2C + i)[0] = saved[i];
}

}  // namespace recomp
