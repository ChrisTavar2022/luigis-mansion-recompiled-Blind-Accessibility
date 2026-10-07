// Port of the room lighting functions in auto_03_8001461C.
#include "game/gfx/room_light.h"

#include <algorithm>

namespace lm::gfx {
namespace {

constexpr GXLightID kLightIds[8] = {GX_LIGHT0, GX_LIGHT1, GX_LIGHT2, GX_LIGHT3,
                                    GX_LIGHT4, GX_LIGHT5, GX_LIGHT6, GX_LIGHT7};  // lbl_802FF3B8
constexpr f32 kRefScale = 0.01f;  // lbl_804D9B10: brightness percent -> fraction

RoomLight sLights[kMaxRoomLights];  // lbl_803A4398
int sLightCount;                    // lbl_804D81B8
bool sUseRoomLights;                // lbl_804D07C8
u32 sLightMask = GX_LIGHT0;         // lbl_804D0044
GXAttnFn sAttn = GX_AF_SPOT;        // lbl_804D0040

// fn_80015F6C: a plain point light at a world position.
void loadPointLight(int slot, const Vec& worldPos, GXColor color, const Mtx view) {
    Vec viewPos;
    MTXMultVec(const_cast<f32(*)[4]>(view), &worldPos, &viewPos);
    GXLightObj obj;
    GXInitLightPos(&obj, viewPos.x, viewPos.y, viewPos.z);
    GXInitLightColor(&obj, color);
    GXInitLightDir(&obj, 0.0f, 0.0f, 1.0f);
    GXInitLightSpot(&obj, 90.0f, GX_SP_OFF);
    GXInitLightDistAttn(&obj, 100.0f, 1.0f, GX_DA_OFF);
    GXLoadLightObjImm(&obj, kLightIds[slot]);
}

// fn_800171BC: one entry of the room's light list.
void loadRoomLight(const RoomLight& light, int slot, const Mtx view) {
    Vec viewPos;
    MTXMultVec(const_cast<f32(*)[4]>(view), &light.position, &viewPos);
    GXLightObj obj;
    GXInitLightPos(&obj, viewPos.x, viewPos.y, viewPos.z);
    GXInitLightColor(&obj, light.color);
    if (light.spotAngle == 0) {
        GXInitLightDir(&obj, 0.0f, 0.0f, -1.0f);
        GXInitLightSpot(&obj, 0.0f, GX_SP_COS2);
        GXInitLightDistAttn(&obj, f32(light.refDistance), kRefScale * f32(light.refBrightness), GX_DA_STEEP);
    } else {
        Vec viewDir;
        MTXMultVecSR(const_cast<f32(*)[4]>(view), &light.direction, &viewDir);
        GXInitLightDir(&obj, viewDir.x, viewDir.y, viewDir.z);
        GXInitLightSpot(&obj, f32(light.spotAngle), GX_SP_COS2);
        GXInitLightDistAttn(&obj, f32(light.refDistance), kRefScale * f32(light.refBrightness), GX_DA_GENTLE);
    }
    GXLoadLightObjImm(&obj, kLightIds[slot]);
}

}  // namespace

void setRoomLights(const RoomLight* lights, int count) {
    sLightCount = std::clamp(count, 0, kMaxRoomLights);
    std::copy(lights, lights + sLightCount, sLights);
}

void setUseRoomLights(bool use) { sUseRoomLights = use; }

u32 beginRoomLighting(const Mtx view, GXColor ambient) {
    if (!sUseRoomLights) {
        // A single white light far above the mansion.
        loadPointLight(0, Vec{0.0f, 10000.0f, 10000.0f}, GXColor{0xFF, 0xFF, 0xFF, 0xFF}, view);
        sLightMask = GX_LIGHT0;
        sAttn = GX_AF_SPOT;
    } else {
        // fn_800170AC: all room lights; the mask is the OR of their slots.
        sLightMask = 0;
        for (int i = 0; i < sLightCount; i++) {
            loadRoomLight(sLights[i], i, view);
            sLightMask |= kLightIds[i];
        }
        sAttn = GX_AF_SPEC;
    }
    GXSetChanAmbColor(GX_COLOR0A0, ambient);
    GXSetAlphaCompare(GX_GREATER, 0, GX_AOP_OR, GX_GREATER, 0);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_SET);
    GXSetDstAlpha(GX_FALSE, 0xFF);
    return sLightMask;
}

void setSpecularLight(const Vec& pos, const Vec& dir, GXColor color, const Mtx, f32 shininess) {
    GXLightObj obj;
    GXInitSpecularDir(&obj, dir.x, dir.y, dir.z);
    GXInitLightPos(&obj, pos.x, pos.y, pos.z);
    GXInitLightColor(&obj, color);
    const f32 k = shininess * 0.5f;
    GXInitLightAttn(&obj, 0.0f, 0.0f, 1.0f, k, 0.0f, 1.0f - k);
    GXLoadLightObjImm(&obj, kLightIds[6]);
}

bool setCameraLight(const Vec& eye, const Vec& target, const Mtx view, const Flashlight* flashlight) {
    if (flashlight != nullptr && flashlight->on) {
        // fn_80016040: the flashlight cone.
        Vec viewPos, viewDir;
        MTXMultVec(const_cast<f32(*)[4]>(view), &flashlight->position, &viewPos);
        MTXMultVecSR(const_cast<f32(*)[4]>(view), &flashlight->direction, &viewDir);
        GXLightObj obj;
        GXInitLightPos(&obj, viewPos.x, viewPos.y, viewPos.z);
        GXInitLightColor(&obj, flashlight->color);
        GXInitLightDir(&obj, viewDir.x, viewDir.y, viewDir.z);
        GXInitLightSpot(&obj, f32(flashlight->spotAngle), GX_SP_COS2);
        GXInitLightDistAttn(&obj, f32(flashlight->refDistance), kRefScale * f32(flashlight->refBrightness),
                            GX_DA_GENTLE);
        GXLoadLightObjImm(&obj, kLightIds[7]);
        return true;
    }
    // A light behind the camera, on the line from the target through the eye.
    const Vec pos = {-1.0f * (eye.x - target.x) + target.x, -1.0f * (eye.y - target.y) + target.y,
                     -1.0f * (eye.z - target.z) + target.z};
    loadPointLight(7, pos, GXColor{0, 0, 0, 0}, view);
    return false;
}

u32 roomLightMask() { return sLightMask; }
GXAttnFn roomLightAttn() { return sAttn; }

}  // namespace lm::gfx
