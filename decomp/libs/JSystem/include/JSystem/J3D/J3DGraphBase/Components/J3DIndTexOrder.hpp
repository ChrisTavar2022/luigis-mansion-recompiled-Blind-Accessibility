#ifndef J3D_GRAPH_BASE_COMPONENTS_INDTEXORDER
#define J3D_GRAPH_BASE_COMPONENTS_INDTEXORDER

#include <JSystem/J3D/J3DGraphBase/J3DStruct.hpp>

extern const J3DIndTexOrderInfo j3dDefaultIndTexOrderNull;

class J3DIndTexOrder : public J3DIndTexOrderInfo {
public:
	J3DIndTexOrder() { *(J3DIndTexOrderInfo*)this = j3dDefaultIndTexOrderNull; }
	J3DIndTexOrder(const J3DIndTexOrderInfo& info)
	{
		*(J3DIndTexOrderInfo*)this = info;
	}

	J3DIndTexOrder& operator=(const J3DIndTexOrder& other)
	{
		mCoord = other.mCoord;
		mMap   = other.mMap;
		field_0x4 = other.field_0x4;
		field_0x5 = other.field_0x5;
		return *this;
	}

	u8 getCoord() const { return mCoord; }
	u8 getMap() const { return mMap; }

	void load(u8) const;
public:
	/* 0x4 */ u8 field_0x4;
	/* 0x5 */ u8 field_0x5;
};

#endif
