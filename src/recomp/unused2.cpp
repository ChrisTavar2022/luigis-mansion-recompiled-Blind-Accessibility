// Second batch of restored unused content: shared wiring and the F1 menu sections
// (docs/UNUSED_CONTENT.md, "Second batch"). The features live in their own files
// (play_as_mario.cpp, luigi_states.cpp, jukebox.cpp, gallery.cpp, text_archive.cpp,
// placements.cpp, spawn.cpp); this file calls them from the existing hooks and draws their options.
#include "recomp/unused2.h"

#include "port/install.h"
#include "port/settings.h"
#include "recomp/coop.h"
#include "recomp/hle.h"
#include "recomp/runtime.h"
#include "recomp/version.h"

#include <imgui.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace recomp::unused2 {

namespace {
constexpr u32 kExtraBase = 0x81820000, kExtraEnd = 0x81F00000;
u32 sExtraNext = kExtraBase;
std::mutex sExtraM;
bool sDirty = false;

void tip(const char* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", text);
}
void note(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", text);
}
}  // namespace

u32 extraAlloc(u32 size, u32 align) {
    std::lock_guard lk(sExtraM);
    const u32 p = (sExtraNext + align - 1) & ~(align - 1);
    if (p + size > kExtraEnd) {
        log("[unused2] extra guest memory exhausted (%u bytes wanted)", size);
        return 0;
    }
    sExtraNext = p + size;
    std::memset(hostp(p), 0, size);
    return p;
}

// ---- disc overlay ----------------------------------------------------------------------------
// Files served rebuilt while an option asks for them. The decision is made when the game opens
// the file, so turning an option on or off applies the next time the file is loaded.
namespace {
constexpr s32 kOverlayBase = 0x50000000;  // virtual FST entries (HM 0x40000000, rooms 0x48000000, maps 0x4C000000)
struct Served {
    const char* path;
    std::vector<u8> (*build)();
    u32 (*wanted)();  // 0: serve the disc file; otherwise a key of the options the copy depends on
    std::vector<u8> bytes;
    u32 builtKey = 0;
};
u32 labEvent26Wanted() {
    static const bool e = std::getenv("LM_LAB_EVENT26") != nullptr;
    return (e || port::settings().unused2.labEvent26) && port::usCodeFeature() == port::Avail::Yes ? 1 : 0;
}
Served sServed[] = {
    {"/Event/event08.szp", buildLabEvent26, labEvent26Wanted},
    {"/Map/map2.szp", buildMapArchive2, mapArchive2Key},
    {"/Map/map3.szp", buildMapArchive3, mapArchive3Key},
};
std::mutex sServedM;
}  // namespace

s32 overlayEntry(const char* guestPath) {
    std::lock_guard lk(sServedM);
    for (size_t i = 0; i < std::size(sServed); i++) {
        Served& s = sServed[i];
        if (std::strcmp(guestPath, s.path)) continue;
        const u32 key = s.wanted();
        if (!key) continue;
        if (s.builtKey != key) {
            s.builtKey = key;
            s.bytes = s.build();
        }
        if (s.bytes.empty()) return -1;
        log("[unused2] serving rebuilt %s", s.path);
        return kOverlayBase + s32(i);
    }
    return -1;
}
bool isOverlayEntry(s32 entry) { return entry >= kOverlayBase && entry < kOverlayBase + s32(std::size(sServed)); }
u32 overlayLength(s32 entry) {
    std::lock_guard lk(sServedM);
    return isOverlayEntry(entry) ? u32(sServed[entry - kOverlayBase].bytes.size()) : 0;
}
s32 overlayRead(s32 entry, void* dst, s32 length, s32 offset) {
    std::lock_guard lk(sServedM);
    if (!isOverlayEntry(entry)) return -1;
    const std::vector<u8>& b = sServed[entry - kOverlayBase].bytes;
    if (offset < 0 || length < 0 || size_t(offset) > b.size()) return -1;
    const size_t n = std::min(size_t(length), b.size() - size_t(offset));
    std::memcpy(dst, b.data() + offset, n);
    if (n < size_t(length)) std::memset(static_cast<u8*>(dst) + n, 0, size_t(length) - n);
    return s32(n);
}

void onMapSetup(Ctx& c) {
    statesMapSetup(c);
    placementsMapSetup(c);
}

void onFrame(Ctx& c) {
    static int sBorder = -1;  // patch 50: the bordered 3D view (USA code)
    const int border = port::settings().unused2.borderedView && port::usCodeFeature() == port::Avail::Yes;
    if (border != sBorder) {
        sBorder = border;
        setPatch(50, border != 0);
    }
    marioFrame(c);
    endingFrame(c);
    statesFrame(c);
    jukeboxFrame(c);
    spawnFrame(c);
    placementsFrame(c);
}

void onModelLoaded(Ctx& c, u32 entry) { marioModelLoaded(c, entry); }

