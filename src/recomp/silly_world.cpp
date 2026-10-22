// Silly options, world part (silly_world.h; docs/ENHANCEMENTS.md "Silly options: world").
#include "recomp/silly_world.h"

#include "port/settings.h"
#include "recomp/guest_util.h"
#include "recomp/hle.h"
#include "recomp/runtime.h"
#include "recomp/skip.h"
#include "recomp/unused2.h"
#include "recomp/version.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace recomp::sillyworld {
namespace {

const port::SillyWorldSettings& cfg() { return port::settings().sillyWorld; }

int envInt(const char* name, int def = 0) {
    const char* v = std::getenv(name);
    return v && *v ? std::atoi(v) : def;
}

// Live character slots (ghost_lights.cpp): lbl_803D48A0, count lbl_804D8750; type table
// lbl_8033B590 (0x1C per type: +0x08 u16 flags, flag 8 = ghost layer; +0x18 name).
#define kActorSlots VA(0x803D48A0)
#define kActorCount VA(0x804D8750)
#define kTypeTable VA(0x8033B590)
constexpr u32 kMaxSlots = 128, kTypeCount = 0x29C8 / 0x1C;

const char* typeName(u32 type) {
    if (type >= kTypeCount) return "?";
    const u32 p = rd32(kTypeTable + type * 0x1C + 0x18);
    return gu::inMem1(p) ? gstr(p) : "?";
}
bool isGhostType(u32 type) { return type < kTypeCount && (rd16(kTypeTable + type * 0x1C + 0x08) & 8); }

// ---- research aids ---------------------------------------------------------------------------
const int sJointLog = envInt("LM_SW_JOINT_LOG");
const int sSeLog = envInt("LM_SW_SE_LOG");
const int sPhysLog = envInt("LM_SW_PHYS_LOG");

// ---- moon gravity ----------------------------------------------------------------------------
// Spilled money is not made of actors: coins and bills are "grains" of the money managers
// (saGrainControl<CoinControl, MoneyParam> / <BillControl, MoneyParam>), whose physics
// constants are static floats in .sdata (one block per manager type):
//   coins (fn_80157030, one grain per call): 0x804D68B8 gravity 2.5 (subtracted from the
//     acceleration's y every frame), 0x804D68BC restitution 0.75 (floor bounce, fn_801580C8 ->
//     fn_8015243C), 0x804D68B4 air drag 0.99.
//   bills (fn_8015066C): 0x804D65FC gravity 1.5 (the fluttering bill's sink rate).
// Treasure items (gems, gold bars, pearls; EnItem strategies) fall in the state function
// fn_800CD1F4(mover): mover +0x30 = TParam "sGravity" (3.0 by default), +0x44 sResistance.
// Each wrapper scales the constant for the call only and puts it back.
float moonScale() { return cfg().moonGravity ? std::clamp(cfg().moonGravityScale, 0.05f, 1.0f) : 1.0f; }

struct FloatPatch {
    u32 addr = 0;
    u32 saved = 0;
    void apply(u32 a, float v) {
        addr = a;
        saved = rd32(a);
        gu::wrf(a, v);
    }
    ~FloatPatch() {
        if (addr) wr32(addr, saved);
    }
};

}  // namespace

}  // namespace recomp::sillyworld

using namespace recomp;
using namespace recomp::sillyworld;

// Coin grain update (r3 manager, r4 emitter, r5 grain).
HLE(fn_80157030) {
    const float k = moonScale();
    if (k == 1.0f) return callOriginal(c, "fn_80157030");
    FloatPatch g, r;
    g.apply(VA(0x804D68B8), gu::rdf(VA(0x804D68B8)) * k);
    // Bouncier: restitution 0.75 -> up to 0.88 at the lowest gravity.
    const float rest = gu::rdf(VA(0x804D68BC));
    r.apply(VA(0x804D68BC), std::min(0.9f, rest + (1.0f - k) * 0.17f));
    if (sPhysLog) {
        static u32 sN = 0;
        if (sN++ % 200 == 0) log("[silly] coin grain %08X pos %.0f,%.0f,%.0f vel %.2f,%.2f,%.2f state %u life %d", c.r[5],
                                 gu::rdf(c.r[5] + 0x20), gu::rdf(c.r[5] + 0x24), gu::rdf(c.r[5] + 0x28), gu::rdf(c.r[5] + 0x14),
                                 gu::rdf(c.r[5] + 0x18), gu::rdf(c.r[5] + 0x1C), rd32(c.r[5] + 0x74), s16(rd16(c.r[5] + 0x48)));
    }
    callOriginal(c, "fn_80157030");
}

