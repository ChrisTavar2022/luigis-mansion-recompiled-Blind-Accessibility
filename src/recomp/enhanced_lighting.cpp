// Enhanced lighting, port side (enhanced_lighting.h, docs/RENDERING.md "Enhanced lighting").
#include "recomp/enhanced_lighting.h"
#include "recomp/ghost_lights.h"
#include "recomp/world_lights.h"

#include "port/settings.h"
#include "recomp/hle.h"
#include "recomp/interp.h"
#include "recomp/runtime.h"
#include "recomp/version.h"

#include <aurora/enhanced.h>
#include <dolphin/gx.h>
#include <dolphin/gx/GXAurora.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string_view>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace recomp {
u32 copyDispCount();  // hle_gx.cpp
namespace gx {
void recheckDrawState();  // hle_gx.cpp: re-evaluate the draw class before the next raw draw
}

namespace {

std::atomic<bool> sActive{false};
bool sInScene = false, sInCaster = false;  // guest thread
u8 sClass = 0xFF;                          // last draw class sent
int sPerfPhase = -1;       // LM_ENH_PERF_AB phase (host thread)
bool sPerfNoMap = false;
bool sGhostPass = false;
bool sCharPass = false;
u8 sMaterial = 0xFF;  // last material hint sent

// Extra lights submitted for the coming main view (guest thread only). Providers (ghost lights,
// world lights, test lights) append; beginScene culls what cannot reach the view and keeps the
// MaxExtraLights strongest.
constexpr uint32_t kMaxCandidates = 512;
std::array<aurora::gfx::ExtraLight, kMaxCandidates> sPending{};
uint32_t sPendingCount = 0;

// Camera (lbl_803A3820): the player's view eye/target in world space.
constexpr u32 kCamViewEye = 0x00, kCamViewTarget = 0x0C;

// LM_TEST_LIGHTS=<count>[,<radius>[,<intensity>]]: colored point lights orbiting Luigi (US/PAL),
// to check extra lights before a real provider exists.
struct TestLights {
    int count = 0;
    float radius = 450.0f, intensity = 1.5f;
    float speed = 1.0f, dist = 220.0f, height = 110.0f;  // orbit speed (0 = fixed), distance, height above Luigi
};
const TestLights sTest = [] {
    TestLights t;
    if (const char* v = std::getenv("LM_TEST_LIGHTS")) {
        std::sscanf(v, "%d,%f,%f,%f,%f,%f", &t.count, &t.radius, &t.intensity, &t.speed, &t.dist, &t.height);
        t.count = std::clamp(t.count, 0, int(aurora::gfx::MaxExtraLights));
    }
    return t;
}();

void hueToRgb(float h, float out[3]) {
    const float h6 = h * 6.0f;
    out[0] = std::clamp(std::fabs(h6 - 3.0f) - 1.0f, 0.0f, 1.0f);
    out[1] = std::clamp(2.0f - std::fabs(h6 - 2.0f), 0.0f, 1.0f);
    out[2] = std::clamp(2.0f - std::fabs(h6 - 4.0f), 0.0f, 1.0f);
}

void addTestLights() {
    if (sTest.count <= 0) return;
    const u32 actor = rd32(VA(0x803D48A0));
    if (actor < 0x80000000u || actor >= 0x81800000u) return;
    const float luigi[3] = {rdf32(actor + 0x44), rdf32(actor + 0x48), rdf32(actor + 0x4C)};
    static u32 sFrame = 0;
    const float t = float(sFrame++) / 30.0f;
    for (int i = 0; i < sTest.count && sPendingCount < kMaxCandidates; i++) {
        const float a = t * 0.9f * sTest.speed + 6.2831853f * float(i) / float(sTest.count);
        const float world[3] = {luigi[0] + sTest.dist * std::cos(a),
                                luigi[1] + sTest.height + 40.0f * sTest.speed * std::sin(t * 1.7f + float(i)),
                                luigi[2] + sTest.dist * std::sin(a)};
        aurora::gfx::ExtraLight l{};
        if (!enh::worldToView(world, l.pos)) return;
        hueToRgb(float(i) / float(sTest.count), l.color);
        l.radius = sTest.radius;
        l.intensity = sTest.intensity;
        l.flags = aurora::gfx::ExtraLightTranslucent | aurora::gfx::ExtraLightSpecular |
                  (i < 8 ? aurora::gfx::ExtraLightCastShadows : 0u);
        l.id = 0x7E570000u + u32(i);
        sPending[sPendingCount++] = l;
    }
}

}  // namespace

