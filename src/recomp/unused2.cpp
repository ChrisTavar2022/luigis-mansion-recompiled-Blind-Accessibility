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

void onMapSetup(Ctx& c) {
    statesMapSetup(c);
    placementsMapSetup(c);
}

void onFrame(Ctx& c) {
    marioFrame(c);
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

void gameplaySection() {}
void debugSection() {}
void soundSection() {}
void drawOverlays() {}

}  // namespace recomp::unused2
