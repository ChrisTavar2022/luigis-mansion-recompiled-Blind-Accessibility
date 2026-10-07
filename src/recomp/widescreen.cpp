#include "recomp/widescreen.h"

#include "port/options.h"
#include "port/settings.h"
#include "recomp/hle.h"

#include <dolphin/gx/GXAurora.h>

#include <atomic>

namespace recomp::ws {
namespace {

constexpr float kBaseAspect = 4.0f / 3.0f;
std::atomic<float> sFactor{1.0f};
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
}

float factor() { return sFactor.load(std::memory_order_relaxed); }

Background backgroundMode() { return Background(port::options().wsBackgrounds); }

// fn_80008D58(camera, top, bottom, left, right, near, far) builds the camera's
// four side culling planes. All 3D projection setters (fn_80008654,
// fn_800087D0, fn_80008928) end with it, so widening left/right here keeps the
// culling volume in step with the Hor+ projection from the GX HLE.
HLE(fn_80008D58) {
    const u32 cam = c.r[3];
    const bool fullScreen = rd32(cam + 0x1EC) == 0 && rd32(cam + 0x1F4) >= 640;  // viewport x, width
    if (active() && fullScreen) {
        const double inv = 1.0 / factor();
        c.setSingle(3, float(c.f[3] * inv));
        c.setSingle(4, float(c.f[4] * inv));
    }
    callOriginal(c, "fn_80008D58");
}

}  // namespace recomp::ws
