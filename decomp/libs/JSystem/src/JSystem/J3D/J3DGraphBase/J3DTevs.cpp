#include "JSystem/J3D/J3DGraphBase/Components/J3DAlphaComp.hpp"
#include "JSystem/J3D/J3DGraphBase/Components/J3DBlend.hpp"
#include "JSystem/J3D/J3DGraphBase/Components/J3DFog.hpp"
#include "JSystem/J3D/J3DGraphBase/Components/J3DIndTexMtx.hpp"
#include "JSystem/J3D/J3DGraphBase/Components/J3DTevStage.hpp"
#include "JSystem/J3D/J3DGraphBase/Components/J3DZMode.hpp"
#include "dolphin/gx/GXBump.h"
#include "dolphin/gx/GXEnum.h"
#include "dolphin/gx/GXTev.h"
#include <JSystem/J3D/J3DGraphBase/J3DTevs.hpp>
#include <JSystem/J3D/J3DGraphBase/Components/J3DLightObj.hpp>
#include <JSystem/J3D/J3DGraphBase/Components/J3DTexMtx.hpp>
#include <JSystem/J3D/J3DGraphBase/Components/J3DNBTScale.hpp>
#include <JSystem/J3D/J3DGraphBase/Components/J3DTevOrder.hpp>
#include <JSystem/J3D/J3DGraphBase/Components/J3DIndTexOrder.hpp>
#include <JSystem/J3D/J3DGraphBase/Components/J3DIndTexCoordScale.hpp>
#include <JSystem/J3D/J3DGraphBase/Components/J3DIndTevStage.hpp>
#include <JSystem/J3D/J3DGraphBase/Components/J3DColorChan.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DTransform.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DSys.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DTexture.hpp>
#include <JSystem/JRenderer.hpp>
#include <JSystem/ResTIMG.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DLoad.hpp>
#include <dolphin/gd.h>
#include <types.h>
#include <macros.h>

u8 GXTexMode0Ids[8]  = { 0x80, 0x81, 0x82, 0x83, 0xA0, 0xA1, 0xA2, 0xA3 };
u8 GXTexMode1Ids[8]  = { 0x84, 0x85, 0x86, 0x87, 0xA4, 0xA5, 0xA6, 0xA7 };
u8 GXTexImage0Ids[8] = { 0x88, 0x89, 0x8A, 0x8B, 0xA8, 0xA9, 0xAA, 0xAB };
u8 GXTexImage1Ids[8] = { 0x8C, 0x8D, 0x8E, 0x8F, 0xAC, 0xAD, 0xAE, 0xAF };
u8 GXTexImage2Ids[8] = { 0x90, 0x91, 0x92, 0x93, 0xB0, 0xB1, 0xB2, 0xB3 };
u8 GXTexImage3Ids[8] = { 0x94, 0x95, 0x96, 0x97, 0xB4, 0xB5, 0xB6, 0xB7 };
u8 GXTexTlutIds[8]   = { 0x98, 0x99, 0x9A, 0x9B, 0xB8, 0xB9, 0xBA, 0xBB };

static u8 GX2HWFiltConv[6] = { 0x00, 0x04, 0x01, 0x05, 0x02, 0x06 };

const GXColor j3dDefaultColInfo = { 0xFF, 0xFF, 0xFF, 0xFF };
const GXColor j3dDefaultAmbInfo = { 0x32, 0x32, 0x32, 0x32 };
const u8 j3dDefaultColorChanNum = 0x01;
const J3DTevOrderInfo j3dDefaultTevOrderInfoNull
    = { GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR_NULL };
const J3DIndTexOrderInfo j3dDefaultIndTexOrderNull
    = { GX_TEXCOORD_NULL, GX_TEXMAP_NULL };
const GXColorS10 j3dDefaultTevColor = { 0xFF, 0xFF, 0xFF, 0xFF };
const J3DIndTexCoordScaleInfo j3dDefaultIndTexCoordScaleInfo = {};
const GXColor j3dDefaultTevKColor              = { 0xFF, 0xFF, 0xFF, 0xFF };
const J3DTevSwapModeInfo j3dDefaultTevSwapMode = {};
const J3DTevSwapModeTableInfo j3dDefaultTevSwapModeTable = { 0, 1, 2, 3 };
const J3DBlendInfo j3dDefaultBlendInfo
    = { GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_NOOP };
