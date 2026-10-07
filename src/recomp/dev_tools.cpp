// Developer tools: leftover debug displays, map warps and the info overlay (model viewer:
// model_viewer.cpp). Addresses are US GLME01; how each was found: docs/UNUSED_CONTENT.md.
#include "recomp/dev_tools.h"

#include "port/settings.h"
#include "recomp/guest_util.h"
#include "recomp/hle.h"
#include "recomp/model_viewer.h"

#include <imgui.h>
#include <SDL3/SDL.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace recomp::dev {
namespace {

// --- game state ------------------------------------------------------------------------
constexpr u32 kCurSceneId = 0x804D80A0;       // sCurSceneId: the scene the manager is running
constexpr u32 kReqSceneId = 0x803A3AE4;       // lbl_803A3AE0+4: the scene the main loop asks for
constexpr u32 kNextMapNo = 0x804D80A4;        // map loaded by the next game-scene setup
constexpr u32 kSceneChanging = 0x804D8088;    // set by scene-change requests, cleared after setup
constexpr u32 kCurMapNo = 0x804D80C8;         // sCurMapNo
constexpr u32 kAppearPoint = 0x804D872C;      // sGlobalLuigiAppearPoint (SETLUIGI)
constexpr u32 kRoomState = 0x803A3CA0;        // +0xC: current room number in the top byte

// Compiled-in debug switches that nothing in the retail game sets (docs/UNUSED_CONTENT.md).
constexpr u32 kParticleReportFlag = 0x804D8E04;  // byte; fn_80160138 prints emitter/particle counts
constexpr u32 kDepthBlurFlag = 0x804D86B0;       // word; fn_8005E9EC depth-of-field style blur pass

const MapInfo kMaps[] = {
    {0, "Test map (Iwamoto test_00-03)", false, true, "toad, butler, dancer, Boos, Mario test actors"},
    {1, "E. Gadd's lab", true, true, nullptr},
    {2, "Mansion", true, true, nullptr},
    {3, "Training room (Poltergust practice)", true, true, nullptr},
    {4, "Ghost Portrificationizer", true, true, nullptr},
    {5, "Unused: statue hall (h_03)", false, true, "4 rooms, Luigi only"},
    {6, "Portrait gallery", true, true, nullptr},
    {7, "Unused: h_05 (copy of map 5's rooms)", false, false, "crashes while loading"},
    {8, "Unused: h_06 (old JMP format)", false, false, "crashes while loading"},
    {9, "Roof (King Boo / Bowser)", true, true, nullptr},
    {10, "Chauncey's room", true, true, nullptr},
    {11, "Balcony (Boolossus)", true, true, nullptr},
    {12, "Portrificationizer (after Bowser / game over)", true, true, "map 4's room, own setup"},
    {13, "Graveyard (Bogmire)", true, true, nullptr},
};

// map2 jmp/characterinfo "luige" entries (appear_point -> room_no, position).
const AppearPoint kAppear[] = {
    {0, 2, "Foyer (new game / default)"},
    {6, 2, "Foyer, by the front door"},
    {2, 16, "1F, west side"},
    {9, 70, "Basement"},
    {1, 24, "2F, west side"},
    {7, 36, "2F, east side"},
    {4, 56, "3F, centre"},
    {5, 59, "3F, north"},
    {8, 48, "3F, west side"},
    {3, 60, "Roof level"},
};

std::atomic<int> sWarpMap{-1};
std::atomic<int> sWarpAppear{0};
u32 sGameScene = 0xFFFFFFFF;  // scene id the game scene setup ran under

// Values sampled on the guest thread for the host-side overlays.
struct Status {
    bool inGame = false;
    int map = -1, room = -1;
    u32 scene = 0;
    float pos[3] = {};
};
Status sStatus;

// The game's debug-print queue (JUTReport, fn_801D71B0 -> fn_801D7114). Retail never creates
// the print object; the hook collects the text instead and the overlay draws it.
struct ReportLine {
    int x, y;
    std::string text;
};
std::vector<ReportLine> sReportBuild, sReportShown;

bool sInfoOverlay = std::getenv("LM_DEV_INFO") != nullptr;  // F3; LM_DEV_INFO=1 starts with it on

int envInt(const char* name, int def) {
    const char* v = std::getenv(name);
    return v && *v ? std::atoi(v) : def;
}

}  // namespace

const MapInfo* maps(int* count) {
    *count = int(std::size(kMaps));
    return kMaps;
}
const AppearPoint* mansionAppearPoints(int* count) {
    *count = int(std::size(kAppear));
    return kAppear;
}
void requestWarp(int map, int appearPoint) {
    sWarpAppear = appearPoint;
    sWarpMap = map;
}
bool inGameScene() { return sStatus.inGame; }
int currentMap() { return sStatus.map; }

