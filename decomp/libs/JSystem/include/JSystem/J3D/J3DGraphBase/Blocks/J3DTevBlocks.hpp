#ifndef J3D_TEV_BLOCKS_HPP
#define J3D_TEV_BLOCKS_HPP

#include <types.h>
#include <JSystem/J3D/J3DGraphBase/Components/J3DTevOrder.hpp>
#include <JSystem/J3D/J3DGraphBase/Components/J3DGXColorS10.hpp>
#include <JSystem/J3D/J3DGraphBase/Components/J3DGXColor.hpp>
#include <JSystem/J3D/J3DGraphBase/Components/J3DTevStage.hpp>
#include <JSystem/J3D/J3DGraphBase/Components/J3DTevSwapModeTable.hpp>
#include <JSystem/J3D/J3DGraphBase/Components/J3DIndTevStage.hpp>

class J3DTevBlock {
public:
	virtual void reset(J3DTevBlock*) { }
	virtual u32 getType()      { return 'TVP'; }
	virtual s32 countDLSize() { return 0; }

	virtual void setTexNo(u8, const u16*) = 0;
	virtual void setTexNo(u8, u16)        = 0;
	virtual u16 getTexNo(u8) const        = 0;

	virtual void setTevOrder(u8, const J3DTevOrder*) = 0;
	virtual void setTevOrder(u8, J3DTevOrder)        = 0;
	virtual J3DTevOrder* getTevOrder(u8)             = 0;

	virtual void setTevColor(u8, const J3DGXColorS10*) = 0;
	virtual void setTevColor(u8, J3DGXColorS10)        = 0;
	virtual J3DGXColorS10* getTevColor(u8)             = 0;

	virtual void setTevKColor(u8, const J3DGXColor*) = 0;
	virtual void setTevKColor(u8, J3DGXColor)        = 0;
	virtual J3DGXColor* getTevKColor(u8)             = 0;

	virtual void setTevKColorSel(u8, const u8*) = 0;
	virtual void setTevKColorSel(u8, u8)        = 0;
	virtual u8 getTevKColorSel(u8)              = 0;

	virtual void setTevKAlphaSel(u8, const u8*) = 0;
	virtual void setTevKAlphaSel(u8, u8)        = 0;
	virtual u8 getTevKAlphaSel(u8)              = 0;

	virtual void setTevStageNum(const u8*) = 0;
	virtual void setTevStageNum(u8)        = 0;
	virtual u8 getTevStageNum() const      = 0;

	virtual void setTevStage(u8, const J3DTevStage*) = 0;
	virtual void setTevStage(u8, J3DTevStage)        = 0;
	virtual J3DTevStage* getTevStage(u8)             = 0;

	virtual void setTevSwapModeInfo(u8, const J3DTevSwapModeInfo*)   = 0;
	virtual void setTevSwapModeInfo(u8, J3DTevSwapModeInfo)          = 0;
	virtual void setTevSwapModeTable(u8, const J3DTevSwapModeTable*) = 0;
	virtual void setTevSwapModeTable(u8, J3DTevSwapModeTable)        = 0;
	virtual J3DTevSwapModeTable* getTevSwapModeTable(u8)             = 0;

	virtual void setIndTevStage(u8, const J3DIndTevStage*) = 0;
	virtual void setIndTevStage(u8, J3DIndTevStage)        = 0;
	virtual J3DIndTevStage* getIndTevStage(u8)             = 0;

	virtual ~J3DTevBlock() { }
	virtual void load() = 0;
};

class J3DTevBlock1 : public J3DTevBlock {
public:
	J3DTevBlock1() {
		initialize();
		mTevStageNum = 0;
	 }

	void initialize();

	virtual void reset(J3DTevBlock*);
	virtual u32 getType() { return 'TVP1'; }
	virtual s32 countDLSize();

	virtual void setTexNo(u8 i, const u16* no) { mTexNo[i] = *no; }
	virtual void setTexNo(u8 i, u16 no) { mTexNo[i] = no; }
	virtual u16 getTexNo(u8 i) const { return mTexNo[i]; }

	virtual void setTevOrder(u8 i, const J3DTevOrder* order)
	{
		mTevOrder[i] = *order;
	}
	virtual void setTevOrder(u8 i, J3DTevOrder order) { mTevOrder[i] = order; }
	virtual J3DTevOrder* getTevOrder(u8 i) { return &mTevOrder[i]; }

