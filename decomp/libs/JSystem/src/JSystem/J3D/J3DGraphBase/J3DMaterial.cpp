#include <JSystem/J3D/J3DGraphBase/J3DMaterial.hpp>

#include <JSystem/J3D/J3DGraphBase/Blocks/J3DTevBlocks.hpp>
#include <JSystem/J3D/J3DGraphBase/Blocks/J3DPEBlocks.hpp>
#include <JSystem/J3D/J3DGraphBase/Blocks/J3DColorBlocks.hpp>
#include <JSystem/J3D/J3DGraphBase/Blocks/J3DIndBlocks.hpp>
#include <JSystem/J3D/J3DGraphBase/Blocks/J3DTexGenBlocks.hpp>

#include <JSystem/J3D/J3DGraphBase/J3DSys.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DTransform.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DPacket.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DShape.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DTevs.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DLoad.hpp>
#include <dolphin/os.h>

void J3DColorBlockLightOff::initialize()
{
	mColorChanNum = 0;
	for (u32 i = 0; i < ARRAY_COUNT(mMatColor); ++i)
		mMatColor[i] = j3dDefaultColInfo;
	for (u32 i = 0; i < ARRAY_COUNT(mColorChan); ++i)
		mColorChan[i].mChanCtrl = 0xFFFF;
}

void J3DColorBlockLightOn::initialize()
{
	mColorChanNum = 0;
	for (u32 i = 0; i < ARRAY_COUNT(mMatColor); i++)
		mMatColor[i] = j3dDefaultColInfo;
	for (u32 i = 0; i < ARRAY_COUNT(mAmbColor); i++)
		mAmbColor[i] = j3dDefaultAmbInfo;
	for (u32 i = 0; i < ARRAY_COUNT(mColorChan); ++i)
		mColorChan[i].mChanCtrl = 0xFFFF;
	for (u32 i = 0; i < ARRAY_COUNT(mLight); ++i)
		mLight[i] = nullptr;
}

void J3DTexGenBlockFull::initialize()
{
	mTexGenNum = 0;

	for (int i = 0; i < ARRAY_COUNT(mTexMtx); i++) {
		mTexMtx[i] = nullptr;
	}

	for (int i = 0; i < ARRAY_COUNT(mTexMtx2); i++) {
		mTexMtx2[i] = nullptr;
	}
}

void J3DTexGenBlockBasic::initialize()
{
	mTexGenNum = 0;
	for (int i = 0; i < 10; i++)
		mTexMtx[i] = nullptr;
}

void J3DPEBlockFull::initialize()
{
	mFog                   = nullptr;
	mAlphaComp.mAlphaCmpID = 0xFFFF;
	mZMode.mZModeID        = 0xFFFF;
	mZCompLoc              = 0xFF;
	mDither                = 0xFF;
}

void J3DTevBlock1::initialize()
{
	mTexNo[0]                 = 0xFFFF;
}

void J3DTevBlock2::initialize()
{
	mTexNo[0] = 0xFFFF;
	mTexNo[1] = 0xFFFF;

	mTevStageNum = 0;

	for (int i = 0; i < ARRAY_COUNT(mTevKColorSel); ++i)
		mTevKColorSel[i] = 0xFF;
	for (int i = 0; i < ARRAY_COUNT(mTevKAlphaSel); ++i)
		mTevKAlphaSel[i] = 0xFF;

	for (int i = 0; i < 4; ++i)
		mTevColor[i] = j3dDefaultTevColor;

	for (int i = 0; i < ARRAY_COUNT(mTevKColor); ++i)
		mTevKColor[i] = j3dDefaultTevKColor;
}
void J3DTevBlock4::initialize()
{
	mTexNo[0] = 0xFFFF;
	mTexNo[1] = 0xFFFF;
	mTexNo[2] = 0xFFFF;
	mTexNo[3] = 0xFFFF;

	mTevStageNum = 0;

	for (int i = 0; i < ARRAY_COUNT(mTevKColorSel); ++i)
		mTevKColorSel[i] = 0xFF;
	for (int i = 0; i < ARRAY_COUNT(mTevKAlphaSel); ++i)
		mTevKAlphaSel[i] = 0xFF;

	for (int i = 0; i < 4; ++i)
		mTevColor[i] = j3dDefaultTevColor;

	for (int i = 0; i < ARRAY_COUNT(mTevKColor); ++i)
		mTevKColor[i] = j3dDefaultTevKColor;
}
void J3DTevBlock16::initialize()
{
	for (int i = 0; i < ARRAY_COUNT(mTexNo); ++i)
		mTexNo[i] = 0xFFFF;

	mTevStageNum = 0;

	for (int i = 0; i < 4; i++)
		mTevColor[i] = j3dDefaultTevColor;

	for (int i = 0; i < ARRAY_COUNT(mTevKColor); ++i)
		mTevKColor[i] = j3dDefaultTevKColor;

	for (int i = 0; i < ARRAY_COUNT(mTevKColorSel); ++i)
		mTevKColorSel[i] = 0xFF;

	for (int i = 0; i < ARRAY_COUNT(mTevKAlphaSel); ++i)
		mTevKAlphaSel[i] = 0xFF;

}

