#pragma once
// High-frame-rate presentation by frame interpolation (docs/ENHANCEMENTS.md, section 3).
//
// The game keeps running at its own rate (30 Hz, paced by the emulated VI). While
// interpolation is on, the guest no longer draws into the host's Aurora frame:
// each guest frame's GX commands are captured into a "tape" (Aurora command
// capture, patch 0005), together with the stream offsets of every matrix and
// projection load. The host presents at display rate and replays the newest
// tape with those matrices blended toward the previous tape's values, one game
// frame behind real time.
#include "recomp/ctx.h"

namespace recomp::interp {

enum class Kind : u32 { PosMtx, NrmMtx, TexMtx, Projection };

// --- guest thread ---------------------------------------------------------------
bool capturing();
void beginTape();                 // starts capturing the next guest frame
void endTape(u32 retraceCount);   // closes it and hands it to the host

// Bracket an Aurora call that writes one XF block of `nFloats` floats (matrix or
// projection load): mark() before, note() after. `source` identifies the matrix
// across frames (guest address of the matrix, or 0); `slot` is the XF slot.
u32 mark();
void note(u32 at, Kind kind, u32 source, u32 slot, int nFloats);

// The game recording its own display list suspends the capture around it.
void suspendForDisplayList();
void resumeAfterDisplayList();

// --- host thread -----------------------------------------------------------------
// Replays the newest tape into the open Aurora frame. Returns false if there is
// no tape yet. `final` replays the newest tape exactly (no blending).
bool replay(bool final = false);
void reset();  // drop all tapes (after leaving interpolated mode)

}  // namespace recomp::interp
