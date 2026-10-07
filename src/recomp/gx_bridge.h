#pragma once
namespace recomp::gx {
// Closes a draw opened by GXBegin whose vertices were written through the FIFO.
void endPendingDraw();
}  // namespace recomp::gx
