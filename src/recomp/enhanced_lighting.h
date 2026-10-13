#pragma once
// Enhanced lighting, port side (docs/RENDERING.md "Enhanced lighting"; Aurora patch 0016,
// include/aurora/enhanced.h and include/aurora/lights.h).
//
// - Settings (F1 > Visuals > Enhanced lighting) are pushed to Aurora every host frame.
// - Draw classes: hle_gx.cpp marks world-geometry draws (perspective, full viewport, outside the
//   shadow-map pass), ghosts (the actor ghost pass) and everything else, only while enabled.
// - The main 3D view (fn_8000BB98) is bracketed: extra lights are sent before it, the
//   scene-end marker (bloom, volumetric beam, ambient occlusion) after it, before the HUD.
// - Extra lights: a provider (e.g. ghosts) calls submitExtraLights once per game frame.
#include <aurora/lights.h>

#include <cstdint>

namespace recomp {

// Extra dynamic lights for the current game frame (guest thread). Positions are in VIEW space
// (GX eye space: the space of GXLoadLightObjImm positions; see enh::worldToView). The set is
// sent with the next main-view draw and replaces the previous one; a frame in which nobody
// submits has no extra lights. Up to aurora::gfx::MaxExtraLights (strongest first). Lights
// are blended between game frames by `id` when frame interpolation is on.
void submitExtraLights(const aurora::gfx::ExtraLight* lights, uint32_t count);

namespace enh {
// Master switch as last applied (host and guest thread; cheap).
bool active();
// Master switch only (active() is also false on the plain frames of LM_ENHANCED_AB).
bool masterOn();
// Master switch as configured (settings or LM_ENHANCED), before applySettings runs: the
// accuracy options that enhanced lighting turns off (hle_gx.cpp) are decided from it.
bool configuredOn();
// LM_ENH_PERF_AB phase: -1 off, 0 part on, 1 part off.
int perfPhase();
// F1 settings -> Aurora (host thread, every frame).
void applySettings();
// Ghost actor pass (fn_80060E70 pass 0x30): draws in it are class GHOST.
void setGhostPass(bool on);
bool ghostPass();
// Actor draw pass (fn_80060E70 other than the ghost layer): Luigi, NPCs and other characters get
// the matte character material (GX_AURORA_SET_MATERIAL_HINT): the game's own vertex lighting, no
// glossy highlights, soft wrap-around response to the colored lights.
void setCharacterPass(bool on);
bool characterPass();
// Shadow-map caster pass (shadows.cpp: fn_8005D984 .. fn_8005D8CC): draws in it are class OTHER.
void setCasterPass(bool on);
// Main view bracket (fn_8000BB98 hook): sends the frame's extra lights / the scene-end marker.
void beginScene();
void endScene();
// World (game) space -> view space with the current camera (lbl_803A3820 view eye/target).
// False if there is no camera yet.
bool worldToView(const float world[3], float view[3]);
// Enhanced "Shadow map size": width of the offscreen shadow map (height 3/4 of it), 0 = the
// EFB (render resolution).
uint32_t shadowMapWidth();
// Colored-light shadows: how many lights may cast them this frame (settings; screen-space at
// most 4; 0 when off). Providers flag that many of their strongest lights.
uint32_t shadowLightCap();
}  // namespace enh

}  // namespace recomp
