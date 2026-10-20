// In-game overlay menu (F1 toggles, Esc closes): Visuals / Sound / Controls / Enhancements / Game.
// Drawn with Aurora's Dear ImGui on the host thread between the guest's present
// and aurora_end_frame. While it is open the game receives neutral pad input.
#include "recomp/menu.h"
#include "recomp/gx_bridge.h"

#include "port/install.h"
#include "port/options.h"
#include "port/settings.h"
#include "recomp/audio.h"
#include "recomp/controls.h"
#include "recomp/crt.h"
#include "recomp/dev_tools.h"
#include "recomp/enhanced_lighting.h"
#include "recomp/frame.h"
#include "recomp/hidden_mansion.h"
#include "recomp/interp.h"
#include "recomp/pacing.h"
#include "recomp/skip.h"
#include "recomp/unused2.h"

#include <aurora/aurora.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <SDL3/SDL.h>

#include <dolphin/vi.h>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace recomp {
u32 copyDispCount();  // hle_gx.cpp
}

namespace recomp::menu {
namespace {

// Test aid: LM_MENU_TAB=<Enhancements sub-tab name> starts with the menu open on that tab.
const char* sEnvTab = std::getenv("LM_MENU_TAB");
bool sOpen = sEnvTab != nullptr;

std::string sOpenSub;  // sub-tab to select once (LM_MENU_ID_CHECK sweep)

// Flags for a sub-tab: selected while LM_MENU_TAB names it, or once when the sweep asks for it.
ImGuiTabItemFlags envTab(const char* name) {
    if (!sOpenSub.empty() && sOpenSub == name) {
        sOpenSub.clear();
        return ImGuiTabItemFlags_SetSelected;
    }
    if (!sEnvTab || std::strcmp(sEnvTab, name) != 0) return 0;
    return ImGuiTabItemFlags_SetSelected;
}
bool sDirty = false;
bool sQuit = false;  // "Quit game" confirmed
std::string sOpenTab = sEnvTab ? "Enhancements" : "";  // tab to select once (LM_MENU / LM_MENU_TAB)

// ---- Test aid LM_MENU_ID_CHECK=1: Dear ImGui ID-conflict sweep ----------------------------
// Two widgets with the same ID in one scope (same label without a ##suffix, or the same ##id)
// are drawn red on hover by ImGui and neither can be clicked. ImGui only notices a conflict
// for the hovered item, so this walks every tab and sub-tab (tree nodes opened), moves ImGui's
// mouse over the whole page row by row (scrolling down) and logs each conflicting ID with its
// ID-stack path (from ImGui's ID Stack Tool, i.e. the labels). The menu is made as tall as the
// window (680 wide, the default width) and each page/scroll position is held for a moment
// with a "[menu-id] shot" line, so a capture script can screenshot every page.
// LM_MENU_ID_CHECK=exit quits when the sweep is done; =selftest also draws two buttons with one
// ID on the Game tab (they must be reported: proves the detection works) and quits.
namespace idcheck {
struct Page {
    const char* top;
    const char* sub;
};
constexpr Page kPages[] = {
    {"Visuals", "General"},       {"Visuals", "Rendering"},        {"Visuals", "Enhanced lighting"},
    {"Sound", nullptr},           {"Controls", "Ports"},           {"Controls", "Bindings"},
    {"Controls", "Analog"},       {"Enhancements", "General"},     {"Enhancements", "Gameplay"},
    {"Enhancements", "Unused content"}, {"Enhancements", "Debug"}, {"Game", nullptr},
};
constexpr int kPageCount = int(sizeof(kPages) / sizeof(kPages[0]));
constexpr float kXs[] = {0.05f, 0.3f, 0.6f, 0.88f};
constexpr int kXCount = int(sizeof(kXs) / sizeof(kXs[0]));
constexpr float kRowStep = 8.0f;
constexpr int kSettle = 20, kDwell = 45;  // frames: tab switch / scroll settle, screenshot hold

const char* sMode = std::getenv("LM_MENU_ID_CHECK");
bool active() { return sMode && *sMode && std::strcmp(sMode, "0") != 0; }
bool selfTest() { return active() && std::strcmp(sMode, "selftest") == 0; }

int sPage = -1, sScrollIndex = 0, sWait = 0, sSample = 0, sResolve = 0, sConflicts = 0;
bool sDone = false, sShotPrinted = false;
float sScroll = 0.0f;
ImRect sClip;               // menu window's inner clip rect (last frame)
float sScrollMax = 0.0f;
ImGuiWindow* sWindow = nullptr;
ImVec2 sHistory[4];          // mouse positions issued in the last frames (detection lags 3 frames)
ImVec2 sResolvePos;
ImGuiID sResolveId = 0;
std::vector<ImGuiID> sSeen;

ImVec2 sNextPos(-1.0f, -1.0f);  // mouse position for the next frame

// The position is applied at the start of the next frame's menu (applyMouse): the SDL backend
// feeds the real cursor every frame while the window has focus, so an input event would lose.
void mouse(ImVec2 p) {
    sNextPos = p;
    for (int i = 3; i > 0; i--) sHistory[i] = sHistory[i - 1];
    sHistory[0] = p;
}

void applyMouse() {
    ImGuiContext& g = *GImGui;
    g.IO.MousePos = sNextPos;
    ImGuiWindow* hovered = nullptr;
    ImGuiWindow* under = nullptr;
    ImGui::FindHoveredWindowEx(sNextPos, false, &hovered, &under);
    g.HoveredWindow = hovered;
    g.HoveredWindowUnderMovingWindow = under;
}

void startPage() {
    const Page& p = kPages[sPage];
    sOpenTab = p.top;
    sScroll = 0.0f;
    sScrollIndex = 0;
    sSample = 0;
    sWait = kSettle + kDwell;
    sShotPrinted = false;
}

// Before the menu window: keep it 680 wide (default) and as tall as the display.
void beforeWindow() {
    if (!active()) return;
    // Never save the sweep's window size/position to imgui.ini: on Windows it lives in the real
    // %APPDATA% (SDL's pref path) even when a test run overrides APPDATA.
    ImGui::GetIO().IniFilename = nullptr;
    if (sDone) return;
    if (sPage >= 0) applyMouse();
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSize(ImVec2(680.0f, std::max(400.0f, ds.y - 16.0f)), ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImVec2(8.0f, 8.0f), ImGuiCond_Always);
}

// Inside the menu window, after its contents.
void insideWindow() {
    if (!active() || sDone) return;
    sWindow = ImGui::GetCurrentWindow();
    sClip = sWindow->InnerClipRect;
    sScrollMax = ImGui::GetScrollMaxY();
    if (sPage >= 0 && sWait > 0 && sWait == kSettle + kDwell - 8) {
        // Sub-tab (a few frames after the top tab switched, so the right tab bar is drawn).
        const Page& p = kPages[sPage];
        if (p.sub) {
            if (std::strcmp(p.top, "Controls") == 0) recomp::controls::selectSubTab(p.sub);
            else sOpenSub = p.sub;
        }
    }
    if (sWait > 0) ImGui::SetScrollY(sScroll);
}

// After the menu window: detection, mouse movement, paging.
void afterWindow() {
    if (!active() || sDone) return;
    ImGuiContext& g = *GImGui;
    if (sPage < 0) {
        sPage = 0;
        startPage();
        std::printf("[menu-id] sweep start (%d pages)\n", kPageCount);
        std::fflush(stdout);
        return;
    }
    const Page& page = kPages[sPage];
    if (sResolve > 0) {
        // Hold the mouse on the conflicting item while the ID Stack Tool resolves its path.
        mouse(sResolvePos);
        ImGui::SetNextWindowPos(ImVec2(g.IO.DisplaySize.x - 420.0f, g.IO.DisplaySize.y - 160.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(410.0f, 150.0f), ImGuiCond_Always);
        ImGui::ShowIDStackToolWindow();
        if (--sResolve == 0) {
            std::printf("[menu-id] CONFLICT 0x%08X on %s%s%s at (%.0f, %.0f) scroll %.0f: %s\n", sResolveId, page.top,
                        page.sub ? " > " : "", page.sub ? page.sub : "", sResolvePos.x - sClip.Min.x,
                        sResolvePos.y - sClip.Min.y, sScroll, g.DebugIDStackTool.ResultPathBuf.c_str());
            std::fflush(stdout);
        }
        return;
    }
    if (g.DebugDrawIdConflicts != 0 &&
        std::find(sSeen.begin(), sSeen.end(), g.DebugDrawIdConflicts) == sSeen.end()) {
        sSeen.push_back(g.DebugDrawIdConflicts);
        sConflicts++;
        sResolveId = g.DebugDrawIdConflicts;
        sResolvePos = sHistory[2];  // issued 3 frames ago (queued, hovered, counted)
        sResolve = 24;
        mouse(sResolvePos);
        return;
    }
    if (sWait > 0) {
        mouse(ImVec2(g.IO.DisplaySize.x - 2.0f, g.IO.DisplaySize.y - 2.0f));  // off the menu
        if (--sWait < kDwell && !sShotPrinted) {
            sShotPrinted = true;
            std::printf("[menu-id] shot %s%s%s scroll %d\n", page.top, page.sub ? "-" : "", page.sub ? page.sub : "",
                        sScrollIndex);
            std::fflush(stdout);
        }
        return;
    }
    const int rows = std::max(1, int((sClip.GetHeight() - 4.0f) / kRowStep));
    if (sSample < rows * kXCount) {
        const int row = sSample / kXCount, col = sSample % kXCount;
        mouse(ImVec2(sClip.Min.x + sClip.GetWidth() * kXs[col], sClip.Min.y + 2.0f + float(row) * kRowStep));
        sSample++;
        return;
    }
    if (sScroll + 1.0f < sScrollMax) {
        sScroll = std::min(sScrollMax, sScroll + sClip.GetHeight() - 60.0f);
        sScrollIndex++;
        sSample = 0;
        sWait = kSettle + kDwell;
        sShotPrinted = false;
        return;
    }
    if (++sPage < kPageCount) {
        startPage();
        return;
    }
    sDone = true;
    std::printf("[menu-id] sweep done: %d ID conflict%s\n", sConflicts, sConflicts == 1 ? "" : "s");
    std::fflush(stdout);
    if (std::strcmp(sMode, "exit") == 0 || selfTest()) sQuit = true;
}
}  // namespace idcheck

bool tab(const char* name) {
    const bool select = sOpenTab == name;
    if (select) sOpenTab.clear();
    return ImGui::BeginTabItem(name, nullptr, select ? ImGuiTabItemFlags_SetSelected : 0);
}

void applyResolution() { VISetFrameBufferScale(float(port::settings().video.resolutionScale)); }

void applyWindowSettings() {
    const auto& v = port::settings().video;
    applyResolution();
    if (SDL_Window* w = aurora_get_window()) SDL_SetWindowFullscreen(w, v.fullscreen);
    // Automated runs (scripted --input, or LM_BACKGROUND=1) never pause when another window
    // takes focus, so several test instances can run side by side.
    static const bool sBackground = !port::options().inputScript.empty() || std::getenv("LM_BACKGROUND") != nullptr;
    aurora_set_pause_on_focus_lost(port::settings().enhancements.pauseOnFocusLoss && !sBackground);
    if (sBackground) aurora_set_background_input(true);
}

template <typename E>
bool combo(const char* label, E& value, const char* const* names, int count) {
    int i = int(value);
    const bool changed = ImGui::Combo(label, &i, names, count);
    if (changed) value = E(i);
    return changed;
}

// "(note)" after the last item, or wrapped on the next line when it would be cut off.
void pending(const char* what) {
    char text[256];
    std::snprintf(text, sizeof(text), "(%s)", what);
    ImGui::SameLine();
    if (ImGui::CalcTextSize(text).x <= ImGui::GetContentRegionAvail().x) {
        ImGui::TextDisabled("%s", text);
        return;
    }
    ImGui::NewLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

// Feature gating (port::Avail, docs/SETUP.md): a feature missing its disc is greyed out
// with the reason; one that applies to the other installed version stays selectable.
struct Gate {
    port::Avail a;
    explicit Gate(port::Avail avail) : a(avail) { ImGui::BeginDisabled(blocked()); }
    bool blocked() const {
        return a == port::Avail::NeedsUsDisc || a == port::Avail::NeedsPalDisc || a == port::Avail::NotYet;
    }
    ~Gate() {
        ImGui::EndDisabled();
        if (const char* note = port::availNote(a)) pending(note);
    }
};

void gameTab() {
    if (idcheck::selfTest()) {  // LM_MENU_ID_CHECK=selftest: a deliberate ID conflict
        ImGui::Button("ID check self-test");
        ImGui::SameLine();
        ImGui::Button("ID check self-test");
    }
    auto& inst = port::install();
    if (inst.staticBuild) {
        ImGui::Text("Developer build: %s, recompiled at build time.", port::versionName(inst.running));
        ImGui::TextDisabled("LuigisMansionRecomp manages discs and versions (docs/SETUP.md).");
    } else {
        auto row = [](const char* label, const std::string& path, bool ready) {
            ImGui::Text("%s:", label);
            ImGui::SameLine();
            if (ready) ImGui::TextWrapped("%s", path.c_str());
            else ImGui::TextDisabled("not installed");
        };
        row("USA disc", inst.usDisc, inst.usReady);
        row("Europe disc", inst.palDisc, inst.palReady);
        ImGui::Text("Playing: %s version", inst.running == port::GameVersion::PAL ? "European" : "USA");
        if (inst.both()) {
            int base = int(inst.base);
            ImGui::Text("Version to play:");
            ImGui::SameLine();
            bool changed = ImGui::RadioButton("USA", &base, int(port::GameVersion::USA));
            ImGui::SameLine();
            changed |= ImGui::RadioButton("Europe", &base, int(port::GameVersion::PAL));
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            ImGui::SetItemTooltip("Which installed disc's game runs (both are installed). Takes a restart.");
            if (changed) {
                inst.base = port::GameVersion(base);
                port::saveInstall();
            }
            if (inst.base != inst.running) {
                pending("restart to switch");
                if (ImGui::Button("Restart now")) {
                    port::saveSettings();
                    port::relaunch();
                }
                ImGui::SetItemTooltip("Saves the settings and restarts with the chosen version (unsaved game progress is lost).");
            }
        }
        if (ImGui::Button("Add or change discs...")) {
            port::saveSettings();
            port::relaunch(true);
        }
        ImGui::SetItemTooltip("Restarts into the setup screen to add a USA or European disc image or replace one\n"
                              "(unsaved game progress is lost).");
    }
    ImGui::Separator();
    auto& e = port::settings().enhancements;
    {
        Gate g(port::palBaseFeature());
        static const char* kLanguages[] = {"English", "Deutsch", "Francais", "Espanol", "Italiano"};
        int lang = std::clamp(e.palLanguage, 1, 5) - 1;
        if (ImGui::Combo("Language (European version)", &lang, kLanguages, 5)) {
            e.palLanguage = lang + 1;
            sDirty = true;
        }
        ImGui::SetItemTooltip("Text and voice language of the European game for a new save; an existing save keeps\n"
                              "its own (changed in the game's Options).");
    }
    if (port::palBaseFeature() == port::Avail::Yes)
        ImGui::TextDisabled("Used for a new save; an existing save keeps its language (in-game Options).");
}

// Hardware-accuracy toggles (all on by default). Each entry: checkbox bound to
// settings().rendering.<flag>, sDirty |= ..., and a tooltip saying what changes visually
// and roughly what it costs.
void renderingTab() {
    ImGui::TextDisabled("Hardware-accurate rendering. Everything is on by default for the best quality;");
    ImGui::TextDisabled("turn features off only if your PC needs the speed.");
    auto& r = port::settings().rendering;
    ImGui::SeparatorText("Color and depth");
    sDirty |= ImGui::Checkbox("Integer-accurate TEV", &r.integerTev);
    ImGui::SetItemTooltip(
        "Color combiner math as on the GameCube: 8-bit inputs, signed 11-bit registers and rounding\n"
        "after every stage. Slightly darker multi-stage shading and exact overflow; tiny GPU cost.");
    // Enhanced lighting turns the 6-bit framebuffer off (hle_gx.cpp): shown greyed out and unticked.
    const bool enhOverride = recomp::gx::enhancedOverridesEfbDepth();
    constexpr const char* kEnhNote =
        "\n\nOff while Enhanced lighting is on (F1 > Visuals > Enhanced lighting): its smooth\n"
        "per-pixel, colored-light, bloom and occlusion gradients would be cut into visible 6-bit\n"
        "steps (and 5-bit in the game's full-screen RGB565 copy). Turn Enhanced lighting off to\n"
        "use this option again; the setting itself is kept.";
    {
        ImGui::BeginDisabled(enhOverride);
        bool shown = r.efbColorDepth && !enhOverride;
        if (ImGui::Checkbox("EFB color depth (6-bit + dither)", &shown) && !enhOverride) {
            r.efbColorDepth = shown;
            sDirty = true;
        }
        ImGui::EndDisabled();
        const std::string tip = std::string(
            "The game renders into a 6-bit-per-channel framebuffer with an ordered dither, like the\n"
            "console, and its screen/texture copies keep their format's precision (RGB565: 5-bit red\n"
            "and blue in the full-screen copy the game redraws every frame; RGB5A3 for the Mario\n"
            "painting). Off: smooth 8-bit color (less banding/dither grain in dark rooms). No real cost.") +
                                (enhOverride ? kEnhNote : "");
        ImGui::SetItemTooltip("%s", tip.c_str());
    }
    ImGui::BeginDisabled(!r.efbColorDepth || enhOverride);
    ImGui::Indent();
    {
        bool shown = r.efbBlend6 && !enhOverride;
        if (ImGui::Checkbox("6-bit blending", &shown) && !enhOverride) {
            r.efbBlend6 = shown;
            sDirty = true;
        }
        const std::string tip = std::string(
            "Translucent things drawn over the scene (the Game Boy Horror screen, health counter and\n"
            "room captions, glows) are stored in the 6-bit framebuffer after blending, like on the\n"
            "console: the same banding and dither grain as the rest of the image at their edges and\n"
            "see-through parts instead of smooth 8-bit gradients. Subtle in this game.\n"
            "Costs a few full-screen passes per frame (well under 1 ms at 1440p; more with MSAA).") +
                                (enhOverride ? kEnhNote : "");
        ImGui::SetItemTooltip("%s", tip.c_str());
    }
    {
        // Enhancement on top of the 6-bit framebuffer (Aurora 0190); OFF = the console's look.
        // (Already disabled with the parent group while enhanced lighting turns the 6-bit EFB off.)
        const bool forced = recomp::gx::smoothBandingForced();
        bool on = (forced || r.smoothBanding) && !enhOverride;
        ImGui::BeginDisabled(forced);
        if (ImGui::Checkbox("Smooth color banding", &on) && !enhOverride) {
            r.smoothBanding = on;
            sDirty = true;
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip(
            "The console's 6-bit framebuffer and the 5-bit screen copy the game redraws every frame\n"
            "turn smooth gradients (dark rooms, the flashlight beam and its pool, glows) into visible\n"
            "bands and greenish/bluish blotches, partly hidden by dither grain. On: the same colors and\n"
            "brightness as the accurate picture, but each band is drawn as the smooth average of its\n"
            "steps (plus an invisible sub-step noise against the PC's own 8-bit steps): no contour\n"
            "lines, no blotches, no grain. Off: exactly what the GameCube outputs (default).\n"
            "No measurable cost.%s",
            enhOverride ? "\n\nNot used while Enhanced lighting is on: the 6-bit framebuffer is off then and\n"
                          "the picture is plain 8-bit, with nothing left to smooth." : "");
    }
    ImGui::Unindent();
    ImGui::EndDisabled();
    sDirty |= ImGui::Checkbox("Early depth for cut-out textures", &r.earlyZAlphaDepth);
    ImGui::SetItemTooltip(
        "Room geometry and Luigi test depth before texturing. On the console that also writes the\n"
        "depth of pixels the alpha test then cuts out (transparent texels of cut-out textures), so\n"
        "things drawn behind them later are hidden there. Off: cut-out pixels write no depth (PC\n"
        "behaviour). Costs one extra depth-only draw for each such draw.");
    ImGui::SeparatorText("Screen copies");
    sDirty |= ImGui::Checkbox("Display copy filter", &r.displayCopyFilter);
    ImGui::SetItemTooltip(
        "The vertical filter (and gamma) the console applies when copying the frame to the TV\n"
        "image: slightly softer vertical detail, smooths the dither. One extra full-screen pass.");
    sDirty |= ImGui::Checkbox("Texture copy filter", &r.textureCopyFilter);
    ImGui::SetItemTooltip(
        "The console applies the same vertical filter to the game's screen copies (the full-screen\n"
        "copy it redraws every frame, the half-size and transition copies): the picture is a little\n"
        "softer vertically, as on the GameCube. Off: sharper. Tiny GPU cost.");
    sDirty |= ImGui::Checkbox("EFB copies to RAM (accurate)", &r.efbCopyToRam);
    ImGui::SetItemTooltip("Screen copies also exist in console memory format, like on the GameCube.\n"
                          "Textures that read a copy in another format (the shadow depth map is\n"
                          "read as an IA8 texture) get the console's exact bytes, and memory the game\n"
                          "overwrites stops showing a stale copy. Aimed at the rare Luigi mouth/hose flicker.\n"
                          "Off: copies stay GPU-only textures (saves ~0.6 ms CPU per frame).");
    ImGui::SeparatorText("Timing");
    sDirty |= ImGui::Checkbox("Snapshot vertex data at draw time", &r.vtxSnapshot);
    ImGui::SetItemTooltip(
        "The vertex data each draw uses is copied when the game issues the draw, as the\n"
        "GameCube's GPU reads it, instead of when the renderer thread gets to it. Without it,\n"
        "a draw can pick up data the game already rewrote for a later draw: Luigi's mouth or\n"
        "the Poltergust hose flicker now and then. Off: saves the copies (about 0.2-0.3 ms CPU per frame).\n"
        "Above 30 fps (interpolation) vertex data is always snapshotted.");
    sDirty |= ImGui::Checkbox("Keep GPU clocks up in menus", &r.gpuKeepWarm);
    ImGui::SetItemTooltip(
        "When a frame is much lighter than play (a menu over a still picture), the GPU drops to its\n"
        "power-saving clocks within a second; the first frames of the full scene after the menu then\n"
        "take several times longer (a stutter when closing the pause screen or the Game Boy Horror,\n"
        "measured 45 ms instead of 6 at 4K with enhanced lighting). On: filler GPU work keeps the GPU\n"
        "about as busy as in play while the frame is light. Costs GPU power in menus, never frame time.\n"
        "Only active when recent frames were heavy (more than 3 ms of GPU work).");
    ImGui::SeparatorText("Shadows (flashlight and lightning)");
    sDirty |= ImGui::Checkbox("Shadow map at render resolution", &r.shadowRenderResolution);
    ImGui::SetItemTooltip(
        "The game draws its shadows from a depth map rendered from the light. On: that map uses the\n"
        "internal render resolution, so shadow edges stay sharp at high resolutions.\n"
        "Off: the console's 640x480 map (blockier, stair-stepped edges at high resolution).\n"
        "Without 'EFB copies to RAM' the map always follows the render resolution. Tiny GPU cost.");
    sDirty |= ImGui::Checkbox("Widescreen shadows", &r.shadowWidescreen);
    ImGui::SetItemTooltip(
        "The game only renders shadows for what a 4:3 screen shows. On: the light's view is widened\n"
        "with the screen, so lightning and flashlight shadows reach the left/right edges at 16:9 and\n"
        "wider (slightly lower horizontal shadow detail). Off: objects near the edges cast no\n"
        "shadows, like widescreen hacks on the original. No cost. Only matters in widescreen.");
    sDirty |= ImGui::Checkbox("Shadows at any distance", &r.shadowAnyDistance);
    ImGui::SetItemTooltip(
        "The original game only draws shadows within a limited distance band around Luigi: in big\n"
        "rooms, shadows of things farther away (or close to the camera) drop out as he walks.\n"
        "On: the shadow light's range covers everything in view, so all shadows always render.\n"
        "Off: the original behaviour. No cost.");
}

// Appended to the tooltip of an option the quality preset sets (greyed out unless Custom).
const char* presetNote(bool custom) {
    return custom ? "" : "\n\nSet by the quality preset: choose \"Custom\" to change it.";
}

// ---- Enhanced lighting > Lights > World lights (src/recomp/world_lights.cpp) ----
void worldLightsSection() {
    auto& w = port::settings().worldLights;
    sDirty |= ImGui::Checkbox("World lights", &w.enabled);
    ImGui::SetItemTooltip(
        "Candles, fireplaces, lamps, the Poltergust's element sprays, fire, lightning and sparkles\n"
        "light the room around them in color, like the ghost lights. Positions come from the game's\n"
        "own flames and effects. Off: only the game's own lighting. GPU cost grows with the number\n"
        "of lights in view (up to 64 colored lights in total).");
    ImGui::BeginDisabled(!w.enabled);
    ImGui::Indent();
    sDirty |= ImGui::SliderFloat("World light intensity", &w.masterIntensity, 0.0f, 3.0f, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("Brightness of every world light (1 = default), on top of each kind's own slider.");
    struct Row {
        int cat;
        const char* name;
        const char* tip;
    };
    static const Row kRows[] = {
        {port::kWlCandles, "Candles", "Candles and candelabras: fiery orange (violet where the game's flame is violet), flickering."},
        {port::kWlFires, "Fireplaces and torches", "Hearth fires and torches: strong flickering orange light."},
        {port::kWlLamps, "Lamps and chandeliers", "Lamps and chandeliers: steady warm light."},
        {port::kWlSprays, "Element sprays", "The Poltergust's fire spray lights the room orange; water and ice sprays a subtle blue/cyan."},
        {port::kWlFireFx, "Fire effects", "Fireballs, burning objects and characters."},
        {port::kWlLightning, "Lightning", "A cool blue-white flash from the windows when lightning strikes."},
        {port::kWlSparkles, "Sparkles and glows", "Subtle glows of treasure, coins, keys and other sparkling pickups."},
    };
    for (const Row& r : kRows) {
        ImGui::PushID(r.cat);
        sDirty |= ImGui::Checkbox(r.name, &w.on[r.cat]);
        ImGui::SetItemTooltip("%s", r.tip);
        ImGui::SameLine(ImGui::GetWindowWidth() * 0.42f);
        ImGui::SetNextItemWidth(ImGui::GetWindowWidth() * 0.3f);
        ImGui::BeginDisabled(!w.on[r.cat]);
        sDirty |= ImGui::SliderFloat("##i", &w.intensity[r.cat], 0.0f, 3.0f, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SetItemTooltip("Brightness of these lights (1 = default).");
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    sDirty |= ImGui::Checkbox("Flame flicker", &w.flicker);
    ImGui::SetItemTooltip("Flames flicker naturally. Off: steady flames.");
    sDirty |= ImGui::Checkbox("Merge nearby flames", &w.mergeNearby);
    ImGui::SetItemTooltip("Flames of one kind close together (a candelabra) become one light. Faster; off: one light per flame.");
    ImGui::Unindent();
    ImGui::EndDisabled();
}

// ---- Enhanced lighting > Lights > Ghost lights (src/recomp/ghost_lights.cpp) ----
void ghostLightsSection() {
    auto& g = port::settings().ghostLights;
    sDirty |= ImGui::Checkbox("Ghost lights", &g.enabled);
    ImGui::SetItemTooltip(
        "Every visible ghost lights the room around it in its own glow colour (the colour the game\n"
        "uses for that ghost's light effects; fire, water and ice ghosts glow orange, blue, cyan).\n"
        "Invisible ghosts give no light. Off: the original lighting. Small GPU cost per light.");
    ImGui::BeginDisabled(!g.enabled);
    ImGui::Indent();
    sDirty |= ImGui::SliderFloat("Ghost light intensity", &g.intensity, 0.0f, 4.0f, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("Brightness of every ghost light (1 = default).");
    sDirty |= ImGui::SliderFloat("Ghost light radius", &g.radiusScale, 0.25f, 3.0f, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("How far each ghost's light reaches (scaled by the ghost's size; bosses reach farthest).");
    sDirty |= ImGui::Checkbox("Dramatic ghost light effects", &g.dramatic);
    ImGui::SetItemTooltip(
        "The light pulses gently, flares when a ghost appears, ramps up and flickers while the\n"
        "Poltergust drains the ghost, and bursts then fades when it is captured.\n"
        "Off: a steady light that only follows the ghost's visibility.");
    sDirty |= ImGui::SliderInt("Max ghost lights", &g.maxLights, 1, 64, "%d lights", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("Most ghost lights at once (the strongest/nearest first; all colored lights share\n"
                          "a budget of 64). Lower is faster.");
    sDirty |= ImGui::Checkbox("Shadows from ghost lights", &g.shadows);
    ImGui::SetItemTooltip("The one or two strongest ghost lights cast shadows (with \"Colored light shadows\"\n"
                          "under Shadows). Costs a shadow pass per light.");
    ImGui::Unindent();
    ImGui::EndDisabled();
}

// Enhanced lighting (src/recomp/enhanced_lighting.cpp, docs/RENDERING.md "Enhanced lighting"):
// a master switch (off by default) and every part with its own switch and controls, grouped
// into Lights / Shadows / Flashlight / Effects.
void enhancedLightingTab() {
    auto& l = port::settings().enhancedLighting;
    ImGui::TextDisabled("Optional effects on top of the accurate rendering. Off by default: the original");
    ImGui::TextDisabled("look is one click away. Each part can be switched and tuned on its own.");
    ImGui::Separator();
    sDirty |= ImGui::Checkbox("Enhanced lighting", &l.enabled);
    ImGui::SetItemTooltip(
        "Master switch for everything on this page. Off: the game renders exactly as the accurate\n"
        "renderer does (no extra cost). Turning it on builds extra shader variants once (brief hitch).");
    if (!l.enabled) pending("turn on to use the options below");
    ImGui::BeginDisabled(!l.enabled);
    static const char* kPresets[] = {"Low", "Medium", "High", "Ultra", "Custom"};
    sDirty |= combo("Quality preset", l.preset, kPresets, 5);
    ImGui::SetItemTooltip(
        "Sets the soft shadow, volumetric beam and colored-light shadow quality. Low suits weaker\n"
        "GPUs; Ultra is for high-end cards at high resolutions. Custom keeps the values chosen below.");
    const bool custom = l.preset == port::EnhancedPreset::Custom;

    // ---- Lights ----
    ImGui::SeparatorText("Lights");
    sDirty |= ImGui::Checkbox("Per-pixel lighting", &l.perPixelLighting);
    ImGui::SetItemTooltip(
        "The game's own lights (room lights, flashlight, specular) are computed for every pixel\n"
        "instead of every vertex, with the same formulas. Smoother, rounder light pools on walls\n"
        "and floors instead of blotchy triangles. Small GPU cost.");
    sDirty |= ImGui::Checkbox("Dynamic colored lights", &l.extraLights);
    ImGui::SetItemTooltip(
        "Colored real-time light from ghosts and other light-emitting objects, lighting the room\n"
        "around them (through the game's own materials). Cost grows with the number of lights.\n"
        "Needed by the ghost lights, world lights and colored light shadows.");
    ImGui::BeginDisabled(!l.extraLights);
    ImGui::Indent();
    sDirty |= ImGui::SliderFloat("Light intensity", &l.extraLightIntensity, 0.0f, 3.0f, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("Brightness of every colored light (ghost lights, world lights; 1 = default).");
    sDirty |= ImGui::SliderFloat("Light color vibrance", &l.extraLightSaturation, 0.0f, 2.5f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("Saturation of every colored light (ghosts and other sources) and its glow.\n"
                          "1: the colors as the game defines them; higher: deeper, more contrasty colors.");
    ghostLightsSection();
    worldLightsSection();
    ImGui::Unindent();
    ImGui::EndDisabled();

    // ---- Shadows ----
    ImGui::SeparatorText("Shadows");
    sDirty |= ImGui::Checkbox("Soft shadows", &l.softShadows);
    ImGui::SetItemTooltip(
        "Filters the game's flashlight/lightning shadow map: soft edges instead of hard, stair-stepped\n"
        "ones, and (High/Ultra) contact hardening: sharp where an object touches the surface, softer\n"
        "farther away. Also removes the faint stripes of the original depth packing. Cost: a few\n"
        "dozen texture reads per shadowed pixel.");
    ImGui::BeginDisabled(!l.softShadows);
    ImGui::Indent();
    static const char* kShadowQ[] = {"Low (PCF 8)", "Medium (PCF 16)", "High (PCSS 16)", "Ultra (PCSS 32)"};
    ImGui::BeginDisabled(!custom);
    sDirty |= ImGui::Combo("Shadow quality", &l.softShadowQuality, kShadowQ, 4);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Filter of the soft edge: PCF (fixed softness) with 8 or 16 taps, or PCSS (contact\n"
                          "hardening) with 16 or 32 taps. More taps: smoother edges, more GPU time.%s",
                          presetNote(custom));
    sDirty |= ImGui::SliderFloat("Shadow softness", &l.softShadowRadius, 0.25f, 6.0f, "%.2f texels", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("Width of the soft edge, in texels of the console's 640x480 shadow map.");
    sDirty |= ImGui::SliderFloat("Contact hardening", &l.softShadowPenumbra, 0.0f, 20.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("High/Ultra only: how quickly shadows soften with distance from the object casting them.");
    ImGui::Unindent();
    ImGui::EndDisabled();
    sDirty |= ImGui::SliderFloat("Shadow darkness", &l.shadowDarkness, 0.0f, 1.5f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip(
        "How dark the game's shadows get (1 = as the game draws them, near black).\n"
        "Colored lights (ghost lights, test lights) always keep lighting the shadowed area, so a\n"
        "shadow only removes the light it stands for.");
    sDirty |= ImGui::SliderFloat("Colored shadow tint", &l.shadowAmbientKeep, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip(
        "How much of the room's ambient light stays in shadows: higher = shadows take on the\n"
        "surrounding color instead of going black (0 = the game's look).");
    static const char* kMapSize[] = {"Render resolution", "2048 x 1536", "4096 x 3072"};
    ImGui::BeginDisabled(!port::settings().rendering.shadowRenderResolution);
    sDirty |= ImGui::Combo("Shadow map size##flashlight", &l.shadowMapSize, kMapSize, 3);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(
        "Resolution of the flashlight/lightning shadow map. Render resolution: as the accurate\n"
        "renderer (the shadow pass uses the internal resolution). 2048/4096: the shadow pass is\n"
        "drawn into its own target of that size, so shadows stay sharp at low render resolutions.\n"
        "Needs Visuals > Rendering > \"Shadow map at render resolution\"%s. Cost: one larger depth\n"
        "pass per frame (4096: noticeable on weak GPUs).",
        port::settings().rendering.shadowRenderResolution ? "" : " (currently off)");
    ImGui::BeginDisabled(!l.extraLights);
    sDirty |= ImGui::Checkbox("Colored light shadows", &l.extraLightShadows);
    ImGui::SetItemTooltip(
        "The strongest colored lights (ghosts, fireplaces and braziers, candelabras, the fire spray)\n"
        "cast shadows: furniture and characters between a light and a wall block its glow.%s",
        l.extraLights ? "" : "\n\nNeeds \"Dynamic colored lights\" (under Lights).");
    static const char* kXlMode[] = {"Screen-space (fast)", "Shadow maps (detailed)"};
    ImGui::BeginDisabled(!l.extraLightShadows);
    ImGui::Indent();
    sDirty |= ImGui::Combo("Light shadow method", &l.extraShadowMode, kXlMode, 2);
    ImGui::SetItemTooltip(
        "Shadow maps: the room is rendered from each shadow-casting light into a cube map, so\n"
        "objects outside the picture (behind the camera, off to the side) still cast shadows, with\n"
        "accurate soft edges. Cost: one extra render of the room per frame for all lights together.\n"
        "Screen-space: a ray march through the visible depth at half resolution (up to 4 lights);\n"
        "cheap, but only what is on screen casts shadows and the edges are approximate.");
    static const char* kXlQ[] = {"Low", "Medium", "High"};
    static const char* kXlN[] = {"Off", "2 lights", "4 lights", "8 lights", "12 lights", "16 lights"};
    static const char* kXlS[] = {"256", "512", "1024", "2048"};
    ImGui::BeginDisabled(!custom);
    sDirty |= ImGui::Combo("Shadow-casting lights", &l.extraShadowLights, kXlN, 6);
    ImGui::SetItemTooltip("How many of the strongest colored lights cast shadows each frame (screen-space: at most 4).\n"
                          "Each light adds a cube map (shadow maps) or a ray march (screen-space). With 12 or 16,\n"
                          "single candles and lit lamps cast shadows too.\n"
                          "Cost with shadow maps at 1024, High quality (RTX 3090 Ti, 1280x960): 8 lights ~1.5 ms,\n"
                          "12 ~1.9 ms, 16 ~2.4 ms per frame; shadow-map memory ~200 / 300 / 450 MB.%s",
                          presetNote(custom));
    sDirty |= ImGui::Combo("Light shadow quality", &l.extraShadowQuality, kXlQ, 3);
    ImGui::SetItemTooltip("Filter taps of the soft edge (shadow maps: 8 / 12 / 20) or ray-march steps (screen-space).%s",
                          presetNote(custom));
    ImGui::BeginDisabled(l.extraShadowMode != 1);
    sDirty |= ImGui::Combo("Shadow map size##colored", &l.extraShadowMapSize, kXlS, 4);
    ImGui::SetItemTooltip("Texels per cube face (shadow maps method only). Larger: sharper, more detailed shadows,\n"
                          "more GPU memory (at 1024: 8 lights about 200 MB, 16 about 450 MB; 2048 is reduced to fit\n"
                          "the texture size limit).%s",
                          presetNote(custom));
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::BeginDisabled(l.extraShadowMode != 1);
    sDirty |= ImGui::SliderFloat("Light shadow softness", &l.extraShadowSoftness, 0.0f, 60.0f, "%.0f units", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("Shadow maps method only. Size of the light source (world units): contact-hardening\n"
                          "penumbrae, sharp where an object meets the surface and softer farther from it. 0: hard edges.");
    ImGui::EndDisabled();
    ImGui::Unindent();
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    // ---- Flashlight ----
    ImGui::SeparatorText("Flashlight");
    sDirty |= ImGui::Checkbox("Flashlight quality", &l.flashlightQuality);
    ImGui::SetItemTooltip(
        "The flashlight cone is evaluated per pixel with a smooth penumbra: a clean round beam\n"
        "edge without a hard rim.");
    ImGui::BeginDisabled(!l.flashlightQuality);
    ImGui::Indent();
    sDirty |= ImGui::SliderFloat("Penumbra softness", &l.flashlightSoftness, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("Softness of the beam's edge: from the original falloff (0) to fully smoothed (1).");
    sDirty |= ImGui::SliderFloat("Beam highlights", &l.flashlightSpecular, 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("Glossy highlights where the flashlight hits floors, furniture and characters\n"
                          "(per pixel, tinted by the surface). 0: the original matte response.");
    ImGui::Unindent();
    ImGui::EndDisabled();
    static const char* kRealQ[] = {"Off", "Medium", "High", "Ultra"};
    sDirty |= ImGui::Combo("Realistic flashlight", &l.realisticFlashlight, kRealQ, 4);
    ImGui::SetItemTooltip(
        "A physically based flashlight beam: light scattering in the air of the room, ray-marched\n"
        "against the flashlight's own shadow map, so Luigi's hand, furniture, ghosts and door frames\n"
        "cut shadow shafts into the haze; a reflector-shaped pool on walls and floors (hot centre,\n"
        "faint rings, soft edge, inverse-square falloff); dust motes lit only inside the beam; a\n"
        "flare when the light flashes. Replaces the game's cone mesh and the volumetric beam below.\n"
        "GPU cost while the beam is on screen (RTX 3090 Ti, 2560x1920; the volumetric beam below\n"
        "costs ~0.3 ms): Medium ~0.5-0.9 ms (half resolution up to 540 lines, 20 steps), High\n"
        "~0.6-1.4 ms (up to 720 lines, 32 steps), Ultra ~2-3 ms (up to 1080 lines, 48 steps, more\n"
        "dust). Nothing when the flashlight is off. Weak GPUs: Medium. Off: the volumetric beam below.");
    ImGui::BeginDisabled(l.realisticFlashlight == 0);
    ImGui::Indent();
    sDirty |= ImGui::SliderFloat("Beam density", &l.beamDensity, 0.0f, 3.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("How hazy the air is: brightness of the beam and its shadow shafts, and how quickly\n"
                          "it fades with distance (denser air also absorbs more). No cost difference.");
    sDirty |= ImGui::SliderFloat("Dust motes", &l.beamDust, 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("Dust drifting in the beam, lit only where the beam (and not a shadow) reaches it.\n"
                          "Small cost (a walk through the dust cells near the lens); 0 turns it off.");
    ImGui::Unindent();
    ImGui::EndDisabled();
    ImGui::BeginDisabled(l.realisticFlashlight != 0);
    sDirty |= ImGui::Checkbox("Volumetric flashlight beam", &l.volumetricBeam);
    ImGui::SetItemTooltip(
        "Light scattering in the air of the flashlight beam (dusty haze), stopped by walls and\n"
        "objects. One reduced-resolution pass; quality = ray-march steps/resolution.%s",
        l.realisticFlashlight != 0 ? "\n\nNot used while \"Realistic flashlight\" is on (it replaces this beam)." : "");
    ImGui::BeginDisabled(!l.volumetricBeam);
    ImGui::Indent();
    sDirty |= ImGui::SliderFloat("Beam intensity", &l.volumetricIntensity, 0.0f, 3.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("Brightness of the haze in the beam.");
    static const char* kVolQ[] = {"Low", "Medium", "High"};
    ImGui::BeginDisabled(!custom);
    sDirty |= ImGui::Combo("Beam quality", &l.volumetricQuality, kVolQ, 3);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Ray-march steps and resolution of the beam pass. Higher: smoother haze, more GPU time.%s",
                          presetNote(custom));
    ImGui::Unindent();
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    // ---- Effects ----
    ImGui::SeparatorText("Effects");
    sDirty |= ImGui::Checkbox("Bloom / glow", &l.bloom);
    ImGui::SetItemTooltip(
        "Light sources glow: lamps, candles, fireplaces, the flashlight lens, ghost auras,\n"
        "lightning, nozzle effects. The HUD never glows. Computed at a capped resolution, so the\n"
        "cost hardly depends on the render resolution.");
    ImGui::BeginDisabled(!l.bloom);
    ImGui::Indent();
    sDirty |= ImGui::SliderFloat("Glow intensity", &l.bloomIntensity, 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("Overall strength of the glow.");
    sDirty |= ImGui::SliderFloat("Light sources", &l.bloomEmissive, 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("Glow of light-emitting effects (what the game draws as added light): lamp and candle\n"
                          "glows, fire, the flashlight lens, ghost auras, sparkles, lightning and nozzle effects.");
    sDirty |= ImGui::SliderFloat("Bright surfaces", &l.bloomScene, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("Glow of ordinary surfaces that are brighter than the threshold (lit walls, white\n"
                          "objects). Low keeps the glow on actual light sources.");
    sDirty |= ImGui::SliderFloat("Glow threshold", &l.bloomThreshold, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("Brightness where bright surfaces start to glow. Lower: more of the scene glows.");
    sDirty |= ImGui::SliderFloat("Glow radius", &l.bloomRadius, 0.25f, 2.0f, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("How far the glow spreads around a light (1 = default).");
    sDirty |= ImGui::SliderFloat("Flashlight-lit walls glow less", &l.bloomFlashlightSuppress, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("Surfaces that are only bright because the flashlight shines on them glow less\n"
                          "than actual light sources.");
    ImGui::Unindent();
    ImGui::EndDisabled();
    sDirty |= ImGui::Checkbox("Ambient occlusion", &l.ssao);
    ImGui::SetItemTooltip(
        "Soft contact shading: darkens corners, creases, the floor under furniture and around\n"
        "characters' feet (horizon-based, from the depth buffer and the models' own normals, so\n"
        "round characters shade smoothly). Applied only to solid surfaces: ghosts, the flashlight\n"
        "beam, glows, particles and glass keep their brightness. About 0.5-1 ms per frame at 4K.\n"
        "Switching it rebuilds some shaders once.");
    ImGui::BeginDisabled(!l.ssao);
    ImGui::Indent();
    sDirty |= ImGui::SliderFloat("Occlusion intensity", &l.ssaoIntensity, 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("How dark occluded corners and contact areas get.");
    sDirty |= ImGui::SliderFloat("Occlusion radius", &l.ssaoRadius, 10.0f, 200.0f, "%.0f units", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("How far (world units; Luigi is about 100 tall) nearby surfaces darken a point.\n"
                          "Larger: broader, softer darkening; smaller: tight contact shadows.");
    ImGui::Unindent();
    ImGui::EndDisabled();
    ImGui::EndDisabled();
}

// Frame rate: 30 (the game's own rate, drawn directly) up to 320, or the display's refresh rate.
void frameRateControls(port::VideoSettings& v) {
    const float hz = recomp::frame::displayRefreshHz();
    const int target = recomp::frame::targetFps();
    char label[64];
    if (v.matchDisplay) std::snprintf(label, sizeof(label), "%d fps (display)", target);
    else if (v.targetFps <= port::kMinFps) std::snprintf(label, sizeof(label), "Original");
    else std::snprintf(label, sizeof(label), "%d fps", v.targetFps);
    if (v.unlimitedFps) std::snprintf(label, sizeof(label), "Unlimited");
    ImGui::BeginDisabled(v.matchDisplay || v.unlimitedFps);
    sDirty |= ImGui::SliderInt("Frame rate", &v.targetFps, port::kMinFps, port::kMaxFps, label, ImGuiSliderFlags_AlwaysClamp);
    ImGui::EndDisabled();
    const bool sliderHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    ImGui::BeginDisabled(v.unlimitedFps);
    sDirty |= ImGui::Checkbox("Match display refresh rate", &v.matchDisplay);
    ImGui::EndDisabled();
    const bool checkHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    sDirty |= ImGui::Checkbox("Unlimited frame rate", &v.unlimitedFps);
    ImGui::SetItemTooltip("Interpolated frames as fast as the PC can draw them: no frame-rate target, no frame\n"
                          "pacing and no cap below the display's refresh rate (the game itself still runs at 30).\n"
                          "With VSync on, the display's refresh rate still limits it (with G-SYNC the frame\n"
                          "rate can then sit right at the refresh rate); turn VSync off for a truly unlimited\n"
                          "rate. Uneven frame times can look less smooth than a paced target.");
    if (v.matchDisplay && hz > 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%.0f Hz)", hz);
    } else if (v.vsync && hz > 0 && float(target) > hz + 0.5f) {
        ImGui::SameLine();
        ImGui::TextDisabled("(VSync limits this to %.0f fps)", hz);
    }
    if (sliderHovered || checkHovered) {
        char vs[160] = "";
        if (v.vsync && hz > 0 && float(target) > hz + 0.5f)
            std::snprintf(vs, sizeof(vs),
                          "\n\nVSync is on: frames are limited to the display's %.0f Hz, so %d fps\n"
                          "is not reached. Turn VSync off (next launch) to go above it (with tearing).",
                          hz, target);
        ImGui::SetTooltip("30 fps is the game's own rate, drawn exactly as the console does.\n"
                          "Above 30, frames in between are interpolated (the game still runs at 30):\n"
                          "movement, the camera, lights and effects are blended between game frames,\n"
                          "shown one game frame (33 ms) behind. Cuts and teleports are not blended.\n"
                          "Match display refresh rate: the refresh rate of the monitor the window is on\n"
                          "(currently %.0f Hz).%s",
                          hz, vs);
    }
    static const char* kVrr[] = {"Auto (detect)", "On", "Off"};
    sDirty |= combo("Variable refresh rate", v.vrr, kVrr, 3);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("For G-SYNC / FreeSync displays. Interpolated frames are started on an even schedule\n"
                          "and each one shows the game exactly at its scheduled time, so motion steps are equal.\n"
                          "Variable refresh: up to a little below the refresh rate (inside the VRR range); if a\n"
                          "frame takes longer, the rate drops evenly to what the PC can hold (e.g. 95 fps).\n"
                          "Fixed refresh: the refresh rate or an even fraction of it (120 Hz: 120, 60, 40 fps),\n"
                          "never an uneven rate that would show frames for one or two refreshes.\n"
                          "Auto: variable refresh is detected from the display's refresh timing%s.",
                          recomp::pacing::vrrActive() ? " (detected)" : " (not detected so far)");
    if (const char* st = recomp::pacing::status(); st && *st) {
        ImGui::SameLine();
        ImGui::TextDisabled("now %s", st);
    }
}

void visualsTab() {
    auto& v = port::settings().video;
    ImGui::SeparatorText("Frame rate and view");
    frameRateControls(v);
    static const char* kAspects[] = {"Original 4:3", "Match window", "16:9", "16:10", "21:9", "32:9"};
    sDirty |= combo("Aspect ratio", v.aspect, kAspects, 6);
    ImGui::SetItemTooltip("Shape of the 3D view. Original 4:3: as on the console, with black bars in a wider\n"
                          "window. Match window: fills the window. The fixed ratios add black bars as needed.");
    sDirty |= ImGui::Checkbox("Keep HUD at 4:3", &v.hudKeep4x3);
    ImGui::SetItemTooltip("On: the HUD stays where it is on a 4:3 screen, centred. Off: in widescreen the health\n"
                          "counter moves to the left edge and the Game Boy Horror to the right edge.");
    if (!v.hudKeep4x3) pending("in-game HUD moves to the screen edges");
    sDirty |= ImGui::SliderFloat("Field of view", &v.fovScale, 0.75f, 1.5f, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("Scales the camera's field of view (1 = original). Higher shows more of the room.");

    ImGui::SeparatorText("Resolution and display");
    {
        int& rs = v.resolutionScale;
        char label[64];
        if (rs <= 0) {
            std::snprintf(label, sizeof(label), "Match window");
        } else {
            const AuroraWindowSize ws = aurora_get_window_size();
            const float aspect = ws.native_fb_height ? float(ws.native_fb_width) / float(ws.native_fb_height) : 4.0f / 3.0f;
            const int h = 480 * rs;
            const int w = int(float(h) * std::max(aspect, 4.0f / 3.0f) + 0.5f);
            std::snprintf(label, sizeof(label), "%dx  (%d x %d)", rs, w, h);
        }
        if (ImGui::SliderInt("Render resolution", &rs, 0, 8, label, ImGuiSliderFlags_AlwaysClamp)) {
            sDirty = true;
            applyResolution();
        }
        ImGui::SetItemTooltip("Internal resolution as a multiple of the console's 480 lines (1x = 640x480).\n"
                              "Match window: the window's own resolution. Higher is sharper and costs GPU time.");
    }
    if (ImGui::Checkbox("Fullscreen", &v.fullscreen)) {
        sDirty = true;
        applyWindowSettings();
    }
    ImGui::SetItemTooltip("The game window fills the whole screen.");
    sDirty |= ImGui::Checkbox("VSync", &v.vsync);
    ImGui::SetItemTooltip("Frames wait for the display's refresh: no tearing, but the frame rate is limited to\n"
                          "the refresh rate. Applies on the next launch.");
    pending("next launch");
    static const char* kMsaa[] = {"Off", "2x", "4x", "8x"};
    int msaaIdx = v.msaa >= 8 ? 3 : v.msaa >= 4 ? 2 : v.msaa >= 2 ? 1 : 0;
    if (ImGui::Combo("Anti-aliasing", &msaaIdx, kMsaa, 4)) {
        v.msaa = 1 << msaaIdx;
        sDirty = true;
    }
    ImGui::SetItemTooltip("Multisample anti-aliasing: smoother polygon edges. Higher settings cost more GPU time\n"
                          "and memory. Applies on the next launch.");
    pending("next launch");
    {
        // The European game paces its frames with a timer, so 50 Hz does not change its speed;
        // it is the console's PAL video timing (VI retrace and its callbacks).
        Gate g(port::palBaseFeature());
        int hz = v.pal60 ? 1 : 0;
        static const char* kRates[] = {"50 Hz (original PAL)", "60 Hz (PAL60)"};
        if (ImGui::Combo("PAL refresh rate", &hz, kRates, 2)) {
            v.pal60 = hz == 1;
            sDirty = true;
        }
        ImGui::SetItemTooltip("European game only: the console's video timing, 50 Hz PAL or 60 Hz PAL60. The game\n"
                              "paces itself with a timer, so its speed is the same either way.");
    }

    ImGui::SeparatorText("CRT filter");
    sDirty |= recomp::crt::drawMenu();
}

// Volume slider shown in percent (the setting is 0..1).
bool volumeSlider(const char* label, float& v) {
    float pct = v * 100.0f;
    if (!ImGui::SliderFloat(label, &pct, 0.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) return false;
    v = pct / 100.0f;
    return true;
}

void soundTab() {
    auto& a = port::settings().audio;
    ImGui::SeparatorText("Volume");
    sDirty |= volumeSlider("Master volume", a.masterVolume);
    ImGui::SetItemTooltip("Everything the game plays.");
    sDirty |= volumeSlider("Music volume", a.musicVolume);
    ImGui::SetItemTooltip("Background music: the game's music sequences and streamed music.");
    sDirty |= volumeSlider("Sound effects volume", a.sfxVolume);
    ImGui::SetItemTooltip("Everything else: sound effects, voices, ambience.");
    ImGui::SeparatorText("Output");
    static const char* kOutputs[] = {"Stereo", "Mono", "Surround (matrix-encoded stereo)", "Surround 5.1 (discrete)",
                                     "Surround 7.1 (discrete)"};
    sDirty |= combo("Output", a.output, kOutputs, 5);
    ImGui::SetItemTooltip(
            "The game's own sound option (its options menu changes the same setting).\n"
            "Stereo / Mono: the game's stereo or mono mix.\n"
            "Surround: the game's surround mode as a console outputs it: stereo with the\n"
            "surround channel matrix-encoded (in phase left, inverted right) for a Dolby\n"
            "Pro Logic / Pro Logic II decoder.\n"
            "5.1 / 7.1: the game's surround mode with its centre and surround channels\n"
            "played on their own speakers instead of matrix-encoded. Needs a device with\n"
            "6 / 8 channels (else falls back to 5.1 or matrix-encoded stereo).");
    {
        const int dev = recomp::audio::outDeviceChannels();
        if (dev > 0) ImGui::TextDisabled("Device: %d channels. Playing: %s", dev, recomp::audio::outStatus());
        else ImGui::TextDisabled("Playing: %s", recomp::audio::outStatus());
    }
    sDirty |= ImGui::Checkbox("Swap left/right", &a.swapChannels);
    ImGui::SetItemTooltip("Exchanges the left and right channels (e.g. for headphones worn the other way round).");
    sDirty |= ImGui::Checkbox("High-quality resampler", &a.sincResampler);
    ImGui::SetItemTooltip("Converts the console's 32028 Hz output to the device rate with a 64-tap\n"
                          "windowed-sinc filter (flat to 13.8 kHz, images above 16 kHz removed).\n"
                          "Off: SDL's resampler, slightly cheaper.");
    ImGui::SeparatorText("DSP accuracy");
    sDirty |= ImGui::Checkbox("Reverb / echo", &a.reverb);
    ImGui::SetItemTooltip("The DSP's effect lines (feedback delays used as room echo) and its chorus bus.\n"
                          "Off: dry sound only, slightly cheaper.");
    sDirty |= ImGui::Checkbox("Voice filters", &a.voiceFilters);
    ImGui::SetItemTooltip("Per-voice FIR / IIR / distortion stages the game asks the DSP for (muffled or\n"
                          "distorted sounds). Off: those sounds play unfiltered, slightly cheaper.");
    recomp::unused2::soundSection();
}

// Gameplay / difficulty (port::GameplaySettings, src/recomp/gameplay_hooks.cpp). All neutral by
// default (ctrl+click a slider to type a value).
void gameplaySection() {
    auto& g = port::settings().gameplay;
    ImGui::TextDisabled("Neutral by default: the original game is unchanged until you move something.");
    ImGui::SeparatorText("Lighting");
    sDirty |= ImGui::Checkbox("Hidden Mansion darkness everywhere", &g.palDarknessEverywhere);
    ImGui::SetItemTooltip("Room ambient light x0.2 in every mansion, as in the European Hidden Mansion.");
    sDirty |= ImGui::SliderFloat("Ambient light", &g.ambientScale, 0.05f, 1.5f, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("Multiplies room ambient light (1 = original; also on top of the darkness above).");
    ImGui::SeparatorText("Difficulty");
    sDirty |= ImGui::SliderFloat("Damage taken", &g.damageScale, 0.0f, 5.0f, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("Multiplies the damage Luigi takes (1 = original, 0 = none).");
    sDirty |= ImGui::SliderFloat("Luigi's health", &g.healthScale, 0.25f, 5.0f, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("Starting and maximum health (1 = original; applies when Luigi is next created).");
    sDirty |= ImGui::SliderFloat("Ghost health", &g.ghostHealthScale, 0.25f, 5.0f, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("Multiplies every ghost's HP (1 = original): higher takes longer to vacuum up.");
    sDirty |= ImGui::SliderFloat("Poltergust strength", &g.vacuumStrength, 0.25f, 4.0f, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("How fast the vacuum drains a captured ghost's HP (1 = original).");
    sDirty |= ImGui::Checkbox("Disable recovery hearts", &g.noHeartDrops);
    ImGui::SetItemTooltip("Recovery hearts never appear (and any that do heal nothing).");
    ImGui::SeparatorText("Movement");
    sDirty |= ImGui::Checkbox("Always ride the Poltergust", &g.alwaysRide);
    ImGui::SetItemTooltip("Luigi glides with momentum everywhere (the European Boolossus-fight movement).");
    if (ImGui::Button("Reset gameplay options")) {
        g = port::GameplaySettings{};
        sDirty = true;
    }
    ImGui::SetItemTooltip("Puts the options above back to their neutral defaults (the unfinished Luigi states\n"
                          "below keep their settings).");
    recomp::unused2::gameplaySection();
}

void generalEnhancementsTab() {
    auto& e = port::settings().enhancements;
    ImGui::SeparatorText("Interface");
    sDirty |= ImGui::Checkbox("Show FPS", &e.showFps);
    ImGui::SetItemTooltip("Frames per second shown in the top-left corner.");
    if (ImGui::Checkbox("Pause when window loses focus", &e.pauseOnFocusLoss)) {
        sDirty = true;
        applyWindowSettings();
    }
    ImGui::SetItemTooltip("The game pauses (and goes quiet) while another window is in front.");
    ImGui::SeparatorText("Cutscenes");
    sDirty |= ImGui::Checkbox("Skippable cutscenes", &e.skipCutscenes);
    ImGui::SetItemTooltip("Press START during a cutscene to skip it. The game plays the rest of the\n"
                          "cutscene at high speed behind a black screen, so everything it sets up\n"
                          "(items, keys, doors, story progress) is the same as after watching it.\n"
                          "Choices and the save screen still wait for you.");
    sDirty |= ImGui::Checkbox("Hold Tab to fast-forward", &e.fastForwardKey);
    ImGui::SetItemTooltip("While Tab is held the game runs as fast as it can, muted.");
    ImGui::SeparatorText("Fixes");
    sDirty |= ImGui::Checkbox("Fix rooms vanishing near doorways", &e.portalClipFix);
    ImGui::SetItemTooltip("The game hides a whole room behind a doorway when the camera gets so close\n"
                          "to the doorway that part of it is behind the camera (an original bug, e.g.\n"
                          "the upper foyer during the cutscene when Luigi first enters). On: the doorway\n"
                          "is clipped properly and the room stays visible. Off: original behaviour.");
    ImGui::SeparatorText("Hidden Mansion");
    {
        // Switching mid-visit would leave PAL data/patches half applied, so only allow it outside.
        const bool inside = recomp::hm::hiddenActive();
        const port::Avail avail[2] = {port::hiddenMansionUs(), port::hiddenMansionPal()};
        const bool pal = recomp::hm::variant() == recomp::hm::Variant::Pal;
        // Either release's rules on either game (the other one's needs both discs; docs/SETUP.md).
        const bool usGame = port::install().running == port::GameVersion::USA;
        const char* kVariants[] = {usGame ? "USA (original)" : "USA (easier, not mirrored)",
                                   usGame ? "European (harder, mirrored)" : "European (original)"};
        auto usable = [&](int i) { return avail[i] == port::Avail::Yes; };
        if (recomp::hm::labChoiceAvailable()) {
            // Both releases' data: E. Gadd's lab asks on every visit (docs/HIDDEN_MANSION.md).
            ImGui::TextUnformatted("Hidden Mansion version: chosen in E. Gadd's lab");
            ImGui::SetItemTooltip("Picking the Hidden Mansion in E. Gadd's lab asks which one:\n"
                                  "\"(USA)\" is the %s release's, \"(PAL)\" the %s release's\n"
                                  "(harder, mirrored, darker). The save file keeps the answer for that visit.",
                                  usGame ? "original USA" : "USA", usGame ? "European" : "original European");
            if (inside) ImGui::TextDisabled("This visit: %s", kVariants[pal ? 1 : 0]);
        } else {
            ImGui::BeginDisabled(inside);
            if (ImGui::BeginCombo("Hidden Mansion version", kVariants[pal ? 1 : 0])) {
                for (int i = 0; i < 2; i++) {
                    if (ImGui::Selectable(kVariants[i], (pal ? 1 : 0) == i,
                                          usable(i) ? 0 : ImGuiSelectableFlags_Disabled)) {
                        e.hiddenMansionPal = i == 1;
                        recomp::hm::setVariant(e.hiddenMansionPal ? recomp::hm::Variant::Pal : recomp::hm::Variant::Us);
                        sDirty = true;
                    }
                    if (const char* note = port::availNote(avail[i])) {
                        ImGui::SameLine();
                        ImGui::TextDisabled("(%s)", note);
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("Which release's Hidden Mansion rules the game uses: the USA one, or the European one\n"
                                  "(harder ghosts and bosses, mirrored, darker). Both need that release's disc.%s",
                                  inside ? "\n\nLeave the Hidden Mansion to change it." : "");
            if (inside) pending("leave the Hidden Mansion to change");
        }
        if (!usable(0)) pending(port::availNote(avail[0]));
        else if (!usable(1)) pending(port::availNote(avail[1]));
    }
}

// Restored unused/beta content (docs/UNUSED_CONTENT.md). Every entry is off by default:
// the retail game is unchanged unless the player turns something on here.
void unusedContentTab() {
    using recomp::unused2::UnusedGroup;
    auto& e = port::settings().enhancements;
    ImGui::TextDisabled("Content left unused in the retail game. Everything here is off by default.");
    // Checkbox + tooltip + "(note)" after it (the tooltip belongs to the checkbox, not the note).
    auto item = [&](const char* label, bool& v, const char* tip, const char* note) {
        sDirty |= ImGui::Checkbox(label, &v);
        ImGui::SetItemTooltip("%s", tip);
        if (note) pending(note);
    };

    ImGui::SeparatorText("Game Boy Horror");
    item("Game Boy Horror camera", e.gbhCamera,
         "The live first-person view on the Game Boy Horror screen (bottom right) shown in\n"
         "pre-release builds; the retail game still has the whole camera. D-pad Up switches it\n"
         "on and off in game. E. Gadd's calls still show his portrait.",
         "D-pad Up in game");
    item("Game Boy Horror E3 clock", e.gbhClock,
         "The cut E3 2001 clock and radar border screen of the Game Boy Horror (minutes and\n"
         "seconds, border segments animated by the ghost tracker). D-pad Left toggles it in game.",
         "D-pad Left in game");
    item("Game Boy Horror ghost tracker", e.gbhTracker,
         "The unused ghost tracker of the Game Boy Horror screen. D-pad Down cycles it in game:\n"
         "off, far, close.",
         "D-pad Down: off / far / close");
    item("Game Boy Horror map: full Luigi model", e.betaMapLuigi,
         "The Game Boy Horror map shows Luigi as the small full-body figure from the older map\n"
         "archive left on the disc, instead of the big Luigi head.",
         "next map load");

    ImGui::SeparatorText("Luigi and the Poltergust");
    item("Early Luigi model", e.earlyLuigi,
         "Luigi uses the unused early model left on the disc (/model/luige.szp): slightly\n"
         "different face, hair and clothes. Same skeleton, so all retail animations, the\n"
         "Poltergust, flashlight and Game Boy Horror work as usual. Also used by the opening\n"
         "and E. Gadd cutscene Luigis.",
         "next map load");
    recomp::unused2::unusedSection(UnusedGroup::Player);
    item("Air and water nozzles", e.airWaterNozzles,
         "The Poltergust's unused nozzle models: the red wpair nozzle when blowing air\n"
         "(L without an element) and the blue wpwater nozzle when spraying water.\n"
         "Vacuum, fire and ice keep the retail nozzle.",
         nullptr);
    item("Early Poltergust 3000", e.earlyPoltergust,
         "The E3 2001 Poltergust model still on the disc (/model/vbody.szp): a red canister\n"
         "on a wheeled base instead of the retail tank.",
         "next map load");

    ImGui::SeparatorText("Ghosts and characters");
    {
        static const char* kGhost[] = {"Retail", "Beta"};
        int gs = e.earlyGhostLighting ? 1 : 0;
        if (ImGui::Combo("Ghost shading", &gs, kGhost, 2)) {
            e.earlyGhostLighting = gs == 1;
            sDirty = true;
        }
        ImGui::SetItemTooltip("Retail: ghosts are drawn into their own layer and added to the picture (the glowing,\n"
                              "see-through look). Beta: ghosts are drawn directly with the room, solid and opaque\n"
                              "as in early builds.");
    }
    item("Early Bogmire model", e.earlyBogmire,
         "Bogmire uses his unused early model (/model/shadow.szp): longer wings, thinner\n"
         "hanging flaps, a defined chin and a wavy ghost tail, with its own animations.",
         "next map load");
    item("Portrificationizer: each ghost's own model", e.unusedPortraitModels,
         "In the Portrificationizer scene each portrait ghost uses its own unused model instead\n"
         "of the generic blue blob.",
         nullptr);
    item("Green Toad (kinopio2)", e.greenToad,
         "Toads wear the unused green texture set of the never-placed kinopio2 character\n"
         "(TCRF's Action Replay code), with the unused green-capped dialogue icon.",
         nullptr);
    recomp::unused2::unusedSection(UnusedGroup::Characters);

    ImGui::SeparatorText("Mansion");
    item("Beta skybox", e.betaSkybox,
         "The mansion grounds (the opening, the gardens and every window and balcony) use the\n"
         "older sky model left in the game archive. Very close to the retail sky.",
         "next map load");
    item("Heart door (Chauncey's room)", e.heartDoor,
         "The Nursery door gets the unused heart door model (type 07), and the game plays its\n"
         "unused locked / unlock cutscenes. Retail uses a plain door.",
         "next map load");
    item("Star door (Twins' room)", e.starDoor,
         "The Twins' room door gets the unused star door model (type 08), and the game plays its\n"
         "unused locked / unlock cutscenes. Retail uses a plain door.",
         "next map load");

    ImGui::SeparatorText("2-player");
    item("2-player mode (unfinished)", e.twoPlayer,
         "The game's leftover two-player mode: a second Luigi on controller port 2\n"
         "(assign a device to port 2 in Controls). One shared camera while both are\n"
         "close in the same room, top/bottom split screen when apart. As the game left\n"
         "it: player 2 is left behind at doors, has no health meter, and the HUD\n"
         "disappears in split screen.",
         "next map load");
    ImGui::BeginDisabled(!e.twoPlayer);
    ImGui::Indent();
    item("Co-op improvements", e.coopImprovements,
         "Makes the 2-player mode playable as co-op: player 2 is brought along when\n"
         "player 1 changes rooms, the HUD stays visible in split screen, player 2 gets\n"
         "a health meter, and a downed player is revived with half of the partner's\n"
         "health (game over only when both are down).",
         nullptr);
    recomp::unused2::unusedSection(UnusedGroup::TwoPlayer);
    ImGui::Unindent();
    ImGui::EndDisabled();
    ImGui::SeparatorText("Screens");
    recomp::unused2::unusedSection(UnusedGroup::Screens);
    recomp::dev::unusedSection();  // depth blur pass, then its own Unused areas / Unused rooms sections
}

// Gameplay toggles, difficulty and cheats.
void gameplayTab() { gameplaySection(); }

// Debug features left in the game, warps and viewers.
void debugTab() {
    recomp::dev::debugSection();
    recomp::unused2::debugSection();
}

void enhancementsTab() {
    if (ImGui::BeginTabBar("enhancements")) {
        if (ImGui::BeginTabItem("General", nullptr, envTab("General"))) {
            generalEnhancementsTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Gameplay", nullptr, envTab("Gameplay"))) {
            gameplayTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Unused content", nullptr, envTab("Unused content"))) {
            unusedContentTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Debug", nullptr, envTab("Debug"))) {
            debugTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void fpsOverlay() {
    using clock = std::chrono::steady_clock;
    static auto last = clock::now();
    static int frames = 0;
    static float fps = 0.0f;
    frames++;
    const auto now = clock::now();
    const float dt = std::chrono::duration<float>(now - last).count();
    if (dt >= 0.5f) {
        fps = frames / dt;
        frames = 0;
        last = now;
    }
    ImGui::SetNextWindowPos(ImVec2(8, 8));
    ImGui::SetNextWindowBgAlpha(0.4f);
    ImGui::Begin("##fps", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::Text("%.0f fps", fps);
    // Interpolated presentation: how it is paced, and why it is below the target if it is.
    if (const char* st = recomp::pacing::status(); st && *st) ImGui::TextDisabled("%s", st);
    ImGui::End();
}

}  // namespace

void init() {
    port::loadSettings();
    applyWindowSettings();
    // LM_MENU=<tab> (testing/screenshots): start with the menu open on that tab.
    if (const char* tab = std::getenv("LM_MENU"); tab && *tab) {
        sOpen = true;
        sOpenTab = tab;
    }
    if (idcheck::active()) sOpen = true;
}

void openTreesForCheck() {
    if (idcheck::active()) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
}

bool isOpen() { return sOpen || recomp::dev::capturesInput(); }
bool quitRequested() { return sQuit; }

void shutdown() {
    // Settings are written when the menu closes; closing the window (or Alt+F4) while it is
    // still open would otherwise drop every change made in this visit.
    if (sDirty) {
        port::saveSettings();
        sDirty = false;
    }
}

void handleEvent(const SDL_Event& ev) {
    if (recomp::controls::handleEvent(ev)) return;  // binding capture owns the key
    if (recomp::dev::handleKey(ev)) return;
    if (ev.type != SDL_EVENT_KEY_DOWN || ev.key.repeat) return;
    if (ev.key.key == SDLK_F1) sOpen = !sOpen;
    else if (ev.key.key == SDLK_ESCAPE && sOpen) sOpen = false;
    if (!sOpen && sDirty) {
        port::saveSettings();
        sDirty = false;
    }
}

// Test aid LM_SETTINGS_AT=<frame>:<section>.<key>=<value>[,...][;<frame>:...]: changes settings
// (ini names) live once the game reaches that frame (copyDispCount, the LM_FRAME_DUMP number),
// through the same path as the F1 menu (the settings struct, then the per-frame apply calls
// below). Not saved. E.g. LM_SETTINGS_AT="400:enhanced_lighting.shadow_darkness=0.2".
void scriptedSettings() {
    struct Change {
        u32 frame;
        std::string section, key, value;
    };
    static std::vector<Change> sChanges = [] {
        std::vector<Change> out;
        const char* v = std::getenv("LM_SETTINGS_AT");
        if (!v) return out;
        std::string s = v;
        size_t pos = 0;
        while (pos < s.size()) {
            size_t end = s.find(';', pos);
            if (end == std::string::npos) end = s.size();
            const std::string group = s.substr(pos, end - pos);
            pos = end + 1;
            const size_t colon = group.find(':');
            if (colon == std::string::npos) continue;
            const u32 frame = u32(std::strtoul(group.c_str(), nullptr, 10));
            size_t p = colon + 1;
            while (p < group.size()) {
                size_t e = group.find(',', p);
                if (e == std::string::npos) e = group.size();
                const std::string item = group.substr(p, e - p);
                p = e + 1;
                const size_t dot = item.find('.'), eq = item.find('=');
                if (dot == std::string::npos || eq == std::string::npos || eq < dot) continue;
                out.push_back({frame, item.substr(0, dot), item.substr(dot + 1, eq - dot - 1), item.substr(eq + 1)});
            }
        }
        return out;
    }();
    if (sChanges.empty()) return;
    const u32 now = copyDispCount();
    for (auto it = sChanges.begin(); it != sChanges.end();) {
        if (now >= it->frame) {
            const bool ok = port::setSetting(it->section, it->key, it->value);
            std::printf("[settings] frame %u: %s.%s=%s%s\n", now, it->section.c_str(), it->key.c_str(),
                        it->value.c_str(), ok ? "" : " (unknown key)");
            std::fflush(stdout);
            it = sChanges.erase(it);
        } else {
            ++it;
        }
    }
}

void draw() {
    scriptedSettings();
    skip::drawOverlay();
    recomp::gx::applyAccuracySettings();
    recomp::enh::applySettings();
    recomp::crt::update();
    if (port::settings().enhancements.showFps) fpsOverlay();
    // LM_FPS_LOG=1: presented host frames per second every 2 s (performance measurements).
    static const bool sFpsLog = std::getenv("LM_FPS_LOG") != nullptr;
    if (sFpsLog) {
        using clock = std::chrono::steady_clock;
        static auto last = clock::now();
        static int frames = 0;
        frames++;
        const float dt = std::chrono::duration<float>(clock::now() - last).count();
        if (dt >= 2.0f) {
            std::printf("[fps] %.1f phase %d\n", frames / dt, recomp::enh::perfPhase());
            std::fflush(stdout);
            frames = 0;
            last = clock::now();
        }
    }
    recomp::dev::drawOverlays();
    // LM_FRAME_TAG=1 (testing, tools/interp/screen_run.py): the replay shown in this present as a
    // bar code in the top-left corner: a white and a black reference cell, then 32 bits (present
    // number low 16, tape seq low 8, alpha x 255), most significant first, 16 px cells.
    static const bool sFrameTag = std::getenv("LM_FRAME_TAG") != nullptr;
    if (sFrameTag) {
        const auto info = recomp::interp::lastPresent();
        const u32 v = (info.present & 0xFFFF) << 16 | u32(info.bSeq & 0xFF) << 8 |
                      u32(std::clamp(int(std::lround(info.alpha * 255.0f)), 0, 255));
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const ImVec2 o = ImGui::GetMainViewport()->Pos;
        const float s = 16.0f / std::max(ImGui::GetIO().DisplayFramebufferScale.x, 1.0f);
        dl->AddRectFilled(o, ImVec2(o.x + s * 34, o.y + s), IM_COL32(0, 0, 0, 255));
        dl->AddRectFilled(o, ImVec2(o.x + s, o.y + s), IM_COL32(255, 255, 255, 255));
        for (int i = 0; i < 32; i++)
            if (v >> (31 - i) & 1)
                dl->AddRectFilled(ImVec2(o.x + s * (2 + i), o.y), ImVec2(o.x + s * (3 + i), o.y + s), IM_COL32(255, 255, 255, 255));
    }
    if (!sOpen) return;
    ImGui::SetNextWindowSize(ImVec2(680, 400), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    idcheck::beforeWindow();
    bool open = true;
    if (ImGui::Begin("Luigi's Mansion PC  (F1)", &open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::PushItemWidth(ImGui::GetWindowWidth() * 0.45f);
        if (ImGui::BeginTabBar("tabs")) {
            if (tab("Visuals")) {
                if (ImGui::BeginTabBar("visuals")) {
                    if (ImGui::BeginTabItem("General", nullptr, envTab("General"))) {
                        visualsTab();
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Rendering", nullptr, envTab("Rendering"))) {
                        renderingTab();
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Enhanced lighting", nullptr, envTab("Enhanced lighting"))) {
                        enhancedLightingTab();
                        ImGui::EndTabItem();
                    }
                    ImGui::EndTabBar();
                }
                ImGui::EndTabItem();
            }
            if (tab("Sound")) {
                soundTab();
                ImGui::EndTabItem();
            }
            if (tab("Controls")) {
                sDirty |= recomp::controls::drawTab();
                ImGui::EndTabItem();
            }
            if (tab("Enhancements")) {
                enhancementsTab();
                ImGui::EndTabItem();
            }
            if (tab("Game")) {
                gameTab();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::PopItemWidth();
        // Quit (with confirmation): ends the host loop the same way closing the window does.
        ImGui::Separator();
        if (ImGui::Button("Restore all defaults")) ImGui::OpenPopup("Restore defaults?");
        ImGui::SetItemTooltip("Every setting in this menu (including controller bindings) goes back to its\n"
                              "first-install default. Save files are not touched.");
        if (ImGui::BeginPopupModal("Restore defaults?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Restore every setting to its default? This cannot be undone.");
            ImGui::TextDisabled("A few options (marked in their tooltips) apply on the next launch.");
            if (ImGui::Button("Restore defaults")) {
                port::resetSettingsToDefaults();
                // Settings applied only when their control changes: apply the defaults now too.
                applyWindowSettings();
                const bool hmPal = port::settings().enhancements.hiddenMansionPal;
                if (!recomp::hm::hiddenActive() && !recomp::hm::labChoiceAvailable() &&
                    (hmPal ? port::hiddenMansionPal() : port::hiddenMansionUs()) == port::Avail::Yes)
                    recomp::hm::setVariant(hmPal ? recomp::hm::Variant::Pal : recomp::hm::Variant::Us);
                sDirty = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Quit game")) ImGui::OpenPopup("Quit?");
        ImGui::SetItemTooltip("Closes the game (asks first). Settings are saved; unsaved game progress is lost.");
        if (ImGui::BeginPopupModal("Quit?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Quit Luigi's Mansion? Unsaved progress will be lost.");
            if (ImGui::Button("Quit")) {
                if (sDirty) {
                    port::saveSettings();
                    sDirty = false;
                }
                sQuit = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        idcheck::insideWindow();
    }
    ImGui::End();
    idcheck::afterWindow();
    if (!open) sOpen = false;
    if (!sOpen && sDirty) {
        port::saveSettings();
        sDirty = false;
    }
}

}  // namespace recomp::menu
