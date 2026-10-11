// Unused ghost placements, colour variants and the extra Speedy Spirits (F1 > Enhancements >
// Unused content, each off by default; US game). Docs: docs/UNUSED_CONTENT.md ("Second batch").
//
// Placements: the mansion's map archive (/Map/map2.szp) has path files (path/<name>, a JMP table
// of x,y,z points the enemy code follows: EnZako reads path_name and getPathResource) that no
// table row uses. Their names say which ghost and room they were made for: hustler_01 (Slim
// Bankshot, the hustler, in the Hidden Room), skul_23_1/_2 (Mr. Bones' skeletons, room 23,
// the Courtyard), topoo_8_1 (a topoo ghost, room 8), heypo_60_4.._12 (Ghost Guys on the roof,
// room 60, next to the used heypo_60_1.._3) and poo_60_1 (a ghost on the roof). The training
// room (/Map/map3.szp) has mg_path, a third generator path behind the two its generators use.
// With the option on, the port serves the archives rebuilt with rows for them: copies of a
// retail row of the same kind, moved to the path's first point with the path and room set.
//
// Colour variants: never-placed character types reuse a placed type's model with another
// texture set (type +0x10, index into the model's .txp list) and parameter file
// (/param/ctp/<type>.prm: life, speed, glow colour...). rat2 (type 105, rat.txp set 1: pink) and
// the Ghost Guy sets heypo9 / heypo10 (heypo.szp has heypo9.txp / heypo10.txp, set 9 and 1, and
// game_usa.szp heypo9.prm / heypo10.prm) are never used. The options give the placed mice and
// two of the Ghost Guy types those sets and parameter files.
//
// Extra Speedy Spirits: types iyapoo16..20 (254..258) exist but none is placed and the reward
// table iyapootable stops at 15. This option is NOT original content: it places them at spots
// chosen by the port with rewards modelled on the retail ones.
#include "port/install.h"
#include "port/settings.h"
#include "recomp/guest_util.h"
#include "recomp/hle.h"
#include "recomp/rarc.h"
#include "recomp/unused2.h"
#include "recomp/version.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <cstring>
#include <string>
#include <vector>

namespace recomp::unused2 {
namespace {

using Bytes = std::vector<u8>;

u32 be32(const u8* p) { return u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3]; }
void put32(u8* p, u32 v) {
    for (int i = 0; i < 4; i++) p[i] = u8(v >> (24 - 8 * i));
}

bool envFlag(const char* n) {
    const char* v = std::getenv(n);
    return v && *v && *v != '0';
}
bool us() { return port::usCodeFeature() == port::Avail::Yes; }
bool placementsOn() { return us() && (envFlag("LM_GHOST_PLACEMENTS") || port::settings().unused2.ghostPlacements); }
bool spiritsOn() { return us() && (envFlag("LM_EXTRA_SPIRITS") || port::settings().unused2.extraSpeedySpirits); }
bool pinkMiceOn() { return us() && (envFlag("LM_PINK_MICE") || port::settings().unused2.pinkMice); }
bool blueGuysOn() { return us() && (envFlag("LM_BLUE_GHOST_GUYS") || port::settings().unused2.blueGhostGuys); }

// ---- JMP tables ---------------------------------------------------------------------------
// Header: entry count, field count, data offset, entry size; 12-byte field records (name hash,
// mask, offset u16, shift u8, type u8: 0 int, 1 32-byte string, 2 float); entries; padding '@'.
u32 nameHash(const char* s) {
    u32 h = 0;
    for (; *s; s++) h = (u32(u8(*s)) + (h << 8)) % 0x1FFFFD9u;
    return h;
}

struct Jmp {
    struct Field {
        u32 hash, mask;
        u16 off;
        u8 shift, type;
    };
    std::vector<Field> fields;
    u32 dataOff = 0, entrySize = 0;
    std::vector<Bytes> rows;
    Bytes head;  // header + field records