J3DColorBlock* J3DMaterial::createColorBlock(int param_1)
{
	if (param_1 == 1)
		return new J3DColorBlockLightOff;
	else
		return new J3DColorBlockLightOn;
}

J3DTexGenBlock* J3DMaterial::createTexGenBlock(int param_1)
{
	if (param_1 == 1)
		return new J3DTexGenBlockBasic;
	else
		return new J3DTexGenBlockFull;
}

J3DTevBlock* J3DMaterial::createTevBlock(int param_1)
{
	J3DTevBlock* result = nullptr;
	if (param_1 <= 1)
		result = new J3DTevBlock1;
	else if (param_1 == 2)
		result = new J3DTevBlock2;
	else if (param_1 <= 4)
		result = new J3DTevBlock4;
	else if (param_1 <= 16)
		result = new J3DTevBlock16;
	return result;
}

J3DIndBlock* J3DMaterial::createIndBlock(int param_1)
{
	J3DIndBlock* result;
	if (param_1 != 0)
		result = new J3DIndBlockFull;
	else
		result = new J3DIndBlockNull;
	return result;
}

J3DPEBlock* J3DMaterial::createPEBlock(int param_1, u32 param_2)
{
	if (param_1 == 1) {
		if (param_2 & 1)
			return new J3DPEBlockOpa;

		if (param_2 & 2)
			return new J3DPEBlockTexEdge;

		if (param_2 & 4)
			return new J3DPEBlockXlu;
	}
	return new J3DPEBlockFull;
}

void J3DMaterial::initialize()
{
	mShape            = nullptr;
	mNext             = nullptr;
	unk8              = 1;
	unkC              = -1;
	unkE = 0;
	unk10             = nullptr;
	unk14             = 0;
	mColorBlock       = nullptr;
	mTexGenBlock      = nullptr;
	mTevBlock         = nullptr;
	mIndBlock         = nullptr;
	mPEBlock          = nullptr;
	mOriginalMaterial = nullptr;
	unk30 = 0;
	//unk38             = nullptr;
	//unk3C             = nullptr;
	//unk1C             = 0;
}

void J3DMaterial::addShape(J3DShape* shape) { mShape = shape; }

s32 J3DColorBlockLightOff::countDLSize() { return 0x60; }
s32 J3DColorBlockLightOn::countDLSize() { return 0x280; }

s32 J3DTexGenBlockBasic::countDLSize() { return 0x2C0; }
s32 J3DTexGenBlockFull::countDLSize() { return 0x820; }

s32 J3DTevBlock1::countDLSize() { return 0x80; }
s32 J3DTevBlock2::countDLSize() { return 0x180; }
s32 J3DTevBlock4::countDLSize() { return 0x260; }
s32 J3DTevBlock16::countDLSize() { return 0x780; }

s32 J3DIndBlockFull::countDLSize() { return 0x64; }

s32 J3DPEBlockOpa::countDLSize() { return 0x20; }
s32 J3DPEBlockTexEdge::countDLSize() { return 0x20; }
s32 J3DPEBlockXlu::countDLSize() { return 0x20; }
s32 J3DPEBlockFull::countDLSize() { return 0x60; }

s32 J3DMaterial::countDLSize()
{
	return mColorBlock->countDLSize() + mTexGenBlock->countDLSize()
	       + mTevBlock->countDLSize() + mIndBlock->countDLSize()
	       + mPEBlock->countDLSize();
}

inline void loadTexCoordScale(GXTexCoordID coord,
                              const J3DTexCoordScaleInfo& info)
{
	J3DGDSetTexCoordScale2(coord, info.field_0x00, info.field_0x04 == 1, 0,
	                       info.field_0x02, info.field_0x06 == 1, 0);
}

