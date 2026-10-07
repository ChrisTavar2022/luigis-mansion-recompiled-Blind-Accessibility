// Rebuilt archives for the unused maps 7 and 8 (see unused_maps.h, docs/UNUSED_CONTENT.md).
#include "recomp/unused_maps.h"

#include "recomp/hle.h"
#include "recomp/rarc.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace recomp::unused_maps {
namespace {

using rarc::Bytes;

constexpr s32 kEntryBase = 0x4C000000;  // virtual FST entries (room_swap 0x48000000, HM 0x40000000)

// Every map setup loads this model (lr 0x8017310C) and builds a J3D model from it without
// checking the lookup; maps 7 and 8 do not have it.
constexpr const char* kEyesModel = "j3d_effect/k03-elem1_eyes/k03-elem1_eyes.bmd";

u32 be32(const u8* p) { return u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3]; }
void put32(u8* p, u32 v) {
    for (int i = 0; i < 4; i++) p[i] = u8(v >> (24 - 8 * i));
}

// ---- JMP parameter tables --------------------------------------------------------------
// Header: entry count, field count, data offset, entry size; then 12-byte field records
// (name hash, mask, offset u16, shift u8, type u8: 0 int, 1 32-byte string, 2 float); then
// the entries; the file is padded with '@' to 32 bytes. The game finds fields by name hash
// (ToolData::searchItemInfo), so tables can be converted field by field.

u32 nameHash(const char* s) {
    u32 h = 0;
    for (; *s; s++) h = (u32(u8(*s)) + (h << 8)) % 0x1FFFFD9u;
    return h;
}

struct Field {
    u32 hash, mask;
    u16 off;
    u8 shift, type;
};

struct Table {
    std::vector<Field> fields;
    u32 count = 0, dataOff = 0, entrySize = 0;
    Bytes raw;

    bool parse(const Bytes& b) {
        if (b.size() < 16) return false;
        count = be32(&b[0]);
        const u32 nf = be32(&b[4]);
        dataOff = be32(&b[8]);
        entrySize = be32(&b[12]);
        if (nf > 256 || 16 + size_t(nf) * 12 > b.size() || size_t(dataOff) + size_t(count) * entrySize > b.size())
            return false;
        fields.clear();
        for (u32 i = 0; i < nf; i++) {
            const u8* f = &b[16 + i * 12];
            fields.push_back({be32(f), be32(f + 4), u16(f[8] << 8 | f[9]), f[10], f[11]});
        }
        raw = b;
        return true;
    }
    const Field* field(u32 hash) const {
        for (const Field& f : fields)
            if (f.hash == hash) return &f;
        return nullptr;
    }
    const u8* entry(u32 i) const { return &raw[dataOff + size_t(i) * entrySize]; }
    bool sameLayout(const Table& o) const {
        if (entrySize != o.entrySize || fields.size() != o.fields.size()) return false;
        for (size_t i = 0; i < fields.size(); i++)
            if (std::memcmp(&raw[16 + i * 12], &o.raw[16 + i * 12], 12) != 0) return false;
        return true;
    }
};

u32 intValue(const Field& f, const u8* e) { return (be32(e + f.off) & f.mask) >> f.shift; }

struct Default {
    const char* field;
    const char* str;
};
// Values the retail tables use where a field has nothing to say.
constexpr Default kStringDefaults[] = {{"create_name", "----"}};

// A table in `layout`'s field layout with `count` entries, each field taken by name hash from
// `src` (when given and present there) or defaulted: strings "(null)" (or kStringDefaults),
// numbers 0. Ints and floats are converted when the two layouts disagree on the type.
Bytes convertTable(const Table& layout, const Table* src, u32 count) {
    const size_t nf = layout.fields.size();
    const u32 dataOff = u32(16 + nf * 12);
    Bytes out(dataOff + size_t(count) * layout.entrySize, 0);
    put32(&out[0], count);
    put32(&out[4], u32(nf));
    put32(&out[8], dataOff);
    put32(&out[12], layout.entrySize);
    std::memcpy(&out[16], &layout.raw[16], nf * 12);
    for (u32 i = 0; i < count; i++) {
        u8* e = &out[dataOff + size_t(i) * layout.entrySize];
        const u8* s = src && i < src->count ? src->entry(i) : nullptr;
        for (const Field& f : layout.fields) {
            const Field* sf = s ? src->field(f.hash) : nullptr;
            if (f.type == 1) {
                const size_t room = std::min<size_t>(32, layout.entrySize - std::min<size_t>(f.off, layout.entrySize));
                if (sf && sf->type == 1) {
                    std::memcpy(e + f.off, s + sf->off, std::min<size_t>(room, src->entrySize - sf->off));
                } else {
                    const char* def = "(null)";
                    for (const Default& d : kStringDefaults)
                        if (nameHash(d.field) == f.hash) def = d.str;
                    std::memcpy(e + f.off, def, std::min(room, std::strlen(def) + 1));
                }
                continue;
            }
            u32 v = 0;  // value in the target type (float bits for type 2)
            if (sf) {
                if (f.type == 2 && sf->type == 2) {
                    v = be32(s + sf->off);
                } else if (f.type == 2) {
                    // Signed integer of the source field's width (e.g. 16-bit angles).
                    const int width = std::popcount(sf->mask);
                    const u32 raw = intValue(*sf, s);
                    const s32 iv = width >= 32 ? s32(raw) : s32(raw << (32 - width)) >> (32 - width);
                    const float fv = float(iv);
                    std::memcpy(&v, &fv, 4);
                } else if (sf->type == 2) {
                    float fv;
                    const u32 bits = be32(s + sf->off);
                    std::memcpy(&fv, &bits, 4);
                    v = u32(s32(fv));
                } else {
                    v = intValue(*sf, s);
                }
            }
            if (f.type == 2) {
                put32(e + f.off, v);
            } else {
                const u32 word = be32(e + f.off);
                put32(e + f.off, (word & ~f.mask) | ((v << f.shift) & f.mask));
            }
        }
    }
    out.resize((out.size() + 31) & ~size_t(31), '@');
    return out;
}

