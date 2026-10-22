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

// ---- ghost size --------------------------------------------------------------------------
// S3D models keep their joints in world space (S3DModel +0x14); calcView fn_80059710(model,
// view) puts them in eye space (+0x18 = view x joint) and builds the normal matrices (+0x1C, the
// inverse transpose, fn_8000A0D0) for every pass that draws the model (scene, ghost layer, shadow
// maps). For a ghost the view is replaced by view x S, S = scale about the actor's position
// (+0x44), so the whole ghost is drawn bigger or smaller: its joints, collision, hit test, vacuum
// fight, glow light and camera are the game's own, unchanged. The normal matrices are multiplied
// back by the scale (the inverse transpose shrinks them by 1/s, which would change the lighting),
// and the view copy calcView keeps (lbl_803CFAB0 + 4) is put back to the real view afterwards.
namespace recomp::sillyworld {
namespace {

u32 actorForModel(u32 obj) {
    const u32 n = std::min(rd32(kActorCount), kMaxSlots);
    for (u32 i = 0; i < n; i++) {
        const u32 a = rd32(kActorSlots + 4 * i);
        if (gu::inMem1(a) && rd32(a + 4) == obj) return a;
    }
    return 0;
}

bool isBooType(u32 type) { return std::strstr(typeName(type), "tele") != nullptr; }

float ghostScaleFor(u32 actor) {
    const float s = std::clamp(cfg().ghostSize, 0.5f, 3.0f);
    if (s == 1.0f || !gu::inMem1(actor)) return 1.0f;
    const u32 type = rd32(actor + 0x3C);
    if (!isGhostType(type)) return 1.0f;
    if (cfg().ghostSizeTargets == 1 && !isBooType(type)) return 1.0f;
    return s;
}

}  // namespace
}  // namespace recomp::sillyworld

HLE(fn_80059710) {
    const u32 obj = c.r[3], view = c.r[4];
    const float s = gu::inMem1(obj) && gu::inMem1(view) ? ghostScaleFor(actorForModel(obj)) : 1.0f;
    if (s == 1.0f) return callOriginal(c, "fn_80059710");
    static u32 sTmp = 0;
    if (!sTmp) sTmp = unused2::extraAlloc(0x30, 32);
    const u32 actor = actorForModel(obj);
    if (!sTmp) return callOriginal(c, "fn_80059710");
    float v[12], p[3];
    for (int i = 0; i < 12; i++) v[i] = gu::rdf(view + 4 * i);
    for (int i = 0; i < 3; i++) p[i] = gu::rdf(actor + 0x44 + 4 * i);
    // view x [sI | (1-s) p]
    for (int r = 0; r < 3; r++) {
        float t = v[r * 4 + 3];
        for (int k = 0; k < 3; k++) {
            gu::wrf(sTmp + 4 * (r * 4 + k), v[r * 4 + k] * s);
            t += v[r * 4 + k] * (1.0f - s) * p[k];
        }
        gu::wrf(sTmp + 4 * (r * 4 + 3), t);
    }
    c.r[4] = sTmp;
    callOriginal(c, "fn_80059710");
    const u32 keep = VA(0x803CFAB0) + 4;
    for (int i = 0; i < 12; i++) gu::wrf(keep + 4 * i, v[i]);
    const u32 mdl = rd32(obj + 4), nrm = rd32(obj + 0x1C);
    if (gu::inMem1(mdl) && gu::inMem1(nrm)) {
        const u32 n = u32(rd16(mdl + 8)) + rd16(mdl + 0xC);
        for (u32 m = 0; m < n; m++)
            for (int r = 0; r < 3; r++)
                for (int k = 0; k < 3; k++) {
                    const u32 a = nrm + m * 0x30 + 4 * (r * 4 + k);
                    gu::wrf(a, gu::rdf(a) * s);
                }
    }
}