void J3DColorBlockLightOff::load()
{

	for (u8 i = 0; i < ARRAY_COUNT(mMatColor); ++i)
		loadChanMatColor(i, mMatColor[i].color);

	loadColorChanNum(mColorChanNum);
	for (u8 i = 0; i < mColorChanNum * 2; ++i)
		mColorChan[i].load(i);

	if (mCullMode != 0xFF)
		loadCullMode(mCullMode);
}

void J3DColorBlockLightOn::load()
{
	for (u8 i = 0; i < ARRAY_COUNT(mMatColor); ++i)
		loadChanMatColor(i, mMatColor[i].color);

	for (u8 i = 0; i < ARRAY_COUNT(mAmbColor); ++i)
		loadChanAmbColor(i, mAmbColor[i].color);

	loadColorChanNum(mColorChanNum);

	for (u8 i = 0; i < mColorChanNum * 2; ++i)
		mColorChan[i].load(i);

	for (u8 i = 0; i < 8; i++) {
		if (mLight[i]) {
			mLight[i]->load(i);
		}
	}

	if (mCullMode != 0xFF)
		loadCullMode(mCullMode);
}

void J3DTexGenBlockBasic::load()
{
	loadTexGenNum(mTexGenNum);
	for (u8 i = 0; i < mTexGenNum; ++i)
		mTexCoord[i].load(i);

	for (u8 i = 0; i < ARRAY_COUNT(mTexMtx); ++i)
		if (mTexMtx[i])
			mTexMtx[i]->load(i);
}

void J3DTexGenBlockFull::load()
{
	loadTexGenNum(mTexGenNum);
	for (u8 i = 0; i < mTexGenNum; ++i)
		mTexCoord[i].load(i);
	for (u8 i = 0; i < mTexGenNum; ++i)
		mTexCoord2[i].load(i);

	for (u8 i = 0; i < ARRAY_COUNT(mTexMtx); ++i)
		if (mTexMtx[i])
			mTexMtx[i]->load(i);
	for (u8 i = 0; i < ARRAY_COUNT(mTexMtx2); ++i)
		if (mTexMtx2[i])
			mTexMtx2[i]->load2(i);
}


void J3DTevBlock1::load()
{
	loadTevStageNum(mTevStageNum);
	if (mTexNo[0] != 0xffff) {
		loadTexNo(0, mTexNo[0]);
	}

	s32 tevStageNum = mTevStageNum;

	if (tevStageNum != 0) {
		mTevOrder[0].load(0);
	}

	if (tevStageNum != 0) {
		mTevStage[0].load(0);
		mIndTevStage[0].load(0);
	}
}

void J3DTevBlock2::load()
{

	loadTevStageNum(mTevStageNum);

	s32 tevStageNum = mTevStageNum;

	for (u8 i = 0; i < 2; i++) {
		if (mTexNo[i] != 0xffff) {
			loadTexNo(i, mTexNo[i]);
		}
	}

	for (u8 i = 0; i < tevStageNum; i++) {
		mTevOrder[i].load(i);
	}


	for (u8 i = 0; i < 4; i++) {
		loadTevColor(i, mTevColor[i]);
	}
	for (u8 i = 0; i < 4; i++) {
		loadTevKColor(i, mTevKColor[i]);
	}
	for (u8 i = 0; i < tevStageNum; i++) {
		mTevStage[i].load(i);
		mIndTevStage[i].load(i);
	}

	for (u8 i = 0; i < tevStageNum; i++) {
		if (mTevKColorSel[i] != 0xFF)
			loadTevKColorSel(i, mTevKColorSel[i]);
	}
	for (u8 i = 0; i < tevStageNum; i++) {
		if (mTevKAlphaSel[i] != 0xFF)
			loadTevKAlphaSel(i, mTevKAlphaSel[i]);
	}
	for (u8 i = 0; i < 4; i++) {
		mTevSwapModeTable[i].load(i);
	}

}

