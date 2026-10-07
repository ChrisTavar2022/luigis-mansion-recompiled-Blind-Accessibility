#pragma once
#include "game/scene/scene.h"

class JKRMemArchive;

namespace lm::BootScene {

extern Scene sScene;
extern s32 sAudioReady;                 // lbl_804D8110: set once boot loading finished
extern JKRMemArchive* sStaticDataArchive;
JKRMemArchive* gameArchive();  // fn_80066FB4: the resident /Game/game_usa.szp archive

}  // namespace lm::BootScene
