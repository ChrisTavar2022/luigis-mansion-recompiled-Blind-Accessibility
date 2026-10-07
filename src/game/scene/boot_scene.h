#pragma once
#include "game/scene/scene.h"

class JKRMemArchive;

namespace lm::BootScene {

extern Scene sScene;
extern s32 sAudioReady;                 // lbl_804D8110: set once boot loading finished
extern JKRMemArchive* sStaticDataArchive;

}  // namespace lm::BootScene
