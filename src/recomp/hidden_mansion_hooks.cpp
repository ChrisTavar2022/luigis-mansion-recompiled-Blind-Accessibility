// PAL Hidden Mansion tuning on the US build: hooks (tools/recomp/hooks.txt) that run the
// original US function and adjust its inputs or results while hm::palActive().
// What each change is, and where PAL makes it: docs/HIDDEN_MANSION.md ("Difficulty and tuning").
#include "recomp/hidden_mansion.h"
#include "recomp/hle.h"

#include <bit>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>

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

// Calls a guest function with r3 and f1..f3 set; the caller's registers are left untouched.
u32 guestCallF(Ctx& c, u32 fn, u32 r3, double f1, double f2 = 0, double f3 = 0) {
    const Ctx saved = c;
    c.r[3] = r3;
    c.f[1] = f1;
    c.f[2] = f2;
    c.f[3] = f3;
    c.ps1[1] = f1;
    c.ps1[2] = f2;
    c.ps1[3] = f3;
    c.call(fn);
    const u32 r = c.r[3];
    c = saved;
    return r;
}

u32 guestCall1(Ctx& c, u32 fn, u32 a0) {
    const Ctx saved = c;
    const u32 r = guestCall(c, fn, a0);
    c = saved;
    return r;
}

constexpr u32 kCurMapNo = 0x804D80C8;  // sCurMapNo

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

// Game-scene setup for sCurMapNo. Dev aid for testing the boss changes: LM_START_MAP=<n>
// loads map n (9 Bowser, 10 Chauncey, 11 Boolossus) the first time the mansion (map 2) loads.
HLE(fn_8000C188) {
    static const int sMap = [] {
        const char* env = std::getenv("LM_START_MAP");
        return env && *env ? std::atoi(env) : 0;
    }();
    static bool sDone = false;
    if (sMap > 0 && !sDone && rd32(kCurMapNo) == 2) {
        sDone = true;
        log("[hm] LM_START_MAP: loading map %d instead of the mansion", sMap);
        wr32(kCurMapNo, u32(sMap));
    }
    callOriginal(c, "fn_8000C188");
}

// ---- Bosses ------------------------------------------------------------------------------

// Luigi's per-frame update. The US Luigi already has a "Boolossus" movement mode behind byte
// +0x134C (PAL +0x1350): faster run (38 vs 17/18), its own animations, vacuum handling and a
// sound loop, read by eight Luigi functions. US never enables it; PAL turns it on in map 11
// (the Boolossus fight) in the hidden mansion once Luigi has spent 15 frames there, and keeps
// it off everywhere else. The 15-frame counter (PAL Luigi +0x134C) is kept host-side.
HLE(fn_800B783C) {
    static u32 sLuigi = 0, sFrames = 0;
    const u32 self = c.r[3];
    if (hm::palActive()) {
        if (rd32(kCurMapNo) == 11) {
            if (self != sLuigi) sLuigi = self, sFrames = 0;
            ++sFrames;
            const bool on = sFrames > 15;
            if (on) HM_FIRST("fn_800B783C Boolossus movement mode");
            hostp(self + 0x134C)[0] = on ? 1 : 0;
        } else {
            sFrames = 0;
            hostp(self + 0x134C)[0] = 0;
        }
    }
    callOriginal(c, "fn_800B783C");
    static const bool sTrace = std::getenv("LM_HM_TRACE_LUIGI") != nullptr;  // dev: position log
    static u32 sTick = 0;
    if (sTrace && ++sTick % 30 == 0)
        log("[hm] luigi map %u pos %.1f %.1f %.1f mode %u", rd32(kCurMapNo), rdf(self + 0x44), rdf(self + 0x48),
            rdf(self + 0x4C), u32(rd8(self + 0x134C)));
}

// Luigi on a damaging floor: US hurts him every 2nd frame of the floor timer (+0x11CC); PAL
// hidden uses the 8-frame pattern 1,1,0,1,1,0,1,0. The timer is presented to the original so
// that its "every 2nd frame" test gives PAL's answer, then advanced as usual.
HLE(fn_800B41E0) {
    if (!hm::palActive()) return callOriginal(c, "fn_800B41E0");
    static constexpr u8 kPattern[8] = {1, 1, 0, 1, 1, 0, 1, 0};
    const u32 self = c.r[3];
    const float tf = rdf(self + 0x11CC);
    const u32 t = tf >= 0 ? u32(tf) : 0;
    const u32 shown = kPattern[t & 7] ? (t & ~1u) : (t | 1u);
    wrf(self + 0x11CC, float(shown));
    callOriginal(c, "fn_800B41E0");
    if (rdf(self + 0x11CC) == float(shown) + 1.0f) {
        wrf(self + 0x11CC, float(t) + 1.0f);
        if (kPattern[t & 7] != ((t & 1) == 0)) HM_FIRST("fn_800B41E0 damaging-floor pattern");
    } else if (shown != 0 && rdf(self + 0x11CC) == float(shown)) {
        wrf(self + 0x11CC, tf);  // timer untouched by the original: put the real value back
    }
}

