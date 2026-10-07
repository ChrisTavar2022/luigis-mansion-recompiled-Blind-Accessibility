// Unused assets restored as F1 > Enhancements > Unused content options (all off by default).
// Data-table patches in guest memory plus resource redirects through the game's own archive
// lookup; no game code is rewritten. Findings and addresses: docs/UNUSED_CONTENT.md.
#include "port/settings.h"
#include "recomp/hle.h"
#include "recomp/runtime.h"

#include <dolphin/dvd.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace recomp {
namespace {

bool envFlag(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && *v != '0';
}

const port::EnhancementSettings& opt() { return port::settings().enhancements; }

// ---- Guest-memory patches with verified originals ----------------------------------------

struct Patch32 {
    u32 addr, original, value;
};
struct Patch8 {
    u32 addr;
    u8 original, value;
};

// Writes `value` (on) or `original` (off), but only over one of the two known values, so a
// table that is not what we expect is left alone and reported once.
template <typename P, typename Rd, typename Wr>
void applyPatches(const P* list, size_t n, bool on, const char* what, Rd rd, Wr wr) {
    static bool sWarned = false;
    for (size_t i = 0; i < n; i++) {
        const auto cur = rd(list[i].addr);
        const auto want = on ? list[i].value : list[i].original;
        if (cur == want) continue;
        if (cur != list[i].original && cur != list[i].value) {
            if (!sWarned) log("[unused] %s: unexpected value %X at %08X, not patching", what, u32(cur), list[i].addr);
            sWarned = true;
            continue;
        }
        wr(list[i].addr, want);
    }
}

// Portrificationizer: table lbl_8036E52C (0x18 per ghost; read by fn_80121E14/fn_80124890).
// Field +4 of every entry is 0x60, the generic blue blob; each ghost's own portrait-scene
// models are the unused ids below (the same values as the known Gecko code).
constexpr Patch32 kPortrait[] = {
    {0x8036E548, 0x60, 0x064}, {0x8036E560, 0x60, 0x121}, {0x8036E578, 0x60, 0x125}, {0x8036E590, 0x60, 0x129},
    {0x8036E5A8, 0x60, 0x12D}, {0x8036E5C0, 0x60, 0x131}, {0x8036E5D8, 0x60, 0x135}, {0x8036E5F0, 0x60, 0x139},
    {0x8036E608, 0x60, 0x13D}, {0x8036E620, 0x60, 0x141}, {0x8036E638, 0x60, 0x145}, {0x8036E650, 0x60, 0x149},
    {0x8036E668, 0x60, 0x14D}, {0x8036E680, 0x60, 0x151}, {0x8036E698, 0x60, 0x155}, {0x8036E6B0, 0x60, 0x159},
    {0x8036E6C8, 0x60, 0x15D}, {0x8036E6E0, 0x60, 0x161}, {0x8036E6F8, 0x60, 0x165}, {0x8036E710, 0x60, 0x169},
    {0x8036E728, 0x60, 0x16D},
};

void applyPortraits() {
    static const bool sForce = envFlag("LM_UNUSED_PORTRAITS");
    applyPatches(kPortrait, std::size(kPortrait), sForce || opt().unusedPortraitModels, "portrait table",
                 [](u32 a) { return rd32(a); }, [](u32 a, u32 v) { wr32(a, v); });
}

// Map 2 door table (lbl_802FF218[2]->+0x18, 0x1C per door). Byte +6 is the door type: it picks
// the model (lbl_802FF868: /iwamoto/Door/door_NN.bin) and the lock/unlock cutscenes
// (fn_8011496C: scene 3 + (type-1)*4 + ..., i.e. oo/co/ho/nodemoNN.szp). Types 07 (heart) and
// 08 (star) have models and all four cutscenes but no door uses them. Door 46 is the Nursery
// (Chauncey) door, door 45 the Twins' room door; both ship as type 01.
constexpr Patch8 kHeartDoor[] = {{0x80303488 + 6, 0x01, 0x07}};
constexpr Patch8 kStarDoor[] = {{0x8030346C + 6, 0x01, 0x08}};

void applyDoors() {
    static const bool sHeart = envFlag("LM_HEART_DOOR"), sStar = envFlag("LM_STAR_DOOR");
    auto rd = [](u32 a) { return u8(rd8(a)); };
    auto wr = [](u32 a, u8 v) { wr8(a, v); };
    applyPatches(kHeartDoor, 1, sHeart || opt().heartDoor, "heart door", rd, wr);
    applyPatches(kStarDoor, 1, sStar || opt().starDoor, "star door", rd, wr);
}

// ---- Host-side archive reading (for the beta GBH-map Luigi) ------------------------------

std::vector<u8> readDisc(const char* path) {
    DVDFileInfo fi{};
    if (!DVDOpen(path, &fi)) return {};
    std::vector<u8> buf((fi.length + 31) & ~31u);
    const s32 got = DVDReadPrio(&fi, buf.data(), s32(buf.size()), 0, 2);
    DVDClose(&fi);
    if (got <= 0) return {};
    buf.resize(fi.length);
    return buf;
}

u32 be32(const u8* p) { return u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3]; }
u16 be16(const u8* p) { return u16(p[0] << 8 | p[1]); }

