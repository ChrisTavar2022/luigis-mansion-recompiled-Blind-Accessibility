#include "recomp/widescreen.h"

#include "port/options.h"
#include "recomp/hle.h"

#include <dolphin/gx/GXAurora.h>

#include <atomic>

namespace recomp::ws {
namespace {

constexpr float kBaseAspect = 4.0f / 3.0f;
std::atomic<float> sFactor{1.0f};
bool sOriginal = false;  // --aspect 4:3: Aurora fits the 4:3 frame into the window

float factorFor(float aspect) { return aspect > kBaseAspect ? kBaseAspect / aspect : 1.0f; }

}  // namespace

void init() {
    const float aspect = port::options().aspectRatio;
    sOriginal = aspect > 0.0f && aspect <= kBaseAspect + 1e-3f;
    AuroraSetViewportPolicy(sOriginal ? AURORA_VIEWPORT_FIT : AURORA_VIEWPORT_STRETCH);
    log("[ws] aspect %s", sOriginal ? "4:3 (original)" : aspect > 0.0f ? "fixed" : "auto (window)");
}

void beginFrame() {
    float f = 1.0f;
    if (!sOriginal) {
        const float fixed = port::options().aspectRatio;
        if (fixed > 0.0f) {
            f = factorFor(fixed);
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
