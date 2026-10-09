#pragma once
namespace recomp::gx {
// Closes a draw opened by GXBegin whose vertices were written through the FIFO.
void endPendingDraw();
// Model viewer: draw with textures replaced by white (TEV colour inputs / modes).
void setUntextured(bool on);
// The guest finished a frame (VISetNextFrameBuffer): closes any open draw.
void endFrame();
// Fast-forward (skip.cpp): the current guest frame is not shown, so its draws and
// copies are dropped (state changes still reach Aurora; display lists are still recorded).
extern bool g_discardDraws;
// Current vertex layout (hle_gx shadows): bytes attribute `attr` takes in a vertex of
// format `fmt` (-1 unknown), and its GXAttrType.
int vtxAttrSize(int fmt, int attr);
unsigned char vtxDesc(int attr);
}  // namespace recomp::gx