// Chauncey's ball spawner: balls 0 and 1 of each volley start at z = +700 in the PAL hidden
// mansion instead of -700 (ball 2 keeps z = -500). The spawn position is set again on the ball.
HLE(fn_8009F1DC) {
    if (!hm::palActive()) return callOriginal(c, "fn_8009F1DC");
    const u32 self = c.r[3];
    const u32 before = rd32(self + 0x1F4);
    callOriginal(c, "fn_8009F1DC");
    if (before == 0 || rd32(self + 0x1F4) != before - 1) return;  // nothing spawned
    const u32 idx = (5 - before) % 3;
    if (idx == 2) return;
    const u32 ball = rd32(self + 0x21C + idx * 4);
    if (!inMem1(ball)) return;
    static const u32 setPos = guestSymbol("fn_80067CB0");
    guestCallF(c, setPos, ball, rdf(ball + 0x44), rdf(ball + 0x48), 700.0);
    HM_FIRST("fn_8009F1DC Chauncey ball spawn z = +700");
}

// KtEnMoku2 (Chauncey's rocking horses) charge: in the PAL hidden mansion the horse weaves
// side to side (x velocity 100*sin(20 deg * t), t = the global timer at 0x804D8BF8), faces
// its direction of travel, and its hit shapes against Luigi are twice as large.
HLE(fn_800A184C) {
    if (!hm::palActive()) return callOriginal(c, "fn_800A184C");
    const u32 actor = rd32(c.r[3] + 4);
    const u32 model = inMem1(actor) ? rd32(actor + 0x808) : 0;
    if (!inMem1(model)) return callOriginal(c, "fn_800A184C");

    // Weave: same sine table and angle units as the game (0x10000 = 360 degrees).
    const float t = rdf(0x804D8BF8);
    const s32 ang = s32(65536.0f * (20.0f * t) / 360.0f);
    const float s = rdf(0x80218238 + ((u32(ang) >> 2) & 0x3FFC));
    const float vx = 100.0f * s;
    wrf(model + 0x6C, vx);
    static const u32 atan2Fn = guestSymbol("fn_8000965C");
    const u32 yaw = guestCallF(c, atan2Fn, 0, rdf(model + 0x74), vx);
    wr16(model + 0x88, u16(yaw));
    wr16(model + 0x8A, u16(yaw));

    // Hit shapes x2 for the duration of the call.
    static const u32 shapeFn = guestSymbol("fn_800AC2B4");
    const u32 desc = guestCall1(c, shapeFn, model);
    u32 base = 0, n = 0;
    if (inMem1(desc)) {
        const u32 type = rd32(desc);
        if (type == 1) base = rd32(desc + 4), n = 2;       // two scalars (PAL scales words 0-1)
        else if (type == 2) base = rd32(desc + 8), n = 6;  // two vectors
    }
    u32 saved[6] = {};
    if (inMem1(base)) {
        for (u32 i = 0; i < n; i++) {
            saved[i] = rd32(base + 4 * i);
            wrf(base + 4 * i, rdf(base + 4 * i) * 2.0f);
        }
    } else {
        n = 0;
    }
    HM_FIRST("fn_800A184C rocking horse weave + hit shapes x2");
    callOriginal(c, "fn_800A184C");
    for (u32 i = 0; i < n; i++) wr32(base + 4 * i, saved[i]);
}

// Bowser (King Boo) bomb volleys. PAL adds a byte to the boss strategy (+0x1EE) that flips each
// time all three bombs are gone (hidden mansion only), and a new bomb state: while the byte is
// set, bombs after the first in a volley use it instead of the normal "thrown" state. The new
// state is the thrown state with timers 100/30 instead of 300/90 (PAL's normal thrown state also
// sets the second timer, 90). Host-side: the flip per boss, and which bombs were thrown fast.
namespace {
std::unordered_map<u32, bool> sBowserAlt;
std::unordered_set<u32> sFastBombs;
}  // namespace

HLE(fn_800A47BC) {  // Bowser strategy init
    const u32 self = c.r[3];
    callOriginal(c, "fn_800A47BC");
    sBowserAlt.erase(self);
}

HLE(fn_800A441C) {  // Bowser chase state: switches to state 0x12 when all three bombs are gone
    const u32 self = c.r[3];
    const u32 before = rd16(self + 0xC);
    callOriginal(c, "fn_800A441C");
    if (hm::palActive() && before != 0x12 && rd16(self + 0xC) == 0x12) {
        sBowserAlt[self] = !sBowserAlt[self];
        HM_FIRST("fn_800A441C Bowser volley alternation");
    }
}

HLE(fn_800A5BA0) {  // Bowser throws the next bomb of the volley
    const u32 self = c.r[3];
    const u32 before = rd32(self + 0x22C);
    callOriginal(c, "fn_800A5BA0");
    const u32 after = rd32(self + 0x22C);
    if (!hm::palActive() || after + 1 != before || after == 0 || !sBowserAlt[self]) return;
    const u32 bomb = rd32(self + 0x1AC + (3 - after) * 4);
    if (inMem1(bomb)) sFastBombs.insert(bomb);
}

HLE(fn_800A926C) {  // bomb: enter "thrown" state
    const u32 self = c.r[3];
    callOriginal(c, "fn_800A926C");
    if (!hm::palActive()) {
        sFastBombs.erase(self);
        return;
    }
    if (sFastBombs.erase(self)) {
        wrf(self + 0x1D0, 100.0f);
        wrf(self + 0x1D4, 30.0f);
        HM_FIRST("fn_800A926C Bowser fast bomb");
    } else {
        wrf(self + 0x1D4, 90.0f);
    }
}

}  // namespace recomp