// ---- hooks (guest thread) ------------------------------------------------------------

// Game-scene setup for sCurMapNo. LM_START_MAP=<n> (dev aid, e.g. for the boss maps 9/10/11)
// loads map n instead of the mansion the first time map 2 loads; LM_START_APPEAR=<p> picks the
// mansion appear point.
HLE(fn_8000C188) {
    static const int sMap = envInt("LM_START_MAP", -1);
    static const int sAppear = envInt("LM_START_APPEAR", 0);
    static bool sDone = false;
    if (!sDone && rd32(kCurMapNo) == 2 && (sMap >= 0 || sAppear > 0)) {
        sDone = true;
        if (sMap >= 0) {
            log("[dev] LM_START_MAP: loading map %d instead of the mansion", sMap);
            wr32(kCurMapNo, u32(sMap));
        }
        if (sAppear > 0) wr32(kAppearPoint, u32(sAppear));
    }
    sGameScene = rd32(kReqSceneId);
    callOriginal(c, "fn_8000C188");
}

// Scene manager, once per main-loop iteration. A queued warp is applied exactly like the
// event command <WARP>(n): next map = n and the current scene id cleared, so the manager
// tears the game scene down and sets it up again for the new map on this call.
HLE(fn_8000B4A0) {
    const int map = sWarpMap.load();
    if (map >= 0) {
        const u32 scene = rd32(kCurSceneId);
        if (scene == sGameScene && rd32(kReqSceneId) == scene && rd32(kSceneChanging) == 0) {
            sWarpMap = -1;
            log("[dev] warp to map %d (appear point %d)", map, sWarpAppear.load());
            wr32(kAppearPoint, u32(sWarpAppear.load()));
            wr32(kNextMapNo, u32(map));
            wr32(kCurSceneId, 0);
        }
    }
    callOriginal(c, "fn_8000B4A0");
}

// JUTReport's queue insert: (print object, x, y, flag, text, length). Collected for the overlay.
HLE(fn_801D7114) {
    if (c.r[8] > 0 && c.r[7]) sReportBuild.push_back({s32(c.r[4]), s32(c.r[5]), std::string(gstr(c.r[7]), c.r[8])});
}

// End of the main loop's frame: copy to the XFB and flip. Debug switches are applied, the
// overlays' state sampled and the model viewer drawn here (into this frame's EFB).
HLE(LMChangeFrameBuffer__Fv) {
    const auto& e = port::settings().enhancements;
    static const int sEnvReport = envInt("LM_DEV_PARTICLE_REPORT", -1);
    static const int sEnvBlur = envInt("LM_DEV_DEPTH_BLUR", -1);
    hostp(kParticleReportFlag)[0] = (sEnvReport >= 0 ? sEnvReport != 0 : e.dbgParticleReport) ? 1 : 0;
    wr32(kDepthBlurFlag, (sEnvBlur >= 0 ? sEnvBlur != 0 : e.dbgDepthBlur) ? 1 : 0);

    // Test aid: LM_DEV_WARPS=<map>[:<appear>]@<frame>,... queues warps at given frame counts.
    static std::vector<std::array<int, 3>> sEnvWarps = [] {
        std::vector<std::array<int, 3>> v;
        if (const char* w = std::getenv("LM_DEV_WARPS"))
            for (const char* p = w; *p;) {
                int map = 0, appear = 0, frame = 0;
                if (std::sscanf(p, "%d:%d@%d", &map, &appear, &frame) == 3 || std::sscanf(p, "%d@%d", &map, &frame) == 2)
                    v.push_back({frame, map, appear});
                const char* comma = std::strchr(p, ',');
                if (!comma) break;
                p = comma + 1;
            }
        return v;
    }();
    static u32 sFrames = 0;
    ++sFrames;
    for (const auto& w : sEnvWarps)
        if (u32(w[0]) == sFrames) requestWarp(w[1], w[2]);

    Status s;
    s.scene = rd32(kCurSceneId);
    s.inGame = s.scene == sGameScene && rd32(kSceneChanging) == 0;
    if (s.inGame) {
        s.map = s32(rd32(kCurMapNo));
        s.room = s32(rd32(kRoomState + 0xC) >> 24);
        const u32 luigi = gu::call(c, GU_SYM("fn_800AD39C"), {0});
        if (gu::inMem1(luigi))
            for (int i = 0; i < 3; i++) s.pos[i] = gu::rdf(luigi + 0x44 + 4 * i);
    }
    sStatus = s;

    sReportShown.swap(sReportBuild);
    sReportBuild.clear();

    viewer::guestFrame(c);
    callOriginal(c, "LMChangeFrameBuffer__Fv");
}