const J3DColorChanInfo j3dDefaultColorChanInfo
    = { 0, 0, 0, 2, 2, 0, 0xFF, 0xFF };
const u8 j3dDefaultTevSwapTableID = 0x1B;
const u16 j3dDefaultAlphaCmpID    = 0xE7;
const u16 j3dDefaultZModeID       = 0x17;

void loadCullMode(u8 mode) {
    GXSetCullMode((GXCullMode)mode);
 }

void loadChanMatColor(u8 chan, const GXColor& color) {
    GXSetChanMatColor((GXChannelID)(GX_COLOR0A0 + chan), color);
}
void loadChanAmbColor(u8 chan, const GXColor& color) {
    GXSetChanAmbColor((GXChannelID)(GX_COLOR0A0 + chan), color);
}

void loadColorChanNum(u8 chan) {
    GXSetNumChans(chan);
}

void J3DColorChan::load(u8 id)
{
	const GXChannelID chanTbl[]
		= { GX_COLOR0, GX_ALPHA0, GX_COLOR1, GX_ALPHA1 };

	GXSetChanCtrl(chanTbl[id], getEnable(), getAmbSrc(), getMatSrc(),
						getLightMask(), getDiffuseFn(), getAttnFn());
}

const J3DLightInfo j3dDefaultLightInfo = {
	0.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0xff, 0xff,
	0xff, 0xff, 1.0f, 0.0f, 0.0f,  1.0f, 0.0f, 0.0f,
};

const J3DTexCoordInfo j3dDefaultTexCoordInfo[8] = {
	{ GX_MTX2x4, GX_TG_TEX0, GX_IDENTITY },
	{ GX_MTX2x4, GX_TG_TEX1, GX_IDENTITY },
	{ GX_MTX2x4, GX_TG_TEX2, GX_IDENTITY },
	{ GX_MTX2x4, GX_TG_TEX3, GX_IDENTITY },
	{ GX_MTX2x4, GX_TG_TEX4, GX_IDENTITY },
	{ GX_MTX2x4, GX_TG_TEX5, GX_IDENTITY },
	{ GX_MTX2x4, GX_TG_TEX6, GX_IDENTITY },
	{ GX_MTX2x4, GX_TG_TEX7, GX_IDENTITY },
};

const J3DTexMtxInfo j3dDefaultTexMtxInfo = {
	0x01, 0x00,  0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0,
	0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
	0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
};

const J3DIndTexMtxInfo j3dDefaultIndTexMtxInfo = {
	0.5f, 0.0f, 0.0f, 0.0f, 0.5f, 0.0f, 1,
};

const J3DTevStageInfo j3dDefaultTevStageInfo = {
	0x04, 0x0A, 0x0F, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
	0x05, 0x07, 0x07, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
};

const J3DIndTevStageInfo j3dDefaultIndTevStageInfo = {};

const J3DFogInfo j3dDefaultFogInfo = {
	0, 0, 0x140, 0.0f, 0.0f, 0.0f, 0.0f, 0x00, 0x00, 0x00, 0x00,
};

const J3DNBTScaleInfo j3dDefaultNBTScaleInfo = { 0, 1.0f, 1.0f, 1.0f };

void J3DLightObj::load(u8 id) const
{
    GXInitLightPos(&mLightObj, mLightPosition.x, mLightPosition.y, mLightPosition.z);
    GXInitLightAttn(&mLightObj, mCosAtten.x, mCosAtten.y, mCosAtten.z, mDistAtten.x,
	               mDistAtten.y, mDistAtten.z);
    GXInitLightColor(&mLightObj, mColor);
    GXInitLightDir(&mLightObj, mLightDirection.x, mLightDirection.y,
	          mLightDirection.z);
    GXLoadLightObjImm(&mLightObj, (GXLightID)(1 << id));
}

void loadTexGenNum(u8 id) {
    GXSetNumTexGens(id);
}