	virtual void setTevColor(u8, const J3DGXColorS10*) { }
	virtual void setTevColor(u8, J3DGXColorS10) { }
	virtual J3DGXColorS10* getTevColor(u8) { return nullptr; }

	virtual void setTevKColor(u8, const J3DGXColor*) { }
	virtual void setTevKColor(u8, J3DGXColor) { }
	virtual J3DGXColor* getTevKColor(u8) { return nullptr; }

	virtual void setTevKColorSel(u8, const u8*) { }
	virtual void setTevKColorSel(u8, u8) { }
	virtual u8 getTevKColorSel(u8) { return 0; }

	virtual void setTevKAlphaSel(u8, const u8*) { }
	virtual void setTevKAlphaSel(u8, u8) { }
	virtual u8 getTevKAlphaSel(u8) { return 0; }

	virtual void setTevStageNum(const u8* num) {
		mTevStageNum = *num;
	}
	virtual void setTevStageNum(u8 num) {
		mTevStageNum =  num;
	}
	virtual u8 getTevStageNum() const { return mTevStageNum; }

	virtual void setTevStage(u8 i, const J3DTevStage* stage)
	{
		mTevStage[i] = *stage;
	}
	virtual void setTevStage(u8 i, J3DTevStage stage) { mTevStage[i] = stage; }
	virtual J3DTevStage* getTevStage(u8 i) { return &mTevStage[i]; }

	virtual void setTevSwapModeInfo(u8, const J3DTevSwapModeInfo*) { }
	virtual void setTevSwapModeInfo(u8, J3DTevSwapModeInfo) { }
	virtual void setTevSwapModeTable(u8, const J3DTevSwapModeTable*) { }
	virtual void setTevSwapModeTable(u8, J3DTevSwapModeTable) { }
	virtual J3DTevSwapModeTable* getTevSwapModeTable(u8) { return nullptr; }

	virtual void setIndTevStage(u8 i, const J3DIndTevStage* stage)
	{
		mIndTevStage[i] = *stage;
	}
	virtual void setIndTevStage(u8 i, J3DIndTevStage stage)
	{
		mIndTevStage[i] = stage;
	}
	virtual J3DIndTevStage* getIndTevStage(u8 i) { return &mIndTevStage[i]; }

	virtual ~J3DTevBlock1() { }
	virtual void load();

private:
	/* 0x04 */ u16 mTexNo[1];
	/* 0x06 */ J3DTevOrder mTevOrder[1];
	/* 0x0A */ u8 mTevStageNum;
	/* 0x0B */ J3DTevStage mTevStage[1];
	/* 0x12 */ J3DIndTevStage mIndTevStage[1];
};

class J3DTevBlock2 : public J3DTevBlock {
public:
	J3DTevBlock2() { initialize(); }

	void initialize();

	virtual void reset(J3DTevBlock*);
	virtual u32 getType() { return 'TV2'; }
	virtual s32 countDLSize();

	virtual void setTexNo(u8 i, const u16* no) { mTexNo[i] = *no; }
	virtual void setTexNo(u8 i, u16 no) { mTexNo[i] = no; }
	virtual u16 getTexNo(u8 i) const { return mTexNo[i]; }

	virtual void setTevOrder(u8 i, const J3DTevOrder* order)
	{
		mTevOrder[i] = *order;
	}
	virtual void setTevOrder(u8 i, J3DTevOrder order) { mTevOrder[i] = order; }
	virtual J3DTevOrder* getTevOrder(u8 i) { return &mTevOrder[i]; }

	virtual void setTevColor(u8 i, const J3DGXColorS10* color)
	{
		mTevColor[i] = *color;
	}
	virtual void setTevColor(u8 i, J3DGXColorS10 color)
	{
		mTevColor[i] = color;
	}
	virtual J3DGXColorS10* getTevColor(u8 i) { return &mTevColor[i]; }

	virtual void setTevKColor(u8 i, const J3DGXColor* color)
	{
		mTevKColor[i] = *color;
	}
	virtual void setTevKColor(u8 i, J3DGXColor color)
	{
		mTevKColor[i] = color;
	}
	virtual J3DGXColor* getTevKColor(u8 i) { return &mTevKColor[i]; }

