// Unused content still present in the retail code, re-enabled as F1 > Enhancements options.
// Both are hooks (tools/recomp/hooks.txt) that run the original function and only steer which
// of the game's own code paths it takes. Findings and addresses: docs/UNUSED_CONTENT.md.
#include "port/settings.h"
#include "recomp/hle.h"
#include "recomp/runtime.h"
#include "recomp/version.h"

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
#define kGbhScreenMode VA(0x804D846C)
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
// black quad (fn_8000B014) with Z texture REPLACE from an all-0xFFFFFF Z24X8 texture to reset
// depth to the far plane. Aurora does not implement GXSetZTexture, so depth would come from
// the quad itself (z = -900, lbl_804D99AC; nothing behind it survives the depth test and the
// screen stays black). The quad is moved onto the far plane for the call, which gives the same
// depth without a Z texture. The European LMDefaultOrthoView has its far plane at 4096
// instead of 1000, so the far-plane depth there is -4096.
// The European clear is fn_8000B7CC (it also builds the Z texture object itself); the function
// the matcher pairs with the USA one (PAL 0x8000B928) is a different, full-screen clear, so the
// European hook is listed by address (palfn_8000B7CC in hooks.txt).
namespace {
void subViewClear(Ctx& c, const char* name) {
    const u32 quadZ = VA(0x804D99AC);  // .sdata2 literal read only by fn_8000B014
    const u32 saved = rd32(quadZ);
    // Just inside the far plane of LMDefaultOrthoView (USA far 1000, European far 4096).
    wr32(quadZ, std::bit_cast<u32>(ver::pal() ? -4095.9f : -999.99f));
    callOriginal(c, name);
    wr32(quadZ, saved);
}
}  // namespace
HLE(fn_8000B228) { subViewClear(c, "fn_8000B228"); }
HLE(palfn_8000B7CC) { subViewClear(c, "palfn_8000B7CC"); }

// Actor draw pass. r3 selects the pass: (r3 & 0x30) == 0x10 draws normal actors into the
// scene, 0x30 draws ghosts (type flag 8) into the separate ghost layer that is later added
// back at half resolution weighted by the ghost's alpha (fn_8000C2D0, fn_8005F280). Pass 0
// (used by the never-shipped Game Boy Horror view) draws every actor, ghosts included,
// straight into the scene with their ghost lighting and alpha: the early, more solid look.
HLE(fn_80060E70) {
    static const bool sForce = envFlag("LM_EARLY_GHOSTS");
    if (!sForce) return  // dev-only experiment (LM_EARLY_GHOSTS=1); the menu option is the beta lighting below
        callOriginal(c, "fn_80060E70");
    const u32 pass = c.r[3] & 0x30;
    if (pass == 0x10) {
        c.r[3] &= ~0x30u;  // normal actors + ghosts, drawn directly
    } else if (pass == 0x30) {
        c.r[3] = (c.r[3] & ~0x30u) | 0x20;  // no case for 0x20: the ghost layer stays empty
    }
    callOriginal(c, "fn_80060E70");
}

// Early ghost lighting: the known cheat for the pre-release ghost look patches the channel
// setup call at 0x80058290 in fn_800581E8 (the ghost lighting function): light mask 0x80
// (light 7) -> 0x40 (light 6) and attenuation function 0 -> 2. fn_8005A5D4 takes the
// GXSetChanCtrl-style arguments (r7 = light mask, r9 = attenuation), so adjust them only
// for that call site (the European fn_800581E8 is identical; VA() maps the call site).
HLE(fn_8005A5D4) {
    if (c.lr == VA(0x80058290) + 4 && port::settings().enhancements.earlyGhostLighting) {
        c.r[7] = 0x40;
        c.r[9] = 2;
    }
    callOriginal(c, "fn_8005A5D4");
}

}  // namespace recomp