// ---- host side ------------------------------------------------------------------------

bool capturesInput() { return viewer::isOpen(); }

bool handleKey(const SDL_Event& ev) {
    if (ev.type != SDL_EVENT_KEY_DOWN || ev.key.repeat) return false;
    if (ev.key.key == SDLK_F3) {
        sInfoOverlay = !sInfoOverlay;
        return true;
    }
    if (ev.key.key == SDLK_F4) {
        viewer::setOpen(!viewer::isOpen());
        return true;
    }
    return false;
}

void drawOverlays() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    if (!sReportShown.empty()) {
        // JUTReport coordinates are in the game's 640x480 screen.
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const float sx = vp->Size.x / 640.0f, sy = vp->Size.y / 480.0f;
        for (const auto& l : sReportShown) {
            const ImVec2 p(vp->Pos.x + float(l.x) * sx, vp->Pos.y + float(l.y) * sy - ImGui::GetFontSize());
            dl->AddText(ImVec2(p.x + 1, p.y + 1), IM_COL32(0, 0, 0, 255), l.text.c_str());
            dl->AddText(p, IM_COL32(255, 255, 255, 255), l.text.c_str());
        }
    }
    if (sInfoOverlay) {
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - 8, vp->Pos.y + 8), ImGuiCond_Always, ImVec2(1, 0));
        ImGui::SetNextWindowBgAlpha(0.5f);
        ImGui::Begin("##devinfo", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
        const Status s = sStatus;
        if (s.inGame) {
            int n = 0;
            const MapInfo* m = maps(&n);
            ImGui::Text("map %d  %s", s.map, s.map >= 0 && s.map < n ? m[s.map].name : "?");
            ImGui::Text("room %d", s.room);
            ImGui::Text("luigi %.0f %.0f %.0f", s.pos[0], s.pos[1], s.pos[2]);
        } else {
            ImGui::Text("scene %u (not in game)", s.scene);
        }
        ImGui::End();
    }
    viewer::window();
}

void menuSection() {
    auto& e = port::settings().enhancements;
    bool dirty = false;
    if (!ImGui::CollapsingHeader("Debug")) return;
    ImGui::TextDisabled("Leftover debug switches (compiled into the retail game, never enabled)");
    dirty |= ImGui::Checkbox("Particle manager report (JUTReport)", &e.dbgParticleReport);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Emitter/particle/child counts and per-group usage drawn by the game's\n"
                          "particle manager (flag 0x804D8E04).");
    dirty |= ImGui::Checkbox("Unused depth blur pass", &e.dbgDepthBlur);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("A screen pass in the game-scene draw (fn_8005E9EC) gated by a flag nothing\n"
                          "sets (0x804D86B0): blends a blurred quarter-size copy of the frame by depth.");
    ImGui::Checkbox("Info overlay (F3)", &sInfoOverlay);
    if (ImGui::Button("Model viewer (F4)")) viewer::setOpen(!viewer::isOpen());

    ImGui::SeparatorText("Visit area");
    const bool inGame = sStatus.inGame;
    if (!inGame) ImGui::TextDisabled("Available while playing (load a save or start a new game).");
    ImGui::BeginDisabled(!inGame);
    int n = 0;
    const MapInfo* m = maps(&n);
    if (ImGui::BeginTable("maps", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        for (int i = 0; i < n; i++) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            char label[32];
            std::snprintf(label, sizeof(label), "Go##map%d", m[i].no);
            ImGui::BeginDisabled(!m[i].loads);
            if (ImGui::SmallButton(label)) requestWarp(m[i].no, 0);
            ImGui::EndDisabled();
            ImGui::TableNextColumn();
            ImGui::Text("%2d  %s", m[i].no, m[i].name);
            ImGui::TableNextColumn();
            if (!m[i].used) ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "unused");
            if (m[i].note) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", m[i].note);
            }
        }
        ImGui::EndTable();
    }
    if (ImGui::TreeNode("Mansion entry points")) {
        int na = 0;
        const AppearPoint* a = mansionAppearPoints(&na);
        for (int i = 0; i < na; i++) {
            char label[64];
            std::snprintf(label, sizeof(label), "Go##ap%d", a[i].point);
            if (ImGui::SmallButton(label)) requestWarp(2, a[i].point);
            ImGui::SameLine();
            ImGui::Text("%s  (room %d, point %d)", a[i].where, a[i].room, a[i].point);
        }
        ImGui::TreePop();
    }
    ImGui::EndDisabled();
    if (sWarpMap.load() >= 0) ImGui::TextDisabled("Warp queued...");
    if (dirty) port::saveSettings();
}

}  // namespace recomp::dev
