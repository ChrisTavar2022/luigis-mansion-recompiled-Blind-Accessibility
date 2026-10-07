// Port of Koga/ToolData. Based on lm-decomp's version of this TU.
#include "game/data/tool_data.h"

#include <bit>
#include <cstring>

namespace lm {
namespace {

u32 rd32(const u8* p) { return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | p[3]; }
u16 rd16(const u8* p) { return u16((p[0] << 8) | p[1]); }

constexpr u32 kFieldSize = 0xC;

}  // namespace

u32 ToolData::hashName(const char* name) {
    u32 hash = 0;
    for (; *name != 0; name++) {
        hash = (u32(u8(*name)) + (hash << 8)) % 0x1FFFFD9;
    }
    return hash;
}

bool ToolData::attach(const void* data) {
    if (data == nullptr) {
        return false;
    }
    mData = static_cast<const u8*>(data);
    return true;
}

s32 ToolData::entryCount() const { return s32(rd32(mData + 0x0)); }
s32 ToolData::fieldCount() const { return s32(rd32(mData + 0x4)); }

const u8* ToolData::fieldData(int entry, int field) const {
    const u8* item = mData + 0x10 + field * kFieldSize;
    return mData + rd32(mData + 0x8) + entry * rd32(mData + 0xC) + rd16(item + 0x8);
}

u32 ToolData::fieldMask(int field) const { return rd32(mData + 0x10 + field * kFieldSize + 0x4); }
u8 ToolData::fieldShift(int field) const { return mData[0x10 + field * kFieldSize + 0xA]; }

s32 ToolData::searchItemInfo(const char* name) const {
    const u32 hash = hashName(name);
    const s32 n = fieldCount();
    for (s32 i = 0; i < n; i++) {
        if (rd32(mData + 0x10 + i * kFieldSize) == hash) {
            return i;
        }
    }
    return -1;
}

u32 ToolData::unsignedValue(int entry, int field) const {
    return (rd32(fieldData(entry, field)) & fieldMask(field)) >> fieldShift(field);
}

s32 ToolData::signedValue(int entry, int field) const { return s32(rd32(fieldData(entry, field))); }

f32 ToolData::floatValue(int entry, int field) const { return std::bit_cast<f32>(rd32(fieldData(entry, field))); }

const char* ToolData::stringValue(int entry, int field) const {
    return reinterpret_cast<const char*>(fieldData(entry, field));
}

bool ToolData::boolValue(int entry, int field) const {
    return (rd32(fieldData(entry, field)) & fieldMask(field)) != 0;
}

bool ToolData::getValue(int entry, const char* name, u32* out) const {
    const s32 f = searchItemInfo(name);
    if (f < 0) return false;
    *out = unsignedValue(entry, f);
    return true;
}

bool ToolData::getValue(int entry, const char* name, s32* out) const {
    const s32 f = searchItemInfo(name);
    if (f < 0) return false;
    *out = signedValue(entry, f);
    return true;
}

bool ToolData::getValue(int entry, const char* name, u8* out) const {
    const s32 f = searchItemInfo(name);
    if (f < 0) return false;
    *out = boolValue(entry, f) ? 1 : 0;
    return true;
}

bool ToolData::getValue(int entry, const char* name, f32* out) const {
    const s32 f = searchItemInfo(name);
    if (f < 0) return false;
    *out = floatValue(entry, f);
    return true;
}

bool ToolData::getValue(int entry, const char* name, const char** out) const {
    const s32 f = searchItemInfo(name);
    if (f < 0) return false;
    *out = stringValue(entry, f);
    return true;
}

s32 ToolData::findEntryByValue(int field, u32 value, int startEntry) const {
    for (s32 e = startEntry; e < entryCount(); e++) {
        if (unsignedValue(e, field) == value) {
            return e;
        }
    }
    return -1;
}

s32 ToolData::findEntryByValue(int field, const char* value, int startEntry) const {
    for (s32 e = startEntry; e < entryCount(); e++) {
        if (std::strcmp(stringValue(e, field), value) == 0) {
            return e;
        }
    }
    return -1;
}

}  // namespace lm
