#ifndef MOVE_OBJ_H_
#define MOVE_OBJ_H_

#include "JSystem/JAudio/JAInterface/JAIAnimation.hpp"
#include "JSystem/JGeometry/JGVec3.hpp"
#include <types.h>
#include <dolphin/mtx.h>

// 0xe3, 0xe7
class MoveObj {
public:
    MoveObj();

    /* 0x08 */ virtual ~MoveObj();
    /* 0x0C */ virtual void vt_0C(u16, u16, u16);

    // Defined in the EnManager TU, but exist in this class based on the split, I think.
    MoveObj* fn_800E7414(u32 id);
    MoveObj* fn_800E7510(MoveObj*);
    void fn_800E760C();
    void fn_800E7628();
    bool fn_800E7650();

    
    inline s32 get_38() const { return _38; }
    inline s32 get_3C() const { return _3C; }

protected:
    void fn_80067CB0(double var1, double var2, double var3);

public:
    /* 0x004 */ u8 _0[0x34];
    /* 0x038 */ s32 _38;
    /* 0x03C */ s32 _3C;
    /* 0x040 */ s32 _40;

    
    /* 0x044 */ JGeometry::TVec3<f32> _vec44;
    /* 0x050 */ JGeometry::TVec3<f32> _vec50;
    /* 0x05c */ JGeometry::TVec3<f32> _vec5c;

    /* 0x068 - 0x53c */ u8 _68[0xb4 - 0x68];

    /* 0x0b4 */ float _b4;
    
    /* 0x0b8 - 0x53c */ u8 _b8[0x53c - 0xb8];

    /* 0x53c */ JAIAnimeSound mAnimeSound;

    /* 0x040 */ u8 _54c[0x788 - 0x5d4];

    /* 0x788 */ u8 _size; // TEMP ! USED JUST FOR MY CALCULATION FOR THE SIZE TO NOT DESTROY EVERYTHING
};

#endif
