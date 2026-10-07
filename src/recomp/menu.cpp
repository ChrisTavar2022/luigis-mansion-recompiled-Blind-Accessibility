// In-game overlay menu (F1 toggles, Esc closes): Visuals / Sound / Enhancements / Game.
// Drawn with Aurora's Dear ImGui on the host thread between the guest's present
// and aurora_end_frame. While it is open the game receives neutral pad input.
#include "recomp/menu.h"

#include "port/install.h"
#include "port/settings.h"
#include "recomp/crt.h"
#include "recomp/hidden_mansion.h"
#include "recomp/skip.h"

#include <aurora/aurora.h>
#include <imgui.h>
#include <SDL3/SDL.h>

#include <dolphin/vi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace recomp::menu {
namespace {

bool sOpen = false;
bool sDirty = false;
std::string sOpenTab;  // tab to select once (LM_MENU)

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
    aurora_set_pause_on_focus_lost(port::settings().enhancements.pauseOnFocusLoss);
}

template <typename E>
bool combo(const char* label, E& value, const char* const* names, int count) {
    int i = int(value);
    const bool changed = ImGui::Combo(label, &i, names, count);
    if (changed) value = E(i);
    return changed;
}

void pending(const char* what) {
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", what);
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
            }
        }
        if (ImGui::Button("Add or change discs...")) {
            port::saveSettings();
            port::relaunch(true);
        }
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
    }
    if (port::palBaseFeature() == port::Avail::Yes)
        ImGui::TextDisabled("Used for a new save; an existing save keeps its language (in-game Options).");
}

void visualsTab() {
    auto& v = port::settings().video;
    static const char* kFrameRates[] = {"Original (30 fps)", "Interpolated 60 fps", "Interpolated, unlimited"};
    sDirty |= combo("Frame rate", v.frameRate, kFrameRates, 3);
    static const char* kAspects[] = {"Original 4:3", "Match window", "16:9", "16:10", "21:9", "32:9"};
    sDirty |= combo("Aspect ratio", v.aspect, kAspects, 6);
    {
        Gate g(port::usCodeFeature());
        sDirty |= ImGui::Checkbox("Keep HUD at 4:3", &v.hudKeep4x3);
        if (!v.hudKeep4x3 && g.a == port::Avail::Yes) pending("in-game HUD moves to the screen edges");
    }
    sDirty |= ImGui::SliderFloat("Field of view", &v.fovScale, 0.75f, 1.5f, "%.2fx");
    ImGui::Separator();
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
    }
    if (ImGui::Checkbox("Fullscreen", &v.fullscreen)) {
        sDirty = true;
        applyWindowSettings();
    }
    sDirty |= ImGui::Checkbox("VSync", &v.vsync);
    pending("next launch");
    static const char* kMsaa[] = {"Off", "2x", "4x", "8x"};
    int msaaIdx = v.msaa >= 8 ? 3 : v.msaa >= 4 ? 2 : v.msaa >= 2 ? 1 : 0;
    if (ImGui::Combo("Anti-aliasing", &msaaIdx, kMsaa, 4)) {
        v.msaa = 1 << msaaIdx;
        sDirty = true;
    }
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
    }
    ImGui::Separator();
    sDirty |= recomp::crt::drawMenu();
}

void soundTab() {
    auto& a = port::settings().audio;
    sDirty |= ImGui::SliderFloat("Master volume", &a.masterVolume, 0.0f, 1.0f);
    sDirty |= ImGui::SliderFloat("Music volume", &a.musicVolume, 0.0f, 1.0f);
    sDirty |= ImGui::SliderFloat("Sound effects volume", &a.sfxVolume, 0.0f, 1.0f);
    static const char* kOutputs[] = {"Stereo", "Mono", "Surround (Pro Logic II matrix)"};
    sDirty |= combo("Output", a.output, kOutputs, 3);
    sDirty |= ImGui::Checkbox("Swap left/right", &a.swapChannels);
    sDirty |= ImGui::Checkbox("Reverb / echo", &a.reverb);
}

