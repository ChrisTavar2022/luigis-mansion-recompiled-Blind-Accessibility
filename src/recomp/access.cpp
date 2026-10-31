// Screen reader support (docs/accessibility/README.md).
//
// Speech: one thread owns Tolk, because a guest or render thread must never wait for the screen
// reader. say() only queues.
//
// Menus: ImGui's gamepad navigation moves the focus, and everything else is read back from ImGui
// itself, so a new option needs no accessibility code. ImGui's log (the text every widget draws,
// with decorations such as "[x]" and "{value}") is on for the whole frame; the item hooks mark
// where the focused item and any tooltip are in it.
//
// The game is played with a controller, and so are the spoken menus. Pressing the right stick in
// repeats the last announcement, the left stick opens the Go to menu (access_nav.cpp), and pressing
// both sticks in opens the options menu.
//
// Test aids: LM_ACCESS=1 runs all of this without speaking (for automated runs: with a screen
// reader on the PC, a run on Auto would talk), LM_ACCESS=0 turns it off, LM_ACCESS=speak forces
// real speech. LM_ACCESS_LOG=<file> writes every announcement. LM_VIRTUAL_PAD (controls.cpp) drives
// the menus with a scripted controller.
#include "recomp/access.h"
#include "recomp/access_nav.h"

#include "port/settings.h"

#include <backends/imgui_impl_sdl3.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

extern void (*lmImguiItemAdd)(ImGuiContext* ctx, ImGuiID id);
extern void (*lmImguiItemInfo)(ImGuiContext* ctx, ImGuiID id, const char* label, ImGuiItemStatusFlags flags);

