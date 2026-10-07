#pragma once
// JMP parameter tables ("JMap" data: roominfo, objinfo, characterinfo, ...).
// Retail: Koga/ToolData (based on lm-decomp's version of the TU).
//
// Layout (big-endian):
//   s32 numEntries, s32 numFields, s32 dataOffset, u32 entrySize
//   JMapItem fields[numFields] { u32 nameHash; u32 mask; u16 offset; u8 shift; u8 type; }
//   entries at dataOffset, entrySize bytes each
// The table is read in place on PC; values are byte-swapped on access.
#include <dolphin/types.h>

namespace lm {

class ToolData {
public:
    enum ValueType : u8 {
        TYPE_LONG = 0,
        TYPE_STRING = 1,  // inline 32-byte string
        TYPE_FLOAT = 2,
        TYPE_LONG_2 = 3,
        TYPE_SHORT = 4,
        TYPE_BYTE = 5,
        TYPE_STRING_PTR = 6,
        TYPE_NULL = 7,
    };

    bool attach(const void* data);
    void reset() { mData = nullptr; }
    bool valid() const { return mData != nullptr; }

    s32 entryCount() const;
    s32 fieldCount() const;
    s32 searchItemInfo(const char* name) const;  // field index, or -1

    bool getValue(int entry, const char* name, u32* out) const;
    bool getValue(int entry, const char* name, s32* out) const;
    bool getValue(int entry, const char* name, u8* out) const;  // boolean (masked) value
    bool getValue(int entry, const char* name, f32* out) const;
    bool getValue(int entry, const char* name, const char** out) const;

    s32 findEntryByValue(int field, u32 value, int startEntry) const;
    s32 findEntryByValue(int field, const char* value, int startEntry) const;

    // Raw field access by index.
    u32 unsignedValue(int entry, int field) const;
    s32 signedValue(int entry, int field) const;
    f32 floatValue(int entry, int field) const;
    const char* stringValue(int entry, int field) const;
    bool boolValue(int entry, int field) const;

    static u32 hashName(const char* name);

private:
    const u8* fieldData(int entry, int field) const;
    u32 fieldMask(int field) const;
    u8 fieldShift(int field) const;

    const u8* mData = nullptr;
};

// A table entry reference (retail "ToolDataRef").
struct ToolDataRef {
    ToolData* data = nullptr;
    s32 entry = -1;
    bool valid() const { return data != nullptr && entry >= 0; }
};

}  // namespace lm
