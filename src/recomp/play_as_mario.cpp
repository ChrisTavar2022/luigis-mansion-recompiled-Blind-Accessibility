// "Play as Mario" and "Mario as player 2" (F1 > Enhancements > Unused content, off by default).
//
// The character table (lbl_8033B590) still has a Mario player set that nothing places: type
// 0xB3 `bmario` (/model/bmario.szp) with its own equipment types 0xB5..0xB9 (bvbody, bvhead,
// bwplight, bwpwater, bwpair). Luigi's code has branches for a type-0xB3 player (fn_800AB7xx
// init, the nozzle re-typing in fn_800B36B8, the attach tables of fn_80078008), but the
// equipment types point at the same model files as Luigi's own (/model/vbody.szp etc.), so the
// only Mario-specific asset is the body model.
//
// bmario.mdl has Luigi's skeleton: 65 joints with the same node graph; only the bind pose
// differs slightly (Mario is a little shorter and stockier). So, like the early Luigi model
// (early_luigi.cpp), the swap replaces only the .mdl of a Luigi model-registry entry; all retail
// animations (keys), sound/effect timing tables and the Poltergust/flashlight/Game Boy Horror
// equipment stay Luigi's and line up with Mario's hands and back.
//
// Texture patterns: Luigi's .txp files (blinks, expressions) write *sampler indices* into
// material 4 and 6 stage 0 of whatever .mdl the S3DModel draws (fn_8005CBBC writes straight into
// the model's material table). Mario has 40 materials and no .txp of his own; his materials 4
// and 6 are body parts using sampler 5. The copy therefore gets a sampler table whose first N
// entries (N = the Luigi model's sampler count, so every value a Luigi pattern can write) all
// repeat Mario's sampler 5, followed by Mario's own samplers, and every Mario material is
// re-pointed at its own sampler in that second part. Luigi's patterns then leave Mario's look
// unchanged (Mario does not blink, as his model has no patterns).
//
// Mario as player 2 (2-player mode, coop.cpp): player 2's S3DModel (+4 of the actor, model at
// S3DModel +4) is pointed at the Mario copy each frame; player 1 keeps the registry's Luigi.
// The S3DModel's joint/envelope arrays are sized from Luigi's model (65 + 55) at creation and
// Mario needs fewer (65 + 13), so the swap is safe after construction.
// Docs: docs/UNUSED_CONTENT.md ("Play as Mario").
#include "port/install.h"
#include "port/settings.h"
#include "recomp/coop.h"
#include "recomp/guest_util.h"
#include "recomp/hle.h"
#include "recomp/rarc.h"
#include "recomp/unused2.h"
#include "recomp/version.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

namespace recomp {
bool isLuigiSkeletonMdl(u32 mdl);  // early_luigi.cpp
}

namespace recomp::unused2 {
namespace {

#define kModelRegistry VA(0x8034E46C)  // lbl_8034E46C, 0x34 per model
constexpr u32 kEntrySize = 0x34, kNumModels = 0x106;
constexpr u32 kTxpMatA = 4, kTxpMatB = 6;  // materials Luigi's .txp channels write

bool envFlag(const char* n) {
    const char* v = std::getenv(n);
    return v && *v && *v != '0';
}
bool marioOn() {
    static const bool sEnv = envFlag("LM_PLAY_AS_MARIO");
    return sEnv || port::settings().unused2.playAsMario;
}
bool marioP2On() {
    static const bool sEnv = envFlag("LM_MARIO_P2");
    return sEnv || port::settings().unused2.marioP2;
}

u16 be16(const u8* p) { return u16(p[0] << 8 | p[1]); }
u32 be32(const u8* p) { return u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3]; }
void put16(u8* p, u16 v) { p[0] = u8(v >> 8); p[1] = u8(v); }
void put32(u8* p, u32 v) { p[0] = u8(v >> 24); p[1] = u8(v >> 16); p[2] = u8(v >> 8); p[3] = u8(v); }

const std::vector<u8>& marioMdl() {
    static std::vector<u8> sMdl;
    static bool sTried = false;
    if (sTried) return sMdl;
    sTried = true;
    rarc::Archive a;
    if (!a.parse(rarc::decompress(rarc::readDisc("/model/bmario.szp")))) {
        log("[mario] /model/bmario.szp: not a RARC archive");
        return sMdl;
    }
    const rarc::Item* m = a.find("model/bmario.mdl");
    if (!m || m->data.size() < 0x80 || be32(m->data.data()) != 0x04B40000u || be16(&m->data[8]) != 65) {
        log("[mario] model/bmario.mdl missing or not a 65-joint model");
        return sMdl;
    }
    sMdl = m->data;
    log("[mario] bmario.mdl read (%zu bytes)", sMdl.size());
    return sMdl;
}

// Mario's .mdl laid out for a Luigi target with `n` samplers; 0 on failure.
u32 build(Ctx& c, u32 n) {
    const std::vector<u8>& src = marioMdl();
    if (src.empty()) return 0;
    std::vector<u8> out = src;
    auto align = [&](u32 a) { while (out.size() % a) out.push_back(0); };
    const u32 mSamp = be32(&src[0x6C]), mSampCount = be16(&src[0x24]);
    const u32 matCount = be16(&src[0x28]), mat = be32(&src[0x68]);
    if (matCount <= kTxpMatB) return 0;
    auto stage0 = [&](u32 m) -> int {
        const u8* p = &src[mat + 0x120 * m];
        return p[7] ? be16(p + 0x20 + 2) : -1;
    };
    const int filler = stage0(kTxpMatA);
    if (filler < 0 || u32(filler) >= mSampCount) return 0;
    if (stage0(kTxpMatB) != filler) log("[mario] materials 4/6 use different samplers (%d/%d); 4's is used", filler, stage0(kTxpMatB));
    std::vector<std::array<u8, 8>> samp;
    for (u32 i = 0; i < n; i++) {
        std::array<u8, 8> e;
        std::memcpy(e.data(), &src[mSamp + 8 * u32(filler)], 8);
        samp.push_back(e);
    }
    for (u32 i = 0; i < mSampCount; i++) {
        std::array<u8, 8> e;
        std::memcpy(e.data(), &src[mSamp + 8 * i], 8);
        samp.push_back(e);
    }
    for (u32 m = 0; m < matCount; m++) {
        u8* p = &out[mat + 0x120 * m];
        const u32 stages = p[7] > 8 ? 8 : p[7];
        for (u32 s = 0; s < stages; s++) put16(p + 0x20 + 0x20 * s + 2, u16(n + be16(p + 0x20 + 0x20 * s + 2)));
    }
    align(32);
    const u32 table = u32(out.size());
    for (const auto& s : samp) out.insert(out.end(), s.begin(), s.end());
    align(32);
    put16(&out[0x24], u16(samp.size()));
    put32(&out[0x6C], table);
    const u32 g = extraAlloc(u32(out.size()), 32);
    if (!g) return 0;
    std::memcpy(hostp(g), out.data(), out.size());
    static const u32 sRelocate = guestSymbol("fn_80061B40");
    gu::call(c, sRelocate, {g});
    log("[mario] Mario model built at %08X for %u Luigi samplers (%zu samplers)", g, n, samp.size());
    return g;
}

std::map<u32, u32> sBuilt;  // Luigi sampler count -> Mario copy

u32 marioFor(Ctx& c, u32 luigiMdl) {
    if (!isLuigiSkeletonMdl(luigiMdl)) return 0;
    const u32 n = rd16(luigiMdl + 0x24);
    auto it = sBuilt.find(n);
    if (it == sBuilt.end()) it = sBuilt.emplace(n, build(c, n)).first;
    return it->second;
}

bool isMarioCopy(u32 mdl) {
    for (const auto& [n, g] : sBuilt)
        if (g && g == mdl) return true;
    return false;
}

}  // namespace