namespace recomp::access {
namespace {

enum class Env { None, Off, Silent, Speak };

Env envMode() {
    static const Env sMode = [] {
        const char* v = std::getenv("LM_ACCESS");
        if (!v) return Env::None;
        if (std::strcmp(v, "speak") == 0) return Env::Speak;
        return *v == '0' ? Env::Off : Env::Silent;
    }();
    return sMode;
}

// ---- Speech thread ---------------------------------------------------------------------------

struct Queue {
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::pair<std::string, bool>> items;
    bool ready = false;
};
Queue& queue() {
    static Queue* q = new Queue;  // never destroyed: the thread outlives static destructors
    return *q;
}

std::atomic<bool> sDetected{false};  // a screen reader is running
std::atomic<bool> sActive{false};
std::atomic<bool> sTrySapi{false};  // setting On with no screen reader: Windows speech

#ifdef _WIN32
struct Tolk {
    void(__cdecl* load)();
    bool(__cdecl* trySapi)(bool);
    const wchar_t*(__cdecl* detect)();
    bool(__cdecl* output)(const wchar_t*, bool);
};

bool loadTolk(Tolk& t) {
    HMODULE dll = LoadLibraryW(L"Tolk.dll");
    if (!dll) {
        std::printf("[access] Tolk.dll not found next to the program: no speech\n");
        return false;
    }
    t.load = reinterpret_cast<decltype(t.load)>(reinterpret_cast<void*>(GetProcAddress(dll, "Tolk_Load")));
    t.trySapi = reinterpret_cast<decltype(t.trySapi)>(reinterpret_cast<void*>(GetProcAddress(dll, "Tolk_TrySAPI")));
    t.detect = reinterpret_cast<decltype(t.detect)>(reinterpret_cast<void*>(GetProcAddress(dll, "Tolk_DetectScreenReader")));
    t.output = reinterpret_cast<decltype(t.output)>(reinterpret_cast<void*>(GetProcAddress(dll, "Tolk_Output")));
    if (!t.load || !t.trySapi || !t.detect || !t.output) {
        std::printf("[access] Tolk.dll lacks a function: no speech\n");
        return false;
    }
    t.load();
    return true;
}

std::wstring wide(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}
#endif

void speechThread() {
#ifdef _WIN32
    Tolk tolk{};
    const bool speak = envMode() != Env::Silent && envMode() != Env::Off && loadTolk(tolk);
    if (speak) {
        const wchar_t* name = tolk.detect();
        sDetected = name != nullptr;
        std::printf("[access] screen reader: %ls\n", name ? name : L"none");
    }
#endif
    auto& q = queue();
    {
        std::lock_guard lk(q.m);
        q.ready = true;
    }
    q.cv.notify_all();
    bool sapi = false;
    for (;;) {
        std::pair<std::string, bool> item;
        {
            std::unique_lock lk(q.m);
            q.cv.wait(lk, [&] { return !q.items.empty(); });
            item = std::move(q.items.front());
            q.items.pop_front();
        }
#ifdef _WIN32
        if (speak) {
            if (sapi != sTrySapi) {
                sapi = sTrySapi;
                tolk.trySapi(sapi);
            }
            if (!tolk.output(wide(item.first).c_str(), item.second)) std::printf("[access] Tolk could not speak\n");
        }
#endif
    }
}

std::mutex sLastM;
std::string sLastSaid;

// ---- Reading the focused item ----------------------------------------------------------------

struct FrameScan {
    int itemStart = -1, itemEnd = -1;  // the focused item's text in the ImGui log
    std::string label;                 // from the item's info hook; combo boxes have none
    bool hasInfo = false;
    bool selectedTab = false;
    int items = 0, position = 0;  // items that can take focus in the focused window; the focused one
    int tabs = 0, tabPosition = 0;
    ImGuiID id = 0;
    int tipStart = -1, tipEnd = -1;
};
FrameScan sScan;
std::string sLastItem, sLastTip;
bool sSkipItem = false;  // this frame's text for the focused item is not its real state
std::unordered_map<ImGuiID, std::string> sItemSpeech;

std::string trimmed(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

void onItemAdd(ImGuiContext* ctx, ImGuiID id) {
    ImGuiContext& g = *ctx;
    const int pos = g.LogBuffer.size();
    // Position in the menu: tabs count in their own bar, everything else in the window.
    if (id != 0 && g.CurrentWindow == g.NavWindow && id != g.CurrentWindow->ID && g.CurrentWindow->DC.NavLayerCurrent == ImGuiNavLayer_Main &&
        !(g.LastItemData.ItemFlags & (ImGuiItemFlags_NoNav | ImGuiItemFlags_Disabled))) {
        ImGuiTabBar* bar = g.CurrentTabBar;
        ImGuiTabItem* tab = bar ? ImGui::TabBarFindTabByID(bar, id) : nullptr;
        if (!tab) sScan.items++;
        if (id == g.NavId) {
            if (tab) {
                sScan.tabPosition = ImGui::TabBarGetTabOrder(bar, tab) + 1;
                sScan.tabs = bar->Tabs.Size;
            } else {
                sScan.position = sScan.items;
            }
        }
    }
    if (sScan.tipStart >= 0 && sScan.tipEnd < 0) sScan.tipEnd = pos;
    if (sScan.itemStart >= 0 && sScan.itemEnd < 0) sScan.itemEnd = pos;
    if (id == g.NavId && g.NavWindow == g.CurrentWindow) {
        sScan.selectedTab = g.CurrentTabBar && g.CurrentTabBar->SelectedTabId == id;
        sScan.itemStart = pos;
        sScan.itemEnd = -1;
        sScan.label.clear();
        sScan.hasInfo = false;
        sScan.id = id;
    }
}

void onItemInfo(ImGuiContext* ctx, ImGuiID id, const char* label, ImGuiItemStatusFlags) {
    ImGuiContext& g = *ctx;
    const int pos = g.LogBuffer.size();
    if (std::strncmp(label, "##Tooltip_", 10) == 0) {
        sScan.tipStart = pos;
        sScan.tipEnd = -1;
        return;
    }
    if (id == sScan.id && sScan.itemStart >= 0 && !sScan.hasInfo) {
        sScan.itemEnd = pos;
        sScan.label = trimmed(std::string(label, ImGui::FindRenderedTextEnd(label)));
        sScan.hasInfo = true;
    }
}

std::string firstLine(const std::string& s) {
    const std::string t = trimmed(s);
    return trimmed(t.substr(0, t.find('\n')));
}

// Wrapped text: the log breaks lines where the window did.
std::string joinedLines(const std::string& s) {
    std::string out;
    for (const char c : trimmed(s)) {
        if (c != '\r' && c != '\n' && c != ' ') out += c;
        else if (!out.empty() && out.back() != ' ') out += ' ';
    }
    return out;
}

// The item's log text with ImGui's decorations turned into words.
std::string describe(const std::string& logged) {
    const std::string line = firstLine(logged);
    const auto rest = [&](size_t from) { return trimmed(line.substr(std::min(from, line.size()))); };
    const auto named = [&](const std::string& fallback) { return sScan.hasInfo && !sScan.label.empty() ? sScan.label : fallback; };
    const auto speech = sItemSpeech.find(sScan.id);
    const auto spoken = [&](const std::string& fallback) { return speech != sItemSpeech.end() ? speech->second : named(fallback); };
    if (line.rfind("[x]", 0) == 0) return spoken(rest(3)) + ", checked";
    if (line.rfind("[ ]", 0) == 0) return spoken(rest(3)) + ", not checked";
    if (line.rfind("[~]", 0) == 0) return spoken(rest(3)) + ", partly checked";
    if (line.rfind("(x)", 0) == 0) return spoken(rest(3)) + ", selected";
    if (line.rfind("( )", 0) == 0) return spoken(rest(3)) + ", not selected";
    if (!line.empty() && line[0] == '{') {
        const size_t close = line.find('}');
        const std::string value = trimmed(line.substr(1, close == std::string::npos ? std::string::npos : close - 1));
        const std::string name = close == std::string::npos ? std::string() : rest(close + 1);
        return spoken(name) + (sScan.hasInfo ? ", slider, " : ", combo box, ") + value;
    }
    if (!line.empty() && line[0] == '[') {
        const size_t close = line.find(']');
        return spoken(line.substr(1, close == std::string::npos ? std::string::npos : close - 1)) + ", button";
    }
    if (!line.empty() && line[0] == '/') {
        const size_t close = line.find('\\');
        return spoken(trimmed(line.substr(1, close == std::string::npos ? std::string::npos : close - 1))) +
               (sScan.selectedTab ? ", tab, selected" : ", tab");
    }
    return spoken(line);
}

void onNewFrame(ImGuiContext* ctx, ImGuiContextHook*) {
    sScan = {};
    sSkipItem = false;
    // A controller player's focus always counts, or A is ignored until the first D-pad press.
    if (sActive) ctx->NavCursorVisible = true;
    if (sActive) ImGui::LogToBuffer(0);  // 0: tree nodes keep their state
}

void onEndFrame(ImGuiContext* ctx, ImGuiContextHook*) {
    ImGuiContext& g = *ctx;
    if (!g.LogEnabled) return;
    const std::string text(g.LogBuffer.c_str(), size_t(g.LogBuffer.size()));
    const auto piece = [&](int a, int b) { return text.substr(size_t(a), size_t((b < 0 ? int(text.size()) : b) - a)); };
    if (sScan.itemStart < 0) sLastItem.clear();  // nothing focused: the next focus is new, even on the same item
    if (sScan.itemStart >= 0 && g.ActiveId != sScan.id && !sSkipItem) {
        std::string item = describe(piece(sScan.itemStart, sScan.itemEnd));
        const int position = sScan.tabs ? sScan.tabPosition : sScan.position, total = sScan.tabs ? sScan.tabs : sScan.items;
        if (position > 0 && total > 1) item += ", " + std::to_string(position) + " of " + std::to_string(total);
        if (item != sLastItem && !item.empty()) {
            sLastItem = item;
            sLastTip.clear();
            say(item);
        }
    }
    if (sScan.tipStart >= 0) {
        const std::string tip = joinedLines(piece(sScan.tipStart, sScan.tipEnd));
        if (tip != sLastTip && !tip.empty()) {
            sLastTip = tip;
            say(tip, false);
        }
    }
    ImGui::LogFinish();
}

}  // namespace

void init() {
    if (envMode() == Env::Off) return;
    auto& q = queue();
    std::thread(speechThread).detach();
    std::unique_lock lk(q.m);
    q.cv.wait_for(lk, std::chrono::seconds(3), [&] { return q.ready; });
}

bool active() { return sActive; }

void sync() {
    if (!ImGui::GetCurrentContext()) return;
    const int mode = port::settings().accessibility.mode;
    const bool on = envMode() == Env::Silent || envMode() == Env::Speak ||
                    (envMode() == Env::None && (mode == 1 || (mode == 0 && sDetected)));
    sTrySapi = envMode() != Env::Silent && on && !sDetected;
    ImGuiContext& g = *ImGui::GetCurrentContext();
    static bool sHooked = false;
    if (on && !sHooked) {
        sHooked = true;
        ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_AutoAll);  // whichever controller is in hand
        lmImguiItemAdd = onItemAdd;
        lmImguiItemInfo = onItemInfo;
        ImGuiContextHook hook;
        hook.Type = ImGuiContextHookType_NewFramePost;
        hook.Callback = onNewFrame;
        ImGui::AddContextHook(&g, &hook);
        hook.Type = ImGuiContextHookType_EndFramePre;
        hook.Callback = onEndFrame;
        ImGui::AddContextHook(&g, &hook);
    }
    if (on != sActive) {
        if (!on) say("Screen reader support off");
        sActive = on;
        g.TestEngineHookItems = on;
        ImGuiIO& io = ImGui::GetIO();
        if (on) io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        else io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
        if (on) say("Screen reader support on");
    }
}

void say(const std::string& text, bool interrupt) {
    if (!sActive || text.empty()) return;
    {
        std::lock_guard lk(sLastM);
        sLastSaid = text;
    }
    static const char* sLogPath = std::getenv("LM_ACCESS_LOG");
    if (sLogPath) {
        if (FILE* f = std::fopen(sLogPath, "a")) {
            std::fprintf(f, "%s%s\n", interrupt ? "" : "+ ", text.c_str());
            std::fclose(f);
        }
    }
    auto& q = queue();
    {
        std::lock_guard lk(q.m);
        if (interrupt) q.items.clear();
        q.items.emplace_back(text, interrupt);
    }
    q.cv.notify_all();
}

bool handleEvent(const SDL_Event& ev, bool goToAllowed) {
    if (!sActive) return false;
    static bool sLeftAlone = false;  // the left stick is down and the right has not joined it
    if (ev.type == SDL_EVENT_GAMEPAD_BUTTON_UP && SDL_GamepadButton(ev.gbutton.button) == SDL_GAMEPAD_BUTTON_LEFT_STICK) {
        if (sLeftAlone) {
            sLeftAlone = false;
            if (goToAllowed) nav::toggleMenu();
        }
        return false;
    }
    if (ev.type != SDL_EVENT_GAMEPAD_BUTTON_DOWN) return false;
    const auto button = SDL_GamepadButton(ev.gbutton.button);
    if (button == SDL_GAMEPAD_BUTTON_EAST && nav::menuOpen() && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
        nav::toggleMenu();
        return false;
    }
    if (button != SDL_GAMEPAD_BUTTON_LEFT_STICK && button != SDL_GAMEPAD_BUTTON_RIGHT_STICK) return false;
    SDL_Gamepad* pad = SDL_GetGamepadFromID(ev.gbutton.which);
    if (pad && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_STICK) && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_STICK)) {
        sLeftAlone = false;
        return !nav::menuOpen();
    }
    if (button == SDL_GAMEPAD_BUTTON_LEFT_STICK) {
        sLeftAlone = true;
        return false;
    }
    std::string last;
    {
        std::lock_guard lk(sLastM);
        last = sLastSaid;
    }
    say(last);
    return false;
}

void itemSpeech(const std::string& text) {
    if (sActive) sItemSpeech[ImGui::GetItemID()] = text;
}

int sliderKeys() {
    if (!sActive) return 0;
    ImGuiContext& g = *ImGui::GetCurrentContext();
    const ImGuiID id = ImGui::GetItemID();
    if (id == 0 || g.NavId != id) return 0;
    // A would hold the slider until it is pressed again: left and right change it directly here.
    if (g.ActiveId == id && g.ActiveIdSource != ImGuiInputSource_Mouse) {
        ImGui::ClearActiveID();
        sSkipItem = true;
    }
    static constexpr ImGuiKey kLeft[] = {ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadLStickLeft};
    static constexpr ImGuiKey kRight[] = {ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickRight};
    int dir = 0;
    for (const ImGuiKey key : kLeft) {
        ImGui::SetKeyOwner(key, id);
        if (ImGui::IsKeyPressed(key, ImGuiInputFlags_Repeat, id)) dir = -1;
    }
    for (const ImGuiKey key : kRight) {
        ImGui::SetKeyOwner(key, id);
        if (ImGui::IsKeyPressed(key, ImGuiInputFlags_Repeat, id)) dir = 1;
    }
    return dir;
}

}  // namespace recomp::access
