#ifndef J3D_TEX_GEN_BLOCKS_HPP
#define J3D_TEX_GEN_BLOCKS_HPP

#include <JSystem/J3D/J3DGraphBase/Components/J3DTexCoord.hpp>
#include <JSystem/J3D/J3DGraphBase/Components/J3DTexMtx.hpp>
#include <JSystem/J3D/J3DGraphBase/Components/J3DNBTScale.hpp>

class J3DTexGenBlock {
public:
	virtual void reset(J3DTexGenBlock*) { }
	virtual void calc(MtxPtr) = 0;
	virtual s32 countDLSize() = 0;
	virtual u32 getType()     { return 'TGBK'; }

	virtual void setTexGenNum(const u8*) = 0;
	virtual void setTexGenNum(u32)        = 0;
	virtual u32 getTexGenNum() const      = 0;

	virtual void setTexCoord(u8, const J3DTexCoord*) = 0;
	virtual J3DTexCoord* getTexCoord(u8)             = 0;

	virtual void setTexCoord2(u8, const J3DTexCoord2*) = 0;
	virtual J3DTexCoord2* getTexCoord2(u8) = 0;

	virtual void setTexMtx(u8, J3DTexMtx*)           = 0;
	virtual J3DTexMtx* replaceTexMtx(u8, J3DTexMtx*) = 0;
	virtual J3DTexMtx* getTexMtx(u8)                 = 0;

	virtual void setTexMtx2(u8, J3DTexMtx*) = 0;
	virtual J3DTexMtx* replaceTexMtx2(u8, J3DTexMtx*) = 0;
	virtual J3DTexMtx* getTexMtx2(u8) = 0;

	virtual void setNBTScale(const J3DNBTScale*) = 0;
	virtual void setNBTScale(J3DNBTScale)        = 0;
	virtual J3DNBTScale* getNBTScale()           = 0;

	virtual ~J3DTexGenBlock() { }
	virtual void load()  = 0;
};

class J3DTexGenBlockFull : public J3DTexGenBlock {
public:
	J3DTexGenBlockFull()
	    : mNBTScale(j3dDefaultNBTScaleInfo)
	{
		initialize();
	}

	void initialize();

	virtual void reset(J3DTexGenBlock*);
	virtual void calc(MtxPtr);
	virtual s32 countDLSize();
	virtual u32 getType() { return 'TGFL'; }

	virtual void setTexGenNum(const u8* num) { mTexGenNum = *num; }
	virtual void setTexGenNum(u32 num) { mTexGenNum = num; }
	virtual u32 getTexGenNum() const { return mTexGenNum; }

	virtual void setTexCoord(u8 i, const J3DTexCoord* coord)
	{
		mTexCoord[i] = *coord;
	}
	virtual J3DTexCoord* getTexCoord(u8 i) { return &mTexCoord[i]; }

	virtual void setTexCoord2(u8 i, const J3DTexCoord2* coord) {
		mTexCoord2[i] = *coord;
	}

	virtual J3DTexCoord2* getTexCoord2(u8 i) { return &mTexCoord2[i]; }

	virtual void setTexMtx(u8 i, J3DTexMtx* mtx) { mTexMtx[i] = mtx; }
	virtual J3DTexMtx* replaceTexMtx(u8 i, J3DTexMtx* mtx)
	{
		J3DTexMtx* ret = mTexMtx[i];
		mTexMtx[i]     = mtx;
		return ret;
	}
	virtual J3DTexMtx* getTexMtx(u8 i) { return mTexMtx[i]; }

	virtual void setTexMtx2(u8 i, J3DTexMtx* mtx) { mTexMtx2[i] = mtx; }
	virtual J3DTexMtx* replaceTexMtx2(u8 i, J3DTexMtx* mtx)
	{
		J3DTexMtx* ret = mTexMtx2[i];
		mTexMtx2[i]     = mtx;
		return ret;
	}

	virtual J3DTexMtx* getTexMtx2(u8 i) { return mTexMtx2[i]; }

	virtual void setNBTScale(const J3DNBTScale* scale) { mNBTScale = *scale; }
	virtual void setNBTScale(J3DNBTScale scale) { mNBTScale = scale; }
	virtual J3DNBTScale* getNBTScale() { return &mNBTScale; }

	virtual ~J3DTexGenBlockFull() { }
	virtual void load();

public:
	/* 0x4 */ u8 mTexGenNum;
	/* 0x8 */ J3DTexCoord mTexCoord[8];
	/* 0x28 */ J3DTexCoord2 mTexCoord2[8]; //Probably a copy of mTexCoord
	/* 0x58 */ J3DTexMtx* mTexMtx[10];
	/* 0x80 */ J3DTexMtx* mTexMtx2[20];
	/* 0xD0 */ J3DNBTScale mNBTScale;
};

class J3DTexGenBlockBasic : public J3DTexGenBlock {
public:
	J3DTexGenBlockBasic()
	    : mNBTScale(j3dDefaultNBTScaleInfo)
	{
		initialize();
	}

	void initialize();

	virtual void reset(J3DTexGenBlock*);
	virtual void calc(MtxPtr);
	virtual s32 countDLSize();
	virtual u32 getType() { return 'TGBC'; }

	virtual void setTexGenNum(const u8* num) { mTexGenNum = *num; }
	virtual void setTexGenNum(u32 num) { mTexGenNum = num; }
	virtual u32 getTexGenNum() const { return mTexGenNum; }

	virtual void setTexCoord(u8 i, const J3DTexCoord* coord)
	{
		mTexCoord[i] = *coord;
	}
	virtual J3DTexCoord* getTexCoord(u8 i) { return &mTexCoord[i]; }

	virtual void setTexCoord2(u8, const J3DTexCoord2*) {}
	virtual J3DTexCoord2* getTexCoord2(u8) { return 0; }

	virtual void setTexMtx(u8 i, J3DTexMtx* mtx) { mTexMtx[i] = mtx; }
	virtual J3DTexMtx* replaceTexMtx(u8 i, J3DTexMtx* mtx)
	{
		J3DTexMtx* ret = mTexMtx[i];
		mTexMtx[i]     = mtx;
		return ret;
	}
	virtual J3DTexMtx* getTexMtx(u8 i) { return mTexMtx[i]; }

	virtual void setTexMtx2(u8, J3DTexMtx*) {}
	virtual J3DTexMtx* replaceTexMtx2(u8, J3DTexMtx*) { return 0; }
	virtual J3DTexMtx* getTexMtx2(u8) { return 0; }

	virtual void setNBTScale(const J3DNBTScale* scale) { mNBTScale = *scale; }
	virtual void setNBTScale(J3DNBTScale scale) { mNBTScale = scale; }
	virtual J3DNBTScale* getNBTScale() { return &mNBTScale; }

	virtual ~J3DTexGenBlockBasic() { }
	virtual void load();

public:
	/* 0x4 */ u8 mTexGenNum;
	/* 0x8 */ J3DTexCoord mTexCoord[8];
	/* 0x28 */ J3DTexMtx* mTexMtx[10];
	/* 0x50 */ J3DNBTScale mNBTScale;
};

#endif
