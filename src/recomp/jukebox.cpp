// Jukebox (F1 > Sound > Jukebox): plays any of the sequenced music tracks through the game's own
// music player. Nothing plays until the player presses a Play button.
//
// Tracks: the BARC block of /AudioRes/JaiInit.aaf (sequence archive JaiArcS.arc) lists 89
// sequences; entry 0 is the sound-effect sequence LuigiSe.bms, the other 88 are music
// (names cut to 13 characters by the format). Some entries point at the same sequence data as an
// earlier one (aliases, e.g. 9 Mario.com = 5). The game starts music with fn_80188760(0x80000000 |
// index) (e.g. 0x12 GoodNight when Luigi's health runs out); event scripts use <BGM>(n), which
// maps n through the table lbl_8038EAB4 (54 entries) in fn_8018A470. fn_8018A570 stops the
// current music.
//
// "Not heard in retail" marks the tracks that no event script's <BGM> command, no constant music
// id in the code (fn_80188760 / fn_801889FC calls) and no data table selects (USA analysis,
// docs/UNUSED_CONTENT.md "Jukebox"); a track reached only through a computed id could be missed.
#include "port/install.h"
#include "recomp/guest_util.h"
#include "recomp/hle.h"
#include "recomp/rarc.h"
#include "recomp/skip.h"
#include "recomp/unused2.h"
#include "recomp/version.h"

#include <imgui.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace recomp::unused2 {
namespace {

struct Track {
    int index;
    std::string name;
    u32 offset;  // sequence data offset in JaiArcS.arc
    int aliasOf = -1;
};

std::vector<Track> sTracks;
std::once_flag sLoad;
std::atomic<int> sPlay{-1};
std::atomic<bool> sStop{false};
int sNowPlaying = -1;
bool sUnusedOnly = false;

// Music ids the retail game selects (USA): event <BGM> table entries the scripts use, constant
// ids in code, data tables (see the file comment). Everything else is listed as not heard.
const int kUsed[] = {0x01, 0x02, 0x03, 0x04, 0x07, 0x08, 0x0C, 0x0E, 0x0F, 0x10, 0x12, 0x13, 0x14, 0x15, 0x16,
                     0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x21, 0x22, 0x24, 0x25, 0x26, 0x27, 0x29,
                     0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
                     0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x47, 0x48, 0x49,
                     0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x50, 0x51, 0x52, 0x53, 0x57, 0x58};

bool usedInRetail(const Track& t) {
    for (int u : kUsed)
        if (u == t.index) return true;
    return false;
}

void load() {
    const rarc::Bytes aaf = rarc::readDisc("/AudioRes/JaiInit.aaf");
    for (size_t i = 0; i + 0x20 <= aaf.size(); i += 4) {
        if (std::memcmp(&aaf[i], "BARC----", 8)) continue;
        auto be = [&](size_t o) { return u32(aaf[o]) << 24 | u32(aaf[o + 1]) << 16 | u32(aaf[o + 2]) << 8 | aaf[o + 3]; };
        const u32 count = be(i + 0xC);
        for (u32 k = 0; k < count && i + 0x40 + 0x20 * k <= aaf.size(); k++) {
            const size_t e = i + 0x20 + 0x20 * k;
            Track t;
            t.index = int(k);
            t.name.assign(reinterpret_cast<const char*>(&aaf[e]), strnlen(reinterpret_cast<const char*>(&aaf[e]), 14));
            t.offset = be(e + 0x18);
            for (const Track& p : sTracks)
                if (p.offset == t.offset && p.aliasOf < 0) t.aliasOf = p.index;
            sTracks.push_back(t);
        }
        break;
    }
    log("[jukebox] %zu sequences in JaiInit.aaf", sTracks.size());
}

}  // namespace

void jukeboxFrame(Ctx& c) {
    // Test aid: LM_JUKEBOX=<sequence>@<guest frame>.
    static int sEnvSeq = -1, sEnvFrame = -1;
    static bool sEnvParsed = false;
    if (!sEnvParsed) {
        sEnvParsed = true;
        if (const char* v = std::getenv("LM_JUKEBOX")) std::sscanf(v, "%d@%d", &sEnvSeq, &sEnvFrame);
    }
    if (sEnvSeq > 0 && int(skip::guestFrames()) == sEnvFrame) sPlay = sEnvSeq;
    if (sStop.exchange(false)) gu::call(c, GU_SYM("fn_8018A570"), {0});
    const int n = sPlay.exchange(-1);
    if (n > 0) {
        gu::call(c, GU_SYM("fn_8018A570"), {0});
        gu::call(c, GU_SYM("fn_80188760"), {0x80000000u | u32(n)});
        log("[jukebox] playing sequence %d", n);
    }
    // Log what the music player runs (main handle lbl_804D91A4 +8 = sound id), for testing.
    static u32 sLastId = 0;
    const u32 h = rd32(VA(0x804D91A4));
    const u32 id = gu::inMem1(h) ? rd32(h + 8) : 0;
    if (id != sLastId) {
        sLastId = id;
        log("[jukebox] music now %08X", id);
    }
}

void soundSection() {
    ImGui::SeparatorText("Jukebox");
    std::call_once(sLoad, load);
    if (sTracks.empty()) {
        ImGui::TextDisabled("No music table found on the disc.");
        return;
    }
    ImGui::TextDisabled("Plays the game's sequenced music (the mansion's streamed ambience is separate).\n"
                        "The game may change the music again on its own, e.g. when Luigi enters a room.");
    if (ImGui::Button("Stop music")) {
        sStop = true;
        sNowPlaying = -1;
    }
    ImGui::SameLine();
    ImGui::Checkbox("Only tracks not heard in retail", &sUnusedOnly);
    const bool us = !ver::pal();
    if (ImGui::BeginTable("jukebox", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                          ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 14))) {
        for (const Track& t : sTracks) {
            if (t.index == 0) continue;  // sound effects
            const bool unused = us && !usedInRetail(t);
            if (sUnusedOnly && !unused) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            char label[32];
            std::snprintf(label, sizeof label, "%s##jb%d", sNowPlaying == t.index ? "Playing" : "Play", t.index);
            if (ImGui::SmallButton(label)) {
                sPlay = t.index;
                sNowPlaying = t.index;
            }
            ImGui::TableNextColumn();
            ImGui::Text("%2d  %s", t.index, t.name.c_str());
            ImGui::TableNextColumn();
            if (t.aliasOf >= 0) ImGui::TextDisabled("same as %d", t.aliasOf);
            else if (unused) ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "not heard in retail");
        }
        ImGui::EndTable();
    }
}

}  // namespace recomp::unused2
