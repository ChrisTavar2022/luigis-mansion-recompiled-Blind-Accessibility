// PAL Hidden Mansion rules on the US recompiled build -- prototype (see hidden_mansion.h and
// docs/HIDDEN_MANSION.md). Original port code: it reads the game's own flag, serves files
// from an overlay the user builds from their own discs, and adjusts a few literals.
#include "recomp/hidden_mansion.h"

#include "port/install.h"
#include "port/options.h"
#include "port/settings.h"
#include "recomp/hle.h"
#include "recomp/version.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <span>
#include <vector>

namespace recomp::hm {
namespace {

// The game's hidden-mansion flag and sCurMapNo (USA addresses, found with tools/pal_diff.py
// `var pal 804C5784`; VA() gives the running game's: PAL 0x804C5784 / 0x804C578C).
#define kUraFlag VA(0x804D80C0)
#define kCurMapNo VA(0x804D80C8)

// On the USA game the PAL rules are added by the overlay and the hooks below; on the European
// game they are its own, and the USA rules are restored by the reverse overlay and hooks
// (docs/SETUP.md, "USA Hidden Mansion on the European game").
bool usBase() { return port::install().running == port::GameVersion::USA; }

// LM_HM_OVERLAY, else the overlay setup built in the data directory, else the first
// orig/hidden_mansion (tools/hidden_mansion_data.py) found walking up from the working
// directory (same search as the disc image).
std::string overlayDir() {
    static const std::string dir = [] {
        const char* env = std::getenv("LM_HM_OVERLAY");
        std::filesystem::path d;
        if (env && *env) {
            d = env;
        } else if (std::filesystem::exists(port::hiddenMansionOverlayDir() / "manifest.txt")) {
            d = port::hiddenMansionOverlayDir();  // built by setup (docs/SETUP.md)
        } else {
            for (auto p = std::filesystem::current_path();; p = p.parent_path()) {
                if (std::filesystem::exists(p / "orig" / "hidden_mansion" / "manifest.txt")) {
                    d = p / "orig" / "hidden_mansion";
                    break;
                }
                if (p == p.parent_path()) {
                    d = "orig/hidden_mansion";
                    break;
                }
            }
        }
        return d.generic_string() + "/";
    }();
    return dir;
}

struct Redirect {
    const char* guestPath;  // as the game spells it
    const char* overlayFile;
    bool onlyWhileHidden;  // map data: only while the ura flag is set
};

// Mirrors the manifest written by tools/hidden_mansion_data.py / setup (hm_overlay.cpp).
// PAL rules on the USA game:
constexpr Redirect kRedirects[] = {
    {"/Map/map2.szp", "Map/map2u.szp", true},  // PAL MissionMode::init: "%s%du.szp" when ura && map 2
    {"/Event/event08.szp", "Event/event08.szp", false},
    {"/Event/event16.szp", "Event/event16.szp", false},
    {"/Event/event73.szp", "Event/event73.szp", false},
};
// USA rules on the European game (the reverse): the USA map data for the hidden mansion and
// the European event archives with the USA scripts.
constexpr Redirect kRedirectsUsa[] = {
    {"/Map/map2u.szp", "usa/Map/map2.szp", true},
    {"/Event/event08.szp", "usa/Event/event08.szp", false},
    {"/Event/event16.szp", "usa/Event/event16.szp", false},
    {"/Event/event73.szp", "usa/Event/event73.szp", false},
};

bool samePath(const char* a, const char* b) {
    // DVD paths are case-insensitive; the game sometimes omits the leading slash.
    if (*a == '/') ++a;
    if (*b == '/') ++b;
    for (; *a && *b; ++a, ++b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = char(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = char(y - 'A' + 'a');
        if (x != y) return false;
    }
    return *a == *b;
}

struct OverlayFile {
    std::string hostPath;
    std::vector<u8> bytes;
    bool loaded = false;
};
constexpr s32 kEntryBase = 0x40000000;  // well above any real FST entry
std::mutex sOverlayMutex;
std::vector<OverlayFile> sOverlay;

OverlayFile* entryFile(s32 entry) {
    const s32 i = entry - kEntryBase;
    return (i >= 0 && i < s32(sOverlay.size())) ? &sOverlay[size_t(i)] : nullptr;
}

bool ensureLoaded(OverlayFile& f) {
    if (f.loaded) return !f.bytes.empty();
    f.loaded = true;
    std::ifstream in(f.hostPath, std::ios::binary);
    if (!in) {
        log("[hm] overlay file missing: %s (run tools/hidden_mansion_data.py)", f.hostPath.c_str());
        return false;
    }
    f.bytes.assign(std::istreambuf_iterator<char>(in), {});
    return !f.bytes.empty();
}

// ---- constant patches ----
struct ConstPatch {
    u32 addr;      // US .sdata2 literal, referenced by exactly one function
    float usValue;
    float palValue;
    bool partial;  // PAL also changes a code path that has no literal to patch (needs a hook)
    const char* what;
};

constexpr ConstPatch kConstPatches[] = {
    // KtEnMoku (Chauncey's rocking horses), US fn_800A09D0 / PAL fn_800A3A50: scale x2.
    {0x804DB278, 1.8f, 3.6f, false, "rocking horse scale"},
    // KtEnMoku2, US fn_800A1BCC / PAL fn_800A4DC4: value passed to fn_8008907C 50 -> 100.
    {0x804DB2B0, 50.0f, 100.0f, false, "rocking horse (2) fn_8008907C argument"},
    // (Room ambient x0.2 is a hook on fn_800168EC: hidden_mansion_hooks.cpp.)
};

u32 fbits(float f) { return std::bit_cast<u32>(f); }

}  // namespace

namespace {
Variant initialVariant() {
    const char* env = std::getenv("LM_HIDDEN_MANSION_VARIANT");
    if (!env || !*env) env = std::getenv("LM_HIDDEN_MANSION");
    std::string v = env && *env ? env : port::options().hiddenMansion;
    if (v.empty()) v = port::settings().enhancements.hiddenMansionPal ? "pal" : "us";
    for (char& ch : v) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    // A single-disc install only has its own release's rules (port::hiddenMansionPal/Us).
    bool pal = v != "us";
    if (pal && port::hiddenMansionPal() != port::Avail::Yes) pal = false;
    if (!pal && port::hiddenMansionUs() != port::Avail::Yes) pal = true;
    const bool have = std::filesystem::exists(overlayDir() + (usBase() ? "manifest.txt" : "usa/manifest.txt"));
    log("[hm] Hidden Mansion variant: %s on the %s game (overlay %s%s)", pal ? "PAL" : "USA", usBase() ? "USA" : "European",
        overlayDir().c_str(), have || pal != usBase() ? "" : ": missing, run tools/hidden_mansion_data.py");
    return pal ? Variant::Pal : Variant::Us;
}
std::atomic<int> sVariant{-1};
}  // namespace

Variant variant() {
    int v = sVariant.load();
    if (v < 0) {
        int want = int(initialVariant());
        sVariant.compare_exchange_strong(v, want);
        v = sVariant.load();
    }
    return Variant(v);
}

void setVariant(Variant v) {
    if (sVariant.exchange(int(v)) != int(v)) log("[hm] Hidden Mansion variant -> %s", v == Variant::Pal ? "PAL" : "USA");
}

namespace {
// See hidden_mansion.h, "USA rules on the European game". While sHidden, the flag in guest
// memory reads 0 and its real value is sHiddenSaved.
bool sHidden = false;
u32 sHiddenSaved = 0;
}  // namespace

void callWithoutFlag(Ctx& c, const char* name) {
    if (!sHidden && !usaOnPalActive()) return callOriginal(c, name);
    const bool outer = !sHidden;
    if (outer) {
        sHiddenSaved = rd32(kUraFlag);
        wr32(kUraFlag, 0);
        sHidden = true;
    }
    callOriginal(c, name);
    if (outer && sHidden) {
        // (Nothing inside sets the flag; if something did, keep its value.)
        if (rd32(kUraFlag) == 0) wr32(kUraFlag, sHiddenSaved);
        sHidden = false;
    }
}

void callWithFlag(Ctx& c, const char* name) {
    if (!sHidden) return callOriginal(c, name);
    wr32(kUraFlag, sHiddenSaved);
    sHidden = false;
    callOriginal(c, name);
    sHiddenSaved = rd32(kUraFlag);
    wr32(kUraFlag, 0);
    sHidden = true;
}

bool hiddenActive() { return sHidden ? sHiddenSaved != 0 : rd32(kUraFlag) != 0; }

void onFrame() {
    // LM_HM_FORCE=1: keep the hidden-mansion flag set (testing without a cleared save).
    static const bool sForce = [] {
        const char* env = std::getenv("LM_HM_FORCE");
        return env && *env == '1';
    }();
    if (sForce && !hiddenActive()) {
        log("[hm] LM_HM_FORCE: setting the hidden-mansion flag");
        wr32(kUraFlag, 1);
    }
    static bool sWas = false;
    if (hiddenActive() != sWas) {
        sWas = hiddenActive();
        log("[hm] hidden-mansion flag %s (%s variant)", sWas ? "set" : "cleared", variant() == Variant::Pal ? "PAL" : "USA");
    }
    syncConstants();
}
bool palActive() { return usBase() && variant() == Variant::Pal && hiddenActive(); }
bool usaOnPalActive() { return !usBase() && variant() == Variant::Us && hiddenActive(); }
bool palRulesActive() { return variant() == Variant::Pal && hiddenActive(); }

std::optional<std::string> redirect(const char* guestPath) {
    if (!guestPath) return std::nullopt;
    // Only the variant that is not the running game's own needs data from the other disc.
    const bool reverse = !usBase();
    if (variant() == (reverse ? Variant::Pal : Variant::Us)) return std::nullopt;
    for (const Redirect& r : reverse ? std::span<const Redirect>(kRedirectsUsa) : std::span<const Redirect>(kRedirects)) {
        if (!samePath(guestPath, r.guestPath)) continue;
        if (r.onlyWhileHidden && !(hiddenActive() && rd32(kCurMapNo) == 2)) return std::nullopt;
        return overlayDir() + r.overlayFile;
    }
    return std::nullopt;
}

s32 overlayEntry(const char* guestPath) {
    const auto host = redirect(guestPath);
    if (!host) return -1;
    std::lock_guard lock(sOverlayMutex);
    for (size_t i = 0; i < sOverlay.size(); ++i)
        if (sOverlay[i].hostPath == *host) return kEntryBase + s32(i);
    sOverlay.push_back({*host, {}, false});
    if (!ensureLoaded(sOverlay.back())) {
        sOverlay.pop_back();
        return -1;  // fall back to the US disc file
    }
    log("[hm] %s -> %s", guestPath, host->c_str());
    return kEntryBase + s32(sOverlay.size() - 1);
}

bool isOverlayEntry(s32 entry) {
    std::lock_guard lock(sOverlayMutex);
    return entryFile(entry) != nullptr;
}

u32 overlayLength(s32 entry) {
    std::lock_guard lock(sOverlayMutex);
    const OverlayFile* f = entryFile(entry);
    return f ? u32(f->bytes.size()) : 0;
}

s32 overlayRead(s32 entry, void* dst, s32 length, s32 offset) {
    std::lock_guard lock(sOverlayMutex);
    const OverlayFile* f = entryFile(entry);
    if (!f || offset < 0 || length < 0 || size_t(offset) > f->bytes.size()) return -1;
    const size_t n = std::min(size_t(length), f->bytes.size() - size_t(offset));
    std::memcpy(dst, f->bytes.data() + offset, n);
    // Disc reads are padded to 32 bytes; zero the tail like a read past EOF would.
    if (n < size_t(length)) std::memset(static_cast<u8*>(dst) + n, 0, size_t(length) - n);
    return s32(n);
}

void syncConstants() {
    if (!usBase()) {
        // European game: the hidden mansion's room ambient factor is a .sdata float (PAL-only
        // 0x804BD728, 0.2, read by the PAL-only fn_800179F8 when the flag is set). 1.0 = USA.
        constexpr u32 kPalAmbient = 0x804BD728;
        const u32 want = fbits(variant() == Variant::Us ? 1.0f : 0.2f);
        const u32 cur = rd32(kPalAmbient);
        if (cur != want && (cur == fbits(1.0f) || cur == fbits(0.2f))) wr32(kPalAmbient, want);
        return;
    }
    static bool sApplied = false;
    static bool sChecked = false, sOk = true;
    if (!sChecked) {  // refuse to touch memory that does not hold the expected US literal
        sChecked = true;
        for (const ConstPatch& p : kConstPatches) {
            if (rd32(p.addr) != fbits(p.usValue)) {
                log("[hm] unexpected literal at %08X (%s); constant patches disabled", p.addr, p.what);
                sOk = false;
            }
        }
    }
    if (!sOk) return;
    const bool want = palActive();  // switching variant or leaving the hidden mansion restores US values
    if (want == sApplied) return;
    for (const ConstPatch& p : kConstPatches) wr32(p.addr, fbits(want ? p.palValue : p.usValue));
    sApplied = want;
}

bool mirrorActive() {
    static const bool sOff = [] {  // LM_HM_MIRROR=0 disables the mirror (debugging)
        const char* env = std::getenv("LM_HM_MIRROR");
        return env && *env == '0';
    }();
    return !sOff && palActive();
}

void mirrorProjection(float m[4][4], bool perspective) {
    if (!perspective || !mirrorActive()) return;
    for (int i = 0; i < 4; ++i) m[0][i] = -m[0][i];
}

u32 mirrorCullMode(u32 gxCullMode) {
    if (!mirrorActive()) return gxCullMode;
    if (gxCullMode == 1) return 2;  // GX_CULL_FRONT -> GX_CULL_BACK
    if (gxCullMode == 2) return 1;
    return gxCullMode;
}

float mirrorScreenX(float x, float width) { return mirrorActive() ? width - x : x; }

void mirrorStereo(s16* interleavedStereo, std::size_t frames) {
    if (!mirrorActive() || !interleavedStereo) return;
    for (std::size_t i = 0; i < frames; ++i) std::swap(interleavedStereo[2 * i], interleavedStereo[2 * i + 1]);
}

}  // namespace recomp::hm
