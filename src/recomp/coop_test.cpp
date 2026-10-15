// Test aids for the 2-player mode across the whole game (bosses, portrait ghosts, later areas).
// Testing only: nothing here runs unless its LM_ environment variable is set. Docs:
// docs/UNUSED_CONTENT.md (2-player mode, test aids).
#include "recomp/coop.h"
#include "recomp/coop_test.h"

#include "recomp/guest_util.h"
#include "recomp/hle.h"
#include "recomp/skip.h"
#include "recomp/version.h"

#include <dolphin/pad.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace recomp::cooptest {
namespace {

int envInt(const char* name, int def) {
    const char* v = std::getenv(name);
    return v && *v ? std::atoi(v) : def;
}

#define kTypeTable VA(0x8033B590)  // character types, 0x1C each: +0x18 name
#define kActorSlots VA(0x803D48A0)  // live characters
constexpr u32 kActorSlotCount = 128;

const char* typeName(u32 type) {
    if (type >= 0x29C8 / 0x1C) return "?";
    const u32 p = rd32(kTypeTable + 0x1C * type + 0x18);
    return gu::inMem1(p) ? reinterpret_cast<const char*>(hostp(p)) : "?";
}

// Every AI object (EnThought) that received a message this map, with the last state logged.
struct Known {
    u32 thought;
    u32 state;
};
std::vector<Known> sKnown;

u32 strategyState(u32 thought) {
    const u32 st = rd32(thought + 0x800);
    return gu::inMem1(st) ? rd16(st + 0xE) : 0xFFFF;
}

}  // namespace

void onMapSetup() {
    sKnown.clear();
    if (envInt("LM_BOSS_LOG", 0))
        log("[boss] map setup: map %u (guest frame %u)", rd32(VA(0x804D80C8)), skip::guestFrames());
}

void onMessage(u32 thought, u32 msg) {
    if (!gu::inMem1(thought)) return;
    for (const Known& k : sKnown)
        if (k.thought == thought) return;
    sKnown.push_back({thought, 0xFFFFFFFFu});
    (void)msg;
}

// LM_BOSS_LOG=<n>: every AI object's strategy state change (actor type, health, position), and
// every n game frames the full list.
void frame(Ctx& c) {
    // LM_BOSS_STAB=<hex addr>:<count>[,...]: logs EnemyStrategy state tables (filled at run time by
    // static initialisers): u16 state, then the init and behaviour member-function pointers.
    static bool sStabDone = false;
    if (!sStabDone && std::getenv("LM_BOSS_STAB")) {
        sStabDone = true;
        for (const char* q = std::getenv("LM_BOSS_STAB"); q && *q;) {
            unsigned a = 0, n = 0;
            if (std::sscanf(q, "%x:%u", &a, &n) == 2)
                for (u32 i = 0; i < n; i++) {
                    const u32 e = a + 0x1C * i;
                    log("[boss] table %08X state %X: init %08X behavior %08X", a, rd16(e), rd32(e + 0xC), rd32(e + 0x18));
                }
            q = std::strchr(q, ',');
            if (q) q++;
        }
    }
    static const int sLog = envInt("LM_BOSS_LOG", 0);
    if (!sLog) return;
    static u32 sFrame = 0;
    sFrame++;
    const u32 now = skip::guestFrames();
    for (Known& k : sKnown) {
        const u32 a = rd32(k.thought + 0x808);
        if (!gu::inMem1(a)) continue;
        const u32 s = strategyState(k.thought);
        if (s != k.state) {
            const u32 st = rd32(k.thought + 0x800);
            if (k.state == 0xFFFFFFFFu && gu::inMem1(st))
                log("[boss] %s strategy %08X: %08X %08X %08X %08X %08X %08X %08X %08X", typeName(rd32(a + 0x3C)), st, rd32(st),
                    rd32(st + 4), rd32(st + 8), rd32(st + 0xC), rd32(st + 0x10), rd32(st + 0x14), rd32(st + 0x18), rd32(st + 0x1C));
            log("[boss] f%u %s %08X state %X -> %X hp %d pos %.0f,%.0f,%.0f 40 %08X", now, typeName(rd32(a + 0x3C)), k.thought,
                k.state, s, s16(rd16(a + 0xFC)), gu::rdf(a + 0x44), gu::rdf(a + 0x48), gu::rdf(a + 0x4C), rd32(a + 0x40));
            k.state = s;
        }
    }
    if (sFrame % u32(sLog) == 0) {
        std::string line;
        for (u32 i = 0; i < kActorSlotCount; i++) {
            const u32 a = rd32(kActorSlots + 4 * i);
            if (!gu::inMem1(a)) continue;
            char b[96];
            std::snprintf(b, sizeof b, " %s@%.0f,%.0f,%.0f/hp%d/%X", typeName(rd32(a + 0x3C)), gu::rdf(a + 0x44), gu::rdf(a + 0x48),
                          gu::rdf(a + 0x4C), s16(rd16(a + 0xFC)), rd32(a + 0x40));
            line += b;
        }
        log("[boss] f%u actors:%s", now, line.c_str());
    }
    (void)c;
}