// Bill grain update (r3 manager, r4 emitter, r5 grain).
HLE(fn_8015066C) {
    const float k = moonScale();
    if (k == 1.0f) return callOriginal(c, "fn_8015066C");
    FloatPatch g;
    g.apply(VA(0x804D65FC), gu::rdf(VA(0x804D65FC)) * k);
    callOriginal(c, "fn_8015066C");
}

// Treasure item falling state (r3 = the strategy's mover: +0x30 gravity).
HLE(fn_800CD1F4) {
    const float k = moonScale();
    if (sPhysLog) {
        static u32 sN = 0;
        const u32 actor = gu::inMem1(rd32(c.r[3] + 4)) ? rd32(rd32(c.r[3] + 4) + 0x808) : 0;
        if (sN++ % 60 == 0 && gu::inMem1(actor))
            log("[silly] item fall %08X type %s grav %.2f vel y %.2f pos y %.1f", actor, typeName(rd32(actor + 0x3C)),
                gu::rdf(c.r[3] + 0x30), gu::rdf(actor + 0x70), gu::rdf(actor + 0x48));
    }
    if (k == 1.0f) return callOriginal(c, "fn_800CD1F4");
    FloatPatch g;
    g.apply(c.r[3] + 0x30, gu::rdf(c.r[3] + 0x30) * k);
    callOriginal(c, "fn_800CD1F4");
}

// ---- confetti coins --------------------------------------------------------------------------
namespace recomp::sillyworld {
namespace {
u32 sUp = 0;  // guest vector (0, 1, 0): effect direction

// Colourful effects of the game's effect table (fn_80164F5C ids, lbl_80381B58 + 0xA90): the
// four coloured spark bursts of the Boo-key/portrait rewards (bkey_p/g/b/y_senko: pink, green,
// blue, yellow) and the gold sparkle of picked-up treasure.
const u32 kConfetti[] = {62, 64, 66, 68, 74};

void confetti(Ctx& c, u32 pos) {
    if (!cfg().confettiCoins || !gu::inMem1(pos)) return;
    if (!sUp && (sUp = unused2::extraAlloc(16, 8))) gu::wrf(sUp + 4, 1.0f);
    if (!sUp) return;
    for (u32 id : kConfetti) gu::call(c, GU_SYM("fn_80164F5C"), {id, pos, sUp});
}
}  // namespace
}  // namespace recomp::sillyworld

// Coins thrown out (furniture, chests: fn_800316FC, fn_801541D0): r3 position, r4 direction,
// r5 count, r6 lifetime, r7 flag, f1 speed.
HLE(fn_801589A8) {
    const u32 pos = c.r[3], count = c.r[5] & 0xFF;
    callOriginal(c, "fn_801589A8");
    if (count) confetti(c, pos);
}
// Bills thrown out (r3 position, r4 direction, r5 spread, r6 count, r7 lifetime, f1 speed).
HLE(fn_80151CC8) {
    const u32 pos = c.r[3], count = c.r[6] & 0xFF;
    callOriginal(c, "fn_80151CC8");
    if (count) confetti(c, pos);
}

// ---- Big Head Mode (research: joint dump) ---------------------------------------------------
namespace recomp::sillyworld {
namespace {
std::unordered_set<u32> sDumped;

u32 actorForModel(u32 obj) {
    const u32 n = std::min(rd32(kActorCount), kMaxSlots);
    for (u32 i = 0; i < n; i++) {
        const u32 a = rd32(kActorSlots + 4 * i);
        if (gu::inMem1(a) && rd32(a + 4) == obj) return a;
    }
    return 0;
}

void dumpJoints(u32 actor, u32 obj) {
    const u32 type = rd32(actor + 0x3C);
    if (!sDumped.insert(type).second) return;
    const u32 mdl = rd32(obj + 4);
    const u32 nj = rd16(mdl + 8), nodes = rd32(mdl + 0x30), joints = rd32(obj + 0x14);
    log("[silly-joints] type %u %s: %u joints, nodes %08X", type, typeName(type), nj, nodes);
    for (u32 n = 0; n < nj + 8 && gu::inMem1(nodes); n++) {
        const u32 nd = nodes + n * 16;
        const s16 j = s16(rd16(nd)), o2 = s16(rd16(nd + 2)), o4 = s16(rd16(nd + 4));
        if (j < 0 || u32(j) >= nj) break;
        const u32 m = joints + u32(j) * 0x30;
        log("[silly-joints]   node %2u joint %2d +2 %d +4 %d  world %.1f %.1f %.1f  scale %.2f", n, j, o2, o4, gu::rdf(m + 0x0C),
            gu::rdf(m + 0x1C), gu::rdf(m + 0x2C),
            std::sqrt(gu::rdf(m) * gu::rdf(m) + gu::rdf(m + 0x10) * gu::rdf(m + 0x10) + gu::rdf(m + 0x20) * gu::rdf(m + 0x20)));
    }
}
}  // namespace
}  // namespace recomp::sillyworld

