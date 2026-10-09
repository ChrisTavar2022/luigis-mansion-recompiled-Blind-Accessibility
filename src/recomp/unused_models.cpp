// Unused character models restored as F1 > Enhancements > Unused content options (off by
// default): the early (E3 2001) Poltergust body, the early Bogmire and the green Toad.
// Docs: docs/UNUSED_CONTENT.md.
//
// Character types (lbl_8033B590, 0x1C each: +0 model index, +0x10 texture-set index, +0x18
// name) point into the model registry (lbl_8034E46C, 0x34 each: +0 file name, +8 relocated
// .mdl once loaded). Map setup (fn_8000C188 -> fn_8000BFF0 -> fn_8006227C) clears the registry
// and loads the models the map needs: from the resident game archive when it has
// `model/<name>.arc`, otherwise from the loose disc file `/model/<name>.szp`.
#include "port/install.h"
#include "port/settings.h"
#include "recomp/hle.h"
#include "recomp/rarc.h"
#include "recomp/runtime.h"
#include "recomp/version.h"

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

// ---- Model swaps: early Poltergust (vbody) and early Bogmire (shadow) ------------------------
// fn_80061C94(archive, registry entry) sets up a loaded model archive: the /key files into the
// entry's key list (+0x10, archive order), then the .mdl into +8 (relocated by fn_80061B40;
// keys by fn_80061A24). It runs for resident archives directly from map setup and for disc
// archives from the DVD callback. With an option on, after it finishes the entry is pointed
// at a relocated copy of the unused model, and each key slot at the unused archive's key that
// plays the same role (by name); the other files of the retail archive stay in use.
//
// Early Poltergust: two `vbody` models exist. Retail loads `model/vbody.arc` from the resident
// game archive (game_usa.szp, 34808-byte .mdl): the rounded red tank with the big silver wheel.
// The loose disc file `/model/vbody.szp` is never read, because the resident copy wins: its
// .mdl (25764 bytes) is the E3 2001 Poltergust, a red canister with dark end caps on a base
// with four black wheels. Both have 4 joints; the E3 archive's own keys (same names, minus the
// retail flashlight pose lght_vby) replace the retail ones.
//
// Early Bogmire: `/model/shadow.szp` (registry model 129) is the early Bogmire; no character
// type uses it. Retail Bogmire is type 213 `bfire` (model 130, /model/bfire.szp). The early
// model has 42 joints against bfire's 41, so bfire's keys cannot drive it; every bfire key
// slot gets the shadow key for the same action: appear -> syutugen (appear), dappear -> kieru
// (vanish), wait/dwait -> wait, nigeru/nigeru2 (pulling against the vacuum) -> kinko (the
// 12-frame tug of war, same length), biribiri and suikomi by name.
constexpr u32 kModelRegistry = 0x8034E46C;

struct KeyMap {
    const char* retail;
    const char* early;
};
struct ModelSwap {
    const char* what;
    u32 model;                // registry index being replaced
    const char* retailDisc;   // archive whose /key order the registry follows
    const char* retailInner;  // ... nested in it (resident archive), or nullptr
    const char* earlyDisc;    // archive with the unused model and keys
    const char* earlyMdl;
    const KeyMap* keys;       // retail key -> early key; nullptr: same names
    bool (*enabled)();
    // filled on first use
    bool tried = false;
    u32 mdl = 0;
    std::vector<u32> keySlots;  // per registry key slot: relocated early key, or 0 (keep retail)
};

bool earlyPoltergustOn() {
    static const bool sEnv = envFlag("LM_EARLY_POLTERGUST");
    return sEnv || port::settings().enhancements.earlyPoltergust;
}
bool earlyBogmireOn() {
    static const bool sEnv = envFlag("LM_EARLY_BOGMIRE");
    return sEnv || port::settings().enhancements.earlyBogmire;
}

