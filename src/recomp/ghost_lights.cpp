// Ghost lights (F1 > Visuals > Enhanced lighting > Ghost lights; docs/RENDERING.md,
// "Enhanced lighting > Ghost lights").
//
// Every ghost the game draws this frame becomes a coloured point light for the renderer's
// extra-light list (aurora/lights.h, set_extra_lights), so ghosts light the room around them.
// Everything comes from the game's own data:
//  - Actors: the live character slots lbl_803D48A0 (count lbl_804D8750), the list the actor
//    draw pass fn_80060E70 walks. A ghost is a character whose type (actor +0x3C) has type flag
//    8 in the type table lbl_8033B590 (0x1C per type: +0x08 u16 flags, +0x14 s16 class
//    1 small/2 normal/3 portrait/4 boss, +0x18 name), the flag the draw pass uses to put it in
//    the ghost layer. The visibility test is the draw pass's own: actor +0x40 flags (0x6000 =
//    not drawn, 0x100 = hidden from the normal passes, 0x200 = drawn in any room) and the room
//    byte (+0xB4 >> 24) visible through fn_8001CE1C(room, -1).
//  - Colour: the ghost's glow colour, EnTypesManager "BrightColor" (enemy parameter file
//    /param/ctp/<type>.prm), which the game copies to the actor at +0xE4 (RGBA) and uses for
//    the ghost's light effects (EnZako::fn_800C1FFC, the capture code at 0x8013B808). The
//    actor's own copy is read so runtime changes show; the parameter table is the fallback, a
//    small per-class table the last resort. Elemental ghosts (parameter Element 1 fire, 2 water,
//    3 ice) glow in their element's colour.
//  - Visibility/fade: actor +0xE3 (the alpha the ghost layer is drawn with: 0 invisible ..
//    255 fully visible).
//  - State: the CharacterEvent messages the game sends to the ghost's AI (fn_800C0F08, hooked in
//    gameplay_hooks.cpp): 2 in the flashlight beam (sent every frame the beam touches it),
//    3 capture begin (HP reached 0), 4 capture end (inside the Poltergust), 9 fishing begin
//    (the Poltergust has hooked it), 12 collided with Luigi (an attack landed), 13-18 element
//    spray/ball hits; the Poltergust capture update fn_80081BCC (ghost at capture +0x18) runs
//    every frame a ghost is held; HP actor +0xFC (s16) against the type's EnemyParam life.
//  - Camera: the GX view matrix lbl_803A3820+0x18C, the one fn_80057CFC/fn_80057E40 use to put
//    the S3D lights into eye space before GXLoadLightObjImm, so positions are in the same space.
// Lights are built at the start of the main view fn_8000BB98(0) (after the camera update, before
// the room and actors are drawn) and handed to recomp::submitExtraLights (enhanced_lighting.cpp),
// which sends them to Aurora for that frame; a frame without the game scene has none.
#include "recomp/ghost_lights.h"

#include "port/settings.h"
#include "recomp/hle.h"
#include "recomp/runtime.h"
#include "recomp/version.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "recomp/enhanced_lighting.h"

#include <aurora/lights.h>

namespace recomp {
u32 viRetraceCount();  // hle_vi.cpp
}