    bool parse(const Bytes& b) {
        if (b.size() < 16) return false;
        const u32 count = be32(&b[0]), nf = be32(&b[4]);
        dataOff = be32(&b[8]);
        entrySize = be32(&b[12]);
        if (nf > 256 || size_t(dataOff) + size_t(count) * entrySize > b.size()) return false;
        for (u32 i = 0; i < nf; i++) {
            const u8* f = &b[16 + i * 12];
            fields.push_back({be32(f), be32(f + 4), u16(f[8] << 8 | f[9]), f[10], f[11]});
        }
        head.assign(b.begin(), b.begin() + dataOff);
        for (u32 i = 0; i < count; i++) rows.emplace_back(b.begin() + dataOff + i * entrySize, b.begin() + dataOff + (i + 1) * entrySize);
        return true;
    }
    const Field* field(const char* name) const {
        const u32 h = nameHash(name);
        for (const Field& f : fields)
            if (f.hash == h) return &f;
        return nullptr;
    }
    std::string str(const Bytes& row, const char* name) const {
        const Field* f = field(name);
        if (!f || f->type != 1) return {};
        return std::string(reinterpret_cast<const char*>(&row[f->off]), strnlen(reinterpret_cast<const char*>(&row[f->off]), 32));
    }
    void setStr(Bytes& row, const char* name, const char* v) const {
        const Field* f = field(name);
        if (!f || f->type != 1) return;
        std::memset(&row[f->off], 0, std::min<size_t>(32, entrySize - f->off));
        std::memcpy(&row[f->off], v, std::min<size_t>(31, std::strlen(v)));
    }
    void setInt(Bytes& row, const char* name, s32 v) const {
        const Field* f = field(name);
        if (!f || f->type != 0) return;
        const u32 w = be32(&row[f->off]);
        put32(&row[f->off], (w & ~f->mask) | ((u32(v) << f->shift) & f->mask));
    }
    void setFloat(Bytes& row, const char* name, float v) const {
        const Field* f = field(name);
        if (!f) return;
        if (f->type == 2) {
            u32 bits;
            std::memcpy(&bits, &v, 4);
            put32(&row[f->off], bits);
        } else if (f->type == 0) {
            setInt(row, name, s32(v));
        }
    }
    // First row whose string fields match (name, and path_name when given).
    const Bytes* find(const char* name, const char* path = nullptr) const {
        for (const Bytes& r : rows)
            if (str(r, "name") == name && (!path || str(r, "path_name") == path)) return &r;
        return nullptr;
    }
    Bytes build() const {
        Bytes out = head;
        put32(&out[0], u32(rows.size()));
        for (const Bytes& r : rows) out.insert(out.end(), r.begin(), r.end());
        while (out.size() % 32) out.push_back('@');
        return out;
    }
};

// First point of a path file (a JMP table of x, y, z).
bool pathStart(rarc::Archive& a, const std::string& name, float* xyz) {
    const rarc::Item* it = a.find("path/" + name);
    if (!it || it->data.size() < 16) return false;
    const Bytes& d = it->data;
    const u32 count = be32(&d[0]), off = be32(&d[8]), size = be32(&d[12]);
    if (!count || off + size > d.size() || size < 12) return false;
    for (int i = 0; i < 3; i++) {
        const u32 bits = be32(&d[off + 4 * i]);
        std::memcpy(&xyz[i], &bits, 4);
    }
    return true;
}

// ---- Yay0 compression (LZ77, 4 KiB window, matches 3..273 bytes, greedy with hash chains) --
Bytes yay0Compress(const Bytes& src) {
    const size_t n = src.size();
    std::vector<u32> masks;
    std::vector<u16> links;
    Bytes chunks;
    u32 mask = 0;
    int bit = 31;
    auto flushBit = [&](bool literal) {
        if (literal) mask |= 1u << bit;
        if (--bit < 0) {
            masks.push_back(mask);
            mask = 0;
            bit = 31;
        }
    };
    constexpr size_t kHash = 1 << 15;
    std::vector<s32> head(kHash, -1), prev(n, -1);
    auto hash3 = [&](size_t i) { return ((u32(src[i]) << 10) ^ (u32(src[i + 1]) << 5) ^ src[i + 2]) & (kHash - 1); };
    auto insert = [&](size_t i) {
        if (i + 2 >= n) return;
        const u32 h = hash3(i);
        prev[i] = head[h];
        head[h] = s32(i);
    };
    size_t i = 0;
    while (i < n) {
        size_t bestLen = 0, bestDist = 0;
        if (i + 2 < n) {
            int chain = 0;
            for (s32 j = head[hash3(i)]; j >= 0 && chain < 96; j = prev[size_t(j)], chain++) {
                const size_t dist = i - size_t(j);
                if (dist > 0x1000) break;
                size_t l = 0;
                const size_t maxLen = std::min<size_t>(0x111, n - i);
                while (l < maxLen && src[size_t(j) + l] == src[i + l]) l++;
                if (l > bestLen) {
                    bestLen = l, bestDist = dist;
                    if (l == maxLen) break;
                }
            }
        }
        if (bestLen >= 3) {
            flushBit(false);
            if (bestLen >= 0x12) {
                links.push_back(u16(bestDist - 1));
                chunks.push_back(u8(bestLen - 0x12));
            } else {
                links.push_back(u16(((bestLen - 2) << 12) | (bestDist - 1)));
            }
            for (size_t k = 0; k < bestLen; k++) insert(i + k);
            i += bestLen;
        } else {
            flushBit(true);
            chunks.push_back(src[i]);
            insert(i);
            i++;
        }
    }
    if (bit != 31) masks.push_back(mask);
    Bytes out(16);
    std::memcpy(&out[0], "Yay0", 4);
    put32(&out[4], u32(n));
    for (u32 m : masks) {
        out.resize(out.size() + 4);
        put32(&out[out.size() - 4], m);
    }
    const u32 linkOff = u32(out.size());
    for (u16 l : links) {
        out.push_back(u8(l >> 8));
        out.push_back(u8(l));
    }
    const u32 chunkOff = u32(out.size());
    out.insert(out.end(), chunks.begin(), chunks.end());
    while (out.size() % 32) out.push_back(0);
    put32(&out[8], linkOff);
    put32(&out[12], chunkOff);
    return out;
}

struct Placement {
    const char* table;      // jmp table
    const char* templName;  // retail row to copy (by name)
    const char* templPath;  // ... and path, or nullptr
    const char* name;       // character name of the new row
    const char* path;       // the unused path
    int room;
};

const Placement kMap2[] = {
    {"characterinfo", "hustler", nullptr, "hustler", "hustler_01", 1},  // Slim in the Hidden Room
    {"enemyinfo", "skul", "skul_16_1", "skul", "skul_23_1", 23},      // Mr. Bones' skeletons in the Courtyard
    {"enemyinfo", "skul", "skul_16_1", "skul", "skul_23_2", 23},
    {"enemyinfo", "topoo1", nullptr, "topoo1", "topoo_8_1", 8},
    {"enemyinfo", "yapoo1", nullptr, "yapoo1", "poo_60_1", 60},
    {"enemyinfo", "heypo3", "heypo_60_1", "heypo3", "heypo_60_4", 60},
    {"enemyinfo", "heypo3", "heypo_60_1", "heypo4", "heypo_60_5", 60},
    {"enemyinfo", "heypo3", "heypo_60_1", "heypo5", "heypo_60_6", 60},
    {"enemyinfo", "heypo3", "heypo_60_1", "heypo6", "heypo_60_7", 60},
    {"enemyinfo", "heypo3", "heypo_60_1", "heypo7", "heypo_60_8", 60},
    {"enemyinfo", "heypo3", "heypo_60_1", "heypo8", "heypo_60_9", 60},
    {"enemyinfo", "heypo3", "heypo_60_1", "heypo3", "heypo_60_10", 60},
    {"enemyinfo", "heypo3", "heypo_60_1", "heypo4", "heypo_60_11", 60},
    {"enemyinfo", "heypo3", "heypo_60_1", "heypo5", "heypo_60_12", 60},
};

// Extra Speedy Spirits (port's own placement): rooms without one, positions on their floors
// (taken from retail rows in the same rooms), rewards in the pattern of iyapoo1..15.
struct Spirit {
    const char* name;
    int room;
    float x, y, z;
    int coin, bill, gold, sapphire, emerald, ruby;
};
const Spirit kSpirits[] = {
    // Rooms that have ghosts but no Speedy Spirit, at a retail ghost's spot in each.
    {"iyapoo16", 4, 3060.0f, 150.0f, -460.0f, 20, 10, 2, 0, 0, 0},
    {"iyapoo17", 10, 1940.0f, 150.0f, -2440.0f, 20, 5, 0, 1, 0, 0},
    {"iyapoo18", 13, -180.0f, 150.0f, -3260.0f, 20, 5, 0, 0, 1, 0},
    {"iyapoo19", 27, 1650.0f, 920.0f, -660.0f, 20, 5, 0, 0, 0, 1},
    {"iyapoo20", 40, 2000.0f, 700.0f, -3630.0f, 20, 30, 0, 0, 0, 0},
};

bool editTable(rarc::Archive& a, const char* path, const std::function<void(Jmp&)>& fn) {
    rarc::Item* it = a.find(path);
    Jmp t;
    if (!it || !t.parse(it->data)) {
        log("[placements] %s: table missing", path);
        return false;
    }
    const size_t before = t.rows.size();
    fn(t);
    a.put(path, t.build());
    log("[placements] %s: %zu -> %zu rows", path, before, t.rows.size());
    return true;
}

}  // namespace

