#include "Koga/ToolData.hpp"

namespace Koga {

    ToolData::ToolData() {
        mData = nullptr;
    }

    ToolData::~ToolData() {
    }

    BOOL ToolData::attach(const JMapData* data) {
        if (data == nullptr) {
            return FALSE;
        }
        mData = data;
        return TRUE;
    }

    void ToolData::reset() {
        mData = nullptr;
    }

    bool ToolData::getValue(int entryIndex, const char* key, u32* valueOut) const {
        s32 itemIndex = searchItemInfo(key);
        if (itemIndex < 0) {
            return FALSE;
        }

        *valueOut = getUnsignedValue< u32 >(entryIndex, itemIndex);
        return TRUE;
    }

    bool ToolData::getValue(int entryIndex, const char* key, s32* valueOut) const {
        s32 itemIndex = searchItemInfo(key);
        if (itemIndex < 0) {
            return FALSE;
        }

        *valueOut = getSignedValue< s32 >(entryIndex, itemIndex);
        return TRUE;
    }

    bool ToolData::getValue(int entryIndex, const char* key, u8* valueOut) const {
        s32 itemIndex = searchItemInfo(key);
        if (itemIndex < 0) {
            return FALSE;
        }

        *valueOut = getBoolValue(entryIndex, itemIndex);
        return TRUE;
    }

    bool ToolData::getValue(int entryIndex, const char* key, const char** valueOut) const {
        s32 itemIndex = searchItemInfo(key);
        if (itemIndex < 0) {
            return FALSE;
        }

        *valueOut = getStringValue(entryIndex, itemIndex);
        return TRUE;
    }

    bool ToolData::getValue(int entryIndex, const char* key, f32* valueOut) const {
        s32 itemIndex = searchItemInfo(key);
        if (itemIndex < 0) {
            return FALSE;
        }

        *valueOut = getFloatValue(entryIndex, itemIndex);
        return TRUE;
    }

    s32 ToolData::searchItemInfo(const char* key) const {
        s32 nFields = mData->mNumFields;
        u32 hash = hashString(key);

        for (int i = 0; i < nFields; ++i) {
            if (hash == mData->mItems[i].mHash) {
                return i;
            }
        }

        return -1;
    }

    s32 ToolData::findEntryByValue(int fieldIndex, const u32 expectedValue, int entryIndex) {
        int currIndex = entryIndex;
        int maxIndex = mData->mNumEntries;

        while (currIndex < maxIndex) {
            u32 currVal = getExpectedUnsignedValue(currIndex, fieldIndex);

            if (currVal == expectedValue) {
                return currIndex;
            }

            currIndex++;
        }

        return -1;
    }

    s32 ToolData::findEntryByValue(int fieldIndex, const char* cmpStr, int entryIndex) {
        while (entryIndex < mData->mNumEntries) {
            const char* currStr = getStringValue(entryIndex, fieldIndex);
            if (strcmp(currStr, cmpStr) == 0) {
                return entryIndex;
            }
            entryIndex++;
        }

        return -1;
    }

}  // namespace Koga
