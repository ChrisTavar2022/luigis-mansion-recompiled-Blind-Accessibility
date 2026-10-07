#include "game/system/camera.h"
#include "game/system/display.h"
#include "game/system/gx_util.h"
#include "game/unported.h"

#include <dolphin/gx.h>

#include <cmath>

namespace lm {

Camera gCamera;

void initCamera(Camera* cam, const Vec* pos) {
    cam->defaultUp = {0.0f, 1.0f, 0.0f};
    cam->defaultRight = {1.0f, 0.0f, 0.0f};
    cam->target = *pos;
    cam->eye = *pos;
    cam->eye.z += 1000.0f;
    cam->up = cam->defaultUp;
    cam->nearTarget = cam->target;
    cam->nearTarget.z -= 200.0f;
    cam->farTarget = cam->target;
    cam->farTarget.y += 10.0f;
    cam->farTarget.z += 4000.0f;
    cam->aspect = 4.0f / 3.0f;
    cam->fovY = 26.0f;
    cam->nearZ = 16.0f;
    cam->farZ = 8192.0f;
    cam->lightFovY = 26.0f;
    cam->lightNear = 16.0f;
    cam->lightFar = 8192.0f;
    cam->viewportX = 0;
    cam->viewportY = 0;
    cam->viewportW = 640;
    cam->viewportH = 480;
}

// fn_80008D58: computes the four side planes of the view frustum for culling.
static void computeFrustumPlanes(Camera* cam, f32 top, f32 bottom, f32 left, f32 right, f32 nearZ, f32 farZ) {
    (void)cam, (void)top, (void)bottom, (void)left, (void)right, (void)nearZ, (void)farZ;
    // TODO(port): fn_80008D58 (culling only; nothing drawn by the boot path depends on it).
}

void applyCamera(Camera* cam) {
    MTXPerspective(cam->projMtx, cam->fovY, cam->aspect, cam->nearZ, cam->farZ);
    GXSetProjection(cam->projMtx, GX_PERSPECTIVE);
    setViewportJitter(f32(cam->viewportX), f32(cam->viewportY), f32(cam->viewportW), f32(cam->viewportH), 0.0f,
                      1.0f);
    GXSetScissor(cam->viewportX, cam->viewportY, cam->viewportW, cam->viewportH);
    MTXLightPerspective(cam->lightProjMtx, cam->lightFovY, cam->aspect, 0.5f, 0.5f, 0.5f, 0.5f);

    // Half-FOV degrees -> binary angle (65536/360 = 182.04...), rounded exactly as retail does.
    const f64 halfFov = 0.5 + 182.04444444444445 * f64(cam->fovY * 0.5f);
    const s32 angle = halfFov >= 0.0 ? s32(halfFov + 0.5) : s32(halfFov - 0.5);
    const f32 tanHalf = sinIdx(u16(angle)) / cosIdx(u16(angle));
    const f32 top = cam->nearZ * tanHalf;
    computeFrustumPlanes(cam, top, -top, -top * cam->aspect, top * cam->aspect, cam->nearZ, cam->farZ);
}

void initDefaultCamera() {
    const Vec origin = {0.0f, 0.0f, 0.0f};  // lbl_8021DA40
    initCamera(&gCamera, &origin);
    unported::fn_801886D8_setAudioListener(&gCamera, &gCamera.viewMtx);
}

}  // namespace lm