	virtual void setTevKColorSel(u8 i, const u8* sel)
	{
		mTevKColorSel[i] = *sel;
	}
	virtual void setTevKColorSel(u8 i, u8 sel) { mTevKColorSel[i] = sel; }
	virtual u8 getTevKColorSel(u8 i) { return mTevKColorSel[i]; }

	virtual void setTevKAlphaSel(u8 i, const u8* sel)
	{
		mTevKAlphaSel[i] = *sel;
	}
	virtual void setTevKAlphaSel(u8 i, u8 sel) { mTevKAlphaSel[i] = sel; }
	virtual u8 getTevKAlphaSel(u8 i) { return mTevKAlphaSel[i]; }

	virtual void setTevStageNum(const u8* num) { mTevStageNum = *num; }
	virtual void setTevStageNum(u8 num) { mTevStageNum = num; }
	virtual u8 getTevStageNum() const { return mTevStageNum; }

	virtual void setTevStage(u8 i, const J3DTevStage* stage)
	{
		mTevStage[i] = *stage;
	}
	virtual void setTevStage(u8 i, J3DTevStage stage) { mTevStage[i] = stage; }
	virtual J3DTevStage* getTevStage(u8 i) { return &mTevStage[i]; }

	virtual void setTevSwapModeInfo(u8 i, const J3DTevSwapModeInfo* info)
	{
		mTevStage[i].setTevSwapModeInfo(*info);
	}
	virtual void setTevSwapModeInfo(u8 i, J3DTevSwapModeInfo info)
	{
		mTevStage[i].setTevSwapModeInfo(info);
	}
	virtual void setTevSwapModeTable(u8 i, const J3DTevSwapModeTable* table)
	{
		mTevSwapModeTable[i] = *table;
	}
	virtual void setTevSwapModeTable(u8 i, J3DTevSwapModeTable table)
	{
		mTevSwapModeTable[i] = table;
	}
	virtual J3DTevSwapModeTable* getTevSwapModeTable(u8 i)
	{
		return &mTevSwapModeTable[i];
	}

	virtual void setIndTevStage(u8 i, const J3DIndTevStage* stage)
	{
		mIndTevStage[i] = *stage;
	}
	virtual void setIndTevStage(u8 i, J3DIndTevStage stage)
	{
		mIndTevStage[i] = stage;
	}
	virtual J3DIndTevStage* getIndTevStage(u8 i) { return &mIndTevStage[i]; }

	virtual ~J3DTevBlock2() { }
	virtual void load();

private:
	/* 0x04 */ u16 mTexNo[2];
	/* 0x08 */ J3DTevOrder mTevOrder[2];
	/* 0x10 */ J3DGXColorS10 mTevColor[4];
	/* 0x30 */ u8 mTevStageNum;
	/* 0x31 */ J3DTevStage mTevStage[2];
	/* 0x41 */ J3DGXColor mTevKColor[4];
	/* 0x51 */ u8 mTevKColorSel[2];
	/* 0x53 */ u8 mTevKAlphaSel[2];
	/* 0x55 */ J3DTevSwapModeTable mTevSwapModeTable[4];
	/* 0x59 */ J3DIndTevStage mIndTevStage[2];
};

class J3DTevBlock4 : public J3DTevBlock {
public:
	J3DTevBlock4() { initialize(); }

	void initialize();

	virtual void reset(J3DTevBlock*);
	virtual u32 getType() { return 'TV4'; }
	virtual s32 countDLSize();

	virtual void setTexNo(u8 i, const u16* no) { mTexNo[i] = *no; }
	virtual void setTexNo(u8 i, u16 no) { mTexNo[i] = no; }
	virtual u16 getTexNo(u8 i) const { return mTexNo[i]; }

	virtual void setTevOrder(u8 i, const J3DTevOrder* order)
	{
		mTevOrder[i] = *order;
	}
	virtual void setTevOrder(u8 i, J3DTevOrder order) { mTevOrder[i] = order; }
	virtual J3DTevOrder* getTevOrder(u8 i) { return &mTevOrder[i]; }

