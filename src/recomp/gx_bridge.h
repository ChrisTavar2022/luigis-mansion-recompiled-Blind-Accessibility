#pragma once
namespace recomp::gx {
// Closes a draw opened by GXBegin whose vertices were written through the FIFO.
void endPendingDraw();
// The guest finished a frame (VISetNextFrameBuffer): closes any open draw.
void endFrame();
}  // namespace recomp::gx
