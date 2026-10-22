#pragma once
// F1 > Enhancements > Silly, Luigi options (silly.cpp; docs/ENHANCEMENTS.md "Silly"): head size,
// body size, wobbly head, paper Luigi, clothes and Poltergust colours. Visual only, off by default.
#include "recomp/ctx.h"

namespace recomp::silly {
// Host thread (F1 menu, Enhancements > Silly): the Luigi widgets.
void drawLuigiOptions();
// Guest thread, GXInitTexObj: a recoloured host copy of the texture at `image` when it is one of
// Luigi's clothing or Poltergust textures and a colour option is on; nullptr otherwise.
const void* texOverride(u32 image, u32 width, u32 height, u32 gxFormat);

// Shared joint hook (guest thread). silly.cpp owns the one handler of fn_80058850, which builds an
// S3D model's skinning matrices from its joint matrices; other modules change joints through it.
// A scaler is called for every S3D model (S3DModel `s3d`, its relocated .mdl `mdl`) right before
// the skinning matrices are built, after Luigi's own options, with `joints` = guest address of the
// joint world matrices (numJoints x 0x30 bytes, 3x4 row-major big-endian floats, read/write with
// gu::rdf/gu::wrf). Changing a joint matrix moves everything skinned or attached to it, in every
// pass (shadows included), and frame interpolation blends the result. Not called while the model
// viewer is open, nor when the joints were not recomputed since the last call (no double apply).
// Register once, e.g. from a static initializer.
using JointScaler = void (*)(Ctx& c, u32 s3d, u32 mdl, u32 joints, u32 numJoints);
void addJointScaler(JointScaler fn);
}  // namespace recomp::silly
