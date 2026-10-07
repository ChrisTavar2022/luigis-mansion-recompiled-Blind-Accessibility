#ifndef CHARACTER_H_
#define CHARACTER_H_

#include "Unsorted/MoveObj.hpp"
#include <JSystem/JGeometry/JGVec3.hpp>

// The split is not right. (lower bound and upper bound are just vrom vtable for now)
class Character : public MoveObj {
public:
    Character();

    /* 0x08 */ virtual ~Character();
    /* 0x0c */ virtual void vt_0c();
    /* 0x10 */ virtual void vt_10();
    /* 0x14 */ virtual void vt_14();
    /* 0x18 */ virtual s32 vt_18();
    /* 0x1C */ virtual void vt_1C(JGeometry::TVec3f*);
    /* 0x20 */ virtual void vt_20(void*); // Unsure of arg type.

public:
    /* 0x78C */ s32 _78C;
};

// All of these are referenced in EnManager. No arg types or return types are guaranteed.
void characterInit(void*, void*); // lbl_8033B1B0 = character.cpp,  lbl_8033B1C0 = Character init failed
void* fn_80069130(void*);

#endif