void enhancementsTab() {
    auto& e = port::settings().enhancements;
    sDirty |= ImGui::Checkbox("Show FPS", &e.showFps);
    if (ImGui::Checkbox("Pause when window loses focus", &e.pauseOnFocusLoss)) {
        sDirty = true;
        applyWindowSettings();
    }
    sDirty |= ImGui::Checkbox("Skip intro logos", &e.skipIntroLogos);
    pending("planned");
    {
        Gate g(port::usCodeFeature());
        sDirty |= ImGui::Checkbox("Skippable cutscenes", &e.skipCutscenes);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Press START during a cutscene to skip it. The game plays the rest of the\n"
                              "cutscene at high speed behind a black screen, so everything it sets up\n"
                              "(items, keys, doors, story progress) is the same as after watching it.\n"
                              "Choices and the save screen still wait for you.");
    }
    sDirty |= ImGui::Checkbox("Hold Tab to fast-forward", &e.fastForwardKey);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("While Tab is held the game runs as fast as it can, muted.");
    ImGui::Separator();
    {
        // Switching mid-visit would leave PAL data/patches half applied, so only allow it outside.
        const bool inside = recomp::hm::hiddenActive();
        const port::Avail avail[2] = {port::hiddenMansionUs(), port::hiddenMansionPal()};
        const bool pal = recomp::hm::variant() == recomp::hm::Variant::Pal;
        static const char* kVariants[] = {"USA (original)", "European (harder, mirrored)"};
        auto usable = [&](int i) { return avail[i] == port::Avail::Yes; };
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
        if (inside) pending("leave the Hidden Mansion to change");
        else if (!usable(0)) pending(avail[0] == port::Avail::NotYet ? "USA option: not on the European game yet" : port::availNote(avail[0]));
        else if (!usable(1)) pending(port::availNote(avail[1]));
    }
    ImGui::Separator();
    // Unused content restored from the retail code (docs/UNUSED_CONTENT.md).
    {
        Gate g(port::usCodeFeature());
        sDirty |= ImGui::Checkbox("Game Boy Horror camera", &e.gbhCamera);
        if (g.a == port::Avail::Yes) pending("D-pad Up in game");
    }
    {
        Gate g(port::usCodeFeature());
        sDirty |= ImGui::Checkbox("Early (opaque) ghost shading", &e.earlyGhostShading);
    }
    ImGui::Separator();
    ImGui::TextDisabled("Gameplay toggles, cheats and debug viewers will appear here.");
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
}

bool isOpen() { return sOpen; }

void handleEvent(const SDL_Event& ev) {
    if (ev.type != SDL_EVENT_KEY_DOWN || ev.key.repeat) return;
    if (ev.key.key == SDLK_F1) sOpen = !sOpen;
    else if (ev.key.key == SDLK_ESCAPE && sOpen) sOpen = false;
    if (!sOpen && sDirty) {
        port::saveSettings();
        sDirty = false;
    }
}

void draw() {
    skip::drawOverlay();
    recomp::crt::update();
    if (port::settings().enhancements.showFps) fpsOverlay();
    if (!sOpen) return;
    ImGui::SetNextWindowSize(ImVec2(680, 400), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    bool open = true;
    if (ImGui::Begin("Luigi's Mansion PC  (F1)", &open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::PushItemWidth(ImGui::GetWindowWidth() * 0.45f);
        if (ImGui::BeginTabBar("tabs")) {
            if (tab("Visuals")) {
                visualsTab();
                ImGui::EndTabItem();
            }
            if (tab("Sound")) {
                soundTab();
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
    }
    ImGui::End();
    if (!open) sOpen = false;
    if (!sOpen && sDirty) {
        port::saveSettings();
        sDirty = false;
    }
}

}  // namespace recomp::menu
