#pragma once
// High-frame-rate presentation by frame interpolation (docs/INTERPOLATION.md).
//
// The game keeps running at its own rate (30 Hz, paced by the emulated VI). While
// interpolation is on, the guest no longer draws into the host's Aurora frame:
// each guest frame's GX commands are captured into a "tape" (Aurora command
// capture, patch 0005), together with the stream offsets of every value that moves
// (matrices, projections, lights, CPU-built vertex positions) and copies of the
// vertex arrays its draws read. The host presents at display rate and replays the
// newest tape with those values blended toward the previous tape's, one game frame
// behind real time.
#include "recomp/ctx.h"

#include <string>
#include <vector>

namespace recomp::interp {

enum class Kind : u32 { PosMtx, NrmMtx, TexMtx, Projection, Light, Verts, Count };

// --- guest thread ---------------------------------------------------------------
bool capturing();
void beginTape();                 // starts capturing the next guest frame
void endTape(u32 retraceCount);   // closes it and hands it to the host (LM_INTERP_TEST: and waits)

// Bracket an Aurora call that writes one XF block of `nFloats` floats (matrix,
// projection or light load): mark() before, note() after. `source` is the guest
// address of the matrix/light object (or 0); `slot` is the XF slot or light index.
// `first` is the index of the first blended float in the block.
u32 mark();
void note(u32 at, Kind kind, u32 source, u32 slot, int nFloats, int first = 0);

// LM_FRAME_DUMP's EFB copy (hle_gx GXCopyDisp), marked at `at`: LM_INTERP_TEST
// redirects it per replay.
void noteDump(u32 at, const void* target, u32 frameNo);

// Draws (interp_vtx.cpp). Values noted before a draw are paired between frames by
// what that draw is (display list, vertex layout), not by where they came from.
// Vertex arrays: a tape is replayed up to a game frame after it was captured, while
// the guest is already rewriting dynamic vertex data, so the array ranges each
// tape's draws index are copied into the tape at capture time.
void arrayBase(u32 at, u32 attr, u32 guestAddr, u32 stride);  // GXSetArray, marked at `at`
void invalidateVtxCache();                                    // GXInvalidateVtxCache
void vtxBegin(u32 prim, u32 fmt, u32 nVerts, bool perspective);  // GXBegin (before Aurora's header)
void vtxBytes(u32 v, int size);                               // raw FIFO write from game code
void vtxDisplayList(u32 guestAddr, const u8* data, u32 size);  // after GXCallDisplayList

// The game recording its own display list suspends the capture around it.
void suspendForDisplayList();
void resumeAfterDisplayList();

// --- host thread -----------------------------------------------------------------
// Replays the newest tape into the open Aurora frame. Returns false if there is
// no tape yet. `final` replays the newest tape exactly (no blending).
bool replay(bool final = false);
void reset();  // drop all tapes (after leaving interpolated mode)

namespace detail {  // interp.cpp <-> interp_vtx.cpp
void vtxBeginTape();
// Copies are finished: points the tape's array loads at them (moved into `keep`).
void vtxEndTape(u8* tape, u32 size, std::vector<std::vector<u8>>& keep, u64& snapshotBytes);
// A draw with identity `id`: values noted since the previous draw belong to it.
void noteDraw(u64 id);
// Immediate-mode vertices with direct f32 XYZ positions: `count` vertices of `stride`
// bytes starting at tape offset `at`, position at `posOffset` in each.
void noteVertices(u32 at, u32 count, u32 stride, u32 posOffset, u64 id);
u64 mix(u64 h);
void drawLog(const char* fmt, ...);  // LM_INTERP_DRAWS
}  // namespace detail

// LM_INTERP_DRAWS: whether this draw is being logged, and its guest call chain.
bool drawLogActive();
void drawLogCaller(const std::string& chain);

}  // namespace recomp::interp
