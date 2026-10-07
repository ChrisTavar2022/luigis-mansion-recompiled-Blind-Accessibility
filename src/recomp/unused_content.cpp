// Unused content still present in the retail code, re-enabled as F1 > Enhancements options.
// Both are hooks (tools/recomp/hooks.txt) that run the original function and only steer which
// of the game's own code paths it takes. Findings and addresses: docs/UNUSED_CONTENT.md.
#include "port/settings.h"
#include "recomp/hle.h"
#include "recomp/runtime.h"

#include <dolphin/pad.h>

#include <bit>
#include <cstdlib>

namespace recomp {
u16 padButtons();  // hle_pad.cpp: port 1 buttons as last handed to the game

namespace {

bool envFlag(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && *v != '0';
}

// Game Boy Horror HUD screen mode (lbl_804D846C). Retail only ever sets 4 (normal) and 3
// (E. Gadd's call, fn_8003EA78/fn_8003EA84). Mode 2 is the leftover live camera: the HUD
// update reports the screen rectangle to the renderer (fn_80010EBC), which then renders the
// second view (first-person camera fn_80021DDC) into it; static noise covers the screen
// while it is off-screen.
constexpr u32 kGbhScreenMode = 0x804D846C;
constexpr u32 kGbhModeCamera = 2;
constexpr u32 kGbhModeNormal = 4;

bool sGbhCamOn = envFlag("LM_GBH_CAM");  // test aid: start with the camera on
u16 sPrevButtons = 0;

}  // namespace

// Game Boy Horror HUD update (once per game frame while the HUD is up).
HLE(fn_8003CFC0) {
    static const bool sForce = envFlag("LM_GBH_CAM");
    const bool enabled = sForce || port::settings().enhancements.gbhCamera;
    const u16 buttons = padButtons();
    if (!enabled) {
        sGbhCamOn = false;
    } else if ((buttons & PAD_BUTTON_UP) && !(sPrevButtons & PAD_BUTTON_UP)) {
        sGbhCamOn = !sGbhCamOn;
        log("[unused] Game Boy Horror camera %s", sGbhCamOn ? "on" : "off");
    }
    sPrevButtons = buttons;
    static const bool sShowHud = [] {
        const char* v = std::getenv("LM_GBH_CAM");
        return v && *v == '2';
    }();
    if (sShowHud) {  // test aid (LM_GBH_CAM=2): GBH HUD up before Luigi has the Game Boy Horror
        static const u32 player = guestSymbol("fn_800AD39C");
        const Ctx saved = c;
        const u32 p = guestCall(c, player, 0);
        c = saved;
        if (p >= 0x80000000u && p < 0x81800000u) wr8(p + 0x55, 1);
    }
    const u32 mode = rd32(kGbhScreenMode);
    if (sGbhCamOn && mode == kGbhModeNormal) {
        wr32(kGbhScreenMode, kGbhModeCamera);  // leaves the call screen (mode 3) alone
    } else if (!sGbhCamOn && mode == kGbhModeCamera) {
        wr32(kGbhScreenMode, kGbhModeNormal);
    }
    callOriginal(c, "fn_8003CFC0");
}

// Sub-view clear (only used for view 1, i.e. the GBH screen, from fn_800115F8). It draws a
// black quad with Z texture REPLACE from an all-0xFFFFFF Z24X8 texture to reset depth to the
// far plane. Aurora does not implement GXSetZTexture, so depth would come from the quad
// itself (z = -900, lbl_804D99AC; nothing behind it survives the depth test and the screen
// stays black). The quad is moved onto the far plane for the call, which gives the same
// depth without a Z texture.
HLE(fn_8000B228) {
    constexpr u32 kQuadZ = 0x804D99AC;  // .sdata2 literal read only by fn_8000B014
    const u32 saved = rd32(kQuadZ);
    wr32(kQuadZ, std::bit_cast<u32>(-999.99f));
    callOriginal(c, "fn_8000B228");
    wr32(kQuadZ, saved);
}

// Actor draw pass. r3 selects the pass: (r3 & 0x30) == 0x10 draws normal actors into the
// scene, 0x30 draws ghosts (type flag 8) into the separate ghost layer that is later added
// back at half resolution weighted by the ghost's alpha (fn_8000C2D0, fn_8005F280). Pass 0
// (used by the never-shipped Game Boy Horror view) draws every actor, ghosts included,
// straight into the scene with their ghost lighting and alpha: the early, more solid look.
HLE(fn_80060E70) {
    static const bool sForce = envFlag("LM_EARLY_GHOSTS");
    if (!sForce && !port::settings().enhancements.earlyGhostShading) return callOriginal(c, "fn_80060E70");
    const u32 pass = c.r[3] & 0x30;
    if (pass == 0x10) {
        c.r[3] &= ~0x30u;  // normal actors + ghosts, drawn directly
    } else if (pass == 0x30) {
        c.r[3] = (c.r[3] & ~0x30u) | 0x20;  // no case for 0x20: the ghost layer stays empty
    }
    callOriginal(c, "fn_80060E70");
}

}  // namespace recomp