void J3DTexCoord::load(u8 id) {
    GXSetTexCoordGen2((GXTexCoordID)id,
    getTexGenType(), getTexGenSrc(), getTexGenMtx(), GX_FALSE, 125);
}

void J3DTexCoord2::load(u8 id) {
	if (unk4 != 0xFFFF) {
		GXSetTexCoordGen2((GXTexCoordID)id,
		getTexGenType(), getTexGenSrc(), getTexGenMtx(), getUnk4Something(), getUnk4Something2());
	}
}

void J3DTexMtx::calc()
{
	Mtx44 mtx2;
	Mtx44 mtx1;

	// clang-format off
	Mtx fixupMtx1 = {
    0.5,  0.0, 0.5, 0.0,
    0.0, -0.5, 0.5, 0.0,
    0.0,  0.0, 1.0, 0.0,
  };
	Mtx fixupMtx2 = {
    0.5,  0.0, 0.0, 0.5,
    0.0, -0.5, 0.0, 0.5,
    0.0,  0.0, 1.0, 0.0,
  };
	// clang-format on

	u32 useMayaFormat = (mInfo >> 7) & 1;
	u32 format        = mInfo & 0x7F;
	if (format == J3DTexMtxMode_Projmap || format == J3DTexMtxMode_ViewProjmap
	    || format == J3DTexMtxMode_EnvmapEffectMtx) {
		J3DGetTextureMtx(mSRT, mCenter, mtx1);
		MTXConcat(mtx1, fixupMtx1, mtx1);
	} else if (format == J3DTexMtxMode_Envmap) {
		J3DGetTextureMtx(mSRT, mCenter, mtx2);

		MTXConcat(mtx2, fixupMtx1, mtx2);
		MTXConcat(mtx2, mViewMtx, mTotalMtx);
	} else if (format == J3DTexMtxMode_EnvmapOldEffectMtx) {
			J3DGetTextureMtxOld(mSRT, mCenter, mtx1);
	} else if (format == J3DTexMtxMode_EnvmapOld) {
			J3DGetTextureMtxOld(mSRT, mCenter, mtx2);
	} else if (format == J3DTexMtxMode_EnvmapBasic) {
		J3DGetTextureMtxOld(mSRT, mCenter, mtx2);
		MTXConcat(mtx2, mViewMtx, mTotalMtx);
	} else if (format == J3DTexMtxMode_ProjmapBasic
	           || format == J3DTexMtxMode_ViewProjmapBasic
	           || format == J3DTexMtxMode_Unknown5) {
		J3DGetTextureMtxOld(mSRT, mCenter, mtx1);
		J3DMtxProjConcat(mtx1, mEffectMtx, mtx2);
		MTXConcat(mtx2, mViewMtx, mTotalMtx);
	} else if (format == J3DTexMtxMode_Unknown4) {
		J3DGetTextureMtxOld(mSRT, mCenter, mtx1);
		J3DMtxProjConcat(mtx1, mEffectMtx, mTotalMtx);
	} else {
		J3DGetTextureMtxOld(mSRT, mCenter, mTotalMtx);
	}
}

void J3DTexMtx::load(u8 id) const
{
    GXLoadTexMtxImm((MtxPtr)&mTotalMtx,  id * 3 + 30, (GXTexMtxType)mProjection);
}

void J3DTexMtx::load2(u8 id) const
{
	GXLoadTexMtxImm((MtxPtr)&mTotalMtx,  id * 3 + 64, (GXTexMtxType)mProjection);
}