// S3D model envelope matrices from the joints (second half of calcJoints fn_800589B0; the actor
// update fn_800613C8 calls it after the joint animation fn_8005A944).
HLE(fn_80058850) {
    if (sJointLog) {
        const u32 obj = c.r[3];
        const u32 actor = actorForModel(obj);
        if (actor && isGhostType(rd32(actor + 0x3C))) dumpJoints(actor, obj);
    }
    callOriginal(c, "fn_80058850");
}

namespace recomp::sillyworld {

void onFrame(Ctx& c) {
    // Test aid: LM_SW_TREASURE=<guest frame>,... throws 15 coins, 9 bills and a gold bar out in
    // front of Luigi as furniture does (fn_801589A8 / fn_80151CC8 / fn_800E8904).
    static const char* sEnv = std::getenv("LM_SW_TREASURE");
    if (sEnv) {
        const u32 now = skip::guestFrames();
        for (const char* p = sEnv; p && *p;) {
            if (u32(std::strtoul(p, nullptr, 10)) == now) {
                const u32 luigi = gu::call(c, GU_SYM("fn_800E9750__Q24Koga9EnManagerFl"), {0});
                static u32 sVec = 0;
                if (!sVec) sVec = unused2::extraAlloc(0x20, 8);
                if (gu::inMem1(luigi) && sVec) {
                    const float fx = gu::rdf(luigi + 0xF48), fz = gu::rdf(luigi + 0xF50);
                    gu::wrf(sVec + 0, gu::rdf(luigi + 0x44) + fx * 180.0f);
                    gu::wrf(sVec + 4, gu::rdf(luigi + 0x48) + 80.0f);
                    gu::wrf(sVec + 8, gu::rdf(luigi + 0x4C) + fz * 180.0f);
                    gu::wrf(sVec + 0x10, -fx * 0.4f);
                    gu::wrf(sVec + 0x14, 0.92f);
                    gu::wrf(sVec + 0x18, -fz * 0.4f);
                    gu::call(c, GU_SYM("fn_801589A8"), {sVec, sVec + 0x10, 15, 0x1E0, 0}, {25.0});
                    gu::call(c, GU_SYM("fn_80151CC8"), {sVec, sVec + 0x10, 0x1E, 9, 0x1E0}, {25.0});
                    gu::call(c, GU_SYM("fn_800E8904"), {0xCB, sVec, 0});
                    log("[silly] f%u treasure thrown at %.0f,%.0f,%.0f", now, gu::rdf(sVec), gu::rdf(sVec + 4), gu::rdf(sVec + 8));
                }
            }
            const char* comma = std::strchr(p, ',');
            p = comma ? comma + 1 : nullptr;
        }
    }
}

void onSeStarted(Ctx& c, u32 id, u32 handle) {
    (void)c;
    const u32 snd = gu::inMem1(handle) ? rd32(handle) : 0;
    if (sSeLog) log("[silly-se] f%u id %08X sound %08X", skip::guestFrames(), id, snd);
}

void ghostMaterialColor(u32 actor, u32 colorPtr) {
    (void)actor;
    (void)colorPtr;
}
bool ghostGlowColor(u32 actor, u32 rgb[3]) {
    (void)actor;
    (void)rgb;
    return false;
}
void discoAmbient(u8 rgb[3]) { (void)rgb; }
void discoLight(u32 id, float color[3]) {
    (void)id;
    (void)color;
}
bool discoOn() { return cfg().disco; }

}  // namespace recomp::sillyworld

namespace recomp::silly {
void drawWorldOptions() {}
}  // namespace recomp::silly