void J3DTevBlock4::load()
{
	loadTevStageNum(mTevStageNum);
	s32 tevStageNum = mTevStageNum;

	for (u8 i = 0; i < 4; i++) {
		if (mTexNo[i] != 0xffff) {
			loadTexNo(i, mTexNo[i]);
		}
	}

	for (u8 i = 0; i < tevStageNum; i++) {
		mTevOrder[i].load(i);
	}

	for (u8 i = 0; i < 4; i++) {
		loadTevColor(i, mTevColor[i]);
	}
	for (u8 i = 0; i < 4; i++) {
		loadTevKColor(i, mTevKColor[i]);
	}
	for (u8 i = 0; i < tevStageNum; i++) {
		mTevStage[i].load(i);
		mIndTevStage[i].load(i);
	}
	for (u8 i = 0; i < tevStageNum; i++) {
		if (mTevKColorSel[i] != 0xFF)
			loadTevKColorSel(i, mTevKColorSel[i]);
	}
	for (u8 i = 0; i < tevStageNum; i++) {
		if (mTevKAlphaSel[i] != 0xFF)
			loadTevKAlphaSel(i, mTevKAlphaSel[i]);
	}
	for (u8 i = 0; i < 4; i++) {
		mTevSwapModeTable[i].load(i);
	}
}

void J3DTevBlock16::load()
{
	for (u8 i = 0; i < 8; i++) {
		if (mTexNo[i] != 0xffff) {
			loadTexNo(i, mTexNo[i]);
		}
	}

	loadTevStageNum(mTevStageNum);
	s32 tevStageNum = mTevStageNum;

	for (u8 i = 0; i < tevStageNum; i++) {
		mTevOrder[i].load(i);
	}

	for (u8 i = 0; i < 4; i++) {
		loadTevColor(i, mTevColor[i]);
	}
	for (u8 i = 0; i < 4; i++) {
		loadTevKColor(i, mTevKColor[i]);
	}
	for (u8 i = 0; i < tevStageNum; i++) {
		mTevStage[i].load(i);
		mIndTevStage[i].load(i);
	}
	for (u8 i = 0; i < tevStageNum; i++) {
		if (mTevKColorSel[i] != 0xFF)
			loadTevKColorSel(i, mTevKColorSel[i]);
	}
	for (u8 i = 0; i < tevStageNum; i++) {
		if (mTevKAlphaSel[i] != 0xFF)
			loadTevKAlphaSel(i, mTevKAlphaSel[i]);
	}
	for (u8 i = 0; i < 4; i++) {
		mTevSwapModeTable[i].load(i);
	}
}

void J3DIndBlockFull::load(J3DTevBlock*)
{
	loadIndTexStageNum(mIndTexStageNum);
	s32 indTexStageNum = mIndTexStageNum;

	for (u8 i = 0; i < indTexStageNum; i++) {
		mIndTexMtx[i].load(i);
	}

	for (u8 i = 0; i < indTexStageNum; i += 1) {
		mIndTexCoordScale[i].load(i);
	}

	for (u8 i = 0; i < indTexStageNum; i += 1) {
		mIndTexOrder[i].load(i);
	}

}

void J3DPEBlockOpa::load()
{
	GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
	GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_COPY);
	GXSetZMode(1, GX_LEQUAL, 1);
	GXSetZCompLoc(GX_TRUE);
}

void J3DPEBlockTexEdge::load()
{
	GXSetAlphaCompare(GX_GEQUAL, 0x80, GX_AOP_AND, GX_LEQUAL, 0xff);
	GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_COPY);
	GXSetZMode(1, GX_LEQUAL, 1);
	GXSetZCompLoc(GX_FALSE);
}

void J3DPEBlockXlu::load()
{
	GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
	GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_COPY);
	GXSetZMode(1, GX_LEQUAL, 0);
	GXSetZCompLoc(GX_TRUE);
}

void J3DPEBlockFull::load()
{
	if (mFog) {
		mFog->load();
	}
	if (mAlphaComp.mAlphaCmpID != 0xFFFF)
		mAlphaComp.load();

	mBlend.load();

	if (mZMode.mZModeID != 0xFFFF)
		mZMode.load();

	if (mZCompLoc != 0xFF)
		loadZCompLoc(mZCompLoc);

	if (mDither != 0xFF)
		loadDither(mDither);
}

void J3DColorBlockLightOff::reset(J3DColorBlock* block)
{
	mColorChanNum = block->getColorChanNum();

	for (u8 i = 0; i < ARRAY_COUNT(mMatColor); i++)
		mMatColor[i] = *block->getMatColor(i);

	for (u8 i = 0; i < ARRAY_COUNT(mColorChan); i++)
		mColorChan[i] = *block->getColorChan(i);
}