u32 mapArchive2Key() { return (placementsOn() ? 1u : 0u) | (spiritsOn() ? 2u : 0u); }

Bytes buildMapArchive2() {
    rarc::Archive a;
    if (!a.parse(rarc::decompress(rarc::readDisc("/Map/map2.szp")))) return {};
    const bool place = placementsOn(), spirits = spiritsOn();
    if (place) {
        for (const char* table : {"characterinfo", "enemyinfo"}) {
            editTable(a, (std::string("jmp/") + table).c_str(), [&](Jmp& t) {
                for (const Placement& p : kMap2) {
                    if (std::strcmp(p.table, table)) continue;
                    const Bytes* tmpl = t.find(p.templName, p.templPath);
                    float xyz[3];
                    if (!tmpl || !pathStart(a, p.path, xyz)) {
                        log("[placements] %s on %s: template or path missing", p.name, p.path);
                        continue;
                    }
                    Bytes r = *tmpl;
                    t.setStr(r, "name", p.name);
                    t.setStr(r, "path_name", p.path);
                    t.setStr(r, "create_name", "----");
                    t.setStr(r, "access_name", "(null)");
                    t.setFloat(r, "pos_x", xyz[0]);
                    t.setFloat(r, "pos_y", xyz[1]);
                    t.setFloat(r, "pos_z", xyz[2]);
                    t.setInt(r, "room_no", p.room);
                    t.setInt(r, "appear_flag", 0);
                    t.setInt(r, "disappear_flag", 0);
                    t.rows.push_back(r);
                }
            });
        }
    }
    if (spirits) {
        editTable(a, "jmp/enemyinfo", [&](Jmp& t) {
            const Bytes* tmpl = t.find("iyapoo1");
            if (!tmpl) return;
            for (const Spirit& s : kSpirits) {
                Bytes r = *tmpl;
                char create[16];
                std::snprintf(create, sizeof create, "%d_99", s.room);
                t.setStr(r, "name", s.name);
                t.setStr(r, "create_name", create);
                t.setStr(r, "access_name", create);
                t.setFloat(r, "pos_x", s.x);
                t.setFloat(r, "pos_y", s.y);
                t.setFloat(r, "pos_z", s.z);
                t.setInt(r, "room_no", s.room);
                t.rows.push_back(r);
            }
        });
        editTable(a, "jmp/iyapootable", [&](Jmp& t) {
            if (t.rows.empty()) return;
            for (const Spirit& s : kSpirits) {
                Bytes r = t.rows[0];
                t.setStr(r, "name", s.name);
                t.setInt(r, "coin", s.coin);
                t.setInt(r, "bill", s.bill);
                t.setInt(r, "gold", s.gold);
                t.setInt(r, "sapphire", s.sapphire);
                t.setInt(r, "emerald", s.emerald);
                t.setInt(r, "ruby", s.ruby);
                t.rows.push_back(r);
            }
        });
    }
    const Bytes raw = a.build();
    Bytes out = yay0Compress(raw);
    log("[placements] map2 rebuilt: %zu bytes raw, %zu compressed", raw.size(), out.size());
    return out;
}

