// Unused early Luigi model (F1 > Enhancements > Unused content, off by default).
//
// Character models are registered in lbl_8034E46C (0x34 per model: +0 file name, +4 archive,
// +8 relocated .mdl, +0xC/+0x10 key count/table, +0x30 load state). Entry 0 is
// "/model/luige.szp". At map setup fn_8006227C first looks for the same name with ".arc" in the
// resident game archive (game_usa.szp: model/luige.arc) and only falls back to the disc file
// when it is missing, so the disc's /model/luige.szp (an earlier Luigi) is never loaded.
// fn_80061C94 then fills the entry from the archive: every /key, /sls, /slk and /txp resource,
// the per-model tables of fn_80187B24, and finally the /model .mdl, relocated by fn_80061B40.
//
// The early archive has the same skeleton (65 joints, 55 envelopes, identical nodes, inverse
// bind matrices and skin weights), but a different key set (121 vs 141, in a different order;
// the game indexes keys by archive order), so only its .mdl is used: after fn_80061C94 has
// built the retail entry, +8 is pointed at a relocated copy of the early .mdl and all retail
// animations, texture patterns and sound/effect tables stay as they are.
//
// The .txp texture patterns (eye blinks, expressions) store *sampler* indices of the retail
// model. The early model numbers its samplers differently and lacks two retail eye textures
// (the looking-around eye and the dark "poisoned" eye), so the copy gets a sampler table laid
// out like the target model's: each target sampler points at the early texture with identical
// pixels, or at a copy of the target's texture appended to the early model when the early
// model has none. The early materials are re-pointed at matching entries of that table.
// The in-engine cutscene Luigis that go through the same registry (dluige01, dluige03: same
// skeleton and bind pose) get the same treatment. The pre-rendered-style demo scenes
// (/Ajioka/ADemo: opening, ending) carry their own Luigi models with a 122-joint rig or another
// bind pose and are left alone. Docs: docs/UNUSED_CONTENT.md ("Early Luigi model").
#include "port/install.h"
#include "port/settings.h"
#include "recomp/hle.h"
#include "recomp/rarc.h"
#include "recomp/runtime.h"
#include "recomp/version.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

