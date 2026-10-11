// World lights (F1 > Visuals > Enhanced lighting > World lights; docs/RENDERING.md,
// "Enhanced lighting > World lights").
//
// Light sources come from the game's own particle system (the LPS manager lbl_803D990C, drawn by
// fn_801604FC): every live emitter is in the manager's active list, its resource (.pcr) tells
// what it is (candle_*, danro_fire, elemh_fire_*, chandelier, kira_*, ...) and +0x214 is its
// world position. Resource names are recorded when the game loads them
// (JKRArchive::getResource(type, name)) and emitters are matched to their resource when they are
// created (fn_8015FE9C(manager, pcr, texture, ...)).
#include "recomp/world_lights.h"

#include "port/settings.h"
#include "recomp/enhanced_lighting.h"
#include "recomp/hle.h"
#include "recomp/runtime.h"
#include "recomp/version.h"

#include <aurora/lights.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace recomp::worldlights {
namespace {

int envInt(const char* name, int def) {
    const char* v = std::getenv(name);
    return v && *v ? std::atoi(v) : def;
}
const int sLog = envInt("LM_WORLD_LIGHT_LOG", 0);

bool guestPtr(u32 p) { return p >= 0x80000000u && p < 0x81800000u; }

#define kPtclMgr VA(0x803D990C)  // lbl_803D990C: LPS particle manager
// Manager +0xAB4: the active group; group +0x2A0: sentinel emitter of its active list. Emitter
// +0x20C next, +0x210 prev, +0xBE flags (8 = drawn), +0x214 world position.
constexpr u32 kMgrGroup = 0xAB4, kGroupList = 0x2A0;
constexpr u32 kEmNext = 0x20C, kEmFlags = 0xBE, kEmPos = 0x214;

std::unordered_map<u32, std::string> sResNames;  // .pcr resource data -> name
std::unordered_map<u32, u32> sEmitterRes;        // emitter -> .pcr resource data
u32 sFrame = 0;

const char* emitterName(u32 em) {
    const auto it = sEmitterRes.find(em);
    if (it == sEmitterRes.end()) return "?";
    const auto n = sResNames.find(it->second);
    return n == sResNames.end() ? "?" : n->second.c_str();
}

}  // namespace

void onMainView(Ctx&) {
    sFrame++;
    if (!sLog || sFrame % u32(std::max(sLog, 1))) return;
    const u32 group = rd32(kPtclMgr + kMgrGroup);
    if (!guestPtr(group)) return;
    const u32 sentinel = rd32(group + kGroupList);
    if (!guestPtr(sentinel)) return;
    int n = 0;
    for (u32 e = rd32(sentinel + kEmNext); guestPtr(e) && e != sentinel && n < 256; e = rd32(e + kEmNext), n++) {
        log("[worldlight] f%u emitter %08X %-16s flags %04X pos %.0f,%.0f,%.0f", sFrame, e, emitterName(e),
            rd16(e + kEmFlags), rdf32(e + kEmPos), rdf32(e + kEmPos + 4), rdf32(e + kEmPos + 8));
    }
    log("[worldlight] f%u %d emitters", sFrame, n);
}

// JKRArchive::getResource(type, name): remember particle resources by their data pointer.
HLE(getResource__10JKRArchiveFUlPCc) {
    const u32 namePtr = c.r[5];
    callOriginal(c, "getResource__10JKRArchiveFUlPCc");
    if (!guestPtr(namePtr) || !guestPtr(c.r[3])) return;
    const char* name = gstr(namePtr);
    const size_t len = strnlen(name, 64);
    if (len > 4 && !std::strcmp(name + len - 4, ".pcr")) sResNames[c.r[3]] = std::string(name, len - 4);
}

// LPS emitter creation (manager, .pcr data, texture, ...): returns the emitter.
HLE(fn_8015FE9C) {
    const u32 res = c.r[4];
    callOriginal(c, "fn_8015FE9C");
    if (guestPtr(c.r[3])) sEmitterRes[c.r[3]] = res;
}

}  // namespace recomp::worldlights