const KeyMap kBogmireKeys[] = {
    {"appear.key", "syutugen.key"}, {"biribiri.key", "biribiri.key"}, {"dappear.key", "kieru.key"},
    {"dwait.key", "wait.key"},      {"nigeru.key", "kinko.key"},      {"nigeru2.key", "kinko.key"},
    {"suikomi.key", "suikomi.key"}, {"wait.key", "wait.key"},         {nullptr, nullptr},
};

ModelSwap sSwaps[] = {
    {"early Poltergust", 3, "/Game/game_usa.szp", "model/vbody.arc", "/model/vbody.szp", "model/vbody.mdl", nullptr,
     earlyPoltergustOn},
    {"early Bogmire", 130, "/model/bfire.szp", nullptr, "/model/shadow.szp", "model/shadow.mdl", kBogmireKeys,
     earlyBogmireOn},
};

u32 guestCopy(Ctx& c, const rarc::Bytes& data, const char* relocator) {
    const u32 g = hleAlloc(u32(data.size()), 32);
    std::memcpy(hostp(g), data.data(), data.size());
    const Ctx saved = c;
    guestCall(c, guestSymbol(relocator), g);
    c = saved;
    return g;
}

const rarc::Node* keyDir(rarc::Archive& arc) {
    const rarc::Item* k = arc.find("key");
    return k && k->dir ? &arc.nodes[size_t(k->node)] : nullptr;
}

bool prepare(Ctx& c, ModelSwap& s) {
    if (s.tried) return s.mdl != 0;
    s.tried = true;
    rarc::Archive early, outer, retail;
    if (!early.parse(rarc::decompress(rarc::readDisc(s.earlyDisc)))) {
        log("[unused] %s: %s is not a RARC archive", s.what, s.earlyDisc);
        return false;
    }
    const rarc::Item* mdl = early.find(s.earlyMdl);
    if (!mdl || mdl->data.size() < 0x80) {
        log("[unused] %s: %s not found", s.what, s.earlyMdl);
        return false;
    }
    bool ok = outer.parse(rarc::decompress(rarc::readDisc(s.retailDisc)));
    if (ok && s.retailInner) {
        const rarc::Item* a = outer.find(s.retailInner);
        ok = a && retail.parse(rarc::decompress(a->data));
    }
    rarc::Archive& r = s.retailInner ? retail : outer;
    const rarc::Node* rk = ok ? keyDir(r) : nullptr;
    const rarc::Node* ek = keyDir(early);
    if (!rk || !ek) {
        log("[unused] %s: key directories not found", s.what);
        return false;
    }
    for (const rarc::Item& k : rk->items) {
        std::string want = k.name;
        if (s.keys) {
            want.clear();
            for (const KeyMap* m = s.keys; m->retail; m++)
                if (k.name == m->retail) want = m->early;
        }
        u32 g = 0;
        for (const rarc::Item& e : ek->items)
            if (!want.empty() && e.name == want) g = guestCopy(c, e.data, "fn_80061A24");
        s.keySlots.push_back(g);
        log("[unused] %s: key slot %zu %s -> %s", s.what, s.keySlots.size() - 1, k.name.c_str(),
            g ? want.c_str() : "(retail)");
    }
    s.mdl = guestCopy(c, mdl->data, "fn_80061B40");
    log("[unused] %s: %s loaded (%zu bytes)", s.what, s.earlyMdl, mdl->data.size());
    return true;
}

}  // namespace

void earlyLuigiModelLoaded(Ctx& c, u32 entry);  // early_luigi.cpp

