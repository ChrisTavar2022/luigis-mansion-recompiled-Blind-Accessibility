#pragma once
#include <cstdint>

namespace recomp::gx {
// Display copy settings the game made (GXSetCopyFilter, GXSetDispCopyGamma, GXSetDispCopySrc,
// GXSetCopyClamp), applied on presentation by the display copy filter pass (crt.cpp).
struct DisplayCopy {
    uint8_t vfilter[7] = {0, 0, 21, 22, 21, 0, 0};  // vertical filter taps (sum 64)
    uint8_t gamma = 0;                              // GXGamma: 0 = 1.0, 1 = 1.7, 2 = 2.2
    uint8_t clamp = 3;                              // GXFBClamp bits (top 1, bottom 2)
    uint16_t efbHeight = 480;                       // display copy source height (EFB lines)
};
const DisplayCopy& displayCopy();
// Pushes the F1 > Visuals > Rendering accuracy options to Aurora (host thread, every frame).
void applyAccuracySettings();

// Closes a draw opened by GXBegin whose vertices were written through the FIFO.
void endPendingDraw();
// Model viewer: draw with textures replaced by white (TEV colour inputs / modes).
void setUntextured(bool on);
// The guest finished a frame (VISetNextFrameBuffer): closes any open draw.
void endFrame();
// Fast-forward (skip.cpp): the current guest frame is not shown, so its draws and
// copies are dropped (state changes still reach Aurora; display lists are still recorded).
extern bool g_discardDraws;
}  // namespace recomp::gx
