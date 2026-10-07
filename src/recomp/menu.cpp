// In-game overlay menu (F1 toggles, Esc closes): Visuals / Sound / Enhancements.
// Drawn with Aurora's Dear ImGui on the host thread between the guest's present
// and aurora_end_frame. While it is open the game receives neutral pad input.
#include "recomp/menu.h"

#include "port/settings.h"
#include "recomp/hidden_mansion.h"
#include "recomp/skip.h"

#include <aurora/aurora.h>
#include <imgui.h>
#include <SDL3/SDL.h>

#include <dolphin/vi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace recomp::menu {
namespace {

bool sOpen = false;
bool sDirty = false;

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

void visualsTab() {
    auto& v = port::settings().video;
    static const char* kFrameRates[] = {"Original (30 fps)", "Interpolated 60 fps", "Interpolated, unlimited"};
    sDirty |= combo("Frame rate", v.frameRate, kFrameRates, 3);
    pending("in progress");
    static const char* kAspects[] = {"Original 4:3", "Match window", "16:9", "16:10", "21:9", "32:9"};
    sDirty |= combo("Aspect ratio", v.aspect, kAspects, 6);
    sDirty |= ImGui::Checkbox("Keep HUD at 4:3", &v.hudKeep4x3);
    pending("edge anchoring planned");
    sDirty |= ImGui::SliderFloat("Field of view", &v.fovScale, 0.75f, 1.5f, "%.2fx");
    pending("in progress");
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
    sDirty |= ImGui::Checkbox("Skippable cutscenes", &e.skipCutscenes);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Press START during a cutscene to skip it. The game plays the rest of the\n"
                          "cutscene at high speed behind a black screen, so everything it sets up\n"
                          "(items, keys, doors, story progress) is the same as after watching it.\n"
                          "Choices and the save screen still wait for you.");
    sDirty |= ImGui::Checkbox("Hold Tab to fast-forward", &e.fastForwardKey);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("While Tab is held the game runs as fast as it can, muted.");
    ImGui::Separator();
    {
        // Switching mid-visit would leave PAL data/patches half applied, so only allow it outside.
        const bool inside = recomp::hm::hiddenActive();
        ImGui::BeginDisabled(inside);
        int v = e.hiddenMansionPal ? 1 : 0;
        static const char* kVariants[] = {"USA (original)", "PAL (harder, mirrored)"};
        if (ImGui::Combo("Hidden Mansion version", &v, kVariants, 2)) {
            e.hiddenMansionPal = v == 1;
            recomp::hm::setVariant(e.hiddenMansionPal ? recomp::hm::Variant::Pal : recomp::hm::Variant::Us);
            sDirty = true;
        }
        ImGui::EndDisabled();
        if (inside) pending("leave the Hidden Mansion to change");
    }
    ImGui::Separator();
    // Unused content restored from the retail code (docs/UNUSED_CONTENT.md).
    sDirty |= ImGui::Checkbox("Game Boy Horror camera", &e.gbhCamera);
    pending("D-pad Up in game");
    sDirty |= ImGui::Checkbox("Early (opaque) ghost shading", &e.earlyGhostShading);
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
    if (port::settings().enhancements.showFps) fpsOverlay();
    if (!sOpen) return;
    ImGui::SetNextWindowSize(ImVec2(680, 400), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    bool open = true;
    if (ImGui::Begin("Luigi's Mansion PC  (F1)", &open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::PushItemWidth(ImGui::GetWindowWidth() * 0.45f);
        if (ImGui::BeginTabBar("tabs")) {
            if (ImGui::BeginTabItem("Visuals")) {
                visualsTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Sound")) {
                soundTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Enhancements")) {
                enhancementsTab();
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