// ---- Boss driver ------------------------------------------------------------------------------
// LM_BOSS_DRIVE=<1|2>: drives that player's controller (port 1 or 2) through a boss fight with the
// game's own mechanics: walk to the fight's projectile, vacuum it, turn to the boss and let go of
// R to fire it, then vacuum the stunned boss and pull away. Targets are picked per map from the
// AI objects that received messages (onMessage).
namespace {

struct V2 {
    float x, z;
};

u32 actorOf(u32 thought) { return rd32(thought + 0x808); }

// Stick values that walk Luigi toward world direction (dx, dz): the stick is relative to the
// camera Luigi uses (+0x798: camera object, +0x8C target, +0x98 eye; fn_800B2084).
void stickToward(u32 luigi, float dx, float dz, float scale, s8& sx, s8& sy) {
    const u32 cam = rd32(luigi + 0x798);
    float fx = 0, fz = -1;
    if (gu::inMem1(cam)) {
        fx = gu::rdf(cam + 0x8C) - gu::rdf(cam + 0x98);  // target - eye
        fz = gu::rdf(cam + 0x94) - gu::rdf(cam + 0xA0);
        const float fl = std::sqrt(fx * fx + fz * fz);
        if (fl > 1e-3f) fx /= fl, fz /= fl;
        else fx = 0, fz = -1;
    }
    const float dl = std::sqrt(dx * dx + dz * dz);
    if (dl < 1e-3f) return;
    dx /= dl, dz /= dl;
    const float up = dx * fx + dz * fz;      // along the camera's forward
    const float right = dx * -fz + dz * fx;  // along its right
    sx = s8(std::clamp(right * 127.0f * scale, -127.0f, 127.0f));
    sy = s8(std::clamp(up * 127.0f * scale, -127.0f, 127.0f));
}

bool stateIn(u32 s, const char* list) {
    for (const char* q = list; q && *q; q++) {
        const char ch[2] = {*q, 0};
        if (std::strtoul(ch, nullptr, 16) == s) return true;
    }
    return false;
}

struct Found {
    u32 thought = 0, actor = 0;
    float d = 1e9f;
};

// The nearest actor (to Luigi l) of a type whose AI is in one of the states (hex digits).
Found nearest(u32 l, const char* type, const char* states) {
    Found f;
    if (!type || !*type) return f;
    for (const Known& k : sKnown) {
        const u32 a = actorOf(k.thought);
        if (!gu::inMem1(a) || std::strcmp(typeName(rd32(a + 0x3C)), type) != 0) continue;
        if (states && !stateIn(strategyState(k.thought), states)) continue;
        const float dx = gu::rdf(a + 0x44) - gu::rdf(l + 0x44), dz = gu::rdf(a + 0x4C) - gu::rdf(l + 0x4C);
        const float d = std::sqrt(dx * dx + dz * dz);
        if (d < f.d) f = {k.thought, a, d};
    }
    return f;
}

// One boss fight: the projectile the boss is hurt with (vacuumed while in catchStates, held on
// the nozzle while in heldStates, fired by letting go of R), and the boss's own AI states while
// it can be vacuumed (vulnStates) and while it is hooked (hookedStates).
struct BossCfg {
    u32 map;
    const char* proj;
    const char* catchStates;
    const char* heldStates;
    const char* boss;
    const char* vulnStates;
    const char* hookedStates;
    float fireMaxY;  // fire only while the boss is at most this high above Luigi
};
const BossCfg kBosses[] = {
    {10, "babyball", "34", "5", "bbaby", "C", "D", 300.0f},  // Chauncey: his balls
};

}  // namespace

