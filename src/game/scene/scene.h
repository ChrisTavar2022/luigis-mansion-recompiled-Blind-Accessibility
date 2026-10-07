#pragma once
// Top-level scene machine (retail: auto_03_8000B014, Unsorted/Scene).
#include <dolphin/types.h>

namespace lm {

enum SceneID : s32 {
    SCENE_BOOT,
    SCENE_TITLE,
    SCENE_MAIN_GAME,
    SCENE_SELECT,
    SCENE_OPENING,
    SCENE_STAFF_ROLL,
    SCENE_ENDING,
    SCENE_MOVIE,
    SCENE_MAX,
};

struct Scene {
    /* 0x00 */ char name[16];
    /* 0x10 */ s16 retraceInterval;  // 1 = 60 Hz scene, 2 = 30 Hz scene
    /* 0x14 */ void (*init)();       // runs on the loader thread
    /* 0x18 */ s32 (*update)();      // returns the scene id to run next
    /* 0x1C */ void (*draw)();
    /* 0x20 */ void (*destroy)();
};

extern Scene* gSceneTable[SCENE_MAX];  // sSceneTable
extern Scene* gCurScene;               // sCurScene
extern s32 gCurSceneId;                // sCurSceneId

extern u64 gFrameCounter;       // lbl_804D8090: frames since boot
extern u64 gSceneFrameCounter;  // lbl_804D8098: frames since the current scene started

[[noreturn]] void runScenes();  // fn_8000B7F4

// fn_8000B334: switch to `scene` at the end of this frame (via the scene-control queue).
void requestScene(s32 scene);

}  // namespace lm