std::vector<u8> yaz0(const std::vector<u8>& in) {
    const u32 size = be32(&in[4]);
    std::vector<u8> out;
    out.reserve(size);
    size_t p = 16;
    while (out.size() < size && p < in.size()) {
        const u8 code = in[p++];
        for (int b = 7; b >= 0 && out.size() < size; b--) {
            if (code & (1 << b)) {
                out.push_back(in[p++]);
            } else {
                const u8 b1 = in[p++], b2 = in[p++];
                const size_t dist = ((b1 & 0x0F) << 8 | b2) + 1;
                size_t n = b1 >> 4;
                n = n ? n + 2 : size_t(in[p++]) + 0x12;
                for (size_t i = 0; i < n; i++) out.push_back(out[out.size() - dist]);
            }
        }
    }
    return out;
}

std::vector<u8> yay0(const std::vector<u8>& in) {
    const u32 size = be32(&in[4]);
    u32 linkOff = be32(&in[8]), chunkOff = be32(&in[12]), maskOff = 16, mask = 0;
    int bits = 0;
    std::vector<u8> out;
    out.reserve(size);
    while (out.size() < size) {
        if (bits == 0) {
            mask = be32(&in[maskOff]);
            maskOff += 4;
            bits = 32;
        }
        if (mask & 0x80000000u) {
            out.push_back(in[chunkOff++]);
        } else {
            const u16 link = be16(&in[linkOff]);
            linkOff += 2;
            const size_t dist = (link & 0xFFF) + 1;
            size_t n = link >> 12;
            n = n ? n + 2 : size_t(in[chunkOff++]) + 0x12;
            for (size_t i = 0; i < n; i++) out.push_back(out[out.size() - dist]);
        }
        mask <<= 1;
        bits--;
    }
    return out;
}

// Returns the bytes of the first file called `name` in a (possibly compressed) RARC.
std::vector<u8> rarcFile(std::vector<u8> data, const char* name) {
    if (data.size() >= 16 && !std::memcmp(data.data(), "Yaz0", 4)) data = yaz0(data);
    else if (data.size() >= 16 && !std::memcmp(data.data(), "Yay0", 4)) data = yay0(data);
    if (data.size() < 0x40 || std::memcmp(data.data(), "RARC", 4)) return {};
    const u32 dataOff = be32(&data[0x0C]) + 0x20;
    const u32 info = 0x20;
    const u32 numEntries = be32(&data[info + 0x08]);
    const u32 entryOff = be32(&data[info + 0x0C]) + info;
    const u32 strOff = be32(&data[info + 0x14]) + info;
    for (u32 i = 0; i < numEntries; i++) {
        const u8* e = &data[entryOff + i * 20];
        const u32 flagsName = be32(e + 4), off = be32(e + 8), size = be32(e + 12);
        if ((flagsName >> 24) & 0x02) continue;  // directory
        const char* n = reinterpret_cast<const char*>(&data[strOff + (flagsName & 0xFFFFFF)]);
        if (!std::strcmp(n, name)) return {data.begin() + dataOff + off, data.begin() + dataOff + off + size};
    }
    return {};
}

// Guest copy of a string (for redirected resource paths).
u32 guestString(const char* s) {
    const u32 n = u32(std::strlen(s) + 1);
    const u32 g = hleAlloc(n, 4);
    std::memcpy(hostp(g), s, n);
    return g;
}

