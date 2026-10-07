#include "dolphin/gx/GXDispList.h"
#include <JSystem/J3D/J3DGraphBase/J3DPacket.hpp>
#include <JSystem/JKernel/JKRHeap.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DSys.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DMaterial.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DShape.hpp>
#include <dolphin/os.h>
#include <dolphin/gd.h>
#include <dolphin/gx.h>

int J3DDrawPacket::sInterruptFlag;

void J3DDrawPacket::swapBuffer()
{
	void* tmp = unk10;
	unk10      = unk14;
	unk14      = tmp;
}


bool J3DPacket::isSame(J3DMatPacket*) const { return false; }

bool J3DPacket::entry(J3DDrawBuffer*) { return true; }

void J3DPacket::addChildPacket(J3DPacket* packet)
{
	if (unk8 == nullptr) {
		unk8 = packet;
		return;
	}
	packet->unk4 = unk8;
	unk8         = packet;
}

void J3DCallBackPacket::draw()
{
	if (unk10 != nullptr) {
		unk10(this, 0);
	}
	for (J3DPacket* packet = unk8; packet != nullptr; packet = packet->unk4) {
		packet->draw();
	}
	if (unk10 != nullptr) {
		unk10(this, 1);
	}
}

J3DDrawPacket::J3DDrawPacket()
{
	//unk10 = 0;
	//unk30 = nullptr;
}

J3DDrawPacket::~J3DDrawPacket() { }

void J3DDrawPacket::draw() { callDL(); }

void J3DDrawPacket::callDL() { GXCallDisplayList(unk10, unk18); }

void J3DDrawPacket::newDisplayList(u32 param_1)
{
	unk1C = param_1 + 0x1f & 0xffffffe0;
	unk10 = new (0x20) char[unk1C];
	unk14 = new (0x20) char[unk1C];
	unk18 = 0;
}


void J3DDrawPacket::beginDL()
{
	swapBuffer();
	sInterruptFlag = OSDisableInterrupts();
    GXBeginDisplayList(unk10, unk1C);
}

u32 J3DDrawPacket::endDL()
{
    unk18 = GXEndDisplayList();
	OSRestoreInterrupts(sInterruptFlag);
	DCFlushRange(unk10, unk18);
	return unk18;
}

J3DMatPacket::J3DMatPacket()
{
	unk38    = 0;
	unk3C    = -1;
	mTexture = 0;
	unk44    = 0;
}

J3DMatPacket::~J3DMatPacket() { }

void J3DMatPacket::addShapePacket(J3DShapePacket* packet)
{
	if (unk34 == nullptr) {
		unk34 = packet;
		return;
	}
	packet->unk4 = unk34;
	unk34        = packet;
}

inline bool checkThing(J3DShapePacket* p)
{
	bool ret = true;

	for (J3DShapePacket* i = p; i != nullptr; i = (J3DShapePacket*)i->unk4) {
		if (i->unk30 != 0) {
			ret = false;
			break;
		}
	}

	return ret;
}

void J3DMatPacket::draw()
{
	char trash[0x20];
	if (!checkThing(unk34)) {
		j3dSys.mTexture   = mTexture;
		j3dSys.mMatPacket = this;
		unk38->load();
		for (J3DPacket* j = unk34; j != nullptr; j = j->unk4) {
			j->draw();
		}
	}
}

J3DShapePacket::J3DShapePacket()
{
	unk14 = 0;
	unk18 = 0;
	unk1C = 0;
	unk20 = 0;
}

J3DShapePacket::~J3DShapePacket() { }

void J3DShapePacket::draw()
{

	if (unk14 != 0) {
		j3dSys.unk10C         = unk24;
		j3dSys.unk110         = unk28;
		unk14->mDrawMatrices  = unk18;
		unk14->mNormMatrices  = unk1C;
		unk14->draw();
	}
}
