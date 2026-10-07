#include "recomp/widescreen.h"

#include "port/install.h"
#include "port/options.h"
#include "port/settings.h"
#include "recomp/hle.h"

#include <dolphin/gx/GXAurora.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

namespace recomp::ws {
namespace {

constexpr float kBaseAspect = 4.0f / 3.0f;
std::atomic<float> sFactor{1.0f};
std::atomic<float> sFov{1.0f};
std::atomic<bool> sAnchorHud{false};
bool sOriginal = false;  // 4:3 mode: Aurora fits the 4:3 frame into the window
bool sPolicySet = false;

float factorFor(float aspect) { return aspect > kBaseAspect ? kBaseAspect / aspect : 1.0f; }

}  // namespace

// --aspect on the command line wins; otherwise the Visuals tab's setting (live).
float configuredAspect() {
    if (port::options().aspectRatio > 0.0f) return port::options().aspectRatio;
    switch (port::settings().video.aspect) {
    case port::AspectMode::Original4x3: return kBaseAspect;
    case port::AspectMode::Wide16x9: return 16.0f / 9.0f;
    case port::AspectMode::Wide16x10: return 16.0f / 10.0f;
    case port::AspectMode::Ultra21x9: return 21.0f / 9.0f;
    case port::AspectMode::Ultra32x9: return 32.0f / 9.0f;
    default: return 0.0f;  // match the window
    }
}

void applyPolicy(float aspect) {
    const bool original = aspect > 0.0f && aspect <= kBaseAspect + 1e-3f;
    if (original != sOriginal || !sPolicySet) {
        sOriginal = original;
        sPolicySet = true;
        AuroraSetViewportPolicy(sOriginal ? AURORA_VIEWPORT_FIT : AURORA_VIEWPORT_STRETCH);
        log("[ws] aspect %s", sOriginal ? "4:3 (original)" : aspect > 0.0f ? "fixed" : "auto (window)");
    }
}

void init() { applyPolicy(configuredAspect()); }

void beginFrame() {
    const float aspect = configuredAspect();
    applyPolicy(aspect);
    float f = 1.0f;
    if (!sOriginal) {
        if (aspect > 0.0f) {
            f = factorFor(aspect);
        } else {
            u32 w = 0, h = 0;
            AuroraGetRenderSize(&w, &h);
            if (w && h) f = factorFor(float(w) / float(h));
        }
    }
    const float old = sFactor.exchange(f);
    if (old != f) log("[ws] widescreen factor %.4f (aspect %.4f)", f, kBaseAspect / f);
    sFov = std::clamp(port::settings().video.fovScale, 0.5f, 2.0f);
    sAnchorHud = !port::settings().video.hudKeep4x3;
}

float fovRatio(float m11) {
    const float k = sFov.load(std::memory_order_relaxed);
    if (k == 1.0f || m11 <= 0.0f) return 1.0f;
    const float half = std::atan(1.0f / m11);
    const float scaled = std::clamp(half * k, 0.01f, 1.5f);
    return std::tan(half) / std::tan(scaled);
}

bool adjusting() { return active() || sFov.load(std::memory_order_relaxed) != 1.0f; }


float factor() { return sFactor.load(std::memory_order_relaxed); }

Background backgroundMode() { return Background(port::options().wsBackgrounds); }

// fn_80008D58(camera, top, bottom, left, right, near, far) builds the camera's
// four side culling planes. All 3D projection setters (fn_80008654,
// fn_800087D0, fn_80008928) end with it, so widening left/right here keeps the
// culling volume in step with the Hor+ projection from the GX HLE.
// The field-of-view setting scales all four sides by the same ratio as the
// projection (from the camera's fovY at +0x90, degrees).
HLE(fn_80008D58) {
    const u32 cam = c.r[3];
    const bool fullScreen = rd32(cam + 0x1EC) == 0 && rd32(cam + 0x1F4) >= 640;  // viewport x, width
    if (adjusting() && fullScreen) {
        const float fovY = rdf32(cam + 0x90) * 3.14159265f / 180.0f;
        const double sides = fovY > 0.0f ? 1.0 / fovRatio(1.0f / std::tan(fovY * 0.5f)) : 1.0;
        const double inv = sides / factor();
        c.setSingle(1, float(c.f[1] * sides));
        c.setSingle(2, float(c.f[2] * sides));
        c.setSingle(3, float(c.f[3] * inv));
        c.setSingle(4, float(c.f[4] * inv));
    }
    callOriginal(c, "fn_80008D58");
}


// HUD anchoring. Menus (title, file select, options...) are J2DScreens; the
// in-game HUD is a few J2DScreens drawn by two game functions plus panes and
// pictures that game code draws directly (the health meter, the room name).
// For those, each 2D draw that lies in the outer 30% of the 4:3 frame on one
// side moves to that edge of the display (the GX HLE decodes its vertices;
// hudAnchorFor). HUD pieces are small and sit well inside one band, so a
// counter's digits and its icon move together.
namespace {
struct HudScreen {
    u32 start, size;
};
constexpr HudScreen kHudScreens[] = {
    {0x80056F64, 0x168},  // fn_80056F64
    {0x8003E3D0, 0x3E0},  // fn_8003E3D0
};
int sScreenDepth = 0;   // J2DScreen::draw nesting
bool sInHudScreen = false;
}  // namespace

bool inHud() {
    // Needs the J2DScreen::draw hook and the USA HUD functions above (docs/SETUP.md).
    if (port::usCodeFeature() != port::Avail::Yes) return false;
    return sAnchorHud.load(std::memory_order_relaxed) && active() && (sScreenDepth == 0 || sInHudScreen);
}

int hudAnchorFor(float lo, float hi) { return hi <= -0.4f ? -1 : lo >= 0.4f ? 1 : 0; }

HLE(draw__9J2DScreenFiiPC14J2DGrafContext) {
    const bool outer = sInHudScreen;
    if (sScreenDepth == 0) {
        sInHudScreen = false;
        for (const HudScreen& s : kHudScreens)
            if (c.lr - s.start < s.size) sInHudScreen = true;
    }
    sScreenDepth++;
    callOriginal(c, "draw__9J2DScreenFiiPC14J2DGrafContext");
    sScreenDepth--;
    sInHudScreen = outer;
}

}  // namespace recomp::ws
