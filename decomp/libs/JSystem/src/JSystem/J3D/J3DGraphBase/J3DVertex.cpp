#include <JSystem/J3D/J3DGraphBase/J3DVertex.hpp>

J3DVertexData::J3DVertexData()
{
	unk0            = 0;
	unk4            = 0;
	unk8            = 0;
	mVtxAttrFmtList = nullptr;
	mVtxPosArray    = nullptr;
	mVtxNormArray   = nullptr;
	mVtxNBTArray    = 0;
	for (int i = 0; i < ARRAY_COUNT(mVtxColorArray); ++i)
		mVtxColorArray[i] = nullptr;
	for (int i = 0; i < ARRAY_COUNT(mVtxTexCoordArray); ++i)
		mVtxTexCoordArray[i] = nullptr;
}

J3DVertexData::~J3DVertexData() { }

J3DDrawMtxData::J3DDrawMtxData()
{
	mEntryNum     = 0;
	mDrawMtxFlag  = 0;
	mDrawMtxIndex = 0;
}

J3DDrawMtxData::~J3DDrawMtxData() { }
