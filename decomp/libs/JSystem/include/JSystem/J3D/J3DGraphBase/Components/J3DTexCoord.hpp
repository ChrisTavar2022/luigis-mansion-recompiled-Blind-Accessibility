#ifndef J3D_GRAPH_BASE_COMPONENTS_TEXCOORD
#define J3D_GRAPH_BASE_COMPONENTS_TEXCOORD

#include <JSystem/J3D/J3DGraphBase/J3DStruct.hpp>

extern const J3DTexCoordInfo j3dDefaultTexCoordInfo[8];

class J3DTexCoord : public J3DTexCoordInfo {
public:
	J3DTexCoord() { J3DTexCoordInfo::operator=(j3dDefaultTexCoordInfo[0]); }
	J3DTexCoord(const J3DTexCoordInfo& info) { *(J3DTexCoordInfo*)this = info; }

	J3DTexCoord& operator=(const J3DTexCoord& other)
	{
		mTexGenType = other.mTexGenType;
		mTexGenSrc  = other.mTexGenSrc;
		mTexGenMtx  = other.mTexGenMtx;
		return *this;
	}

	GXTexGenType getTexGenType() { return (GXTexGenType)mTexGenType; }
	GXTexGenSrc getTexGenSrc() { return (GXTexGenSrc)mTexGenSrc; }
	u8 getTexGenMtx() { return mTexGenMtx; }

	void setTexGenType(GXTexGenType v) { mTexGenType = v; }
	void setTexGenSrc(GXTexGenSrc v) { mTexGenSrc = v; }
	void setTexGenMtx(GXTexMtx v) { mTexGenMtx = v; }

	void load(u8);
};

class J3DTexCoord2 : public J3DTexCoordInfo2 {
public:
	J3DTexCoord2() {
		//J3DTexCoord2::operator=(j3dDefaultTexCoordInfo[0]);
		info = j3dDefaultTexCoordInfo[0];
		info = j3dDefaultTexCoordInfo[0];
		unk4 = 0x100;
	}

	J3DTexCoord2(const J3DTexCoord2& info) { *(J3DTexCoordInfo2*)this = info; }

	J3DTexCoord2& operator=(const J3DTexCoord2& other)
	{
		info = other.info;
		unk4 = other.unk4;
		return *this;
	}

	GXTexGenType getTexGenType() { return (GXTexGenType)info.mTexGenType; }
	GXTexGenSrc getTexGenSrc() { return (GXTexGenSrc)info.mTexGenSrc; }
	u8 getTexGenMtx() { return info.mTexGenMtx; }
	GXBool getUnk4Something() { return unk4 >> 8; }
	u32 getUnk4Something2() { return unk4 & 0xFF; }

	void load(u8);
};

#endif