void loadTexNo(u8 id, const u16& param_2)
{
	ResTIMG* img = &j3dSys.getTexture()->mResources[param_2];
    GXTexObj texObj;
    GXTlutObj tlutObj;

    if (img->isIndexTexture == 0) {
        GXInitTexObj(&texObj, (void*)((u8*)img + img->imageDataOffset), img->width, img->height,
        (GXTexFmt)img->format, (GXTexWrapMode)img->wrapS, (GXTexWrapMode)img->wrapT, img->mipmapEnabled);
    } else {
        GXInitTexObjCI(&texObj, (void*)((u8*)img + img->imageDataOffset), img->width, img->height,
        (GXCITexFmt )img->format, (GXTexWrapMode)img->wrapS, (GXTexWrapMode)img->wrapT, img->mipmapEnabled, id);
    }

    GXInitTexObjLOD(&texObj, (GXTexFilter)img->minFilter, (GXTexFilter)img->magFilter, img->minLod * 0.125f, img->maxLod * 0.125f, img->lodBias * 0.01f,
        img->biasClamp, img->doEdgeLod, (GXAnisotropy)img->maxAnisotropy);

    if (img->isIndexTexture == 1) {
        GXInitTlutObj(&tlutObj, (void*)((u8*)img + img->paletteOffset), (GXTlutFmt)img->colorFormat, img->numColors);
        GXLoadTlut(&tlutObj, id);
    }

    GXLoadTexObj(&texObj, (GXTexMapID)id);
}

void J3DTevOrder::load(u8 id) const {
	GXSetTevOrder((GXTevStageID)id, (GXTexCoordID)mTexCoord, (GXTexMapID)mTexMap, (GXChannelID)mColorChan);
}

void J3DIndTexOrder::load(u8 id) const {
	GXSetIndTexOrder((GXIndTexStageID)id, (GXTexCoordID)mCoord,  (GXTexMapID)mMap);
}

void J3DIndTexCoordScale::load(u8 id) const {
	GXSetIndTexCoordScale((GXIndTexStageID)id, (GXIndTexScale)mScaleS, (GXIndTexScale)mScaleT);
}

void J3DIndTexMtx::load(u8 id) {
	GXSetIndTexMtx((GXIndTexMtxID)(id + 1), mOffsetMtx, mScaleExp);
}

void J3DIndTevStage::load(u8 id)
{
	GXSetTevIndirect((GXTevStageID)id, (GXIndTexStageID)mIndStage,
						(GXIndTexFormat)mIndFormat, (GXIndTexBiasSel)mBiasSel,
						(GXIndTexMtxID)mMtxSel, (GXIndTexWrap)mWrapS,
						(GXIndTexWrap)mWrapT, mPrev, mLod,
						(GXIndTexAlphaSel)mAlphaSel);
}

void loadTevColor(u8 id, const J3DGXColorS10& color) {
	GXSetTevColorS10((GXTevRegID)(id + 1), color.color);
}

void loadTevKColor(u8 id, const J3DGXColor& color) {
	GXSetTevKColor((GXTevKColorID)id, color.color);
}

void loadTevKColorSel(u8 id, u8 sel) {
	GXSetTevKColorSel((GXTevStageID)id, (GXTevKColorSel)sel);
}

void loadTevKAlphaSel(u8 id, u8 sel) {
	GXSetTevKAlphaSel((GXTevStageID)id, (GXTevKAlphaSel)sel);
}

void J3DTevSwapModeTable::load(u8 id) {

	GXSetTevSwapModeTable((GXTevSwapSel)id, (GXTevColorChan)getR(),
	(GXTevColorChan)getG(), (GXTevColorChan)getB(), (GXTevColorChan)getA());
}

void loadTevStageNum(u8 num) {
	GXSetNumTevStages(num);
}

void loadIndTexStageNum(u8 num) {
	GXSetNumIndStages(num);
}

void J3DTevStage::load(u8 stage) const {
	GXSetTevColorIn((GXTevStageID)stage, (GXTevColorArg)getTevColorA(),
		(GXTevColorArg)getTevColorB(), (GXTevColorArg)getTevColorC(), (GXTevColorArg)getTevColorD());

	GXSetTevColorOp((GXTevStageID)stage, (GXTevOp)getColorOpSomething1(),
		(GXTevBias)getColorOpSomething2(), (GXTevScale)getColorOpSomething3(),
		(GXTevRegID)getColorOpSomething4(), (GXTevRegID)getColorOpSomething5());

	GXSetTevAlphaIn((GXTevStageID)stage, (GXTevAlphaArg)getAlphaA(),
	(GXTevAlphaArg)getAlphaB(), (GXTevAlphaArg)getAlphaC(),
	(GXTevAlphaArg)getAlphaD());

	GXSetTevAlphaOp((GXTevStageID)stage, (GXTevOp)getAlphaOpSomething1(),
		(GXTevBias)getAlphaOpSomething2(), (GXTevScale)getAlphaOpSomething3(),
		(GXTevRegID)getAlphaOpSomething4(), (GXTevRegID)getAlphaOpSomething5());

	GXSetTevSwapMode((GXTevStageID)stage, (GXTevSwapSel)getTevSwapMode1(), (GXTevSwapSel)getTevSwapMode2());
}

