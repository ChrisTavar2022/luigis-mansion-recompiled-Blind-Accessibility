#pragma once
namespace recomp::gx {
// Closes a draw opened by GXBegin whose vertices were written through the FIFO.
void endPendingDraw();
// The guest finished a frame (VISetNextFrameBuffer): closes any open draw.
void endFrame();
// Fast-forward (skip.cpp): the current guest frame is not shown, so its draws and
// copies are dropped (state changes still reach Aurora; display lists are still recorded).
extern bool g_discardDraws;
}  // namespace recomp::gx