// After fn_80061C94 filled a registry entry (and early_luigi.cpp may have swapped it).
void marioModelLoaded(Ctx& c, u32 entry) {
    if (!marioOn() || entry < kModelRegistry || entry >= kModelRegistry + kEntrySize * kNumModels) return;
    if ((entry - kModelRegistry) % kEntrySize) return;
    const u32 mdl = rd32(entry + 8);
    if (const u32 m = marioFor(c, mdl)) {
        wr32(entry + 8, m);
        static int sLogged = 0;
        if (sLogged++ < 20) log("[mario] registry %s: Mario model %08X", gu::inMem1(rd32(entry)) ? gstr(rd32(entry)) : "?", m);
    }
}

// Vertex morphs (S3DModel +0xAC + 12 * slot, 8 slots: weight, morph data, key; fn_8005C760 runs
// fn_8005C464 for each). Luigi's face and mouth shapes (lip sync, expressions) are morph targets
// of luige.mdl: per target a list of *Luigi's* vertex indices and the positions to blend there,
// written straight into the drawn model's position array (S3DModel +4 = .mdl, +0x48 positions).
// On Mario's model those indices name unrelated vertices, so every morph frame overwrote parts
// of Mario's cap, hair, face and back with Luigi's face positions: holes in the cap, hair through
// the face, spikes out of the back, worse each frame (the writes are absolute and stick, the copy
// is shared by every Mario draw). Mario has no morph targets of his own, so his model is left
// out of the morph pass (his face keeps its modelled shape, as his eyes keep theirs).
HLE(fn_8005C760) {
    const u32 s3d = c.r[3];
    if (gu::inMem1(s3d) && isMarioCopy(rd32(s3d + 4))) return;
    callOriginal(c, "fn_8005C760");
}

// Every frame: player 2's model in the 2-player mode.
void marioFrame(Ctx& c) {
    static bool sApplied = false;
    const bool want = marioP2On() && coop::active();
    if (!want && !sApplied) return;
    const u32 p2 = coop::active() ? coop::player(c, 1) : 0;
    if (!p2) return;
    const u32 s3d = rd32(p2 + 4);
    if (!gu::inMem1(s3d)) return;
    const u32 cur = rd32(s3d + 4);
    if (want) {
        if (isMarioCopy(cur)) return;
        if (const u32 m = marioFor(c, cur)) {
            wr32(s3d + 4, m);
            sApplied = true;
            static int sLogged = 0;
            if (sLogged++ < 3) log("[mario] player 2 (%08X) is Mario", p2);
        }
    } else if (isMarioCopy(cur) && !marioOn()) {
        wr32(s3d + 4, rd32(kModelRegistry + 8));  // back to the registry's Luigi
        sApplied = false;
    }
}

}  // namespace recomp::unused2
