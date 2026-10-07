#ifndef S3DMODEL_H_
#define S3DMODEL_H_

#include <types.h>
#include <dolphin/mtx.h>


class S3DModel {
public:
    struct  unkS3DModel_1 {
        /* 0x00 */ s32 _0;
        /* 0x04 */ s32 _4;
        /* 0x08 */ u16 _8;
        /* 0x0A */ u16 _A;
        /* 0x0C */ u16 _C;
    };

    S3DModel(unkS3DModel_1*, s32);

    void init(unkS3DModel_1*, s32);

public:
    /* 0x00 */ s32 _0;
    /* 0x04 */ unkS3DModel_1* _4;
    // size: 0x118
};

#endif
