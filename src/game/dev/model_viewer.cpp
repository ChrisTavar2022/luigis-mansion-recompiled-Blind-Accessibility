// Developer scene: draws one model archive from /Game/game_usa.szp with the
// ported .mdl renderer. Enabled with `--model-viewer <name>` (e.g. banana); runs
// after the boot scene instead of the title screen.
#include "game/dev/model_viewer.h"
#include "game/gfx/mdl.h"
#include "game/gfx/s3d_model.h"
#include "game/scene/boot_scene.h"
#include "game/system/camera.h"
#include "game/system/display.h"
#include "game/system/gx_util.h"
#include "game/system/heap.h"

#include "port/options.h"

#include "helpers/endian.h"
#include <cstring>
#include <dolphin/gx.h>
#include <dolphin/os.h>

#include <JSystem/JKernel/JKRHeap.h>
#include <JSystem/JKernel/JKRMemArchive.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <string>

namespace lm::dev::ModelViewer {
namespace {

JKRMemArchive* sModelArchive;
gfx::MdlData sMdl;
gfx::KeyAnim sKey;
gfx::S3DModel sModel;
f32 sFrame;
f32 sRadius = 100.0f;
Vec sCenter;
s32 sYaw;

void computeBounds() {
    const auto* p = static_cast<const u8*>(sMdl.positions);
    const u32 count = (u32(static_cast<const u8*>(sMdl.normals) - p)) / 12;
    Vec lo = {1e30f, 1e30f, 1e30f}, hi = {-1e30f, -1e30f, -1e30f};
    for (u32 i = 0; i < count; i++) {
        f32 v[3];
        for (int k = 0; k < 3; k++) {
            u32 bits;
            std::memcpy(&bits, p + i * 12 + k * 4, 4);
            v[k] = std::bit_cast<f32>(be32(bits));
        }
        lo = {std::min(lo.x, v[0]), std::min(lo.y, v[1]), std::min(lo.z, v[2])};
        hi = {std::max(hi.x, v[0]), std::max(hi.y, v[1]), std::max(hi.z, v[2])};
    }
    sCenter = {(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};
    sRadius = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z}) * 1.2f + 1.0f;
}

}  // namespace

static void init() {
    const std::string name = port::options().modelViewer;
    const std::string arcPath = "/model/" + name + ".arc";
    JKRMemArchive* game = BootScene::gameArchive();
    void* arc = game != nullptr ? game->getResource(arcPath.c_str()) : nullptr;
    if (arc == nullptr) {
        OSReport("[viewer] %s not found in game_usa.szp\n", arcPath.c_str());
        return;
    }
    sModelArchive = JKR_NEW JKRMemArchive(arc, 0, JKRMEMBREAK_FLAG_UNKNOWN0);
    const std::string mdlPath = "/model/" + name + ".mdl";
    void* mdl = sModelArchive->getResource(mdlPath.c_str());
    if (mdl == nullptr || !gfx::parseMdl(mdl, sModelArchive->getResSize(mdl), sMdl)) {
        OSReport("[viewer] could not load %s\n", mdlPath.c_str());
        return;
    }
    // First animation in /key, if any.
    void* key = sModelArchive->getResource("/key/wait.key");
    if (key != nullptr && !port::options().viewerRestPose) {
        gfx::parseKey(key, sModelArchive->getResSize(key), sKey);
    } else {
        // A zero-channel animation leaves every joint at its rest transform.
        sKey.joints.assign(sMdl.numJoints, {});
        sKey.flags = 0;
    }
    sModel.init(&sMdl, 0);
    sModel.setAnim(&sKey);
    computeBounds();
    OSReport("[viewer] %s: %u joints, %u materials, %u shapes, %u textures, %u frames\n", name.c_str(),
             sMdl.numJoints, u32(sMdl.materials.size()), u32(sMdl.shapes.size()), u32(sMdl.textures.size()),
             sKey.numFrames);
}

static s32 update() {
    sFrame += 1.0f;
    if (sKey.numFrames != 0 && sFrame > f32(sKey.numFrames)) {
        sFrame = 1.0f;
    }
    sYaw += 0x80;
    return SCENE_TITLE;  // the viewer runs in the title slot; stay here
}

static void draw() {
    if (sMdl.raw == nullptr) {
        return;
    }
    resetGXState();
    Mtx44 proj;
    MTXPerspective(proj, 40.0f, 4.0f / 3.0f, sRadius * 0.05f, sRadius * 20.0f);
    GXSetProjection(proj, GX_PERSPECTIVE);
    setViewportJitter(0.0f, 0.0f, 640.0f, 480.0f, 0.0f, 1.0f);

    const f32 dist = sRadius * 4.0f;  // animations can extend well past the rest-pose bounds
    Vec eye = {sCenter.x + dist * sinIdx(u16(sYaw)), sCenter.y + sRadius * 0.3f, sCenter.z + dist * cosIdx(u16(sYaw))};
    Vec up = {0.0f, 1.0f, 0.0f};
    MTXLookAt(gCamera.viewMtx, &eye, &up, &sCenter);

    GXLightObj light;
    Vec lightPos = {eye.x, eye.y + sRadius, eye.z};
    MTXMultVec(gCamera.viewMtx, &lightPos, &lightPos);
    GXInitLightPos(&light, lightPos.x, lightPos.y, lightPos.z);
    GXInitLightColor(&light, GXColor{0xFF, 0xFF, 0xFF, 0xFF});
    GXInitLightAttn(&light, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);
    GXLoadLightObjImm(&light, GX_LIGHT0);
    GXSetChanAmbColor(GX_COLOR0A0, GXColor{0x50, 0x50, 0x50, 0xFF});
    GXSetChanAmbColor(GX_COLOR1A1, GXColor{0x50, 0x50, 0x50, 0xFF});

    sModel.setFrame(sFrame);
    sModel.calcJoints(gIdentityMtx);
    sModel.calcView(gCamera.viewMtx);
    gfx::beginModelDraw();
    sModel.drawOpaque();
    sModel.drawTranslucent();
}

static void destroy() {}

Scene sScene = {"MODELVIEW", 1, init, update, draw, destroy};

}  // namespace lm::dev::ModelViewer