namespace recomp::ghostlights {
namespace {

using aurora::gfx::ExtraLight;

// The renderer classes every blended draw like a ghost draw (extra lights reach them only with
// ExtraLightTranslucent), and much of a room is drawn blended (in the ballroom the floor and walls
// stayed dark without it). So the flag is set: the ghost's own body also catches its light,
// which reads as the glow it gives off.
constexpr u32 kFlagShadow = aurora::gfx::ExtraLightCastShadows;
constexpr u32 kFlagBase = aurora::gfx::ExtraLightSpecular | aurora::gfx::ExtraLightTranslucent;

// USA addresses (VA() maps them on the European game).
#define kActorSlots VA(0x803D48A0)  // lbl_803D48A0: live characters (slot 0 = Luigi)
#define kActorCount VA(0x804D8750)  // lbl_804D8750
#define kTypeTable VA(0x8033B590)   // lbl_8033B590: character types, 0x1C each
#define kEnTypes VA(0x804D8C80)     // Koga::EnTypesManager::sManager
#define kCamera VA(0x803A3820)      // lbl_803A3820: game camera
#define kCamViewMtx VOFF(0x18C, 0x198)  // 3x4 GX view matrix (fn_80057CFC; +0xC on the European camera)
constexpr u32 kMaxSlots = 128;      // lbl_803D48A0 is 0x200 bytes
constexpr u32 kTypeCount = 0x29C8 / 0x1C;

// Character (MoveObj) fields.
constexpr u32 kActType = 0x3C, kActFlags = 0x40, kActPos = 0x44, kActRoom = 0xB4;
constexpr u32 kActAlpha = 0xE3, kActBright = 0xE4, kActHp = 0xFC;
// Type table fields.
constexpr u32 kTypeStride = 0x1C, kTypeFlags = 0x08, kTypeClass = 0x14, kTypeName = 0x18;
constexpr u32 kTypeFlagGhost = 8;
// EnemyParam (0x218 per character type): TParamT values sit 0x10 into each parameter.
constexpr u32 kParamStride = 0x218, kParamLife = 0x1C, kParamElement = 0x198, kParamBright = 0x1FC;

// Koga::CharacterEvent::Message.
enum : u32 {
    kMsgInBeam = 2,
    kMsgCaptureBegin = 3,
    kMsgCaptureEnd = 4,
    kMsgFishingBegin = 9,
    kMsgCollide = 12,
    kMsgSprayFire = 13,
    kMsgSprayIce = 14,
    kMsgSprayWater = 15,
    kMsgBallFire = 16,
    kMsgBallIce = 17,
    kMsgBallWater = 18,
};

bool envFlag(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && *v != '0';
}
int envInt(const char* name, int def) {
    const char* v = std::getenv(name);
    return v && *v ? std::atoi(v) : def;
}
// LM_GHOST_LIGHT_LOG=1: every ghost every frame (N > 1: every N frames), plus every event.
const int sLog = envInt("LM_GHOST_LIGHT_LOG", 0);
// LM_GHOST_LIGHT_DIFF=1 (research aid): bytes 0..0x200 of each drawn ghost that changed since
// the previous frame.
const bool sDiff = envFlag("LM_GHOST_LIGHT_DIFF");
const bool sForce = envFlag("LM_GHOST_LIGHTS");  // test aid: on regardless of the setting

bool guestPtr(u32 p) { return p >= 0x80000000u && p < 0x81800000u; }

struct V3 {
    float x, y, z;
};

// Fallback glow colours (sRGB), and the element tints of spray/ball hits.
constexpr u32 kFire[3] = {255, 120, 30}, kWater[3] = {40, 110, 255}, kIce[3] = {120, 230, 255};

struct Track {
    u32 lastFrame = 0;  // game frame last seen
    u32 type = 0;
    int cls = 0;
    float alpha = 0;  // 0..1
    int hp = -1, maxHp = 0;
    // Event/frame stamps (0 = never).
    u32 lastBeam = 0, lastHeld = 0, lastDrain = 0, fishStart = 0, captureStart = 0, captureEnd = 0;
    u32 elementHit = 0;
    const u32* elementTint = nullptr;
    float flare = 0;     // appear/attack flare 0..1, decays
    float struggle = 0;  // 0..1, smoothed vacuum struggle
    float stun = 0;      // 0..1, smoothed flashlight exposure
    float phase = 0;     // pulse phase
    V3 world{};          // last emission point (world)
    float color[3]{};    // linear RGB, max channel 1
    float radius = 0;
};

// The last moments of a captured ghost after it left the actor list.
struct Burst {
    u32 id;
    V3 world;
    float color[3];
    float radius;
    u32 start;
};

std::unordered_map<u32, Track> sTracks;
std::vector<Burst> sBursts;
std::unordered_map<u32, std::vector<u8>> sDiffPrev;
u32 sFrame = 1;      // game frames seen by update() (events are stamped with it)

float srgbToLinear(u32 v) {
    const float c = float(v) / 255.0f;
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
void toLinearNormalized(const u32 rgb[3], float out[3]) {
    float mx = 1e-4f;
    for (int k = 0; k < 3; k++) mx = std::max(mx, out[k] = srgbToLinear(rgb[k]));
    for (int k = 0; k < 3; k++) out[k] /= mx;
}

// Hash noise in [-1, 1] for flicker (deterministic per frame and ghost).
float noise(u32 a, u32 b) {
    u32 h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return float(h & 0xFFFF) / 32767.5f - 1.0f;
}

const char* typeName(u32 type) {
    const u32 p = rd32(kTypeTable + type * kTypeStride + kTypeName);
    return guestPtr(p) ? gstr(p) : "?";
}

u32 paramBase(u32 type) {
    const u32 mgr = rd32(kEnTypes);
    if (!guestPtr(mgr)) return 0;
    const u32 params = rd32(mgr + 4);
    return guestPtr(params) ? params + type * kParamStride : 0;
}

bool isGhostActor(u32 a) {
    if (!guestPtr(a)) return false;
    const u32 type = rd32(a + kActType);
    return type < kTypeCount && (rd16(kTypeTable + type * kTypeStride + kTypeFlags) & kTypeFlagGhost);
}

// Ghost element (EnemyParam "Element"): 1 fire, 2 water, 3 ice. Read from the weakness test at
// 0x800C75F8: element 1 is hurt by attack element 2 (water), 2 by 3 (ice), 3 by 1 (fire).
const u32* elementColor(int element) { return element == 1 ? kFire : element == 2 ? kWater : element == 3 ? kIce : nullptr; }

void fallbackColor(int cls, u32 rgb[3]) {
    static const u32 kGhost[3] = {150, 170, 255};  // ordinary ghosts: cold blue-white
    static const u32 kBoss[3] = {200, 140, 255};   // portrait ghosts / bosses: violet
    const u32* c = cls >= 3 ? kBoss : kGhost;
    for (int i = 0; i < 3; i++) rgb[i] = c[i];
}

// Ghost size: light radius (world units) and how far above the actor position (+0x44) the light
// sits. +0x44 is where the game itself puts the small GX light some ghosts carry
// (EnZako::fn_800C1FFC -> fn_80016C98(0, actor + 0x44, BrightColor x alpha, ...)), but for
// walking ghosts it is near the floor (e.g. the ballroom's shy-guy ghosts at y 60), where a point
// light only grazes the floor; lifting it to about the body's middle lights the floor around the
// ghost and the walls and furniture nearby.
void sizeFor(int cls, float& radius, float& height) {
    switch (cls) {
    case 1: radius = 380.0f; height = 40.0f; break;    // bats, mice, Boos, small fry
    case 2: radius = 560.0f; height = 90.0f; break;    // ordinary ghosts
    case 3: radius = 720.0f; height = 110.0f; break;   // portrait ghosts
    case 4: radius = 1100.0f; height = 160.0f; break;  // bosses
    default: radius = 480.0f; height = 80.0f; break;
    }
}

V3 toView(const float m[12], V3 p) {
    return {m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3], m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
            m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]};
}

bool recent(u32 stamp, u32 frames) { return stamp && sFrame - stamp <= frames; }

// Capture burst envelope: ~3x flash ramping in over 3 frames, then fading out by `len`.
float burstEnvelope(u32 age, u32 len) {
    if (age >= len) return 0.0f;
    if (age < 3) return 1.0f + 2.0f * float(age + 1) / 3.0f;
    const float f = float(age - 3) / float(len - 3);
    return 3.0f * (1.0f - f) * (1.0f - f);
}

struct Candidate {
    ExtraLight light;
    float score;
};

void diffDump(u32 actor) {
    std::vector<u8> cur(0x200);
    for (u32 i = 0; i < 0x200; i++) cur[i] = u8(rd8(actor + i));
    auto& prev = sDiffPrev[actor];
    if (prev.size() == cur.size()) {
        char line[1024];
        int n = std::snprintf(line, sizeof(line), "[ghostlight] f%u diff %08X:", sFrame, actor);
        for (u32 i = 0; i < 0x200 && n < 1000; i++)
            if (cur[i] != prev[i] && !(i >= kActPos && i < kActPos + 12))
                n += std::snprintf(line + n, sizeof(line) - n, " %X:%02X>%02X", i, prev[i], cur[i]);
        if (n > 40) log("%s", line);
    }
    prev = std::move(cur);
}

// Ghost lights need the Enhanced lighting master switch and its "Dynamic colored lights" part.
bool enabled() {
    const auto& st = port::settings();
    return sForce || (st.ghostLights.enabled && st.enhancedLighting.enabled && st.enhancedLighting.extraLights);
}

// LM_GHOST_LIGHT_LOG: once the enemy parameters are loaded, every ghost type's glow colour.
void dumpTypes() {
    static bool sDone = false;
    if (sDone || !paramBase(0)) return;
    sDone = true;
    for (u32 type = 0; type < kTypeCount; type++) {
        const u32 e = kTypeTable + type * kTypeStride;
        if (!(rd16(e + kTypeFlags) & kTypeFlagGhost)) continue;
        const u32 pb = paramBase(type);
        log("[ghostlight] type %3u %-10s cls %2d life %4d element %d bright %08X amb %08X", type, typeName(type),
            s16(rd16(e + kTypeClass)), int(rd32(pb + kParamLife)), int(rd32(pb + kParamElement)), rd32(pb + kParamBright),
            rd32(pb + 0x210));
    }
}

void update(Ctx& c) {
    const auto& s = port::settings().ghostLights;
    sFrame++;
    if (!enabled()) {
        sTracks.clear();
        sBursts.clear();
        return;
    }
    const float intensity = std::clamp(s.intensity, 0.0f, 4.0f);
    const float radiusScale = std::clamp(s.radiusScale, 0.25f, 3.0f);
    const bool dramatic = s.dramatic;
    const u32 maxLights = u32(std::clamp(s.maxLights, 1, int(aurora::gfx::MaxExtraLights)));
    const float hint = std::clamp(s.invisibleHint, 0.0f, 0.3f);
    const float t = float(sFrame) / 30.0f;
    constexpr float kBase = 1.6f;  // ghost light strength at intensity 1

    if (sLog) dumpTypes();
    float view[12];
    for (int i = 0; i < 12; i++) view[i] = rdf32(kCamera + kCamViewMtx + 4 * i);

    static const u32 roomVisible = guestSymbol("fn_8001CE1C");
    const Ctx saved = c;
    std::vector<Candidate> cands;
    const u32 count = std::min(rd32(kActorCount), kMaxSlots);
    const bool logNow = sLog > 0 && sFrame % u32(std::max(sLog, 1)) == 0;
    if (logNow) {
        const u32 l = rd32(kActorSlots);
        if (guestPtr(l))
            log("[ghostlight] f%u retrace %u luigi %.0f,%.0f,%.0f", sFrame, viRetraceCount(), rdf32(l + kActPos),
                rdf32(l + kActPos + 4), rdf32(l + kActPos + 8));
    }
    for (u32 i = 0; i < count; i++) {
        const u32 a = rd32(kActorSlots + 4 * i);
        if (!isGhostActor(a)) continue;
        const u32 type = rd32(a + kActType);
        const u32 tEntry = kTypeTable + type * kTypeStride;
        const int cls = s16(rd16(tEntry + kTypeClass));
        const u32 flags = rd32(a + kActFlags);

        Track& tr = sTracks[a];
        if (tr.lastFrame == 0 || tr.type != type || tr.cls != cls) {
            const Track events = tr;  // events may arrive before the first update
            tr = Track{};
            tr.lastBeam = events.lastBeam;
            tr.lastHeld = events.lastHeld;
            tr.fishStart = events.fishStart;
            tr.captureStart = events.captureStart;
            tr.captureEnd = events.captureEnd;
            tr.elementHit = events.elementHit;
            tr.elementTint = events.elementTint;
            tr.flare = events.flare;
            tr.type = type;
            tr.cls = cls;
            tr.phase = float(i) * 1.7f;
        }
        tr.lastFrame = sFrame;

        // The draw pass's visibility test (fn_80060E70).
        bool drawn = !(flags & 0x6000) && !(flags & 0x100);
        if (drawn && !(flags & 0x200)) drawn = guestCall(c, roomVisible, rd32(a + kActRoom) >> 24, u32(-1)) != 0;
        if (sDiff && drawn) diffDump(a);

        const float alpha = float(rd8(a + kActAlpha)) / 255.0f;
        const u32 pb = paramBase(type);
        const int hp = s16(rd16(a + kActHp));
        const int maxHp = pb ? int(rd32(pb + kParamLife)) : 0;
        const int element = pb ? int(rd32(pb + kParamElement)) : 0;

        // Colour: the actor's glow colour, else the type's parameter, else the fallback table.
        u32 rgb[3] = {rd8(a + kActBright), rd8(a + kActBright + 1), rd8(a + kActBright + 2)};
        if (rgb[0] + rgb[1] + rgb[2] < 24 && pb)
            for (int k = 0; k < 3; k++) rgb[k] = rd8(pb + kParamBright + k);
        if (rgb[0] + rgb[1] + rgb[2] < 24) fallbackColor(cls, rgb);
        // Elemental ghosts all share one pale glow colour (C1DCF7) in the parameters; their light
        // takes the element's colour instead (fire orange, water blue, ice cyan).
        if (const u32* ec = elementColor(element))
            for (int k = 0; k < 3; k++) rgb[k] = ec[k];
        toLinearNormalized(rgb, tr.color);
        float color[3] = {tr.color[0], tr.color[1], tr.color[2]};
        // Element hits tint the light with the element for a moment (fire spray on a water ghost...).
        if (dramatic && recent(tr.elementHit, 20) && tr.elementTint) {
            float tint[3];
            toLinearNormalized(tr.elementTint, tint);
            const float w = 0.6f * (1.0f - float(sFrame - tr.elementHit) / 20.0f);
            for (int k = 0; k < 3; k++) color[k] = color[k] * (1.0f - w) + tint[k] * w;
        }

        float radius, height;
        sizeFor(cls, radius, height);
        tr.radius = radius * radiusScale;
        tr.world = {rdf32(a + kActPos), rdf32(a + kActPos + 4) + height, rdf32(a + kActPos + 8)};

        // State. Actors are pooled and reused: capture marks only last for the capture itself.
        if (tr.captureStart && sFrame - tr.captureStart > 60) tr.captureStart = 0;
        if (tr.captureEnd && sFrame - tr.captureEnd > 30) tr.captureEnd = 0;
        if (tr.hp >= 0 && hp < tr.hp) tr.lastDrain = sFrame;
        // Vacuumed down to 0 HP while held: the capture (also signalled by message 3).
        if (tr.hp > 0 && hp <= 0 && (recent(tr.lastHeld, 10) || recent(tr.lastDrain, 10)) && !tr.captureStart) tr.captureStart = sFrame;
        tr.hp = hp;
        tr.maxHp = maxHp;
        if (alpha > tr.alpha + 0.02f && tr.alpha < 0.9f)  // fading in: appear flare
            tr.flare = std::max(tr.flare, std::min(1.0f, (alpha - tr.alpha) * 6.0f + 0.4f));
        tr.alpha = alpha;
        const bool held = recent(tr.lastHeld, 2) || recent(tr.lastDrain, 8) ||
                          (tr.fishStart && !tr.captureStart && recent(tr.fishStart, 300) && recent(tr.lastHeld, 30));
        const bool capturing = tr.captureStart != 0;
        const bool inBeam = recent(tr.lastBeam, 2);
        const float hpFrac = maxHp > 0 ? std::clamp(float(hp) / float(maxHp), 0.0f, 1.0f) : 1.0f;
        tr.struggle += ((held ? 0.35f + 0.65f * (1.0f - hpFrac) : 0.0f) - tr.struggle) * 0.25f;
        tr.stun += ((inBeam ? 1.0f : 0.0f) - tr.stun) * (inBeam ? 0.5f : 0.15f);

        const char* state = "visible";
        if (!drawn) state = "hidden";
        else if (alpha <= 0.01f) state = capturing ? "captured" : "invisible";
        else if (capturing) state = "capturing";
        else if (held) state = "vacuumed";
        else if (inBeam) state = "in-beam";
        else if (recent(tr.elementHit, 20)) state = "element-hit";
        else if (tr.flare > 0.05f) state = "flare";

        float k = 1.0f, radiusK = 1.0f;
        if (dramatic) {
            // Gentle breathing.
            k *= 1.0f + 0.14f * std::sin(t * 3.7f + tr.phase) + 0.05f * std::sin(t * 9.1f + tr.phase * 2.3f);
            // Appear / attack flare.
            k *= 1.0f + 1.3f * tr.flare;
            radiusK += 0.2f * tr.flare;
            // Flashlight: a nervous flicker that brightens with exposure.
            if (tr.stun > 0.01f) k *= 1.0f + tr.stun * (0.35f + 0.45f * noise(sFrame, a));
            // Vacuum struggle: ramps as the HP drains, with a fast tremor.
            if (tr.struggle > 0.01f) {
                k *= 1.0f + 1.6f * tr.struggle + 0.5f * tr.struggle * noise(sFrame, a ^ 0x55u);
                radiusK += 0.25f * tr.struggle;
            }
            // Capture: burst, then fade while it is pulled into the nozzle.
            if (capturing) {
                const float b = burstEnvelope(sFrame - tr.captureStart, 45);
                k *= std::max(b, 0.05f);
                radiusK += 0.3f * std::min(b, 1.0f);
            }
        } else if (capturing) {
            k *= std::max(0.0f, 1.0f - float(sFrame - tr.captureStart) / 20.0f);
        }
        tr.flare *= 0.9f;
        float vis = alpha;
        if (vis <= 0.01f) vis = capturing && dramatic ? 1.0f : hint;  // the capture burst shows even as it shrinks away
        if (!drawn || tr.captureEnd) vis = 0.0f;

        const float lightI = intensity * kBase * k * vis;
        const V3 vp = toView(view, tr.world);
        if (logNow)
            log("[ghostlight] f%u id %08X type %u %s cls %d el %d state %s a %.2f hp %d/%d flags %08X rgb %02X%02X%02X "
                "world %.0f,%.0f,%.0f view %.0f,%.0f,%.0f I %.2f r %.0f",
                sFrame, a, type, typeName(type), cls, element, state, alpha, hp, maxHp, flags, rgb[0], rgb[1], rgb[2],
                tr.world.x, tr.world.y, tr.world.z, vp.x, vp.y, vp.z, lightI, tr.radius * radiusK);
        if (lightI <= 0.005f) continue;
        Candidate cd{};
        cd.light = {{vp.x, vp.y, vp.z}, {color[0], color[1], color[2]}, tr.radius * radiusK, lightI, kFlagBase, a};
        const float dist = std::sqrt(vp.x * vp.x + vp.y * vp.y + vp.z * vp.z);
        cd.score = lightI * cd.light.radius / (1.0f + dist / 1500.0f);
        cands.push_back(cd);
    }
    c = saved;

    // Ghosts that left the actor list mid-capture keep their burst at the last position.
    for (auto it = sTracks.begin(); it != sTracks.end();) {
        const Track& tr = it->second;
        if (tr.lastFrame == sFrame) {
            ++it;
            continue;
        }
        if (tr.lastFrame && (tr.captureStart || recent(tr.lastHeld, 10) || recent(tr.lastDrain, 10)) && dramatic) {
            const u32 start = tr.captureStart ? tr.captureStart : sFrame;
            if (sFrame - start < 45) sBursts.push_back({it->first, tr.world, {tr.color[0], tr.color[1], tr.color[2]}, tr.radius, start});
        }
        // Keep entries that only hold events for a ghost not yet seen in the list.
        if (tr.lastFrame == 0 && sFrame - std::max({tr.lastBeam, tr.lastHeld, tr.fishStart, tr.captureStart, tr.elementHit}) < 60) {
            ++it;
            continue;
        }
        it = sTracks.erase(it);
    }
    for (auto it = sBursts.begin(); it != sBursts.end();) {
        const float b = burstEnvelope(sFrame - it->start, 45);
        if (b <= 0.0f || !dramatic) {
            it = sBursts.erase(it);
            continue;
        }
        const V3 vp = toView(view, it->world);
        Candidate cd{};
        cd.light = {{vp.x, vp.y, vp.z}, {it->color[0], it->color[1], it->color[2]}, it->radius * (1.0f + 0.3f * std::min(b, 1.0f)),
                    intensity * kBase * b, kFlagBase, it->id};
        cd.score = cd.light.intensity * cd.light.radius;
        cands.push_back(cd);
        if (logNow) log("[ghostlight] f%u burst id %08X age %u I %.2f view %.0f,%.0f,%.0f", sFrame, it->id, sFrame - it->start, cd.light.intensity, vp.x, vp.y, vp.z);
        ++it;
    }

    std::sort(cands.begin(), cands.end(), [](const Candidate& x, const Candidate& y) { return x.score > y.score; });
    if (cands.size() > maxLights) cands.resize(maxLights);
    ExtraLight lights[aurora::gfx::MaxExtraLights];
    u32 n = 0;
    for (auto& cd : cands) {
        if (s.shadows && n < 2 && cd.light.intensity > 0.3f) cd.light.flags |= kFlagShadow;
        lights[n++] = cd.light;
    }
    if (logNow && n)
        for (u32 i = 0; i < n; i++)
            log("[ghostlight] f%u light %u id %08X view %.0f,%.0f,%.0f rgb %.2f,%.2f,%.2f r %.0f I %.2f flags %u", sFrame, i,
                lights[i].id, lights[i].pos[0], lights[i].pos[1], lights[i].pos[2], lights[i].color[0], lights[i].color[1],
                lights[i].color[2], lights[i].radius, lights[i].intensity, lights[i].flags);
    // Test aid LM_GHOST_LIGHT_AB=1: lights only on even game frames, so consecutive frame dumps
    // (LM_FRAME_DUMP) compare with/without in nearly the same scene.
    static const bool sAB = envFlag("LM_GHOST_LIGHT_AB");
    if (sAB && (sFrame & 1)) n = 0;
    submitExtraLights(lights, n);  // enhanced_lighting.cpp sends them with the main view
}

}  // namespace

