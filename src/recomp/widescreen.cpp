#include "recomp/widescreen.h"

#include "port/install.h"
#include "port/options.h"
#include "port/settings.h"
#include "recomp/hle.h"
#include "recomp/version.h"
#include "recomp/coop.h"

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

namespace {
bool sShadowLightSetup = false;
}  // namespace
void setShadowLightSetup(bool on) { sShadowLightSetup = on; }


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
    // Viewport x and width (the European camera has 12 more bytes before them; fovY is unmoved).
    const bool fullScreen = rd32(cam + VOFF(0x1EC, 0x1F8)) == 0 && rd32(cam + VOFF(0x1F4, 0x200)) >= 640;
    if (adjusting() && fullScreen && !sShadowLightSetup) {
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


// HUD anchoring (widescreen.h, AnchorScope). The in-game HUD is drawn once per frame by
// fn_8003E3D0, which calls one function per element; those are hooked here, so whatever
// they draw (J2DScreens, panes, pictures, text, the GBH's screen) moves as one unit.
// Elements not listed (the element meter, the ghost counters) stay centered with the
// dialogue and menus. The guest thread is the only caller.
namespace {
int sAnchorStack[16];
int sAnchorDepth = 0;
int sRttDepth = 0;  // RenderToTextureScope nesting
}  // namespace

AnchorScope::AnchorScope(int anchor) {
    if (sAnchorDepth < int(std::size(sAnchorStack))) sAnchorStack[sAnchorDepth] = anchor;
    sAnchorDepth++;
}
AnchorScope::~AnchorScope() { sAnchorDepth--; }

int hudAnchor() {
    if (sAnchorDepth <= 0 || !sAnchorHud.load(std::memory_order_relaxed) || !active()) return 0;
    return sAnchorStack[std::min(sAnchorDepth, int(std::size(sAnchorStack))) - 1];
}

bool inHudScope() { return sAnchorDepth > 0 && sRttDepth == 0 && active(); }

RenderToTextureScope::RenderToTextureScope() { sRttDepth++; }
RenderToTextureScope::~RenderToTextureScope() { sRttDepth--; }

// Game Boy Horror (bottom right): frame, LEDs and the J2DScreen of whatever its screen
// shows (money tally, E. Gadd's call, the cut E3 clock).
HLE(fn_8003D634) {
    AnchorScope s(1);
    callOriginal(c, "fn_8003D634");
}
// The static noise drawn over the GBH screen (gbnoise01..03), from the HUD and from the
// GBH view's own pass (fn_8003E7B0(1)).
HLE(fn_8003BE9C) {
    AnchorScope s(1);
    callOriginal(c, "fn_8003BE9C");
}
// Sets up view n of the frame loop: its viewport/scissor (and for view 1 the depth-clear
// quad, fn_8000B228) are set here, and the view's draws that follow use them. View 1 is the
// GBH screen's live 3D view (unused camera mode). View 0 is the main view; it is smaller
// than the screen while the GBH frame surrounds it (r13+0x200, fn_80010F20), and then
// follows the centered frame. In the unused 2-player mode (coop.cpp) view 1 is player 2's
// half of the split screen, which spans the full width like view 0.
HLE(fn_800115F8) {
    AnchorScope s(c.r[3] == 1 && !coop::active() ? 1 : 0);
    callOriginal(c, "fn_800115F8");
}
// Event portrait renderer (<PDEMO>): 3D drawn into its own viewport. During E. Gadd's call
// (GBH screen mode lbl_804D846C = 3) that viewport is the GBH screen.
HLE(fn_80114E18) {
    AnchorScope s(rd32(VA(0x804D846C)) == 3 ? 1 : 0);
    callOriginal(c, "fn_80114E18");
}
// The event scene's render-to-texture pass, called by fn_80114E18 between the scene and the
// portrait draws: when the scene defines a texture target (door scenes: the reflection of
// Luigi's hand on the brass knob) it sets the camera viewport to (0,0,w,h) (fn_8011548C), clears
// that rectangle with a copy, draws the scene's models and copies (0,0,w,h) into the target
// (fn_801168A8). Inside fn_80114E18's scope that viewport used to follow the centered HUD
// (moved right and narrowed in widescreen) while the copies kept the game's rectangle: the
// knob sampled a cleared texture, and the pass stayed on screen above-left of the knob, its
// depth hiding the door there.
HLE(fn_801142C4) {
    RenderToTextureScope rtt;
    callOriginal(c, "fn_801142C4");
}
// Game Boy Horror map screen (the frame loop's scene for it): the map is drawn into a viewport
// over the GBH's screen (112,84 416x312, fn_8002AC8C), which the bezel overlaps in 4:3. Left
// plain, that viewport is stretched across the wide frame and the map shows beside the
// centered bezel; inside a (centered) scope it is placed like the picture-in-picture views.
HLE(fn_8000C598) {
    AnchorScope s(0);
    callOriginal(c, "fn_8000C598");
}
// Room name when entering a room: the floor (top right) and the room's name (top left).
HLE(fn_80045BDC) {
    AnchorScope s(kAnchorBySide);
    callOriginal(c, "fn_80045BDC");
}
// ---- scenes kept at 4:3 / widened 2D (widescreen.h) ----
namespace {
int sLock43Depth = 0;
int sWipeDepth = 0;
}  // namespace
Lock43Scope::Lock43Scope() { sLock43Depth++; }
Lock43Scope::~Lock43Scope() { sLock43Depth--; }
bool lock43() { return sLock43Depth > 0 && active(); }
WipeScope::WipeScope() { sWipeDepth++; }
WipeScope::~WipeScope() { sWipeDepth--; }
bool inWipe() { return sWipeDepth > 0 && active(); }

// Area-clear screen ("Now you've caught: (Area n) ..."): the game-mode 6 draw of the frame loop
// (fn_8000BE20). Luigi shines the flashlight on the area's portraits on a wall
// (/Nakamura/gallery.bin) that ends just outside the 4:3 view; Hor+ showed its edge. The scene
// is kept at 4:3 with black bars; its 2D text is centered as everywhere else.
HLE(fn_8018721C) {
    Lock43Scope lock;
    callOriginal(c, "fn_8018721C");
}

// J2DFillBox(x, y, w, h, color*): inside the iris wipe, a box touching the 4:3 frame's left or
// right edge (or lying beyond it: the side boxes get a negative width once the circle is wider
// than the screen) is extended to the display's edge. Widths stay as the game computed them
// relative to the edge it does not touch, so the hole is unchanged.
HLE(J2DFillBox__FiiiiQ28JUtility6TColor) {
    if (inWipe()) {
        const s32 ext = s32(std::ceil(320.0f / factor() - 320.0f)) + 2;
        s32 x = s32(c.r[3]), w = s32(c.r[5]);
        const s32 right = x + w;
        if (x <= 0) {
            w = right + ext;
            x = -ext;
        }
        if (right >= 640) w = 640 + ext - x;
        c.r[3] = u32(x);
        c.r[5] = u32(w);
    }
    callOriginal(c, "J2DFillBox__FiiiiQ28JUtility6TColor");
}

// Health meter (bottom left). In the training room it shows over Luigi instead
// (fn_80041214, a projected position): that call opens a centered scope.
HLE(fn_800417F8) {
    AnchorScope s(-1);
    callOriginal(c, "fn_800417F8");
}

}  // namespace recomp::ws