	virtual void setTevColor(u8 i, const J3DGXColorS10* color)
	{
		mTevColor[i] = *color;
	}
	virtual void setTevColor(u8 i, J3DGXColorS10 color)
	{
		mTevColor[i] = color;
	}
	virtual J3DGXColorS10* getTevColor(u8 i) { return &mTevColor[i]; }

	virtual void setTevKColor(u8 i, const J3DGXColor* color)
	{
		mTevKColor[i] = *color;
	}
	virtual void setTevKColor(u8 i, J3DGXColor color)
	{
		mTevKColor[i] = color;
	}
	virtual J3DGXColor* getTevKColor(u8 i) { return &mTevKColor[i]; }

	virtual void setTevKColorSel(u8 i, const u8* sel)
	{
		mTevKColorSel[i] = *sel;
	}
	virtual void setTevKColorSel(u8 i, u8 sel) { mTevKColorSel[i] = sel; }
	virtual u8 getTevKColorSel(u8 i) { return mTevKColorSel[i]; }

	virtual void setTevKAlphaSel(u8 i, const u8* sel)
	{
		mTevKAlphaSel[i] = *sel;
	}
	virtual void setTevKAlphaSel(u8 i, u8 sel) { mTevKAlphaSel[i] = sel; }
	virtual u8 getTevKAlphaSel(u8 i) { return mTevKAlphaSel[i]; }

	virtual void setTevStageNum(const u8* num) { mTevStageNum = *num; }
	virtual void setTevStageNum(u8 num) { mTevStageNum = num; }
	virtual u8 getTevStageNum() const { return mTevStageNum; }

	virtual void setTevStage(u8 i, const J3DTevStage* stage)
	{
		mTevStage[i] = *stage;
	}
	virtual void setTevStage(u8 i, J3DTevStage stage) { mTevStage[i] = stage; }
	virtual J3DTevStage* getTevStage(u8 i) { return &mTevStage[i]; }

	virtual void setTevSwapModeInfo(u8 i, const J3DTevSwapModeInfo* info)
	{
		mTevStage[i].setTevSwapModeInfo(*info);
	}
	virtual void setTevSwapModeInfo(u8 i, J3DTevSwapModeInfo info)
	{
		mTevStage[i].setTevSwapModeInfo(info);
	}
	virtual void setTevSwapModeTable(u8 i, const J3DTevSwapModeTable* table)
	{
		mTevSwapModeTable[i] = *table;
	}
	virtual void setTevSwapModeTable(u8 i, J3DTevSwapModeTable table)
	{
		mTevSwapModeTable[i] = table;
	}
	virtual J3DTevSwapModeTable* getTevSwapModeTable(u8 i)
	{
		return &mTevSwapModeTable[i];
	}

	virtual void setIndTevStage(u8 i, const J3DIndTevStage* stage)
	{
		mIndTevStage[i] = *stage;
	}
	virtual void setIndTevStage(u8 i, J3DIndTevStage stage)
	{
		mIndTevStage[i] = stage;
	}
	virtual J3DIndTevStage* getIndTevStage(u8 i) { return &mIndTevStage[i]; }

	virtual ~J3DTevBlock4() { }
	virtual void load();

private:
	/* 0x04 */ u16 mTexNo[4];
	/* 0x0C */ J3DTevOrder mTevOrder[4];
	/* 0x1C */ u8 mTevStageNum;
	/* 0x1D */ J3DTevStage mTevStage[4];
	/* 0x3E */ J3DGXColorS10 mTevColor[4];
	/* 0x5E */ J3DGXColor mTevKColor[4];
	/* 0x6E */ u8 mTevKColorSel[4];
	/* 0x72 */ u8 mTevKAlphaSel[4];
	/* 0x76 */ J3DTevSwapModeTable mTevSwapModeTable[4];
	/* 0x7A */ J3DIndTevStage mIndTevStage[4];
};

class J3DTevBlock16 : public J3DTevBlock {
public:
	J3DTevBlock16() { initialize(); }

	void initialize();

	virtual void reset(J3DTevBlock*);
	virtual u32 getType() { return 'TV16'; }
	virtual s32 countDLSize();

	virtual void setTexNo(u8 i, const u16* no) { mTexNo[i] = *no; }
	virtual void setTexNo(u8 i, u16 no) { mTexNo[i] = no; }
	virtual u16 getTexNo(u8 i) const { return mTexNo[i]; }

