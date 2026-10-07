#ifndef A_EN_AT_STRUCTS_HPP
#define A_EN_AT_STRUCTS_HPP

#include "JSystem/JGeometry/JGVec3.hpp"
#include "dolphin/types.h"

// Maybe the JGeometry is the first offset. Since it's used in AEnAt, i assume that is hitbox stuff
struct AEnAtStruct1 {
    /* 0x00 */ f32 _00;
    /* 0x04 */ f32 _04;
    /* 0x08 */ Vec mOffset;
};

struct AEnAtStruct2 {
    /* 0x00 */ s32 mCount;
    /* 0x04 */ const AEnAtStruct1* mStruct1;
    /* 0x08 */ u32 _08;
};

#endif