void onCharacterEvent(u32 actor, u32 msg) {
    if (!enabled() || !isGhostActor(actor)) return;
    Track& tr = sTracks[actor];
    switch (msg) {
    case kMsgInBeam: tr.lastBeam = sFrame; break;
    case kMsgCaptureBegin: if (!tr.captureStart) tr.captureStart = sFrame; break;
    case kMsgCaptureEnd: tr.captureEnd = sFrame; break;
    case kMsgFishingBegin: tr.fishStart = tr.lastHeld = sFrame; break;
    case kMsgCollide: tr.flare = 1.0f; break;
    case kMsgSprayFire: case kMsgBallFire: tr.elementHit = sFrame; tr.elementTint = kFire; tr.flare = std::max(tr.flare, 0.6f); break;
    case kMsgSprayIce: case kMsgBallIce: tr.elementHit = sFrame; tr.elementTint = kIce; tr.flare = std::max(tr.flare, 0.6f); break;
    case kMsgSprayWater: case kMsgBallWater: tr.elementHit = sFrame; tr.elementTint = kWater; tr.flare = std::max(tr.flare, 0.6f); break;
    default: return;
    }
    if (sLog && msg != kMsgInBeam) log("[ghostlight] f%u event %u -> %08X %s", sFrame, msg, actor, typeName(rd32(actor + kActType)));
    if (sLog && msg == kMsgInBeam && (sFrame % 15) == 0) log("[ghostlight] f%u in beam %08X %s", sFrame, actor, typeName(rd32(actor + kActType)));
}

void onVacuumTick(u32 actor) {
    if (!enabled() || !isGhostActor(actor)) return;
    sTracks[actor].lastHeld = sFrame;
    if (sLog && sFrame % 15 == 0)
        log("[ghostlight] f%u vacuum holds %08X %s hp %d", sFrame, actor, typeName(rd32(actor + kActType)), s16(rd16(actor + kActHp)));
}

// Main view of the game scene (enhanced_lighting.cpp's fn_8000BB98 hook, view 0: fn_8000BCE8
// calls it after setting that view's camera): the lights are built from the current camera
// before anything is drawn and submitted for this frame.
void onMainView(Ctx& c) { update(c); }  // update() restores the registers

}  // namespace recomp::ghostlights