	virtual void setTevOrder(u8 i, const J3DTevOrder* order)
	{
		mTevOrder[i] = *order;
	}
	virtual void setTevOrder(u8 i, J3DTevOrder order) {
		mTevOrder[i] = order;
	}
	virtual J3DTevOrder* getTevOrder(u8 i) { return &mTevOrder[i]; }

	virtual void setTevColor(u8 i, const J3DGXColorS10* color)
	{
		mTevColor[i] = *color;
	}
	virtual void setTevColor(u8 i, J3DGXColorS10 color)
	{
		mTevColor[i] = color;
	}
	virtual J3DGXColorS10* getTevColor(u8 i) { return &mTevColor[i]; }

	virtual void setTevKColor(u8 i, const J3DGXColor* color)
	{
		mTevKColor[i] = *color;
	}
	virtual void setTevKColor(u8 i, J3DGXColor color)
	{
		mTevKColor[i] = color;
	}
	virtual J3DGXColor* getTevKColor(u8 i) { return &mTevKColor[i]; }

	virtual void setTevKColorSel(u8 i, const u8* sel)
	{
		mTevKColorSel[i] = *sel;
	}
	virtual void setTevKColorSel(u8 i, u8 sel) { mTevKColorSel[i] = sel; }
	virtual u8 getTevKColorSel(u8 i) { return mTevKColorSel[i]; }

	virtual void setTevKAlphaSel(u8 i, const u8* sel)
	{
		mTevKAlphaSel[i] = *sel;
	}
	virtual void setTevKAlphaSel(u8 i, u8 sel) { mTevKAlphaSel[i] = sel; }
	virtual u8 getTevKAlphaSel(u8 i) { return mTevKAlphaSel[i]; }

	virtual void setTevStageNum(const u8* num) { mTevStageNum = *num; }
	virtual void setTevStageNum(u8 num) { mTevStageNum = num; }
	virtual u8 getTevStageNum() const { return mTevStageNum; }

	virtual void setTevStage(u8 i, const J3DTevStage* stage)
	{
		mTevStage[i] = *stage;
	}
	virtual void setTevStage(u8 i, J3DTevStage stage) {
		mTevStage[i] = stage;
	}
	virtual J3DTevStage* getTevStage(u8 i) { return &mTevStage[i]; }

	virtual void setTevSwapModeInfo(u8 i, const J3DTevSwapModeInfo* info)
	{
		mTevStage[i].setTevSwapModeInfo(*info);
	}
	virtual void setTevSwapModeInfo(u8 i, J3DTevSwapModeInfo info)
	{
		mTevStage[i].setTevSwapModeInfo(info);
	}
	virtual void setTevSwapModeTable(u8 i, const J3DTevSwapModeTable* table)
	{
		mTevSwapModeTable[i] = *table;
	}
	virtual void setTevSwapModeTable(u8 i, J3DTevSwapModeTable table)
	{
		mTevSwapModeTable[i] = table;
	}
	virtual J3DTevSwapModeTable* getTevSwapModeTable(u8 i)
	{
		return &mTevSwapModeTable[i];
	}

	virtual void setIndTevStage(u8 i, const J3DIndTevStage* stage)
	{
		mIndTevStage[i] = *stage;
	}
	virtual void setIndTevStage(u8 i, J3DIndTevStage stage)
	{
		mIndTevStage[i] = stage;
	}
	virtual J3DIndTevStage* getIndTevStage(u8 i) { return &mIndTevStage[i]; }

	virtual ~J3DTevBlock16() { }
	virtual void load();

private:
	/* 0x004 */ u16 mTexNo[8];
	/* 0x014 */ J3DTevOrder mTevOrder[16];
	/* 0x054 */ u8 mTevStageNum;
	/* 0x055 */ J3DTevStage mTevStage[16];
	/* 0x0D6 */ J3DGXColorS10 mTevColor[4];
	/* 0x0F6 */ J3DGXColor mTevKColor[4];
	/* 0x106 */ u8 mTevKColorSel[16];
	/* 0x116 */ u8 mTevKAlphaSel[16];
	/* 0x126 */ J3DTevSwapModeTable mTevSwapModeTable[4];
	/* 0x12A */ J3DIndTevStage mIndTevStage[16];
};

#endif