u32 mapArchive3Key() { return placementsOn() ? 1u : 0u; }

Bytes buildMapArchive3() {
    rarc::Archive a;
    if (!a.parse(rarc::decompress(rarc::readDisc("/Map/map3.szp")))) return {};
    editTable(a, "jmp/generatorinfo", [&](Jmp& t) {
        if (t.rows.size() < 3) return;
        float xyz[3];
        if (!pathStart(a, "mg_path", xyz)) return;
        // Three generators on mg_path, as the three on mg_path1 (same codes, so the first
        // training stage starts them too), spaced along the path's start.
        for (int i = 0; i < 3; i++) {
            Bytes r = t.rows[size_t(i)];
            t.setStr(r, "path_name", "mg_path");
            t.setFloat(r, "pos_x", xyz[0] + 60.0f * float(i));
            t.setFloat(r, "pos_y", xyz[1]);
            t.setFloat(r, "pos_z", xyz[2]);
            t.rows.push_back(r);
        }
    });
    return yay0Compress(a.build());
}

// ---- colour variants ------------------------------------------------------------------------
namespace {

#define kTypeTableVA VA(0x8033B590)    // character types, 0x1C each (+0x10 texture set)
#define kTxpTableVA VA(0x80389B68)     // per-model .txp lists, 0x40 each
#define kActorSlotsVA VA(0x803D48A0)   // live actors
constexpr u32 kActorSlotCount = 128;

struct Variant {
    u32 type;       // placed type that is recoloured
    u32 retailSet;  // its own texture set
    u32 newSet;     // the unused set
    const char* prmFrom;
    const char* prmTo;
    bool (*on)();
};
const Variant kVariants[] = {
    {104, 0, 1, "/param/ctp/rat1.prm", "/param/ctp/rat2.prm", pinkMiceOn},   // rat1 -> rat2 (pink)
    {25, 0, 9, "/param/ctp/heypo1.prm", "/param/ctp/heypo9.prm", blueGuysOn},  // heypo1 -> heypo9
    {26, 2, 1, "/param/ctp/heypo2.prm", "/param/ctp/heypo10.prm", blueGuysOn}, // heypo2 -> heypo10
};

}  // namespace

