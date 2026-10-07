#pragma once
// PAL Hidden Mansion rules on the US (GLME01) recompiled build.
// Background, addresses and the full list of PAL behaviour changes: docs/HIDDEN_MANSION.md.
//
// Wiring:
//   hle_dvd.cpp            serves overlay files (overlayEntry/isOverlayEntry/overlayRead).
//   frame.cpp              onFrame() once per presented frame.
//   hidden_mansion_hooks   tuning hooks (tools/recomp/hooks.txt).
//   audio output           mirrorStereo() on each mixed buffer (once audio is emulated).
#include "recomp/ctx.h"

#include <cstddef>
#include <optional>
#include <string>

namespace recomp::hm {

// Which release's Hidden Mansion rules apply. This is separate from the game's own
// hidden-mansion flag: the PAL rules are active only while the flag is set AND the variant is
// Pal, and switching back to Us fully restores US behaviour (overlay off, literals restored,
// hooks pass straight through to the original). The intended UI is a second Hidden Mansion
// entry in E. Gadd's lab menu (docs/HIDDEN_MANSION.md); until then the variant comes from
// --hidden-mansion-variant pal|us (alias --hidden-mansion; env LM_HIDDEN_MANSION_VARIANT),
// default pal.
enum class Variant { Us = 0, Pal = 1 };
Variant variant();
void setVariant(Variant v);

// LM_HM_OVERLAY points at the overlay built by tools/hidden_mansion_data.py (default:
// orig/hidden_mansion, searched upwards from the working directory). LM_HM_FORCE=1 keeps the
// game's hidden-mansion flag set, for testing.

// Per presented frame: LM_HM_FORCE, flag-change logging, constant patches.
void onFrame();

// The game's own hidden-mansion ("ura") flag. US: sbss 0x804D80C0 (r13+0x160), set by the
// URALUIGI event command, cleared by OMOTELUIGI and on new game. PAL keeps the same variable
// (0x804C5784) and only adds a deferred "set at next scene" request (0x804C5780).
bool hiddenActive();
bool palActive();  // variant() == Pal && hiddenActive()

// ---- Disc overlay ---------------------------------------------------------------------
// Host file that should be served instead of a guest disc path, if any. Map archives are
// redirected only while palActive(); patched event archives whenever variant() == Pal (their
// PAL changes are inside CHECKFLAG(8) "ura" branches, so the normal mansion is unaffected).
std::optional<std::string> redirect(const char* guestPath);

// Virtual FST entry numbers for redirected files, so entry-number based opens keep working.
s32 overlayEntry(const char* guestPath);  // -1 when the path is not redirected
bool isOverlayEntry(s32 entry);
u32 overlayLength(s32 entry);
s32 overlayRead(s32 entry, void* dst, s32 length, s32 offset);

// ---- Runtime constant patches ---------------------------------------------------------
// Some PAL changes multiply a literal that, in the US build, lives in .sdata2 and is used by
// exactly one function. Those are applied by rewriting the literal in guest memory while
// palActive() and restoring it otherwise. Cheap; call once per frame.
void syncConstants();

// ---- Mirror (PAL draws the hidden mansion left/right mirrored) -----------------------------
bool mirrorActive();
// Negate the X row of a perspective projection (orthographic 2D/HUD is left alone).
void mirrorProjection(float m[4][4], bool perspective);
// Mirroring flips triangle winding: swap GX_CULL_FRONT and GX_CULL_BACK.
u32 mirrorCullMode(u32 gxCullMode);
// Screen-space X of something the game projected itself (PAL: x = 640 - x).
float mirrorScreenX(float x, float width = 640.0f);
// PAL swaps the left/right mix buffers in Jac_VframeWork; here: swap interleaved L/R.
void mirrorStereo(s16* interleavedStereo, std::size_t frames);

}  // namespace recomp::hm