void J3DColorBlockLightOn::reset(J3DColorBlock* block)
{
	mColorChanNum = block->getColorChanNum();

	for (u8 i = 0; i < ARRAY_COUNT(mMatColor); i++)
		mMatColor[i] = *block->getMatColor(i);

	for (u8 i = 0; i < ARRAY_COUNT(mColorChan); i++)
		mColorChan[i] = *block->getColorChan(i);

	for (u8 i = 0; i < ARRAY_COUNT(mAmbColor); i++) {
		if (block->getAmbColor(i))
			mAmbColor[i] = *block->getAmbColor(i);
	}

	for (u8 i = 0; i < ARRAY_COUNT(mLight); i++) {
		if (block->getLight(i) != nullptr) {
			if (!mLight[i])
				mLight[i] = new J3DLightObj;

			memcpy(mLight[i], block->getLight(i), sizeof(*mLight[i]));
			DCStoreRange(mLight[i], sizeof(*mLight[i]));
		}
	}
}

void J3DTexGenBlockBasic::reset(J3DTexGenBlock* block)
{
	mTexGenNum = block->getTexGenNum();

	for (u8 i = 0; i < 8; i++)
		mTexCoord[i] = *block->getTexCoord(i);

	for (u8 i = 0; i < 10; i++) {
		if (block->getTexMtx(i) != nullptr) {
			if (mTexMtx[i] == nullptr)
				mTexMtx[i] = new J3DTexMtx;

			memcpy(mTexMtx[i], block->getTexMtx(i), sizeof(*mTexMtx[i]));
			DCStoreRange(mTexMtx[i], sizeof(*mTexMtx[i]));
		}
	}

	mNBTScale = *block->getNBTScale();
}

void J3DTexGenBlockFull::reset(J3DTexGenBlock* block)
{
	mTexGenNum = block->getTexGenNum();

	for (u8 i = 0; i < 8; i++)
		mTexCoord[i] = *block->getTexCoord(i);

	for (u8 i = 0; i < 10; i++) {
		if (block->getTexMtx(i) != nullptr) {
			if (mTexMtx[i] == nullptr)
				mTexMtx[i] = new J3DTexMtx;

			memcpy(mTexMtx[i], block->getTexMtx(i), sizeof(*mTexMtx[i]));
			DCStoreRange(mTexMtx[i], sizeof(*mTexMtx[i]));
		}
	}
	for (u8 i = 0; i < 8; i++)
		mTexCoord2[i] = *block->getTexCoord2(i);

	for (u8 i = 0; i < 20; i++) {
		if (block->getTexMtx2(i) != nullptr) {
			if (mTexMtx2[i] == nullptr)
				mTexMtx2[i] = new J3DTexMtx;

			memcpy(mTexMtx2[i], block->getTexMtx2(i), sizeof(*mTexMtx2[i]));
			DCStoreRange(mTexMtx2[i], sizeof(*mTexMtx2[i]));
		}
	}

	mNBTScale = *block->getNBTScale();
}

void J3DTevBlock1::reset(J3DTevBlock* block)
{
	mTexNo[0]       = block->getTexNo(0);
	mTevOrder[0]    = *block->getTevOrder(0);
	mTevStageNum = block->getTevStageNum();
	mTevStage[0]    = *block->getTevStage(0);
	mIndTevStage[0] = *block->getIndTevStage(0);
}

void J3DTevBlock2::reset(J3DTevBlock* block)
{
	mTevStageNum     = block->getTevStageNum();
	mTexNo[0]        = block->getTexNo(0);
	mTexNo[1]        = block->getTexNo(1);
	mTevStage[0]     = *block->getTevStage(0);
	mTevStage[1]     = *block->getTevStage(1);
	mIndTevStage[0]  = *block->getIndTevStage(0);
	mIndTevStage[1]  = *block->getIndTevStage(1);
	mTevOrder[0]     = *block->getTevOrder(0);
	mTevOrder[1]     = *block->getTevOrder(1);
	mTevKColorSel[0] = block->getTevKColorSel(0);
	mTevKColorSel[1] = block->getTevKColorSel(1);
	mTevKAlphaSel[0] = block->getTevKAlphaSel(0);
	mTevKAlphaSel[1] = block->getTevKAlphaSel(1);
	for (u8 i = 0; i < 4; ++i)
		mTevColor[i] = *block->getTevColor(i);
	for (u8 i = 0; i < 4; ++i)
		mTevKColor[i] = *block->getTevKColor(i);
	for (u8 i = 0; i < 4; ++i)
		mTevSwapModeTable[i] = *block->getTevSwapModeTable(i);
}