void J3DFog::load() const {
	GXSetFog(GXFogType(mType), mStartZ, mEndZ, mNearZ, mFarZ, mColor);
	GXSetFogRangeAdj(mAdjEnable, mCenter, (GXFogAdjTable*)mFogAdjTable);
}

void J3DAlphaComp::load() {
	GXSetAlphaCompare(getComp0(), getRef0(), getOp(), getComp1(),
						getRef1());
}

void J3DBlend::load() {
	GXSetBlendMode((GXBlendMode)mBlendMode, (GXBlendFactor)mSrcFactor,
					(GXBlendFactor)mDstFactor, (GXLogicOp)mLogicOp);
}

void J3DZMode::load() const {
	GXSetZMode(getCompareEnable(), (GXCompare)getFunc(),
				getUpdateEnable());
}

void loadZCompLoc(u8 compLoc) {
	GXSetZCompLoc(compLoc);
}

void loadDither(u8 dither) {
    GXSetDither(dither);
}

void loadNBTScale(J3DNBTScale& param_0)
{
	if (param_0.mbHasScale == true) {
		j3dSys.setNBTScale(&param_0.mScale);
	} else {
		j3dSys.setNBTScale(nullptr);
	}
}

u8 j3dTexCoordTable[7624];
u8 j3dTevSwapTableTable[1024];
u8 j3dAlphaCmpTable[768];
u8 j3dZModeTable[96];

void makeTexCoordTable()
{
	u8 texMtx[] = {
		GX_TEXMTX0, GX_TEXMTX1, GX_TEXMTX2, GX_TEXMTX3, GX_TEXMTX4,  GX_TEXMTX5,
		GX_TEXMTX6, GX_TEXMTX7, GX_TEXMTX8, GX_TEXMTX9, GX_IDENTITY,
	};

	u8* table = j3dTexCoordTable;
	for (u32 i = 0; i < 11; ++i) {
		for (u32 j = 0; j < 21; ++j) {
			for (int k = 0; k < ARRAY_COUNT(texMtx); ++k) {
				u32 idx = j * 11 + i * 0xe7 + k;

				table[idx * 3 + 0] = i;
				table[idx * 3 + 1] = j;
				table[idx * 3 + 2] = texMtx[k];
			}
		}
	}
}

void makeAlphaCmpTable()
{
	u8* table = j3dAlphaCmpTable;
	for (u32 i = 0; i != 8; ++i) {
		for (int j = 0; j != 4; ++j) {
			for (u32 k = 0; k != 8; ++k) {
				u32 idx = i * 32 + j * 8 + k;

				table[idx * 3 + 0] = i;
				table[idx * 3 + 1] = j;
				table[idx * 3 + 2] = k;
			}
		}
	}
}

void makeZModeTable()
{
	u8* table = j3dZModeTable;
	for (int i = 0; i < 2; ++i) {
		for (u32 j = 0; j < 8; ++j) {
			for (int k = 0; k < 2; ++k) {
				u32 idx = j * 2 + i * 16 + k;

				table[idx * 3 + 0] = i;
				table[idx * 3 + 1] = j;
				table[idx * 3 + 2] = k;
			}
		}
	}
}

void makeTevSwapTable()
{
	u8* table = j3dTevSwapTableTable;
	for (int i = 0; i < 4; ++i) {
		for (int j = 0; j < 4; ++j) {
			for (int k = 0; k < 4; ++k) {
				for (int l = 0; l < 4; ++l) {
					u32 idx = i * 64 + j * 16 + k * 4 + l;

					table[idx * 4 + 0] = i;
					table[idx * 4 + 1] = j;
					table[idx * 4 + 2] = k;
					table[idx * 4 + 3] = l;
				}
			}
		}
	}
}