// Map setup: the type table's texture set for the recoloured types (read when actors are set
// up), written only over the retail or the variant value.
void placementsMapSetup(Ctx&) {
    if (!us()) return;
    for (const Variant& v : kVariants) {
        const u32 a = kTypeTableVA + 0x1C * v.type + 0x10;
        const u32 cur = rd32(a);
        const u32 want = v.on() ? v.newSet : v.retailSet;
        if (cur != v.retailSet && cur != v.newSet) continue;
        if (cur != want) {
            wr32(a, want);
            log("[placements] type %u texture set %u -> %u", v.type, cur, want);
        }
    }
}

// Every frame: actors set up before their model's .txp files were loaded (disc models load
// asynchronously) get the texture pattern now, as for the green Toad (unused_models.cpp).
void placementsFrame(Ctx&) {
    if (!us()) return;
    // Test aid: LM_ACTOR_DUMP=<n> logs every live actor (type, position) every n frames.
    static const int sDump = [] {
        const char* v = std::getenv("LM_ACTOR_DUMP");
        return v ? std::atoi(v) : 0;
    }();
    static u32 sFrame = 0;
    if (sDump > 0 && ++sFrame % u32(sDump) == 0) {
        std::string line;
        for (u32 i = 0; i < kActorSlotCount; i++) {
            const u32 act = rd32(kActorSlotsVA + 4 * i);
            if (!gu::inMem1(act)) continue;
            char b[64];
            std::snprintf(b, sizeof b, " %u@%.0f,%.0f,%.0f", rd32(act + 0x3C), gu::rdf(act + 0x44), gu::rdf(act + 0x48), gu::rdf(act + 0x4C));
            line += b;
        }
        log("[actors] f%u%s", sFrame, line.c_str());
    }
    for (const Variant& v : kVariants) {
        const u32 entry = kTypeTableVA + 0x1C * v.type;
        const u32 txpRec = kTxpTableVA + (rd32(entry) << 6);
        const u32 count = rd32(txpRec + 0x20), list = rd32(txpRec + 0x24);
        if (count <= std::max(v.newSet, v.retailSet) || !gu::inMem1(list)) continue;
        const u32 newTxp = rd32(list + 4 * v.newSet), oldTxp = rd32(list + 4 * v.retailSet);
        const bool on = v.on();
        for (u32 i = 0; i < kActorSlotCount; i++) {
            const u32 act = rd32(kActorSlotsVA + 4 * i);
            if (!gu::inMem1(act) || rd32(act + 0x3C) != v.type) continue;
            const u32 m = rd32(act + 4);
            if (!gu::inMem1(m)) continue;
            const u32 cur = rd32(m + 0xA4);
            if (on && cur != newTxp) wr32(m + 0xA4, newTxp);
            else if (!on && cur == newTxp) wr32(m + 0xA4, oldTxp);
        }
    }
}

// Parameter files: the recoloured types load the variant's .prm (getResource hook,
// unused_assets.cpp). Returns a guest copy of the replacement path, or 0.
u32 resourceRedirect(const char* path) {
    if (!us()) return 0;
    static std::map<std::string, u32> sStrings;
    for (const Variant& v : kVariants)
        if (v.on() && !std::strcmp(path, v.prmFrom)) {
            u32& g = sStrings[v.prmTo];
            if (!g && (g = extraAlloc(u32(std::strlen(v.prmTo) + 1), 4))) std::memcpy(hostp(g), v.prmTo, std::strlen(v.prmTo) + 1);
            if (g) log("[placements] %s -> %s", path, v.prmTo);
            return g;
        }
    return 0;
}

}  // namespace recomp::unused2