namespace recomp::sillyworld {
namespace {

// ---- colours ---------------------------------------------------------------------------------
void hueRgb(float h, float out[3]) {
    h = h - std::floor(h);
    const float x = h * 6.0f;
    out[0] = std::clamp(std::fabs(x - 3.0f) - 1.0f, 0.0f, 1.0f);
    out[1] = std::clamp(2.0f - std::fabs(x - 2.0f), 0.0f, 1.0f);
    out[2] = std::clamp(2.0f - std::fabs(x - 4.0f), 0.0f, 1.0f);
}

u32 hashU32(u32 h) {
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}

// Per-ghost hue: the actor (characters are pooled, so a ghost keeps its slot for its life) and its
// type. Golden-ratio steps of the hash spread neighbouring ghosts over the colour wheel.
float ghostHue(u32 actor) { return float(hashU32(actor * 0x9E3779B1u ^ rd32(actor + 0x3C)) & 0xFFFF) / 65536.0f; }

bool shuffleOn(u32 actor) { return cfg().ghostColorShuffle && gu::inMem1(actor) && isGhostType(rd32(actor + 0x3C)); }

// Disco clock: game frames (the colour changes at the game rate; slow enough not to step visibly).
u32 sDiscoFrames = 0;
float discoHue() { return float(sDiscoFrames) / 300.0f * std::clamp(cfg().discoSpeed, 0.1f, 5.0f); }

// ---- Luigi's voice ---------------------------------------------------------------------------
// Luigi's voice clips are sound effects of their own (JAI SE ids, LM_SW_SE_LOG lists every start):
// the "Mario?" calls, yelps, gasps, laughs, humming and whistling. kVoice holds their ids.
// A started one gets JAISound::setPitch (the same per-sound pitch the game uses for its own
// effects), which JAudio multiplies into the voice's DSP pitch; music and other effects are not
// touched. The ids are shared by Play as Mario (the Mario model keeps Luigi's sounds).
struct IdRange {
    u32 lo, hi;
};
const IdRange kVoice[] = {
    {0, 0},  // filled in below (research)
};
bool isLuigiVoice(u32 id) {
    const u32 n = id & 0xFFFFFu;
    for (const IdRange& r : kVoice)
        if (r.hi && n >= r.lo && n <= r.hi) return true;
    return false;
}

}  // namespace

void onFrame(Ctx& c) {
    sDiscoFrames++;
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
    const u32 snd = gu::inMem1(handle) ? rd32(handle) : 0;
    const bool voice = isLuigiVoice(id);
    if (sSeLog) log("[silly-se] f%u id %08X sound %08X%s", skip::guestFrames(), id, snd, voice ? " (Luigi voice)" : "");
    const float pitch = std::clamp(cfg().luigiVoicePitch, 0.5f, 2.0f);
    if (!voice || pitch == 1.0f || !gu::inMem1(snd) || rd32(snd + 8) != id) return;
    gu::call(c, GU_SYM("setPitch__8JAISoundFfUlUc"), {snd, 0, 0}, {double(pitch)});
}

void ghostMaterialColor(u32 actor, u32 colorPtr) {
    if (!shuffleOn(actor) || !gu::inMem1(colorPtr)) return;
    float rgb[3];
    hueRgb(ghostHue(actor), rgb);
    u8* p = hostp(colorPtr);
    // The material colour is a grey (0x40 for a fully visible ghost) that the ghost layer adds as
    // light: the same strength, in the ghost's own colour (a little brighter, as one saturated
    // channel carries less light than a grey).
    const float m = float(std::max({p[0], p[1], p[2]}));
    for (int k = 0; k < 3; k++) p[k] = u8(std::min(255.0f, rgb[k] * m * 1.6f + 0.5f));
}

bool ghostGlowColor(u32 actor, u32 rgb[3]) {
    if (!shuffleOn(actor)) return false;
    float h[3];
    hueRgb(ghostHue(actor), h);
    for (int k = 0; k < 3; k++) rgb[k] = u32(std::lround(40.0f + 215.0f * h[k]));
    return true;
}

void discoAmbient(u8 rgb[3]) {
    float h[3];
    hueRgb(discoHue(), h);
    // Same overall light, in a saturated colour (a pure hue carries a third to two thirds of a
    // grey's light, so it is lifted).
    const float lum = (float(rgb[0]) + float(rgb[1]) + float(rgb[2])) / 3.0f;
    for (int k = 0; k < 3; k++) rgb[k] = u8(std::min(255.0f, lum * (0.15f + 1.85f * h[k]) + 0.5f));
}

void discoLight(u32 id, float color[3]) {
    float h[3];
    (void)id;  // one colour for the whole mansion, in step with the room light
    hueRgb(discoHue(), h);
    for (int k = 0; k < 3; k++) color[k] = 0.08f + 0.92f * h[k];
}

bool discoOn() { return cfg().disco; }

}  // namespace recomp::sillyworld