// Model archive setup (archive, registry entry), USA code only.
HLE(fn_80061C94) {
    const u32 entry = c.r[4];
    callOriginal(c, "fn_80061C94");
    earlyLuigiModelLoaded(c, entry);
    if (port::usCodeFeature() != port::Avail::Yes || entry < kModelRegistry) return;
    if ((entry - kModelRegistry) % 0x34 || !rd32(entry + 8)) return;  // not a finished load
    const u32 model = (entry - kModelRegistry) / 0x34;
    for (ModelSwap& s : sSwaps) {
        if (s.model != model || !s.enabled() || !prepare(c, s)) continue;
        wr32(entry + 8, s.mdl);
        const u32 count = rd32(entry + 0xC), list = rd32(entry + 0x10);
        if (count == s.keySlots.size() && list) {
            for (u32 i = 0; i < count; i++)
                if (s.keySlots[i] && rd32(list + 4 * i)) wr32(list + 4 * i, s.keySlots[i]);
        } else {
            log("[unused] %s: %u key slots, expected %zu; keys left alone", s.what, count, s.keySlots.size());
        }
        log("[unused] %s in use", s.what);
    }
}

namespace {

// ---- Green Toad (kinopio2) ------------------------------------------------------------------
// Types 42 `kinopio` and 43 `kinopio2` both use model 0x49 (/model/kinopio.szp), whose
// archive carries two texture sets (txp/kinopio1.txp, kinopio2.txp; loaded per model by
// fn_80187B24). The type's +0x10 picks the set when an actor is set up (fn_80067B04 ->
// fn_80187974): kinopio has 0 (red), the never-placed kinopio2 has 1 (green). TCRF's NTSC-U
// code `0433BA28 00000049 / 0033BA31 00000002 / 0433BA38 00000001` writes kinopio's model
// index, its +9 byte and +0x10; the first two already hold those values in the USA DOL, so the
// effect is +0x10 = 1: every Toad uses the green set. The port writes it at each map setup
// (for Toads set up after the model has loaded) and also fixes up live Toads every frame
// (unusedModelsFrame), because the code alone misses the Toads placed at map load.
#define kKinopioTexSet VA(0x8033BA38)  // lbl_8033B590 + 42 * 0x1C + 0x10
#define kTypeTableVA VA(0x8033B590)    // character types
#define kTxpTableVA VA(0x80389B68)     // per-model texture-pattern lists, 0x40 each
#define kActorSlotsVA VA(0x803D48A0)   // live actors (slot 0 = Luigi)
constexpr u32 kTypeKinopio = 42;
constexpr u32 kActorSlotCount = 128;  // lbl_803D48A0 is 0x200 bytes
constexpr u32 kTexSetRetail = 0, kTexSetGreen = 1;

bool inMem1(u32 a) { return a >= 0x80000000u && a < 0x81800000u; }

// Dialogue speaker icons: fn_80041DB8 loads the 19 textures named by the pointer table at
// lbl_80305148 + 0x360 into 0x18-byte sprites at lbl_803CE7D0 + 0xA90. Despite the names,
// entry 16 (kino_ri.bti) is the red Toad every Toad uses and entry 15 (kino_icon.bti) is a
// green-capped Toad that is loaded but never shown. With the option on, the red sprite's
// texture object is replaced by the green one (and put back when it is turned off).
#define kIconNamesVA VA(0x803054A8)
#define kIconSpritesVA VA(0x803CE7D0)
constexpr u32 kIconSpriteOff = 0xA90, kIconSpriteSize = 0x18, kIconToad = 16, kIconGreenToad = 15;

void swapToadIcon(bool green) {
    static u32 sSaved = 0;  // the game's kino_icon texture object while swapped
    static int sChecked = -1;  // the table holds the names we expect
    if (!kIconNamesVA || !kIconSpritesVA) return;
    if (sChecked < 0) {
        const char* a = gstr(rd32(kIconNamesVA + 4 * kIconToad));
        const char* b = gstr(rd32(kIconNamesVA + 4 * kIconGreenToad));
        sChecked = std::strstr(a, "kino_ri") && std::strstr(b, "kino_icon");
        if (!sChecked) log("[unused] dialogue icon table: unexpected names '%s' / '%s'", a, b);
    }
    if (!sChecked) return;
    // Sprite: +0x0C colour, +0x10 texture object (fn_8003BDD4 builds it from the .bti).
    const u32 toad = kIconSpritesVA + kIconSpriteOff + kIconSpriteSize * kIconToad + 0x10;
    const u32 greenTex = rd32(kIconSpritesVA + kIconSpriteOff + kIconSpriteSize * kIconGreenToad + 0x10);
    if (!inMem1(greenTex)) return;  // not loaded yet
    const u32 cur = rd32(toad);
    if (green && cur != greenTex) {  // first time, or the game reloaded its icons
        sSaved = cur;
        wr32(toad, greenTex);
        log("[unused] green Toad: dialogue icon kino_icon (green)");
    } else if (!green && sSaved && cur == greenTex) {
        wr32(toad, sSaved);
        sSaved = 0;
    }
}

}  // namespace

