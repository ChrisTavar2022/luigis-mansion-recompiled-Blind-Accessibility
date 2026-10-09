// Shadow-map enhancements (docs/RENDERING.md, "Shadows").
//
// The game's real-time shadows (flashlight, lightning, Luigi's self-shadow) come from one
// shadow map: fn_8005D984 turns the main camera (lbl_803A3820) into the light's camera
// (perspective from the camera's light fovY +0xA0 and aspect +0x94, or orthographic), the
// casters are drawn into the EFB, fn_8005D8CC copies the depth as Z16 640x480, and every
// receiver projects it with the light matrices (+0xB0 / +0xE0 of the shadow object) and
// compares depths in two TEV stages (fn_8005DD50).
//
// Widescreen: the light's frustum only covers what the 4:3 picture shows, so objects in the
// extra side areas cast no shadows. With "Widescreen shadows" the light camera's aspect is
// multiplied by the widescreen ratio for the setup call, which builds the light projection,
// the receivers' texture projection (MTXLightPerspective) and the light's culling planes from
// the same value, so the map stays consistent; only its horizontal resolution is spread wider.
// The orthographic light (unused by the tested scenes) has fixed extents and is left alone.
//
// Resolution: "Shadow map at render resolution" lets the receivers sample the Z16 copy at the
// internal resolution (Aurora's copy texture) instead of the console's 640x480 bytes.
#include "port/settings.h"
#include "recomp/hle.h"
#include "recomp/runtime.h"
#include "recomp/version.h"
#include "recomp/widescreen.h"

#include <aurora/efb_ram.h>

namespace recomp {
namespace {
constexpr u32 kCamAspect = 0x94;  // f32, the camera's aspect ratio (MTXPerspective)
}  // namespace

// Shadow pass setup (shadow object): runs only while a shadow pass is requested (+0xA0).
HLE(fn_8005D984) {
    const auto& r = port::settings().rendering;
    aurora_efb_ram_set_scaled_depth_reads(r.shadowRenderResolution);
    ws::setShadowLightSetup(true);
    if (!r.shadowWidescreen || !ws::active()) {
        callOriginal(c, "fn_8005D984");
        ws::setShadowLightSetup(false);
        return;
    }
    const u32 cam = VA(0x803A3820);
    const float aspect = rdf32(cam + kCamAspect);
    wrf32(cam + kCamAspect, aspect / ws::factor());
    callOriginal(c, "fn_8005D984");
    wrf32(cam + kCamAspect, aspect);
    ws::setShadowLightSetup(false);
}

}  // namespace recomp
