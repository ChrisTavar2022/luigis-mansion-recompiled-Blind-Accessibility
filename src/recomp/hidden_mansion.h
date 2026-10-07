#pragma once
// PAL Hidden Mansion rules on the US (GLME01) recompiled build -- prototype.
// Background, addresses and the full list of PAL behaviour changes: docs/HIDDEN_MANSION.md.
//
// Nothing here is wired into the shared runtime yet. The integration points are:
//   hle_dvd.cpp    DVDConvertPathToEntrynum / DVDOpen / DVDFastOpen / DVDRead*: consult
//                  overlayEntry()/isOverlayEntry()/overlayRead() before Aurora's disc.
//   frame.cpp      call syncConstants() once per retrace.
//   hle_gx.cpp     GXSetProjection / GXSetCullMode: mirrorProjection() / mirrorCullMode().
//   audio output   mirrorStereo() on each mixed buffer (once audio is emulated).
#include "recomp/ctx.h"

#include <cstddef>
#include <optional>
#include <string>

namespace recomp::hm {

enum class Mode {
    Us,   // retail US behaviour (default)
    Pal,  // PAL (GLMP01) Hidden Mansion: map2u data, PAL scripts, mirror, PAL tuning
};

// LM_HIDDEN_MANSION=pal selects the PAL rules; LM_HM_OVERLAY points at the overlay built by
// tools/hidden_mansion_data.py (default: orig/hidden_mansion).
Mode mode();

// The game's own hidden-mansion ("ura") flag. US: sbss 0x804D80C0 (r13+0x160), set by the
// URALUIGI event command, cleared by OMOTELUIGI and on new game. PAL keeps the same variable
// (0x804C5784) and only adds a deferred "set at next scene" request (0x804C5780).
bool hiddenActive();
bool palActive();  // mode() == Pal && hiddenActive()

// ---- Disc overlay ---------------------------------------------------------------------
// Host file that should be served instead of a guest disc path, if any. Map archives are
// redirected only while palActive(); patched event archives whenever mode() == Pal (their
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