void pad(PADStatus* st) {
    static const int sDrive = envInt("LM_BOSS_DRIVE", 0);
    if (sDrive != 1 && sDrive != 2) return;
    const int me = sDrive - 1;
    const u32 l = coop::cachedPlayer(me) ? coop::cachedPlayer(me) : (me == 0 ? rd32(kActorSlots) : 0);
    if (!gu::inMem1(l)) return;
    PADStatus& p = st[me];
    p.err = PAD_ERR_NONE;  // the port reads as connected (no device needs to be assigned)
    const u32 now = skip::guestFrames();
    const u32 map = rd32(VA(0x804D80C8));  // sCurMapNo
    const BossCfg* cfg = nullptr;
    for (const BossCfg& b : kBosses)
        if (b.map == map) cfg = &b;
    static const bool sFF = envInt("LM_BOSS_FF", 0) != 0;
    if (!cfg) {
        if (sFF) skip::setTestFastForward(false);
        return;
    }
    static const int sCY = envInt("LM_BOSS_CY", 0);
    static const int sNear = envInt("LM_BOSS_NEAR", 250);         // distance kept to a projectile
    static const int sVacNear = envInt("LM_BOSS_VAC_NEAR", 400);  // and to the stunned boss (hooked from below fails)
    enum Phase { Idle, Catch, Aim, Vacuum, Pull };
    static Phase sPhase = Idle;
    static bool sHold = false;
    Phase phase = Idle;
    Found t;
    const Found boss = nearest(l, cfg->boss, nullptr);
    const u32 bossState = boss.thought ? strategyState(boss.thought) : 0xFFFF;
    // LM_BOSS_FF=1: fast-forward (hidden, like a cutscene skip) while the boss is alive.
    if (sFF) skip::setTestFastForward(boss.actor && bossState != 0xFFFF && s16(rd16(boss.actor + 0xFC)) > 0);
    if (boss.actor && stateIn(bossState, cfg->hookedStates)) phase = Pull, t = boss;
    else if (boss.actor && stateIn(bossState, cfg->vulnStates)) phase = Vacuum, t = boss;
    else if (const Found held = nearest(l, cfg->proj, cfg->heldStates); held.actor && boss.actor && rd32(held.thought + 0x950) == u32(me))
        phase = Aim, t = boss;
    else if (const Found ball = nearest(l, cfg->proj, cfg->catchStates); ball.actor) phase = Catch, t = ball;
    if (phase != sPhase) {
        log("[bossdrive] f%u P%d phase %d -> %d (boss state %X hp %d)", now, sDrive, int(sPhase), int(phase), bossState,
            boss.actor ? s16(rd16(boss.actor + 0xFC)) : 0, rd32(l + 0x80C + 0xA8));
        sPhase = phase;
        sHold = phase == Aim || phase == Pull || phase == Vacuum;  // the stunned boss: vacuum at once
    }
    if (phase == Idle || !t.actor) return;
    const float dx = gu::rdf(t.actor + 0x44) - gu::rdf(l + 0x44), dz = gu::rdf(t.actor + 0x4C) - gu::rdf(l + 0x4C);
    const float d = std::max(1.0f, std::sqrt(dx * dx + dz * dz));
    // Facing: Luigi's (+0xF48/+0xF50) while walking; while vacuuming the nozzle's direction (the
    // Poltergust part +0x80C, its +0x700 points at the nozzle direction a fired object takes,
    // fn_800A3460), which the C stick turns ahead of the body.
    float fx = gu::rdf(l + 0xF48), fz = gu::rdf(l + 0xF50);
    const u32 nozzle = rd32(l + 0x80C + 0x700);
    if (sHold && gu::inMem1(nozzle)) {
        const float nx = gu::rdf(nozzle), nz = gu::rdf(nozzle + 8), nl = std::sqrt(nx * nx + nz * nz);
        if (nl > 1e-3f) fx = nx / nl, fz = nz / nl;
    }
    const float dot = (fx * dx + fz * dz) / d;
    const float cross = (fx * dz - fz * dx) / d;
    // While vacuuming, the stick walks without turning and the C stick turns Luigi (and tilts the
    // nozzle up/down); without R he faces where he walks.
    const s8 turn = s8(std::clamp(cross * 400.0f, -127.0f, 127.0f));
    switch (phase) {
    case Catch:
    case Vacuum:
        if (!sHold) {
            if (d > 400.0f) stickToward(l, dx, dz, 1.0f, p.stickX, p.stickY);
            else if (dot < 0.95f) stickToward(l, dx, dz, 0.3f, p.stickX, p.stickY);
            else sHold = true;
        } else {
            p.substickX = turn;
            p.substickY = s8(phase == Vacuum ? 100 : sCY);
            // Keep a vacuuming distance: objects are not caught right at the nozzle.
            const float keep = float(phase == Vacuum ? sVacNear : sNear);
            if (d > keep + 60.0f) stickToward(l, dx, dz, 0.5f, p.stickX, p.stickY);
            else if (d < keep - 60.0f) stickToward(l, -dx, -dz, 0.5f, p.stickX, p.stickY);
            if (d > 900.0f) sHold = false;
        }
        break;
    case Aim:  // projectile on the nozzle: turn to the boss, let go of R once aimed
    {
        p.substickX = turn;
        // Elevation: the nozzle's y component toward the boss's height (C stick up/down).
        // The fired object flies level with the nozzle: wait until the boss is low enough.
        const float ny = gu::inMem1(nozzle) ? gu::rdf(nozzle + 4) : 0.0f;
        p.substickY = s8(sCY);
        static u32 sAimFrames = 0;
        sAimFrames = dot > 0.995f && gu::rdf(t.actor + 0x48) - gu::rdf(l + 0x48) < cfg->fireMaxY ? sAimFrames + 1 : 0;
        if (sAimFrames >= 3) {
            sHold = false;
            log("[bossdrive] f%u P%d fire at the boss (d %.0f, boss y %.0f, nozzle %.2f,%.2f,%.2f)", now, sDrive, d,
                gu::rdf(t.actor + 0x48), fx, ny, fz);
        }
        break;
    }
    case Pull:  // hooked: hold R and walk away from it (the hunt aid's pull, ghost_lights.cpp)
        static const int sPullTurn = envInt("LM_BOSS_PULL_TURN", 0);
        p.substickX = sPullTurn ? turn : 0;
        p.substickY = 100;
        stickToward(l, -dx, -dz, 1.0f, p.stickX, p.stickY);
        sHold = true;
        break;
    default: break;
    }
    if (sHold) p.button |= 0x0020, p.triggerRight = 0xFF;  // R: vacuum
    if (now % 15 == 0)
        log("[bossdrive] f%u P%d phase %d at %.0f,%.0f target %s at %.0f,%.0f,%.0f d %.0f stick %d,%d c %d hold %d dot %.2f boss %X hp %d vac %08X",
            now, sDrive, int(phase), gu::rdf(l + 0x44), gu::rdf(l + 0x4C), typeName(rd32(t.actor + 0x3C)), gu::rdf(t.actor + 0x44),
            gu::rdf(t.actor + 0x48), gu::rdf(t.actor + 0x4C), d, p.stickX, p.stickY, p.substickX, int(sHold), dot, bossState,
            boss.actor ? s16(rd16(boss.actor + 0xFC)) : 0, rd32(l + 0x80C + 0xA8));
}

}  // namespace recomp::cooptest