void J3DTevBlock4::reset(J3DTevBlock* block)
{
	mTevStageNum     = block->getTevStageNum();
	mTexNo[0]        = block->getTexNo(0);
	mTexNo[1]        = block->getTexNo(1);
	mTexNo[2]        = block->getTexNo(2);
	mTexNo[3]        = block->getTexNo(3);
	mTevStage[0]     = *block->getTevStage(0);
	mTevStage[1]     = *block->getTevStage(1);
	mTevStage[2]     = *block->getTevStage(2);
	mTevStage[3]     = *block->getTevStage(3);
	mIndTevStage[0]  = *block->getIndTevStage(0);
	mIndTevStage[1]  = *block->getIndTevStage(1);
	mIndTevStage[2]  = *block->getIndTevStage(2);
	mIndTevStage[3]  = *block->getIndTevStage(3);
	mTevOrder[0]     = *block->getTevOrder(0);
	mTevOrder[1]     = *block->getTevOrder(1);
	mTevOrder[2]     = *block->getTevOrder(2);
	mTevOrder[3]     = *block->getTevOrder(3);
	mTevKColorSel[0] = block->getTevKColorSel(0);
	mTevKColorSel[1] = block->getTevKColorSel(1);
	mTevKColorSel[2] = block->getTevKColorSel(2);
	mTevKColorSel[3] = block->getTevKColorSel(3);
	mTevKAlphaSel[0] = block->getTevKAlphaSel(0);
	mTevKAlphaSel[1] = block->getTevKAlphaSel(1);
	mTevKAlphaSel[2] = block->getTevKAlphaSel(2);
	mTevKAlphaSel[3] = block->getTevKAlphaSel(3);
	for (u8 i = 0; i < 4; ++i)
		mTevColor[i] = *block->getTevColor(i);
	for (u8 i = 0; i < 4; ++i)
		mTevKColor[i] = *block->getTevKColor(i);
	for (u8 i = 0; i < 4; ++i)
		mTevSwapModeTable[i] = *block->getTevSwapModeTable(i);
}

void J3DTevBlock16::reset(J3DTevBlock* block)
{
	mTevStageNum = block->getTevStageNum();
	for (u8 i = 0; i < 8; ++i)
		mTexNo[i] = block->getTexNo(i);
	for (u8 i = 0; i < 16; ++i)
		mTevOrder[i] = *block->getTevOrder(i);
	for (u8 i = 0; i < 16; ++i) {
		mTevStage[i]    = *block->getTevStage(i);
		mIndTevStage[i] = *block->getIndTevStage(i);
	}
	for (u8 i = 0; i < 4; ++i)
		mTevColor[i] = *block->getTevColor(i);
	for (u8 i = 0; i < 4; ++i)
		mTevKColor[i] = *block->getTevKColor(i);
	for (u8 i = 0; i < 16; ++i)
		mTevKColorSel[i] = block->getTevKColorSel(i);
	for (u8 i = 0; i < 16; ++i)
		mTevKAlphaSel[i] = block->getTevKAlphaSel(i);
	for (u8 i = 0; i < 4; ++i)
		mTevSwapModeTable[i] = *block->getTevSwapModeTable(i);
}

void J3DIndBlockFull::reset(J3DIndBlock* block)
{
	mIndTexStageNum = block->getIndTexStageNum();
	for (u8 i = 0; i < 4; i++)
		mIndTexOrder[i] = *block->getIndTexOrder(i);
	for (u8 i = 0; i < 3; i++)
		mIndTexMtx[i] = *block->getIndTexMtx(i);
	for (u8 i = 0; i < 4; i++)
		mIndTexCoordScale[i] = *block->getIndTexCoordScale(i);
}

