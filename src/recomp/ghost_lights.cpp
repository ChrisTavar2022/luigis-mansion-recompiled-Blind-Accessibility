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

// Only draws of the ghost pass (fn_80060E70 pass 0x30) count as ghosts for the renderer; blended
// room geometry is ordinary world geometry. Ghost lights leave ExtraLightTranslucent off, so a
// ghost's body does not light itself (its glow comes from the bloom light glow instead).
constexpr u32 kFlagShadow = aurora::gfx::ExtraLightCastShadows;
constexpr u32 kFlagBase = aurora::gfx::ExtraLightSpecular;

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
    u32 thought = 0;  // the ghost's EnThought (AI), from its messages
    const u32* elementTint = nullptr;
    float flare = 0;     // appear/attack flare 0..1, decays
    float struggle = 0;  // 0..1, smoothed vacuum struggle
    float stun = 0;      // 0..1, smoothed flashlight exposure
    float phase = 0;     // pulse phase
    V3 world{};          // last emission point (world)
    float color[3]{};    // linear RGB, max channel 1
    float radius = 0;
    float outI = 0;      // submitted intensity, smoothed (no frame-to-frame jumps)
    float shadowW = 0;   // 0..1 weight of this light's shadow, faded in/out over a few frames
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

// Smooth noise in [-1, 1]: the hash noise at every 6th game frame, interpolated (smoothstep)
// in between, so the flicker breathes instead of jumping every frame (a light that jumps every
// frame makes its shadows and bloom flicker).
float smoothNoise(u32 frame, u32 b) {
    const u32 k = frame / 6;
    float f = float(frame % 6) / 6.0f;
    f = f * f * (3.0f - 2.0f * f);
    return noise(k, b) * (1.0f - f) + noise(k + 1, b) * f;
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
// EnThought +0x800: the active EnemyStrategy; its current state (+0x0E), and the wrapped
// "tsuri" (vacuum) strategy's state when it is a decorator (+0x14). Logged as tsuri<<16 | state.
u32 aiState(u32 thought) {
    if (!guestPtr(thought)) return 0xFFFF;
    const u32 st = rd32(thought + 0x800);
    if (!guestPtr(st)) return 0xFFFE;
    const u32 inner = rd32(st + 0x14);
    return (guestPtr(inner) ? rd16(inner + 0xE) << 16 : 0) | rd16(st + 0xE);
}

bool enabled() {
    const auto& st = port::settings();
    return sForce || (st.ghostLights.enabled && enh::masterOn() && st.enhancedLighting.extraLights);
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
        log("[ghostlight] type %3u %-10s cls %2d life %4d element %d bright %08X amb %08X bind %d frames %.0f..%.0f",
            type, typeName(type), s16(rd16(e + kTypeClass)), int(rd32(pb + kParamLife)), int(rd32(pb + kParamElement)),
            rd32(pb + kParamBright), rd32(pb + 0x210), int(rd32(pb + 0x94)), rdf32(pb + 0xA8), rdf32(pb + 0xBC));
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
            log("[ghostlight] f%u retrace %u luigi %.0f,%.0f,%.0f room %u", sFrame, viRetraceCount(), rdf32(l + kActPos),
                rdf32(l + kActPos + 4), rdf32(l + kActPos + 8), rd32(VA(0x803A3CA0) + 0xC) >> 24);
        // Once: the room table (lbl_804D8170, 0x30 per room, int bounds at +0x8/+0x14), to find
        // rooms for LM_TELEPORT.
        static bool sRooms = false;
        const u32 rooms = rd32(VA(0x804D8170));
        if (!sRooms && guestPtr(rooms)) {
            sRooms = true;
            for (u32 r = 0; r < 80; r++) {
                const u32 b = rooms + r * 0x30;
                const int lo[3] = {s32(rd32(b + 8)), s32(rd32(b + 12)), s32(rd32(b + 16))};
                const int hi[3] = {s32(rd32(b + 20)), s32(rd32(b + 24)), s32(rd32(b + 28))};
                if (hi[0] <= lo[0] || hi[0] - lo[0] > 20000) continue;
                log("[ghostlight] room %2u min %d,%d,%d max %d,%d,%d", r, lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
            }
        }
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
            tr.thought = events.thought;
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
            if (tr.stun > 0.01f) k *= 1.0f + tr.stun * (0.35f + 0.45f * smoothNoise(sFrame, a));
            // Vacuum struggle: ramps as the HP drains, with a fast tremor.
            // (not during the capture: the burst on top of the struggle peaked at ~17x and
            // washed the whole room out in the first in-game capture)
            if (tr.struggle > 0.01f && !capturing) {
                k *= 1.0f + 1.6f * tr.struggle + 0.5f * tr.struggle * smoothNoise(sFrame, a ^ 0x55u);
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

        // Small ghosts (bats, mice, Boos) glow less: hallways have several of them hanging right
        // under the ceiling, where a full-strength light burns a hot spot into the plaster.
        const float sizeK = tr.cls == 1 ? 0.55f : 1.0f;
        const float rawI = intensity * kBase * k * vis * sizeK;
        // Smoothed: ghosts fade, hide and flare from one game frame to the next (the dance
        // switches visibility often); a light that jumps makes its pool, its shadows and the
        // bloom pop. Fast attack, slower release (about 3 / 5 frames).
        tr.outI += (rawI - tr.outI) * (rawI > tr.outI ? 0.45f : 0.3f);
        if (rawI <= 0.0f && tr.outI < 0.005f) tr.outI = 0.0f;
        const float lightI = tr.outI;
        const V3 vp = toView(view, tr.world);
        if (logNow)
            log("[ghostlight] f%u id %08X type %u %s cls %d el %d state %s a %.2f hp %d/%d flags %08X rgb %02X%02X%02X "
                "world %.0f,%.0f,%.0f view %.0f,%.0f,%.0f I %.2f r %.0f ai %X",
                sFrame, a, type, typeName(type), cls, element, state, alpha, hp, maxHp, flags, rgb[0], rgb[1], rgb[2],
                tr.world.x, tr.world.y, tr.world.z, vp.x, vp.y, vp.z, lightI, tr.radius * radiusK, aiState(tr.thought));
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
    // Shadow casters: the two strongest lights, chosen with hysteresis (a light that already
    // casts a shadow keeps it unless another is clearly stronger), and each light's shadow fades
    // in/out over about five game frames instead of switching (the dancers trade places every
    // few frames; a hard switch made shadows appear and vanish). The weight goes to Aurora in
    // flags bits 8..15 (0 = full); Aurora has four shadow slots, so fading-out shadows fit.
    {
        std::vector<std::pair<float, u32>> ranked;
        for (auto& cd : cands) {
            if (!s.shadows || cd.light.intensity <= 0.3f) continue;
            const auto it = sTracks.find(cd.light.id);
            const bool holding = it != sTracks.end() && it->second.shadowW > 0.5f;
            ranked.push_back({cd.score * (holding ? 1.35f : 1.0f), cd.light.id});
        }
        std::sort(ranked.begin(), ranked.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
        for (auto& [id, tr] : sTracks) {
            bool want = false;
            // As many of the strongest ghosts as colored-light shadows allow (two for screen-space).
            const size_t cap = std::max<size_t>(enh::shadowLightCap(), 2);
            for (size_t r = 0; r < ranked.size() && r < cap; r++) want |= ranked[r].second == id;
            tr.shadowW += ((want ? 1.0f : 0.0f) - tr.shadowW) * 0.25f;
            if (tr.shadowW < 0.02f && !want) tr.shadowW = 0.0f;
        }
    }
    u32 shadowed = 0;
    for (auto& cd : cands) {
        const auto it = sTracks.find(cd.light.id);
        const float w = it != sTracks.end() ? it->second.shadowW : 0.0f;
        if (s.shadows && w > 0.0f && shadowed < 8) {
            cd.light.flags |= kFlagShadow | (std::clamp(u32(w * 255.0f + 0.5f), 1u, 255u) << 8);
            shadowed++;
        }
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

void onCharacterEvent(u32 actor, u32 msg, u32 thought, u32 event) {
    if (!enabled() || !isGhostActor(actor)) return;
    Track& tr = sTracks[actor];
    tr.thought = thought;
    static u32 sVtTypes[8] = {}, sVtN = 0;
    const u32 vtType = rd32(actor + kActType);
    const bool sVt = std::find(sVtTypes, sVtTypes + sVtN, vtType) != sVtTypes + sVtN || sVtN >= 8;
    if (sLog && !sVt && guestPtr(thought) && guestPtr(rd32(thought + 0x800))) {
        sVtTypes[sVtN++] = vtType;
        const u32 st = rd32(thought + 0x800), in = rd32(st + 0x14);
        log("[ghostlight] AI of %s: thought vt %08X, strategy %08X vt %08X, inner %08X vt %08X", typeName(rd32(actor + kActType)),
            rd32(thought + 0x804), st, rd32(st), in, guestPtr(in) ? rd32(in) : 0);
    }
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
    if (sLog && msg == kMsgInBeam && (sFrame % 15) == 0)
        log("[ghostlight] f%u in beam %08X %s ai %X ev %08X %08X %08X %08X %08X %08X %08X %08X", sFrame, actor,
            typeName(rd32(actor + kActType)), aiState(thought), rd32(event), rd32(event + 4), rd32(event + 8),
            rd32(event + 12), rd32(event + 16), rd32(event + 20), rd32(event + 24), rd32(event + 28));
}

void onVacuumTick(u32 actor) {
    if (!enabled() || !isGhostActor(actor)) return;
    sTracks[actor].lastHeld = sFrame;
    if (sLog && sFrame % 15 == 0)
        log("[ghostlight] f%u vacuum holds %08X %s hp %d", sFrame, actor, typeName(rd32(actor + kActType)), s16(rd16(actor + kActHp)));
}

void testPad(u16& buttons, s8& stickX, s8& stickY, s8& cY, u8& triggerR) {
    static const bool sHunt = envFlag("LM_GHOST_HUNT");
    // LM_GHOST_HUNT=1 turns the flashlight onto the ghost when its AI enters state 0xA (the
    // attack, fully visible); =<hex> another state.
    static const u32 sTrigger = [] {
        const u32 v = u32(std::strtoul(std::getenv("LM_GHOST_HUNT") ? std::getenv("LM_GHOST_HUNT") : "1", nullptr, 16));
        return v <= 1 ? 0xAu : v;
    }();
    if (!sHunt || !enabled()) return;
    const u32 l = rd32(kActorSlots);
    if (!guestPtr(l) || sFrame < 150) return;
    // Target: the nearest drawn, visible ghost.
    const V3 lp{rdf32(l + kActPos), rdf32(l + kActPos + 4), rdf32(l + kActPos + 8)};
    u32 best = 0;
    float bestD = 1e9f;
    for (auto& [a, tr] : sTracks) {
        if (tr.lastFrame + 2 < sFrame || (tr.alpha < 0.05f && !recent(tr.lastHeld, 3))) continue;
        const float dx = tr.world.x - lp.x, dz = tr.world.z - lp.z, d = std::sqrt(dx * dx + dz * dz);
        if (d < bestD) bestD = d, best = a;
    }
    // The flashlight is always on (holding R puts it out while vacuuming); where Luigi faces
    // decides what it lights.
    enum { Wait, Aim, Pull };
    static int sMode = Wait;
    static u32 sModeFrame = 0, sLastSeen = 0, sLastAimLog = 0;
    static const u32 sRDelay = u32(std::getenv("LM_GHOST_HUNT_R") ? std::atoi(std::getenv("LM_GHOST_HUNT_R")) : 15);
    static const u32 sDelay = u32(std::atoi(std::getenv("LM_GHOST_HUNT_DELAY") ? std::getenv("LM_GHOST_HUNT_DELAY") : "0"));
    static const int sTurn = std::getenv("LM_GHOST_HUNT_TURN") ? std::atoi(std::getenv("LM_GHOST_HUNT_TURN")) : 6;
    constexpr u16 kPadR = 0x0020;  // PAD_TRIGGER_R
    if (!best) {
        if (sMode != Wait) log("[ghosthunt] f%u no target", sFrame);
        sMode = Wait;
        return;
    }
    const Track& tr = sTracks[best];
    const u32 ai = aiState(tr.thought) & 0xFFFF;
    const float dx = tr.world.x - lp.x, dz = tr.world.z - lp.z;
    const float len = std::max(1.0f, std::sqrt(dx * dx + dz * dz));
    // Camera looks along -z: stick up = -z, right = +x.
    static const int sFlip = std::getenv("LM_GHOST_HUNT_FLIP") ? std::atoi(std::getenv("LM_GHOST_HUNT_FLIP")) : 0;
    const s8 towardX = s8(std::clamp((sFlip & 1 ? -dx : dx) / len * 127.0f, -127.0f, 127.0f));
    const s8 towardY = s8(std::clamp((sFlip & 2 ? dz : -dz) / len * 127.0f, -127.0f, 127.0f));
    const bool held = recent(tr.lastHeld, 3) || recent(tr.fishStart, 3);
    switch (sMode) {
    case Wait:  // face away (the light would scare it off) until it attacks
        if (sFrame % 10 < 2 && bestD < 700.0f) stickX = s8(-towardX / 3), stickY = s8(-towardY / 3);
        static u32 sInTrigger = 0;
        sInTrigger = ai == sTrigger ? sInTrigger + (sFrame != sLastSeen ? 1 : 0) : 0;
        sLastSeen = sFrame;
        if (ai == sTrigger && sInTrigger >= sDelay && bestD < 600.0f && sFrame - sModeFrame > 20 &&
            rd32(l + 0x1180) != 0) {  // not while Luigi is startled (Poltergust mode 0: R does nothing)
            sMode = Aim, sModeFrame = sFrame;
            log("[ghosthunt] f%u target %08X a %.2f d %.0f ai %X: aim", sFrame, best, tr.alpha, bestD, ai);
        }
        break;
    case Aim:  // turn the light onto it; vacuum once the beam has been on it a moment
        if (int(sFrame - sModeFrame) < sTurn) stickX = s8(towardX / 2), stickY = s8(towardY / 2);  // turn, barely walk
        if (recent(tr.lastBeam, 2) && sFrame - tr.lastBeam < 2 && sFrame - sModeFrame > 4) {
            static u32 sBeamFrames = 0;
            if (++sBeamFrames % 10 == 0) log("[ghosthunt] f%u beam on target, ai %X", sFrame, ai);
        }
        if (sFrame % 5 == 0 && sFrame != sLastAimLog) {
            sLastAimLog = sFrame;
            log("[ghosthunt] f%u aim: ai %X a %.2f d %.0f beam %d", sFrame, aiState(tr.thought), tr.alpha, bestD, int(recent(tr.lastBeam, 2)));
        }
        {
            // The ghost's light-bind sub-strategy (gold ghost strategy +0x590, state 3 = bound /
            // stunned): then hold R and close in so the nozzle hooks it.
            const u32 st = guestPtr(tr.thought) ? rd32(tr.thought + 0x800) : 0, sub = guestPtr(st) ? rd32(st + 0x590) : 0;
            const bool bound = guestPtr(sub) && rd16(sub + 0xE) == 3;
            static u32 sBoundFrame = 0;
            if (bound) sBoundFrame = sFrame;
            const bool afterBound = sBoundFrame && sFrame - sBoundFrame < 60;  // keep vacuuming while it fades
            if (bound || afterBound || (recent(tr.lastBeam, 3) && sFrame - sModeFrame > sRDelay)) buttons |= kPadR, triggerR = 0xFF;
            static const int sCY = std::getenv("LM_GHOST_HUNT_CY") ? std::atoi(std::getenv("LM_GHOST_HUNT_CY")) : 100;
            static const int sDiv = std::getenv("LM_GHOST_HUNT_DIV") ? std::atoi(std::getenv("LM_GHOST_HUNT_DIV")) : 1;
            if (bound || afterBound) cY = s8(sCY);  // tilt the nozzle up at the floating ghost (C stick)
            if (bound && bestD > 120.0f) stickX = s8(towardX / sDiv), stickY = s8(towardY / sDiv);
            if (bound && sFrame % 5 == 1) log("[ghosthunt] f%u bound (stunned), closing in d %.0f", sFrame, bestD);
        }
        if (held) {
            sMode = Pull, sModeFrame = sFrame;
            log("[ghosthunt] f%u hooked %08X hp %d ai %X: pulling", sFrame, best, tr.hp, ai);
        } else if (sFrame - sModeFrame > 180) {
            sMode = Wait, sModeFrame = sFrame;
            log("[ghosthunt] f%u not hooked (ai %X): wait", sFrame, ai);
        }
        break;
    case Pull:  // hold R and pull away from the ghost
        buttons |= kPadR, triggerR = 0xFF;
        cY = 100;
        stickX = s8(-towardX), stickY = s8(-towardY);
        if (!held && sFrame - sModeFrame > 30) {
            sMode = Wait, sModeFrame = sFrame;
            log("[ghosthunt] f%u released (hp %d)", sFrame, tr.hp);
        }
        break;
    }
}

// Main view of the game scene (enhanced_lighting.cpp's fn_8000BB98 hook, view 0: fn_8000BCE8
// calls it after setting that view's camera): the lights are built from the current camera
// before anything is drawn and submitted for this frame.
u32 sForcedActor = 0, sForcedFrame = 0;
bool testForceLit(u32 actor) { return actor == sForcedActor && sFrame - sForcedFrame <= 2; }

void onMainView(Ctx& c) {
    update(c);  // update() restores the registers
    // Test aid LM_GHOST_HUNT_BEAM=<states hex, e.g. 8A>: while the nearest ghost is in one of
    // those AI states and 100..400 units from Luigi, deliver the game's own "in the flashlight
    // beam" message (CharacterEvent 2) to its AI each frame, as the flashlight would. Stands in
    // for aiming the beam (the scripted stick cannot aim it up at a hovering ghost).
    static const char* sBeam = std::getenv("LM_GHOST_HUNT_BEAM");
    if (!sBeam || !enabled()) return;
    const u32 l = rd32(kActorSlots);
    if (!guestPtr(l)) return;
    static const u32 ev = [] {
        const u32 p = hleAlloc(0x40, 8);
        for (u32 i = 0; i < 0x40; i += 4) wr32(p + i, 0);
        return p;
    }();
    static const u32 thoughtFn = guestSymbol("fn_800C0F08");
    for (auto& [a, tr] : sTracks) {
        if (tr.lastFrame != sFrame || !guestPtr(tr.thought) || tr.alpha < 0.05f) continue;
        const u32 ai = aiState(tr.thought) & 0xFFFF;
        bool want = false;
        for (const char* p = sBeam; *p; p++) {
            const char ch[2] = {*p, 0};
            if (std::strtoul(ch, nullptr, 16) == ai) want = true;
        }
        const float dx = tr.world.x - rdf32(l + kActPos), dz = tr.world.z - rdf32(l + kActPos + 8);
        const float d = std::sqrt(dx * dx + dz * dz);
        static const float sDMax = std::getenv("LM_GHOST_HUNT_DMAX") ? float(std::atof(std::getenv("LM_GHOST_HUNT_DMAX"))) : 400.0f;
        if (!want || d < 100.0f || d > sDMax) continue;
        if (rd32(l + 0x1180) == 0) continue;  // Luigi startled (Poltergust mode 0): stun only once he can vacuum
        sForcedActor = a, sForcedFrame = sFrame;
        wr32(ev, kMsgInBeam);
        const Ctx saved = c;
        const u32 r = guestCall(c, thoughtFn, tr.thought, ev);
        c = saved;
        const u32 st = rd32(tr.thought + 0x800), sub = guestPtr(st) ? rd32(st + 0x590) : 0;
        log("[ghosthunt] f%u beam message -> %08X ai %X d %.0f result %u lit %d sub %08X st %X t %u n %u", sFrame, a, ai, d, r,
            s32(rd32(tr.thought + 0x908)), sub, guestPtr(sub) ? rd16(sub + 0xE) : 0xFFFF, guestPtr(sub) ? rd32(sub + 0x10) : 0,
            guestPtr(sub) ? rd32(sub + 0x14) : 0);
        break;
    }
}

}  // namespace recomp::ghostlights