void unusedSection() {
    auto& u = port::settings().unused2;
    const bool us = port::usCodeFeature() == port::Avail::Yes;
    ImGui::SeparatorText("Second batch");
    sDirty |= ImGui::Checkbox("Play as Mario", &u.playAsMario);
    tip("The player uses the unused Mario model left in the game (bmario: the character table has a\n"
        "whole Mario player set that nothing places). Mario has Luigi's skeleton, so Luigi's\n"
        "animations, Poltergust, flashlight and Game Boy Horror work unchanged. Mario's model has\n"
        "no eye animations (he does not blink); the voice stays Luigi's (the game has no Mario\n"
        "voice clips for these actions). Also used by the cutscene Luigis.");
    note("next map load");
    {
        ImGui::BeginDisabled(!us);
        sDirty |= ImGui::Checkbox("Bordered view", &u.borderedView);
        tip("The game's view setup has a third, unused layout: a fixed 560x400 3D view (40,30 to\n"
            "560,430) with a black border, like a TV-safe window. The HUD stays where it is.");
        sDirty |= ImGui::Checkbox("Ending pictures of Luigi", &u.endingPictures);
        tip("The ending's rank screen shows one of three unused pictures of Luigi next to its\n"
            "message: two V signs (best rank), one V sign (middle ranks) or sitting with a flower (worst).");
        sDirty |= ImGui::Checkbox("Early E. Gadd lab script (Japanese)", &u.labEvent26);
        tip("Talking to E. Gadd in his lab plays event26, an early Japanese version of the lab\n"
            "script (the Professor is still \"Oya-Mar\"), with its own dialogue in the game's kana\n"
            "font. Its menu always offers the Hidden Mansion, as the early script did. Its debug\n"
            "flag block and its sound effects (an older numbering) are left out.\n"
            "Translation: Debug > Text archive.");
        note("next lab visit");
        sDirty |= ImGui::Checkbox("Mario as player 2", &u.marioP2);
        tip("In the 2-player mode (below), player 2 is Mario (the unused bmario model) instead of a\n"
            "second Luigi. Needs \"2-player mode (unfinished)\".");
        ImGui::EndDisabled();
        if (!us) note(port::availNote(port::usCodeFeature()));
    }
    if (sDirty) {
        port::saveSettings();
        sDirty = false;
    }
}

void gameplaySection() {
    auto& u = port::settings().unused2;
    const bool us = port::usCodeFeature() == port::Avail::Yes;
    ImGui::SeparatorText("Unfinished Luigi states");
    ImGui::TextDisabled("Restored from code left in the game. All off by default.");
    ImGui::BeginDisabled(!us);
    sDirty |= ImGui::Checkbox("Poison mushrooms poison Luigi", &u.poisonState);
    tip("Touching a poison mushroom no longer shrinks Luigi. Instead he plays the unused poisoned\n"
        "animation (doku_lp, with the dark \"poisoned\" eyes), stands still and loses health slowly.\n"
        "The mushroom still takes 10 coins. Poison alone never takes Luigi's last point of health.");
    if (u.poisonState) {
        ImGui::Indent();
        sDirty |= ImGui::SliderFloat("Poison damage", &u.poisonDamage, 1.0f, 100.0f, "%.0f");
        sDirty |= ImGui::SliderFloat("Poison duration", &u.poisonSeconds, 1.0f, 20.0f, "%.1f s");
        ImGui::Unindent();
    }
    sDirty |= ImGui::Checkbox("Crawling fear", &u.crawlFear);
    tip("When a ghost startles Luigi from behind, he falls into the game's unused panic state:\n"
        "about 5 seconds sitting and scooting along the floor (unused cow1wait / cow1move\n"
        "animations), unable to use the Poltergust, taking half damage, then he gets up.\n"
        "The game's fear system has this mode but nothing ever triggers it.");
    sDirty |= ImGui::Checkbox("Vacuum overheat", &u.overheat);
    tip("Cut feature: vacuuming too long overheats the Poltergust. The heat shows on the element\n"
        "meter (fire colours) while it is above zero. When it is full the Poltergust catches fire,\n"
        "Luigi is burned (the game's own on-fire state) and the vacuum stops working until it\n"
        "has cooled to half; the meter shakes meanwhile. Not vacuuming cools it, holding L\n"
        "(blowing air or spraying) cools it faster.");
    if (u.overheat) {
        ImGui::Indent();
        sDirty |= ImGui::SliderFloat("Time to overheat", &u.overheatFillSeconds, 5.0f, 120.0f, "%.0f s of vacuuming");
        sDirty |= ImGui::SliderFloat("Cool-down time", &u.overheatCoolSeconds, 5.0f, 120.0f, "%.0f s (full to empty)");
        sDirty |= ImGui::SliderFloat("Cooling while blowing (L)", &u.overheatBlowCool, 1.0f, 10.0f, "%.1fx faster");
        sDirty |= ImGui::SliderInt("Burn damage", &u.overheatDamage, 0, 50);
        ImGui::Unindent();
    }
    ImGui::EndDisabled();
    if (!us) note(port::availNote(port::usCodeFeature()));
    if (sDirty) {
        port::saveSettings();
        sDirty = false;
    }
}
void debugSection() {
    ImGui::SeparatorText("Unused content viewers");
    galleryButton();
    ImGui::SameLine();
    textArchiveButton();
}
void drawOverlays() {
    galleryWindow();
    textArchiveWindow();
}

}  // namespace recomp::unused2
