#include "Koga/EnManager.hpp"
#include "Koga/GameModeUtil.hpp"
#include "Unsorted/AssortedEnManager.hpp"

namespace Koga {

    // Matches but underlying function gets inlined, causing no match currently.
    Vec* fn_800E9568(s32 appearSlotIndex) {
        return Koga::MissionMode::getMissionMode()->getEnManager()->fn_800E5564(appearSlotIndex);
    }

    // Matches but underlying function gets inlined, causing no match currently.
    s32 fn_800E9594(s32 appearSlotIndex) {
        return Koga::MissionMode::getMissionMode()->getEnManager()->fn_800E55AC(appearSlotIndex);
    }

    // https://decomp.me/scratch/Zhrs6
    BOOL fn_800E95C0(s32 expectedPoint, JGeometry::TVec3f* pParam_1, u16* out) {
        ToolDataRef appearEntry = Koga::MissionMode::getMissionMode()->getEnManager()->fn_800E5488(expectedPoint);
        ToolData* pCharInfo = appearEntry.getToolData();
        s32 entryIndex = appearEntry.getEntryIndex();
        bool bPointFound = false;

        if (pCharInfo != nullptr && 0 <= entryIndex) { //if (appearEntry.isValid()) { 
            bPointFound = true;
        }

        if (!bPointFound) {
            return false;
        }
        
        pCharInfo->getValue(entryIndex, "pos_x", &pParam_1->x);
        pCharInfo->getValue(entryIndex, "pos_y", &pParam_1->y);
        pCharInfo->getValue(entryIndex, "pos_z", &pParam_1->z);

        if (out != nullptr) {
            u32 dir = 0;
            pCharInfo->getValue(entryIndex, "dir_y", &dir);
            *out = dir;
        }

        return true;
    }

    s32 fn_800E96B8(JGeometry::TVec3f* pParam_1, JGeometry::TVec3f* pParam_2, f32 param_3) {
        return Koga::MissionMode::getMissionMode()->getEnManager()->fn_800E5660(pParam_1, pParam_2, param_3);
    }

    s32 fn_800E96E8(JGeometry::TVec3f* pParam_1, JGeometry::TVec3f* pParam_2) {
        return Koga::MissionMode::getMissionMode()->getEnManager()->fn_800E56E4(pParam_1, pParam_2);
    }

    s32 fn_800E971C(JGeometry::TVec3f* pParam_1, JGeometry::TVec3f* pParam_2) {
        return Koga::MissionMode::getMissionMode()->getEnManager()->fn_800E5784(pParam_1, pParam_2);
    }

    void* fn_800E9750(s32 param_1) {
        return Koga::MissionMode::getMissionMode()->getEnManager()->fn_800E5600(param_1);
    }

    s32 fn_800E977C(s32 param_1, s32 param_2) {
        return Koga::MissionMode::getMissionMode()->getEnManager()->fn_800E5868(param_1, param_2);
    }

    // https://decomp.me/scratch/fDI0r
    void fn_800E97B0(s32 param_1) {
        Koga::MissionMode::getMissionMode()->getEnManager()->fn_800E59D4(param_1);
    }
}

// https://decomp.me/scratch/LLh5c
// unkEnManager1::~unkEnManager1() {
//     if (_0 != nullptr) {
//         // Call some destructor through _0+0x804
//     }
// }

// https://decomp.me/scratch/EmrGG
// void fn_800E9A0C(void* pParam_1) {
//     //_0 = pParam_1; // It could just be a pointer to an unkEnCharacter object?
//     // _8 = fn_800DAC84(pParam_1); // This should be getting the 0x808 offset of param_1, then a secondary 0x3C offset?
//     mState = CHARSTATE_1;
//     _C = 0;
// }

// // https://decomp.me/scratch/IUVOm
// BOOL fn_800E9A58(u32) {
//     mState = CHARSTATE_1;
//     _C = 0;
//     char temp = fn_800C15E0(_0);
    
//     if (temp != 0) {
//         fn_800E9CDC();
//         // Then calls _0 offset 0x800, then 0xC, maybe some virtual table call?
//     }


//     return temp != 0;
// }

// // 99%, some stack mismanagement but could also be related to function inputs here.
// void fn_800E9ACC() {
//     mState = CHARSTATE_1;
//     fn_800C17EC(_0);
//     JGeometry::TVec3f defaultPos = JGeometry::TVec3f(-32000.0f);
//     fn_80067CB0(fn_800E9C5C(), defaultPos.x, defaultPos.y, defaultPos.z);
// }

// void fn_800E9B44() {
//     if (mState == 2) {
//         fn_800BF81C(_0);
//     }
// }