// TODO: figure out the stack sizes here
void J3DPEBlockFull::reset(J3DPEBlock* block)
{
	if (block->getFog() != nullptr) {
		if (!mFog)
			mFog = new J3DFog;

		memcpy(mFog, block->getFog(), sizeof(*mFog));
		DCStoreRange(mFog, sizeof(*mFog));
	}

	switch (block->getType()) {
	case 'PEOP': {
		const J3DAlphaCompInfo alphaCompInfo = { 7, 0, 0, 7, 0, 0, 0, 0 };
		const J3DBlendInfo blendInfo         = { 0, 1, 0, 3 };
		const J3DZModeInfo zModeInfo         = { 1, 3, 1, 0 };
		u32 zCompLoc                         = 1;

		mAlphaComp.setAlphaCompInfo(alphaCompInfo);
		mBlend.setBlendInfo(blendInfo);
		mZMode.setZModeInfo(zModeInfo);
		mZCompLoc = zCompLoc;
		break;
	}
	case 'PEED': {
		const J3DAlphaCompInfo alphaCompInfo = { 6, 0x80, 0, 3, 0xff, 0, 0, 0 };
		const J3DBlendInfo blendInfo         = { 0, 1, 0, 3 };
		const J3DZModeInfo zModeInfo         = { 1, 3, 1, 0 };
		u32 zCompLoc                         = 0;

		mAlphaComp.setAlphaCompInfo(alphaCompInfo);
		mBlend.setBlendInfo(blendInfo);
		mZMode.setZModeInfo(zModeInfo);
		mZCompLoc = zCompLoc;
		break;
	}
	case 'PEXL': {
		const J3DAlphaCompInfo alphaCompInfo = { 7, 0, 0, 7, 0, 0, 0, 0 };
		const J3DBlendInfo blendInfo         = { 1, 4, 5, 3 };
		const J3DZModeInfo zModeInfo         = { 1, 3, 0, 0 };
		u32 zCompLoc                         = 1;

		mAlphaComp.setAlphaCompInfo(alphaCompInfo);
		mBlend.setBlendInfo(blendInfo);
		mZMode.setZModeInfo(zModeInfo);
		mZCompLoc = zCompLoc;
		break;
	}
	case 'PEFL':
		char trash[0x10];
		mAlphaComp = *block->getAlphaComp();
		mBlend     = *block->getBlend();
		mZMode     = *block->getZMode();
		mZCompLoc  = block->getZCompLoc();
		break;
	}
}

void J3DMaterial::makeDisplayList()
{

	//if (!j3dSys.getMatPacket()->isLocked()) {
	j3dSys.getMatPacket()->unk3C = unk14;
	j3dSys.getMatPacket()->beginDL();
	mTevBlock->load();
	mIndBlock->load(mTevBlock);
	mPEBlock->load();
	mColorBlock->load();
	mTexGenBlock->load();
	j3dSys.getMatPacket()->endDL();
	//}
}

// TODO: header
extern void loadNBTScale(J3DNBTScale&);

void J3DMaterial::load()
{
	j3dSys.unk50 = unk8;
	if (!j3dSys.checkFlag2()) {
		j3dSys.getMatPacket()->callDL();
		loadNBTScale(*mTexGenBlock->getNBTScale());
	}
}

// not too much safer than the regular one
void J3DMaterial::safeMakeDisplayList() { makeDisplayList(); }

void J3DTexGenBlockBasic::calc(MtxPtr ptr)
{
	if (!j3dSys.checkFlag4() || !j3dSys.checkFlag8()) {
		for (int i = 0; i < ARRAY_COUNT(mTexMtx); ++i) {
			if (!mTexMtx[i])
				continue;

			u32 mode = mTexMtx[i]->getInfo();
			if (mode == J3DTexMtxMode_EnvmapBasic) {
				Mtx viewMat;
				MTXConcat(j3dSys.getViewMtx(), ptr, viewMat);
				viewMat[0][3] = 0.0f;
				viewMat[1][3] = 0.0f;
				viewMat[2][3] = 0.0f;
				mTexMtx[i]->setViewMtx(viewMat);
			} else if ( mode == J3DTexMtxMode_ProjmapBasic) {
				MTXCopy(ptr, mTexMtx[i]->mViewMtx);
			} else if (mode == J3DTexMtxMode_ViewProjmapBasic) {
				Mtx viewMat;
				MTXConcat(j3dSys.getViewMtx(), ptr, viewMat);
				mTexMtx[i]->setViewMtx(viewMat);
			}
			mTexMtx[i]->calc();
		}
	} else {
		for (int i = 0; i < 10; ++i) {
			if (!mTexMtx[i])
				continue;

			u32 mode = mTexMtx[i]->getInfo();
			if (mode == J3DTexMtxMode_EnvmapBasic) {
				Mtx viewMat;
				J3DPSMtx34Copy(j3dSys.getViewMtx(), viewMat);
				viewMat[0][3] = 0.0f;
				viewMat[1][3] = 0.0f;
				viewMat[2][3] = 0.0f;
				mTexMtx[i]->setViewMtx(viewMat);
			} else if (mode == J3DTexMtxMode_ProjmapBasic) {
				mTexMtx[i]->setViewMtx(j3dDefaultMtx);
			} else if (mode == J3DTexMtxMode_ViewProjmapBasic) {
				mTexMtx[i]->setViewMtx(j3dSys.getViewMtx());
			}
			mTexMtx[i]->calc();
		}
	}
}