// ---- Beta skybox -------------------------------------------------------------------------
// Map 2's sky descriptor (lbl_80300B60, via lbl_80300C7C[map]) loads /vrball01.bmd and
// /vrball01.btk from /Iwamoto/vrball_M.szp. The always-mounted game archive
// (fn_80066FB4) also carries an older sky under iwamoto/vrbox/ that nothing loads.
constexpr u32 kSkyDesc = 0x804D8380;      // current sky descriptor (lbl_804D8380)
constexpr u32 kMansionSky = 0x80300B60;   // map 2 descriptor
bool sSkyLoading = false;

u32 gameArchive(Ctx& c) {
    static const u32 fn = guestSymbol("fn_80066FB4");
    const Ctx saved = c;
    const u32 a = guestCall(c, fn);
    c = saved;
    return a;
}

// ---- Beta Game Boy Horror map Luigi ------------------------------------------------------
// The GBH map loads /gide_lui.bin (the Luigi marker drawn at Luigi's position, model
// lbl_804D830C + 0x318) from the guide-map archive (guidemap.szp in the DOL's static data).
// The disc still has an older guide-map archive, /Iwamoto/map2/gidemap.szp, whose
// gide_lui.bin is a full Luigi model instead of the retail head.
u32 sBetaLuigi = 0, sBetaLuigiSize = 0;
bool sBetaLuigiTried = false;

void loadBetaLuigi() {
    if (sBetaLuigiTried) return;
    sBetaLuigiTried = true;
    const std::vector<u8> f = rarcFile(readDisc("/Iwamoto/map2/gidemap.szp"), "gide_lui.bin");
    if (f.empty()) {
        log("[unused] /Iwamoto/map2/gidemap.szp: gide_lui.bin not found");
        return;
    }
    sBetaLuigi = hleAlloc(u32(f.size()), 32);
    sBetaLuigiSize = u32(f.size());
    std::memcpy(hostp(sBetaLuigi), f.data(), f.size());
    log("[unused] beta GBH-map Luigi loaded (%u bytes)", sBetaLuigiSize);
}

}  // namespace

// Portrificationizer scenes: both readers of the portrait table patch it first.
HLE(fn_80121E14) {
    applyPortraits();
    callOriginal(c, "fn_80121E14");
}
HLE(fn_80124890) {
    applyPortraits();
    callOriginal(c, "fn_80124890");
}

// Map setup (doors are instantiated from the door table right after this).
HLE(fn_800122BC) {
    applyDoors();
    callOriginal(c, "fn_800122BC");
}

// Sky load callback (fn_800067D0 finished mounting the sky archive).
HLE(fn_8002F4DC) {
    static const bool sForce = envFlag("LM_BETA_SKY");
    sSkyLoading = (sForce || opt().betaSkybox) && rd32(kSkyDesc) == kMansionSky;
    log("[unused] sky load: descriptor %08X%s", rd32(kSkyDesc), sSkyLoading ? " (beta)" : "");
    callOriginal(c, "fn_8002F4DC");
    sSkyLoading = false;
}

// JKRArchive::getResource(const char*): redirects for the options above.
HLE(getResource__10JKRArchiveFPCc) {
    const char* path = gstr(c.r[4]);
    if (sSkyLoading) {
        static u32 sBmd = 0, sBtk = 0;
        if (!sBmd) sBmd = guestString("/iwamoto/vrbox/vrball01.bmd"), sBtk = guestString("/iwamoto/vrbox/vrball01.btk");
        const u32 alt = !std::strcmp(path, "/vrball01.bmd") ? sBmd : !std::strcmp(path, "/vrball01.btk") ? sBtk : 0;
        if (alt) {
            const u32 arc = gameArchive(c);
            if (arc) {
                c.r[3] = arc;
                c.r[4] = alt;
                log("[unused] beta skybox: %s -> %s", path, gstr(alt));
            }
        }
    } else if (!std::strcmp(path, "/gide_lui.bin")) {
        static const bool sForce = envFlag("LM_BETA_MAP_LUIGI");
        if (sForce || opt().betaMapLuigi) {
            loadBetaLuigi();
            if (sBetaLuigi) {
                c.r[3] = sBetaLuigi;
                return;
            }
        }
    }
    callOriginal(c, "getResource__10JKRArchiveFPCc");
}

// JKRArchive::getResSize(const void*): size of the redirected GBH-map Luigi.
HLE(getResSize__10JKRArchiveCFPCv) {
    if (sBetaLuigi && c.r[4] == sBetaLuigi) {
        c.r[3] = sBetaLuigiSize;
        return;
    }
    callOriginal(c, "getResSize__10JKRArchiveCFPCv");
}

}  // namespace recomp