// Once per gameplay frame (GBH HUD hook, both versions). The type's texture set only reaches
// an actor when its model's .txp files were already loaded when the actor was set up
// (fn_80067B04/fn_80067F84 -> fn_80187974 -> S3DModel +0xA4). Toad's archive is a loose disc
// file loaded asynchronously, so the Toads placed at map load are usually set up first and
// keep +0xA4 = 0 (the model's own red texture). Each Toad's S3DModel therefore gets the
// green pattern here once it is loaded, exactly the value the game's own setup would store;
// turning the option off puts back what was there.
void unusedModelsFrame() {
    static const bool sEnv = envFlag("LM_GREEN_TOAD");
    const bool green = sEnv || port::settings().enhancements.greenToad;
    swapToadIcon(green);
    static bool sApplied = false;
    if (!green && !sApplied) return;
    const u32 typeEntry = kTypeTableVA + 0x1C * kTypeKinopio;
    const u32 txpRec = kTxpTableVA + (rd32(typeEntry) << 6);  // per-model .txp list (fn_80187974)
    if (!kTypeTableVA || !kTxpTableVA || !kActorSlotsVA) return;
    const u32 count = rd32(txpRec + 0x20), list = rd32(txpRec + 0x24);
    if (count <= kTexSetGreen || !inMem1(list)) return;
    const u32 greenTxp = rd32(list + 4 * kTexSetGreen);
    for (u32 i = 0; i < kActorSlotCount; i++) {
        const u32 a = rd32(kActorSlotsVA + 4 * i);
        if (!inMem1(a) || rd32(a + 0x3C) != kTypeKinopio) continue;
        const u32 m = rd32(a + 4);
        if (!inMem1(m)) continue;
        const u32 cur = rd32(m + 0xA4);
        if (green && cur != greenTxp) {
            wr32(m + 0xA4, greenTxp);
            log("[unused] green Toad: actor %08X texture pattern %08X -> %08X", a, cur, greenTxp);
        } else if (!green && cur == greenTxp) {
            wr32(m + 0xA4, 0);
        }
    }
    sApplied = green;
}

// Map setup, every map (fn_8000C188 hook in dev_tools.cpp calls this first).
void unusedModelsMapSetup() {
    static const bool sEnv = envFlag("LM_GREEN_TOAD");
    const bool green = sEnv || port::settings().enhancements.greenToad;
    const u32 a = kKinopioTexSet;
    if (!a) return;
    const u32 cur = rd32(a);
    if (cur != kTexSetRetail && cur != kTexSetGreen) {  // not the table we expect
        static bool sWarned = false;
        if (!sWarned) sWarned = true, log("[unused] kinopio texture set is %08X, left alone", cur);
        return;
    }
    const u32 want = green ? kTexSetGreen : kTexSetRetail;
    if (cur != want) {
        wr32(a, want);
        log("[unused] Toad texture set %u (%s)", want, green ? "green, kinopio2" : "retail");
    }
}

}  // namespace recomp