// ---- F1 > Enhancements > Silly ---------------------------------------------------------------
#include "recomp/menu.h"

#include <imgui.h>

namespace recomp::silly {
namespace {
void tipW(const char* t) { ImGui::SetItemTooltip("%s", t); }
}  // namespace

void drawWorldOptions() {
    auto& s = port::settings().sillyWorld;
    bool save = false;
    ImGui::SeparatorText("Ghosts");
    menu::SliderFloat("Ghost size", &s.ghostSize, 0.5f, 3.0f, "%.2fx");
    save |= ImGui::IsItemDeactivatedAfterEdit();
    tipW("Tiny or giant ghosts: every ghost's whole model is drawn scaled about its position.\n"
         "Visual only: their hit boxes, the vacuum fight and the camera are the original ones.\n"
         "1.00x = original.");
    ImGui::BeginDisabled(s.ghostSize == 1.0f);
    ImGui::Indent();
    save |= ImGui::RadioButton("All ghosts##swsize", &s.ghostSizeTargets, 0);
    ImGui::SameLine();
    save |= ImGui::RadioButton("Boos only##swsize", &s.ghostSizeTargets, 1);
    ImGui::Unindent();
    ImGui::EndDisabled();
    save |= menu::Checkbox("Ghost colour shuffle", &s.ghostColorShuffle);
    tipW("Every ghost glows in a colour of its own, picked at random and kept for that ghost.\n"
         "With Enhanced lighting, the light it casts on the room takes the same colour.\n"
         "Portrait ghosts and bosses change too; Boos as well.");

    ImGui::SeparatorText("Mansion");
    save |= menu::Checkbox("Disco mansion", &s.disco);
    tipW("The room light slowly cycles through the colours of the rainbow. Works on the game's\n"
         "own room light; with Enhanced lighting the lamps and chandeliers join in.\n"
         "Dark rooms stay dark until their lights come on.");
    ImGui::BeginDisabled(!s.disco);
    ImGui::Indent();
    menu::SliderFloat("Disco speed", &s.discoSpeed, 0.1f, 5.0f, "%.1f");
    save |= ImGui::IsItemDeactivatedAfterEdit();
    tipW("Colour cycles per 10 seconds.");
    ImGui::Unindent();
    ImGui::EndDisabled();

    ImGui::SeparatorText("Luigi's voice");
    menu::SliderFloat("Voice pitch", &s.luigiVoicePitch, 0.5f, 2.0f, "%.2fx");
    save |= ImGui::IsItemDeactivatedAfterEdit();
    tipW("Higher (chipmunk) or lower (deep) Luigi: only his voice (calling Mario, yelps, gasps,\n"
         "humming and whistling), not the music or other sounds. Applies to the next thing he\n"
         "says. 1.00x = original.");

    ImGui::SeparatorText("Treasure");
    save |= menu::Checkbox("Moon gravity", &s.moonGravity);
    tipW("Coins, bills, gold bars and gems that spill out fall slowly, fly higher and bounce more,\n"
         "then settle as usual and can be collected. They vanish after the usual time.");
    ImGui::BeginDisabled(!s.moonGravity);
    ImGui::Indent();
    menu::SliderFloat("Gravity", &s.moonGravityScale, 0.05f, 1.0f, "%.2fx");
    save |= ImGui::IsItemDeactivatedAfterEdit();
    tipW("Gravity on spilled treasure while Moon gravity is on (0.17x is the Moon's).");
    ImGui::Unindent();
    ImGui::EndDisabled();
    save |= menu::Checkbox("Confetti coins", &s.confettiCoins);
    tipW("Every burst of coins or bills throws a shower of coloured sparkles (the game's own\n"
         "pink, green, blue and yellow key sparkles and its gold treasure glitter).");
    if (save) port::saveSettings();
}
}  // namespace recomp::silly