void submitExtraLights(const aurora::gfx::ExtraLight* lights, uint32_t count) {
    count = std::min<uint32_t>(count, kMaxCandidates - sPendingCount);
    std::memcpy(sPending.data() + sPendingCount, lights, count * sizeof(aurora::gfx::ExtraLight));
    sPendingCount += count;
}

namespace {
// Lights whose sphere cannot touch the view frustum are dropped (generous side margins: the
// widescreen/FOV options widen the view); the rest are ranked by intensity x reach over distance
// and the strongest MaxExtraLights kept, in that order (Aurora gives shadow slots to the first
// flagged lights).
uint32_t selectLights(aurora::gfx::ExtraLight* l, uint32_t n) {
    struct Ranked {
        float score;
        uint32_t i;
    };
    static std::vector<Ranked> rank;
    rank.clear();
    for (uint32_t i = 0; i < n; i++) {
        const auto& x = l[i];
        if (!(x.intensity > 0.0f) || !(x.radius > 0.0f)) continue;
        const float z = -x.pos[2];  // distance in front of the camera
        if (z < -x.radius) continue;  // entirely behind the camera
        const float side = std::max(z, 0.0f) * 2.5f + x.radius;  // ~136 degrees wide, 2.5:1
        if (std::fabs(x.pos[0]) > side || std::fabs(x.pos[1]) > side) continue;
        const float d = std::sqrt(x.pos[0] * x.pos[0] + x.pos[1] * x.pos[1] + x.pos[2] * x.pos[2]);
        rank.push_back({x.intensity * x.radius / (1.0f + std::max(d - x.radius, 0.0f) / 800.0f), i});
    }
    std::stable_sort(rank.begin(), rank.end(), [](const Ranked& a, const Ranked& b) { return a.score > b.score; });
    static std::array<aurora::gfx::ExtraLight, aurora::gfx::MaxExtraLights> out;
    uint32_t m = 0;
    for (const auto& r : rank) {
        if (m == aurora::gfx::MaxExtraLights) break;
        out[m++] = l[r.i];
    }
    // Shadow slots go to the first flagged lights in this order (Aurora). Each light's shadow fades
    // in over a few frames when it gets one and out when it loses it (weight in flags bits 8..15,
    // multiplied with the provider's own weight), so a change in the strongest set does not pop.
    {
        constexpr u32 kShadow = aurora::gfx::ExtraLightCastShadows;
        static std::unordered_map<u32, float> sW;
        static std::unordered_map<u32, u32> sSeen;
        static u32 sTick = 0;
        sTick++;
        const u32 cap = enh::shadowLightCap();
        u32 wanted = 0;
        for (uint32_t i = 0; i < m; i++) {
            auto& x = out[i];
            if (!(x.flags & kShadow) || x.id == 0) continue;
            const bool want = wanted < cap;
            wanted += want;
            float& w = sW[x.id];
            sSeen[x.id] = sTick;
            w += ((want ? 1.0f : 0.0f) - w) * 0.25f;
            if (!want && w < 0.03f) w = 0.0f;
            const u32 own = (x.flags >> 8) & 0xFFu;
            const float ow = own ? float(own) / 255.0f : 1.0f;
            const float fw = w * ow;
            x.flags &= ~(kShadow | 0xFF00u);
            if (fw > 0.0f) x.flags |= kShadow | (std::clamp(u32(fw * 255.0f + 0.5f), 1u, 255u) << 8);
        }
        if ((sTick & 63) == 0) {
            for (auto it = sSeen.begin(); it != sSeen.end();) {
                if (sTick - it->second > 60) {
                    sW.erase(it->first);
                    it = sSeen.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }
    std::memcpy(l, out.data(), m * sizeof(aurora::gfx::ExtraLight));
    return m;
}
}  // namespace

namespace enh {

// LM_ENHANCED_AB=<n>: A/B test aid. Game frames alternate in blocks of n between the enhanced
// rendering and the plain one (draws classed "other", no extra lights, no scene end: exactly
// the plain renderer), so LM_FRAME_DUMP frames of one run compare both. With n = 1 odd dump
// frames are enhanced, even ones plain.
const u32 sAb = [] {
    const char* v = std::getenv("LM_ENHANCED_AB");
    return v ? u32(std::max(std::atoi(v), 0)) : 0u;
}();
bool frameOn() {
    if (sAb == 0) return true;
    return ((copyDispCount() + 1) / sAb) % 2 == 1;  // copyDispCount() + 1: the dump number of this frame
}

bool active() { return sActive.load(std::memory_order_relaxed) && frameOn(); }

uint32_t shadowLightCap() {
    const auto& l = port::settings().enhancedLighting;
    if (!l.extraLightShadows) return 0;
    static constexpr u32 kXlLights[4] = {0, 2, 4, 8};
    u32 n = kXlLights[std::clamp(l.extraShadowLights, 0, 3)];
    if (const char* v = std::getenv("LM_XL_SHADOWS")) {
        int mode = -1, lights = -1;
        std::sscanf(v, "%d,%d", &mode, &lights);
        if (lights >= 0) n = u32(lights);
        if (mode == 0) n = std::min(n, 4u);
    } else if (l.extraShadowMode == 0) {
        n = std::min(n, 4u);
    }
    return std::min(n, 8u);
}
bool masterOn() { return sActive.load(std::memory_order_relaxed); }
int perfPhase() { return sPerfPhase; }

void applySettings() {
    auto& l = port::settings().enhancedLighting;
    // Presets fill in the quality fields (Custom keeps whatever was set).
    switch (l.preset) {
    case port::EnhancedPreset::Low:
        l.softShadowQuality = 0, l.volumetricQuality = 0, l.extraShadowQuality = 0;
        l.extraShadowLights = 1, l.extraShadowMapSize = 0;
        break;
    case port::EnhancedPreset::Medium:
        l.softShadowQuality = 1, l.volumetricQuality = 1, l.extraShadowQuality = 1;
        l.extraShadowLights = 1, l.extraShadowMapSize = 1;
        break;
    case port::EnhancedPreset::High:
        l.softShadowQuality = 2, l.volumetricQuality = 1, l.extraShadowQuality = 1;
        l.extraShadowLights = 2, l.extraShadowMapSize = 1;
        break;
    case port::EnhancedPreset::Ultra:
        l.softShadowQuality = 3, l.volumetricQuality = 2, l.extraShadowQuality = 2;
        l.extraShadowLights = 3, l.extraShadowMapSize = 2;
        break;
    default:
        break;
    }
    static const int sForce = [] {
        const char* v = std::getenv("LM_ENHANCED");  // test override: 0 off, 1 on
        return v ? std::atoi(v) : -1;
    }();
    AuroraEnhancedConfig c{};
    c.enabled = sForce >= 0 ? sForce != 0 : l.enabled;
    c.perPixelLighting = l.perPixelLighting;
    c.flashlightQuality = l.flashlightQuality;
    c.flashlightSoftness = l.flashlightSoftness;
    c.flashlightSpecular = l.flashlightSpecular;
    c.softShadows = l.softShadows;
    c.softShadowQuality = u32(std::clamp(l.softShadowQuality, 0, 3));
    c.softShadowRadius = l.softShadowRadius;
    c.softShadowPenumbra = l.softShadowPenumbra;
    c.shadowDarkness = l.shadowDarkness;
    c.shadowAmbientKeep = l.shadowAmbientKeep;
    c.extraLights = l.extraLights;
    c.extraLightIntensity = l.extraLightIntensity;
    c.extraLightSaturation = l.extraLightSaturation;
    c.extraShadows = l.extraLightShadows;
    c.extraShadowQuality = u32(std::clamp(l.extraShadowQuality, 0, 2));
    static const int sXlMode = [] {  // test override LM_XL_SHADOWS=<mode 0/1>[,<lights>[,<face size>]]
        const char* v = std::getenv("LM_XL_SHADOWS");
        return v ? std::atoi(v) : -1;
    }();
    static const std::array<int, 2> sXlOver = [] {
        std::array<int, 2> o{-1, -1};
        if (const char* v = std::getenv("LM_XL_SHADOWS")) std::sscanf(v, "%*d,%d,%d", &o[0], &o[1]);
        return o;
    }();
    static constexpr u32 kXlLights[4] = {0, 2, 4, 8};
    static constexpr u32 kXlSize[4] = {256, 512, 1024, 2048};
    c.extraShadowMode = u32(sXlMode >= 0 ? sXlMode : std::clamp(l.extraShadowMode, 0, 1));
    c.extraShadowLights = sXlOver[0] >= 0 ? u32(sXlOver[0]) : kXlLights[std::clamp(l.extraShadowLights, 0, 3)];
    c.extraShadowMapSize = sXlOver[1] > 0 ? u32(sXlOver[1]) : kXlSize[std::clamp(l.extraShadowMapSize, 0, 3)];
    c.extraShadowSoftness = l.extraShadowSoftness;
    c.volumetric = l.volumetricBeam;
    c.volumetricIntensity = l.volumetricIntensity;
    c.volumetricQuality = u32(std::clamp(l.volumetricQuality, 0, 2));
    c.bloom = l.bloom;
    c.bloomIntensity = l.bloomIntensity;
    c.bloomThreshold = l.bloomThreshold;
    c.bloomRadius = l.bloomRadius;
    c.bloomFlashlightSuppress = l.bloomFlashlightSuppress;
    c.bloomScene = l.bloomScene;
    c.bloomEmissive = l.bloomEmissive;
    c.bloomLightGlow = l.bloomLightGlow;
    c.ssao = l.ssao;
    c.ssaoIntensity = l.ssaoIntensity;
    c.ssaoRadius = l.ssaoRadius;
    // LM_ENH_PERF_AB=<seconds>,<part>: performance A/B within one run. Every <seconds> the part
    // (all, ppl, flq, soft, xl, xls, vol, bloom, ssao, map) is switched off/on; LM_FPS_LOG lines
    // carry the phase (perfPhase()).
    static const struct PerfAb {
        float seconds = 0.0f;
        char part[16] = {};
    } sPerf = [] {
        PerfAb p;
        if (const char* v = std::getenv("LM_ENH_PERF_AB")) std::sscanf(v, "%f,%15s", &p.seconds, p.part);
        return p;
    }();
    if (sPerf.seconds > 0.0f) {
        const auto t = std::chrono::duration<float>(std::chrono::steady_clock::now().time_since_epoch()).count();
        sPerfPhase = int(t / sPerf.seconds) % 2;
        if (sPerfPhase == 1) {
            const std::string_view part = sPerf.part;
            if (part == "all") c.enabled = false;
            if (part == "ppl") c.perPixelLighting = false;
            if (part == "flq") c.flashlightQuality = false;
            if (part == "soft") c.softShadows = false;
            if (part == "xl") c.extraLights = false;
            if (part == "xls") c.extraShadows = false;
            if (part == "vol") c.volumetric = false;
            if (part == "bloom") c.bloom = false;
            if (part == "ssao") c.ssao = false;
            if (part == "map") sPerfNoMap = true;
        } else {
            sPerfNoMap = false;
        }
    }
    aurora_enhanced_set_config(&c);
    sActive.store(c.enabled, std::memory_order_relaxed);
}

// Draw class (docs/RENDERING.md "Enhanced lighting"): set at hook boundaries, never between the
// game's own GX commands: WORLD inside the main 3D view (fn_8000BB98), GHOST inside its ghost
// pass, OTHER in the shadow-map caster pass and everywhere else (HUD/2D, GBH, copies). Aurora
// additionally ignores non-perspective draws for its re-rendered passes.
void emitMaterial();
void emitClass() {
    if (!masterOn()) return;
    u8 cls = AURORA_DRAW_CLASS_OTHER;
    if (sInScene && !sInCaster && active()) cls = sGhostPass ? AURORA_DRAW_CLASS_GHOST : AURORA_DRAW_CLASS_WORLD;
    if (cls == sClass) return;
    GXSetDrawClassAurora(cls);
    sClass = cls;
}

void setGhostPass(bool on) {
    if (sGhostPass == on) return;
    sGhostPass = on;
    emitClass();
    emitMaterial();
}
bool ghostPass() { return sGhostPass; }

u32 sSceneCount = 0;
void emitMaterial() {
    if (!masterOn()) return;
    // Test aid LM_CHAR_MATERIAL=0 (off: characters lit like the world, as before) / 2 (only on
    // even frames: consecutive frame dumps compare both).
    static const int sMode = [] {
        const char* v = std::getenv("LM_CHAR_MATERIAL");
        return v ? std::atoi(v) : 1;
    }();
    const bool allowed = sMode == 1 || (sMode == 2 && (sSceneCount & 1) == 0);
    const u8 m = allowed && sInScene && sCharPass && !sGhostPass && !sInCaster ? AURORA_MATERIAL_CHARACTER : AURORA_MATERIAL_WORLD;
    if (m == sMaterial) return;
    GXSetMaterialHintAurora(m);
    sMaterial = m;
}
void setCharacterPass(bool on) {
    if (sCharPass == on) return;
    sCharPass = on;
    emitMaterial();
}
bool characterPass() { return sCharPass; }

void setCasterPass(bool on) {
    if (sInCaster == on) return;
    sInCaster = on;
    emitClass();
    emitMaterial();
}

void beginScene() {
    addTestLights();
    const uint32_t count = selectLights(sPending.data(), sPendingCount);
    sPendingCount = 0;  // a frame in which nobody submits has no extra lights
    sClass = 0xFF;      // every frame (and interpolation tape) sets its class again
    sMaterial = 0xFF;
    sSceneCount++;
    sInScene = true;
    emitClass();
    emitMaterial();
    if (!active()) return;
    const u32 at = interp::mark();
    aurora::gfx::set_extra_lights(sPending.data(), count);
    interp::noteExtraLights(at, count);
}

void endScene() {
    sInScene = false;
    emitClass();
    emitMaterial();
    if (active()) GXSceneEndAurora();
}

uint32_t shadowMapWidth() {
    if (!active()) return 0;
    static const int sForce = [] {
        const char* v = std::getenv("LM_SHADOW_MAP");  // test override: 0 render res, 1 2048, 2 4096
        return v ? std::atoi(v) : -1;
    }();
    if (sPerfNoMap) return 0;
    const int size = sForce >= 0 ? sForce : port::settings().enhancedLighting.shadowMapSize;
    return size == 1 ? 2048 : size == 2 ? 4096 : 0;
}

bool worldToView(const float world[3], float view[3]) {
    const u32 cam = VA(0x803A3820);
    const float eye[3] = {rdf32(cam + kCamViewEye), rdf32(cam + kCamViewEye + 4), rdf32(cam + kCamViewEye + 8)};
    const float at[3] = {rdf32(cam + kCamViewTarget), rdf32(cam + kCamViewTarget + 4), rdf32(cam + kCamViewTarget + 8)};
    // MTXLookAt with up (0, 1, 0): z = normalize(eye - target), x = normalize(up x z), y = z x x.
    float z[3] = {eye[0] - at[0], eye[1] - at[1], eye[2] - at[2]};
    const float zl = std::sqrt(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
    if (!(zl > 1e-3f)) return false;
    for (float& v : z) v /= zl;
    float x[3] = {z[2], 0.0f, -z[0]};  // (0,1,0) x z
    const float xl = std::sqrt(x[0] * x[0] + x[2] * x[2]);
    if (!(xl > 1e-5f)) return false;
    x[0] /= xl, x[2] /= xl;
    const float y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
    const float d[3] = {world[0] - eye[0], world[1] - eye[1], world[2] - eye[2]};
    view[0] = x[0] * d[0] + x[1] * d[1] + x[2] * d[2];
    view[1] = y[0] * d[0] + y[1] * d[1] + y[2] * d[2];
    view[2] = z[0] * d[0] + z[1] * d[1] + z[2] * d[2];
    return true;
}

}  // namespace enh

// Main 3D view of the game scene (rooms, actors, Luigi, effects, the ghost layer), called by the
// scene draw fn_8000BCE8 before the screen copy and the HUD. r3 != 0 selects a reduced pass
// (rooms and some actors only), which is left alone. The European function is identical apart
// from relocations.
HLE(fn_8000BB98) {
    const bool main = c.r[3] == 0;
    if (main) ghostlights::onMainView(c);  // ghost_lights.cpp: this frame's ghost lights
    if (main) worldlights::onMainView(c);  // world_lights.cpp: candles, fires, lamps, sprays
    if (main) enh::beginScene();
    callOriginal(c, "fn_8000BB98");
    if (main) enh::endScene();
}

}  // namespace recomp
