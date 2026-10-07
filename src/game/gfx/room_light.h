#pragma once
// Lighting for rooms and furniture (retail: auto_03_8001461C, fn_80015BDC ..
// fn_800171BC). GX light slots:
//   0-5  room lights (point or spot) from the current room's light list
//   6    specular highlight light
//   7    the camera light, or Luigi's flashlight cone when it is on
#include <dolphin/gx.h>
#include <dolphin/mtx.h>

namespace lm::gfx {

// One room light (retail lbl_803A4398 entries, 0x24 bytes).
struct RoomLight {
    Vec position;
    Vec direction;      // spot lights only
    GXColor color;
    u8 _1C;
    u8 spotAngle;       // 0 = point light
    u16 refDistance;
    u16 refBrightness;  // percent
};

constexpr int kMaxRoomLights = 6;

void setRoomLights(const RoomLight* lights, int count);  // fills lbl_803A4398 / lbl_804D81B8
void setUseRoomLights(bool use);                         // lbl_804D07C8

// fn_80015BDC: load lights for this frame and set room draw state.
// Returns the GX light mask the room materials should use.
u32 beginRoomLighting(const Mtx view, GXColor ambient);

// fn_80016174: specular light (slot 6).
void setSpecularLight(const Vec& pos, const Vec& dir, GXColor color, const Mtx view, f32 shininess);

// fn_80014708 / fn_80016040: camera light or flashlight in slot 7.
struct Flashlight {
    bool on;
    Vec position;   // world
    Vec direction;  // world
    GXColor color;
    u8 spotAngle;
    u16 refDistance;
    u16 refBrightness;
};
bool setCameraLight(const Vec& eye, const Vec& target, const Mtx view, const Flashlight* flashlight);

u32 roomLightMask();        // lbl_804D0044
GXAttnFn roomLightAttn();   // lbl_804D0040

}  // namespace lm::gfx
