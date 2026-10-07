#include <JSystem/JKernel/JKRSolidHeap.hpp>
#include <JSystem/JUtility/JUTConsole.hpp>
#include <macros.h>

JKRSolidHeap* JKRSolidHeap::create(u32 size, JKRHeap* parent, bool errorFlag)
{
	if (!parent)
		parent = sRootHeap;

	const u32 expHeapSize = ALIGN_NEXT(sizeof(JKRSolidHeap), 0x10);
	u32 alignedSize       = ALIGN_PREV(size, 0x10);

	void* ptr     = JKRHeap::alloc(alignedSize, 0x10, parent);
	void* dataPtr = (char*)ptr + expHeapSize;

	return new (ptr)
	    JKRSolidHeap(dataPtr, alignedSize - expHeapSize, parent, errorFlag);
}

void JKRSolidHeap::destroy() {
    JKRHeap* parent = getParent();
    if (parent) {
        this->~JKRSolidHeap();
        JKRFreeToHeap(parent, this);
    }
}

JKRSolidHeap::JKRSolidHeap(void* data, u32 size, JKRHeap* parent,
                           bool errorFlag)
    : JKRHeap(data, size, parent, errorFlag)
{
	mFreeSize = mSize;
	mCurStart = mStart;
	mCurEnd   = mEnd;
	unk74     = nullptr;
}

JKRSolidHeap::~JKRSolidHeap() { dispose(); }

s32 JKRSolidHeap::adjustSize() {
    JKRHeap* parent = getParent();
    if (parent) {
        lock();
        u32 thisSize = (u32)mStart - (u32)this;
        u32 newSize = ALIGN_NEXT((u8*)mCurStart - (u8*)mStart, 0x20);
        if (parent->resize(this, thisSize + newSize) != -1) {
            mFreeSize = 0;
            mSize = newSize;
            mEnd = (u8*)mStart + mSize;
            mCurStart = mEnd;
            mCurEnd = mEnd;
        }

        unlock();

        return thisSize + newSize;
    }

    return -1;
}

void* JKRSolidHeap::alloc(u32 size, int alignment)
{
	lock();
	if (size < 4)
		size = 4;

	void* ret;

	if (alignment >= 0) {
		ret = allocFromHead(size, alignment < 4 ? 4 : alignment);
	} else {
		ret = allocFromTail(size, alignment > -4 ? 4 : -alignment);
	}

	unlock();
	return ret;
}

void* JKRSolidHeap::allocFromHead(u32 size, int align)
{
	size = ALIGN_NEXT(size, align);

	void* ret = nullptr;

	char* alignedStart = (char*)ALIGN_NEXT((u32)mCurStart, align);
	u32 requiredSize   = (alignedStart - (char*)mCurStart) + size;
	if (requiredSize <= mFreeSize) {
		mCurStart = (char*)mCurStart + requiredSize;
		mFreeSize -= requiredSize;
		ret = alignedStart;
	} else {
		if (mErrorFlag == true && mErrorHandler != nullptr) {
			(*mErrorHandler)(this, size, align);
		}
	}
	return ret;
}

void* JKRSolidHeap::allocFromTail(u32 size, int align)
{
	size = ALIGN_NEXT(size, align);

	void* ret = nullptr;

	char* alignedEnd = (char*)ALIGN_PREV((u32)mCurEnd - size, align);
	u32 requiredSize = (char*)mCurEnd - alignedEnd;
	if (requiredSize <= mFreeSize) {
		mCurEnd = (char*)mCurEnd - requiredSize;
		mFreeSize -= requiredSize;
		ret = alignedEnd;
	} else {
		if (mErrorFlag == true && mErrorHandler != nullptr) {
			(*mErrorHandler)(this, size, align);
		}
	}
	return ret;
}

void JKRSolidHeap::free(void* ptr)
{

}

void JKRSolidHeap::freeAll()
{
	lock();
	JKRHeap::freeAll();
	mFreeSize = mSize;
	mCurStart = mStart;
	mCurEnd   = mEnd;
	unk74     = nullptr;
	unlock();
}

void JKRSolidHeap::freeTail()
{
	lock();
	if (mCurEnd != mEnd)
		dispose(mCurEnd, mEnd);
	mFreeSize += (u8*)mEnd - (u8*)mCurEnd;
	mCurEnd = mEnd;
	// more stuff, unk74 has size 18
	for (UnknownStruct* s = unk74; s != nullptr; s = s->unk10) {
		s->unkC = mEnd;
	}
	unlock();
}

s32 JKRSolidHeap::resize(void* ptr, u32 size)
{
	return -1;
}

s32 JKRSolidHeap::getSize(void* ptr)
{
	return -1;
}

bool JKRSolidHeap::check()
{
	lock();

	u32 checkedFreeSize = mFreeSize + ((u8*)mCurStart - (u8*)mStart)
	                      + ((u8*)mEnd - (u8*)mCurEnd);
	bool valid = true;
	if (checkedFreeSize != mSize) {
		valid = false;
	}

	unlock();
	return valid;
}

bool JKRSolidHeap::dump()
{
	bool ret = check();
	lock();

	unlock();
	return ret;
}
