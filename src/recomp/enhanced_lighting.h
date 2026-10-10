#pragma once
// Enhanced lighting, port side (docs/RENDERING.md "Enhanced lighting"; Aurora patch 0014,
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
// LM_ENH_PERF_AB phase: -1 off, 0 part on, 1 part off.
int perfPhase();
// F1 settings -> Aurora (host thread, every frame).
void applySettings();
// Ghost actor pass (fn_80060E70 pass 0x30): draws in it are class GHOST.
void setGhostPass(bool on);
bool ghostPass();
// Main view bracket (fn_8000BB98 hook): sends the frame's extra lights / the scene-end marker.
void beginScene();
void endScene();
// World (game) space -> view space with the current camera (lbl_803A3820 view eye/target).
// False if there is no camera yet.
bool worldToView(const float world[3], float view[3]);
// Enhanced "Shadow map size": width of the offscreen shadow map (height 3/4 of it), 0 = the
// EFB (render resolution).
uint32_t shadowMapWidth();
}  // namespace enh

}  // namespace recomp
