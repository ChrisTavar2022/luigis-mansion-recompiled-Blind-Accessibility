#pragma once
// The game's main camera/view state (retail: lbl_803A3820, functions in
// auto_03_80008418). Field offsets are the retail ones, for cross-referencing.
#include <dolphin/mtx.h>

namespace lm {

struct Camera {
    /* 0x000 */ Vec eye;
    /* 0x00C */ Vec target;
    /* 0x018 */ Vec up;
    /* 0x024 */ Vec farTarget;     // target + (0, 10, 4000)
    /* 0x030 */ Vec nearTarget;    // target - (0, 0, 200)
    /* 0x03C */ u8 _3C[0x48 - 0x3C];
    /* 0x048 */ Vec defaultUp;     // (0, 1, 0)
    /* 0x054 */ Vec defaultRight;  // (1, 0, 0)
    /* 0x060 */ u8 _60[0x90 - 0x60];
    /* 0x090 */ f32 fovY;
    /* 0x094 */ f32 aspect;
    /* 0x098 */ f32 nearZ;
    /* 0x09C */ f32 farZ;
    /* 0x0A0 */ f32 lightFovY;
    /* 0x0A4 */ f32 lightNear;
    /* 0x0A8 */ f32 lightFar;
    /* 0x0AC */ u8 _AC[0x14C - 0xAC];
    /* 0x14C */ Mtx44 projMtx;
    /* 0x18C */ Mtx viewMtx;
    /* 0x1BC */ Mtx lightProjMtx;
    /* 0x1EC */ s32 viewportX, viewportY, viewportW, viewportH;
    /* 0x1FC */ f32 frustumPlanes[4][4];  // filled by fn_80008D58
};

extern Camera gCamera;  // lbl_803A3820

void initCamera(Camera* cam, const Vec* pos);  // fn_80008418
void applyCamera(Camera* cam);                 // fn_80008654: projection, viewport, scissor
void initDefaultCamera();                      // fn_80009490

}  // namespace lm