void J3DTexGenBlockFull::calc(MtxPtr ptr)
{
	if (!j3dSys.checkFlag4() || !j3dSys.checkFlag8()) {
		for (int i = 0; i < ARRAY_COUNT(mTexMtx); ++i) {
			if (!mTexMtx[i])
				continue;

			u32 mode = mTexMtx[i]->getInfo();
			if (mode == J3DTexMtxMode_EnvmapBasic) {
				Mtx viewMat;
				MTXConcat(j3dSys.getViewMtx(), ptr, viewMat);
				viewMat[0][3] = 0.0f;
				viewMat[1][3] = 0.0f;
				viewMat[2][3] = 0.0f;
				mTexMtx[i]->setViewMtx(viewMat);
			} else if ( mode == J3DTexMtxMode_ProjmapBasic) {
				MTXCopy(ptr, mTexMtx[i]->mViewMtx);
			} else if (mode == J3DTexMtxMode_ViewProjmapBasic) {
				Mtx viewMat;
				MTXConcat(j3dSys.getViewMtx(), ptr, viewMat);
				mTexMtx[i]->setViewMtx(viewMat);
			} else if (mode == J3DTexMtxMode_Unknown5) {
				Mtx viewMat;
				mTexMtx[i]->setViewMtx(ptr);
				mTexMtx[i]->mViewMtx[0][3] = 0.0f;
				mTexMtx[i]->mViewMtx[1][3] = 0;
				mTexMtx[i]->mViewMtx[2][3] = 0;
			}

			mTexMtx[i]->calc();
		}
	} else {
		for (int i = 0; i < 10; ++i) {
			if (!mTexMtx[i])
				continue;

			u32 mode = mTexMtx[i]->getInfo();
			if (mode == J3DTexMtxMode_EnvmapBasic) {
				Mtx viewMat;
				MTXCopy(j3dSys.getViewMtx(), viewMat);
				viewMat[0][3] = 0.0f;
				viewMat[1][3] = 0.0f;
				viewMat[2][3] = 0.0f;
				mTexMtx[i]->setViewMtx(viewMat);
			} else if (mode == J3DTexMtxMode_ProjmapBasic) {
				mTexMtx[i]->setViewMtx(j3dDefaultMtx);
			} else if (mode == J3DTexMtxMode_ViewProjmapBasic) {
				mTexMtx[i]->setViewMtx(j3dSys.getViewMtx());
			}
			mTexMtx[i]->calc();
		}
	}
}

void J3DMaterial::calc(MtxPtr ptr) { mTexGenBlock->calc(ptr); }

/*
// TODO: stack size mismatches...
void J3DMaterial::setCurrentMtx()
{
	char trash[0x20];
	mShape->setUnk3C(mTexGenBlock->getTexCoord(0)->getTexGenMtx(),
	                 mTexGenBlock->getTexCoord(1)->getTexGenMtx(),
	                 mTexGenBlock->getTexCoord(2)->getTexGenMtx(),
	                 mTexGenBlock->getTexCoord(3)->getTexGenMtx(),
	                 mTexGenBlock->getTexCoord(4)->getTexGenMtx(),
	                 mTexGenBlock->getTexCoord(5)->getTexGenMtx(),
	                 mTexGenBlock->getTexCoord(6)->getTexGenMtx(),
	                 mTexGenBlock->getTexCoord(7)->getTexGenMtx());
}*/

void J3DMaterial::copy(J3DMaterial* other)
{
	unk14 = other->unk14;
	unk14 = unk14 & 0x7FFFFFFF;
	mColorBlock->reset(other->mColorBlock);
	mTexGenBlock->reset(other->mTexGenBlock);
	mTevBlock->reset(other->mTevBlock);
	mIndBlock->reset(other->mIndBlock);
	mPEBlock->reset(other->mPEBlock);
}

void J3DMaterial::change()
{

	if (unk14 & 0xc0000000) {
		return;
	}
	unk14 = unk14 | 0x80000000;
	unk8  = mOriginalMaterial->unk8;
	unk10 = mOriginalMaterial->unk10;
	//unk38 = nullptr;
}
