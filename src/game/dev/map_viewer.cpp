// Developer fly-through of a whole map: every /Iwamoto/<map>/room_NN.arc drawn
// together (rooms are authored in world space). Main stick: move, C-stick: look,
// L/R triggers (or X/Y): down/up, B held: faster.
#include "game/dev/map_viewer.h"
#include "game/gfx/bin_model.h"
#include "game/gfx/room_light.h"
#include "game/system/camera.h"
#include "game/system/display.h"
#include "game/system/gx_util.h"
#include "game/system/pad.h"

#include "port/options.h"

#include <dolphin/gx.h>
#include <dolphin/os.h>
#include <dolphin/pad.h>

#include <JSystem/JKernel/JKRArchive.h>
#include <JSystem/JUtility/JUTGamePad.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace lm::dev::MapViewer {
namespace {

struct Room {
    JKRArchive* archive;
    gfx::BinData data;
    gfx::BinModel model;
    Vec center;
    f32 radius;
};

// Bounds from the (s16 or f32) position array, for distance culling.
void computeBounds(Room& room) {
    Vec lo = {1e30f, 1e30f, 1e30f}, hi = {-1e30f, -1e30f, -1e30f};
    const u8* p = static_cast<const u8*>(room.data.positions);
    const u8* end = p ? p + room.data.positionsSize : nullptr;
    const u32 stride = room.data.floatPositions ? 12 : 6;
    for (; p && end && p + stride <= end; p += stride) {
        f32 v[3];
        for (int k = 0; k < 3; k++) {
            if (room.data.floatPositions) {
                u32 bits = (u32(p[k * 4]) << 24) | (u32(p[k * 4 + 1]) << 16) | (u32(p[k * 4 + 2]) << 8) | p[k * 4 + 3];
                v[k] = std::bit_cast<f32>(bits);
            } else {
                v[k] = f32(s16((p[k * 2] << 8) | p[k * 2 + 1]));
            }
        }
        lo = {std::min(lo.x, v[0]), std::min(lo.y, v[1]), std::min(lo.z, v[2])};
        hi = {std::max(hi.x, v[0]), std::max(hi.y, v[1]), std::max(hi.z, v[2])};
    }
    if (lo.x > hi.x) {  // no positions: never the nearest room
        room.center = {0.0f, 0.0f, 0.0f};
        room.radius = -1e30f;
        return;
    }
    room.center = {(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};
    room.radius = 0.5f * std::sqrt((hi.x - lo.x) * (hi.x - lo.x) + (hi.y - lo.y) * (hi.y - lo.y) +
                                   (hi.z - lo.z) * (hi.z - lo.z));
}
std::vector<std::unique_ptr<Room>> sRooms;
Vec sEye = {0.0f, 300.0f, 0.0f};
f32 sYaw = 0.0f, sPitch = 0.0f;  // radians

}  // namespace

static void init() {
    const std::string map = port::options().mapViewer;
    for (int i = 0; i < 100; i++) {
        char path[64];
        std::snprintf(path, sizeof(path), "/Iwamoto/%s/room_%02d.arc", map.c_str(), i);
        if (DVDConvertPathToEntrynum(path) < 0) {
            continue;
        }
        auto room = std::make_unique<Room>();
        room->archive = JKRArchive::mount(path, JKRArchive::MOUNT_MEM, nullptr, JKRArchive::MOUNT_DIRECTION_HEAD);
        void* bin = room->archive ? room->archive->getResource("/room.bin") : nullptr;
        if (bin == nullptr || !gfx::parseBin(bin, room->archive->getResSize(bin), room->data)) {
            continue;
        }
        room->model.init(&room->data);
        computeBounds(*room);
        sRooms.push_back(std::move(room));
    }
    // Start inside an ordinary room (some "rooms" are huge exterior shells).
    const Room* start = nullptr;
    for (const auto& room : sRooms) {
        if (room->radius > 300.0f && room->radius < 1500.0f && (start == nullptr || room->radius < start->radius)) {
            start = room.get();
        }
    }
    if (start != nullptr) {
        // Begin just outside the room, looking at it (+Z), then fly in.
        sEye = {start->center.x, start->center.y, start->center.z - start->radius * 1.3f};
    }
    OSReport("[mapviewer] %s: loaded %u rooms, starting at (%.0f, %.0f, %.0f)\n", map.c_str(), u32(sRooms.size()),
             sEye.x, sEye.y, sEye.z);
}

static s32 update() {
    const f32 speed = (gGamePad->getButton() & PAD_BUTTON_B) ? 40.0f : 12.0f;
    sYaw -= gGamePad->getSubStickX() * 0.05f;
    sPitch = std::clamp(sPitch + gGamePad->getSubStickY() * 0.04f, -1.5f, 1.5f);
    const f32 fx = std::sin(sYaw), fz = std::cos(sYaw);
    const f32 mx = gGamePad->getMainStickX(), my = gGamePad->getMainStickY();
    sEye.x += (fx * my - fz * mx) * speed;
    sEye.z += (fz * my + fx * mx) * speed;
    if (gGamePad->getButton() & (PAD_TRIGGER_R | PAD_BUTTON_Y)) sEye.y += speed;
    if (gGamePad->getButton() & (PAD_TRIGGER_L | PAD_BUTTON_X)) sEye.y -= speed;
    return SCENE_TITLE;  // runs in the title slot
}

static void draw() {
    resetGXState();
    Mtx44 proj;
    MTXPerspective(proj, 50.0f, 4.0f / 3.0f, 10.0f, 40000.0f);
    GXSetProjection(proj, GX_PERSPECTIVE);
    setViewportJitter(0.0f, 0.0f, 640.0f, 480.0f, 0.0f, 1.0f);
    Vec at = {sEye.x + std::sin(sYaw) * std::cos(sPitch), sEye.y + std::sin(sPitch),
              sEye.z + std::cos(sYaw) * std::cos(sPitch)};
    Vec up = {0.0f, 1.0f, 0.0f};
    MTXLookAt(gCamera.viewMtx, &sEye, &up, &at);

    // Ported room lighting: default light plus the camera light (slot 7).
    gfx::beginRoomLighting(gCamera.viewMtx, GXColor{0x70, 0x70, 0x70, 0xFF});
    gfx::setCameraLight(sEye, at, gCamera.viewMtx, nullptr);
    GXSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    // Like the game, draw only the rooms around the viewer (the game draws the current
    // room and its neighbours; the renderer's per-frame vertex budget is sized for that).
    constexpr size_t kMaxRooms = 6;
    std::vector<std::pair<f32, Room*>> nearest;
    for (auto& room : sRooms) {
        const f32 dx = room->center.x - sEye.x, dy = room->center.y - sEye.y, dz = room->center.z - sEye.z;
        if (room->radius < 0.0f) {
            continue;  // no geometry
        }
        nearest.push_back({std::sqrt(dx * dx + dy * dy + dz * dz), room.get()});
    }
    std::sort(nearest.begin(), nearest.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    nearest.resize(std::min(nearest.size(), kMaxRooms));
    for (u8 pass = 0; pass < 3; pass++) {
        for (auto& [dist, room] : nearest) {
            room->model.draw(gCamera.viewMtx, pass);
        }
    }
}

static void destroy() {}

Scene sScene = {"MAPVIEW", 1, init, update, draw, destroy};

}  // namespace lm::dev::MapViewer
