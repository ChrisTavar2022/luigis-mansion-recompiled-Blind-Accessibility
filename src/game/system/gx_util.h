#pragma once
// Shared math/GX helpers (retail: auto_03_80009540 and friends).
#include <dolphin/gx.h>
#include <dolphin/mtx.h>

namespace lm {

extern Mtx gIdentityMtx;  // sDefaultMtx

// 4096-step sine table covering 1.25 turns (lbl_80218238/lbl_80219238); the
// cosine table is the same data offset by a quarter turn. The values are not
// reproducible with a formula, so they are loaded from the retail DOL.
void loadTrigTables();
f32 sinIdx(u16 angle);  // angle: s16 binary angle (0x10000 = full turn)
f32 cosIdx(u16 angle);

// fn_8000A40C: dst = T(tx,ty,tz) * R(rx,ry,rz) * S(sx,sy,sz) with binary angles.
void makeSRTMtx(f32 sx, f32 sy, f32 sz, f32 tx, f32 ty, f32 tz, Mtx dst, s32 rx, s32 ry, u32 rz);

// fn_80009B80: resets GX to the game's default 3D drawing state.
void resetGXState();

}  // namespace lm