// ---- Map archives ------------------------------------------------------------------------

rarc::Archive loadMap(const char* path) {
    rarc::Archive a;
    if (!a.parse(rarc::decompress(rarc::readDisc(path)))) a.nodes.clear();
    return a;
}

// Map 7 is map 5's statue hall with its own collision; its tables are already in the
// current layout. Only the eyes model is missing.
bool buildMap7(rarc::Archive& a, rarc::Archive& map5) {
    rarc::Item* eyes = map5.find(kEyesModel);
    if (!eyes) return false;
    if (!a.find(kEyesModel)) a.put(kEyesModel, eyes->data);
    return true;
}

// Map 8's JMP tables predate the retail layout (characterinfo 16 fields instead of 23,
// groupinfo 20 instead of 9, eventinfo 21 instead of 24, polygoninfo 8-byte entries, ...)
// and furnitureinfo, observerinfo, soundgroupinfo, soundpolygoninfo and roominfo are absent.
// Every table map 5 has is rebuilt in map 5's layout: entries come from map 8's table where
// there is one (fields matched by name hash), the sound tables get one zeroed entry per
// collision polygon / group, the rest stay empty.
bool buildMap8(rarc::Archive& a, rarc::Archive& map5) {
    if (!buildMap7(a, map5)) return false;
    rarc::Item* jmp5 = map5.find("jmp");
    if (!jmp5 || !jmp5->dir) return false;
    auto count = [&](const char* name) -> u32 {
        Table t;
        rarc::Item* it = a.find(std::string("jmp/") + name);
        return it && t.parse(it->data) ? t.count : 0;
    };
    const u32 polygons = count("polygoninfo"), groups = count("groupinfo");
    std::vector<std::pair<std::string, Bytes>> tables;
    for (const rarc::Item& it : map5.nodes[size_t(jmp5->node)].items) {
        if (it.dir) continue;
        Table layout, src;
        if (!layout.parse(it.data)) return false;
        rarc::Item* own = a.find("jmp/" + it.name);
        const bool have = own && src.parse(own->data);
        if (have && src.sameLayout(layout)) {
            tables.emplace_back(it.name, own->data);
            continue;
        }
        u32 n = have ? src.count : 0;
        if (it.name == "soundpolygoninfo") n = polygons;
        if (it.name == "soundgroupinfo") n = groups;
        tables.emplace_back(it.name, convertTable(layout, have ? &src : nullptr, n));
        log("[maps] map 8 jmp/%s: %s (%u entries)", it.name.c_str(), have ? "converted to the retail layout" : "added",
            n);
    }
    // Same table order as the retail maps.
    rarc::Item* jmp8 = a.find("jmp");
    if (!jmp8 || !jmp8->dir) return false;
    a.nodes[size_t(jmp8->node)].items.clear();
    for (auto& [name, data] : tables) a.put("jmp/" + name, std::move(data));
    return true;
}

struct Served {
    const char* path;
    int map;
    Bytes bytes;
    bool built = false;
};
Served sServed[] = {{"/Map/map7.szp", 7, {}, false}, {"/Map/map8.szp", 8, {}, false}};
std::mutex sM;

bool build(Served& s) {
    if (s.built) return !s.bytes.empty();
    s.built = true;
    rarc::Archive a = loadMap(s.path), map5 = loadMap("/Map/map5.szp");
    if (a.nodes.empty() || map5.nodes.empty()) {
        log("[maps] %s: could not read the archives; serving the disc file", s.path);
        return false;
    }
    const bool ok = s.map == 7 ? buildMap7(a, map5) : buildMap8(a, map5);
    if (!ok) {
        log("[maps] %s: unexpected archive contents; serving the disc file", s.path);
        return false;
    }
    s.bytes = rarc::yay0Store(a.build());
    log("[maps] %s served rebuilt (eyes model from map 5%s, %zu bytes)", s.path,
        s.map == 8 ? ", JMP tables converted" : "", s.bytes.size());
    return true;
}

}  // namespace

s32 overlayEntry(const char* guestPath) {
    std::lock_guard lk(sM);
    for (size_t i = 0; i < std::size(sServed); i++)
        if (!std::strcmp(guestPath, sServed[i].path)) return build(sServed[i]) ? kEntryBase + s32(i) : -1;
    return -1;
}

bool isOverlayEntry(s32 entry) { return entry >= kEntryBase && entry < kEntryBase + s32(std::size(sServed)); }

u32 overlayLength(s32 entry) {
    std::lock_guard lk(sM);
    return isOverlayEntry(entry) ? u32(sServed[entry - kEntryBase].bytes.size()) : 0;
}

s32 overlayRead(s32 entry, void* dst, s32 length, s32 offset) {
    std::lock_guard lk(sM);
    if (!isOverlayEntry(entry)) return -1;
    const Bytes& b = sServed[entry - kEntryBase].bytes;
    if (offset < 0 || length < 0 || size_t(offset) > b.size()) return -1;
    const size_t n = std::min(size_t(length), b.size() - size_t(offset));
    std::memcpy(dst, b.data() + offset, n);
    if (n < size_t(length)) std::memset(static_cast<u8*>(dst) + n, 0, size_t(length) - n);
    return s32(n);
}

}  // namespace recomp::unused_maps