namespace recomp {
namespace {

#define kModelRegistry VA(0x8034E46C)  // lbl_8034E46C, 0x34 per model (same layout in both releases)
constexpr u32 kEntrySize = 0x34;
constexpr u32 kNumModels = 0x106;
// Guest memory for the relocated early copies: host-backed, above the 24 MiB of MEM1 the game
// uses; the model viewer keeps 0x81810000-0x81812000 and its heap starts at 0x82000000.
constexpr u32 kCopyBase = 0x81F00000, kCopyEnd = 0x81FF0000;
u32 sNext = kCopyBase;

bool envFlag() {
    static const bool sEnv = [] {
        const char* v = std::getenv("LM_EARLY_LUIGI");
        return v && *v && *v != '0';
    }();
    return sEnv;
}

bool enabled() {
    return envFlag() || port::settings().enhancements.earlyLuigi;
}

bool inMem1(u32 a) { return a >= 0x80000000u && a < 0x81800000u; }
// MEM1 or the host-backed area above it where model copies live (early/Mario copies).
bool inGuest(u32 a) { return a >= 0x80000000u && a < 0x81FF0000u; }

u16 be16(const u8* p) { return u16(p[0] << 8 | p[1]); }
u32 be32(const u8* p) { return u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3]; }
void put16(u8* p, u16 v) { p[0] = u8(v >> 8); p[1] = u8(v); }
void put32(u8* p, u32 v) { p[0] = u8(v >> 24); p[1] = u8(v >> 16); p[2] = u8(v >> 8); p[3] = u8(v); }

// Pixel bytes of an .mdl texture (format codes index GX_TF_C4, C8, C14X2, I4, I8, IA4, IA8,
// RGB565, RGB5A3, RGBA8, CMPR). 0 for the palette formats, which are not matched or copied.
u32 texBytes(u8 fmt, u32 w, u32 h) {
    auto tiles = [&](u32 tw, u32 th, u32 bytes) { return ((w + tw - 1) / tw) * ((h + th - 1) / th) * bytes; };
    switch (fmt) {
    case 3: case 10: return tiles(8, 8, 32);           // I4, CMPR
    case 4: case 5: return tiles(8, 4, 32);            // I8, IA4
    case 6: case 7: case 8: return tiles(4, 4, 32);    // IA8, RGB565, RGB5A3
    case 9: return tiles(4, 4, 64);                    // RGBA8
    default: return 0;                                  // C4/C8/C14X2 need a palette
    }
}

// The early .mdl from the disc, unrelocated.
const std::vector<u8>& earlyMdl() {
    static std::vector<u8> sMdl;
    static bool sTried = false;
    if (sTried) return sMdl;
    sTried = true;
    rarc::Archive a;
    if (!a.parse(rarc::decompress(rarc::readDisc("/model/luige.szp")))) {
        log("[early luigi] /model/luige.szp: not a RARC archive");
        return sMdl;
    }
    const rarc::Item* m = a.find("model/luige.mdl");
    if (!m || m->data.size() < 0x80 || be32(m->data.data()) != 0x04B40000u || be16(&m->data[8]) != 65) {
        log("[early luigi] /model/luige.szp: model/luige.mdl missing or unexpected");
        return sMdl;
    }
    sMdl = m->data;
    log("[early luigi] early model read (%zu bytes)", sMdl.size());
    return sMdl;
}

// True for a relocated .mdl in guest memory with Luigi's skeleton exactly as in the early
// model: joint/envelope counts, node graph and inverse bind matrices. That is the gameplay
// model and the cutscene copies dluige01/dluige03; the ending models (dluige02, the
// cutscene archive's db_luigi) are bound in another space and are left alone.
bool luigiSkeleton(u32 target) {
    const std::vector<u8>& early = earlyMdl();
    if (early.empty() || !inGuest(target)) return false;
    if (rd32(target) != 0x04B40000u || !(rd32(target + 0x48) & 0x80000000u)) return false;  // relocated .mdl only
    const u32 joints = be16(&early[0x08]);
    if (rd16(target + 0x08) != joints || rd16(target + 0x0C) != be16(&early[0x0C])) return false;
    const u32 tn = rd32(target + 0x30), ti = rd32(target + 0x38);
    if (!inGuest(tn) || !inGuest(ti)) return false;
    return !std::memcmp(hostp(tn), &early[be32(&early[0x30])], 0x10 * joints) &&
           !std::memcmp(hostp(ti), &early[be32(&early[0x38])], 0x30 * joints);
}

// Builds the early model for `target` (a relocated Luigi .mdl in guest memory) and returns
// its relocated guest copy, or 0. Sampler indices mean the same textures as in the target.
u32 buildFor(Ctx& c, u32 target) {
    const std::vector<u8>& early = earlyMdl();
    std::vector<u8> out = early;
    auto align = [&](u32 a) { while (out.size() % a) out.push_back(0); };
    align(32);

    // Early textures.
    struct Tex {
        u8 fmt;
        u16 w, h;
        const u8* hdr;
        u32 size;
    };
    std::vector<Tex> etex;
    const u32 eTexCount = be16(&early[0x20]);
    std::vector<u32> texOffs;  // new texture table (file offsets)
    for (u32 i = 0; i < eTexCount; i++) {
        const u32 o = be32(&early[be32(&early[0x60]) + 4 * i]);
        etex.push_back({early[o], be16(&early[o + 2]), be16(&early[o + 4]), &early[o],
                        texBytes(early[o], be16(&early[o + 2]), be16(&early[o + 4]))});
        texOffs.push_back(o);
    }

    // Target textures -> early texture with identical pixels, else append a copy.
    const u32 tTexCount = rd16(target + 0x20);
    const u32 tTexTable = rd32(target + 0x60);
    std::vector<u16> texMap(tTexCount, 0);
    for (u32 t = 0; t < tTexCount; t++) {
        const u32 ta = rd32(tTexTable + 4 * t);
        const u8* th = static_cast<const u8*>(hostp(ta));
        const u8 fmt = th[0];
        const u16 w = be16(th + 2), h = be16(th + 4);
        const u32 bytes = texBytes(fmt, w, h);
        int found = -1;
        for (u32 e = 0; e < etex.size() && found < 0; e++) {
            if (etex[e].fmt == fmt && etex[e].w == w && etex[e].h == h && bytes &&
                !std::memcmp(etex[e].hdr + 0x20, th + 0x20, bytes))
                found = int(e);
        }
        if (found < 0 && bytes) {
            align(32);
            const u32 o = u32(out.size());
            out.insert(out.end(), th, th + 0x20 + bytes);
            texOffs.push_back(o);
            found = int(texOffs.size() - 1);
            log("[early luigi] target texture %u (%ux%u fmt %u) not in the early model: copied as %d", t, w, h, fmt, found);
        } else if (found < 0) {
            found = 0;
            log("[early luigi] target texture %u (palette format %u) not matched; uses texture 0", t, fmt);
        }
        texMap[t] = u16(found);
    }

    // Sampler table laid out like the target's, then the early materials' own samplers.
    const u32 tSampCount = rd16(target + 0x24);
    const u32 tSamp = rd32(target + 0x6C);
    std::vector<std::array<u8, 8>> samp;
    for (u32 s = 0; s < tSampCount; s++) {
        std::array<u8, 8> e;
        std::memcpy(e.data(), hostp(tSamp + 8 * s), 8);
        put16(e.data(), texMap[be16(e.data())]);
        samp.push_back(e);
    }
    const u32 eSamp = be32(&early[0x6C]);
    const u32 eMatCount = be16(&early[0x28]), eMat = be32(&early[0x68]);
    u32 remapped = 0;
    for (u32 m = 0; m < eMatCount; m++) {
        u8* mat = &out[eMat + 0x120 * m];
        const u32 stages = mat[7] > 8 ? 8 : mat[7];
        for (u32 s = 0; s < stages; s++) {
            u8* st = mat + 0x20 + 0x20 * s;
            const u16 es = be16(st + 2);
            std::array<u8, 8> want;
            std::memcpy(want.data(), &early[eSamp + 8 * es], 8);
            u32 k = 0;
            while (k < samp.size() && std::memcmp(samp[k].data(), want.data(), 8)) k++;
            if (k == samp.size()) samp.push_back(want);
            if (k != es) remapped++;
            put16(st + 2, u16(k));
        }
    }

    align(32);
    const u32 newTexTable = u32(out.size());
    for (u32 o : texOffs) {
        out.resize(out.size() + 4);
        put32(&out[out.size() - 4], o);
    }
    align(32);
    const u32 newSamp = u32(out.size());
    for (const auto& s : samp) out.insert(out.end(), s.begin(), s.end());
    align(32);
    put16(&out[0x20], u16(texOffs.size()));
    put32(&out[0x60], newTexTable);
    put16(&out[0x24], u16(samp.size()));
    put32(&out[0x6C], newSamp);

    // Host-backed memory above MEM1, like the model viewer's heap: the copies stay for the
    // whole session (one per target layout, at most a few), and the game never frees an
    // .mdl it got from the model registry.
    const u32 g = (sNext + 31) & ~31u;
    if (g + out.size() > kCopyEnd) {
        log("[early luigi] no room for another early model copy");
        return 0;
    }
    sNext = g + u32(out.size());
    std::memcpy(hostp(g), out.data(), out.size());
    static const u32 sRelocate = guestSymbol("fn_80061B40");
    const Ctx saved = c;
    guestCall(c, sRelocate, g);
    c = saved;
    log("[early luigi] early model built at %08X (%zu bytes): %zu textures, %zu samplers (target %u/%u), %u material stages re-pointed",
        g, out.size(), texOffs.size(), samp.size(), tTexCount, tSampCount, remapped);
    return g;
}

// Content key of a target's texture/sampler layout (what the early copy depends on).
u64 layoutKey(u32 target) {
    u64 h = 1469598103934665603ull;
    auto mix = [&](const u8* p, u32 n) {
        for (u32 i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    };
    const u32 nt = rd16(target + 0x20), ns = rd16(target + 0x24);
    for (u32 t = 0; t < nt; t++) {
        const u8* th = static_cast<const u8*>(hostp(rd32(rd32(target + 0x60) + 4 * t)));
        mix(th, 0x20 + texBytes(th[0], be16(th + 2), be16(th + 4)));
    }
    mix(static_cast<const u8*>(hostp(rd32(target + 0x6C))), 8 * ns);
    return h ^ (u64(nt) << 48) ^ (u64(ns) << 32);
}

std::map<u64, u32> sBuilt;  // target layout -> early copy (0 = failed)

// The early copy for a Luigi .mdl, or 0 when `mdl` is something else.
u32 earlyFor(Ctx& c, u32 mdl, const char* what) {
    if (!luigiSkeleton(mdl)) return 0;
    const u64 key = layoutKey(mdl);
    auto it = sBuilt.find(key);
    if (it == sBuilt.end()) it = sBuilt.emplace(key, buildFor(c, mdl)).first;
    static int sLogged = 0;
    if (it->second && sLogged++ < 40) log("[early luigi] %s %08X -> early model %08X", what, mdl, it->second);
    return it->second;
}

}  // namespace

// A relocated .mdl with Luigi's skeleton (gameplay Luigi, dluige01/03, or an early copy of one).
bool isLuigiSkeletonMdl(u32 mdl) { return luigiSkeleton(mdl); }

// After fn_80061C94 (model registry setup from a loaded archive: map setup / async model
// loads, including the gameplay Luigi and the dluige cutscene Luigis) has filled `entry`.
// Called from that function's hook in unused_models.cpp.
void earlyLuigiModelLoaded(Ctx& c, u32 entry) {
    if (!enabled() || entry < kModelRegistry || entry >= kModelRegistry + kEntrySize * kNumModels) return;
    if ((entry - kModelRegistry) % kEntrySize) return;
    const u32 mdl = rd32(entry + 8);
    if (const u32 e = earlyFor(c, mdl, inMem1(rd32(entry)) ? gstr(rd32(entry)) : "model")) wr32(entry + 8, e);
}

}  // namespace recomp
