// Screen reader support for the game's own text (docs/accessibility/game-api.md). Runs once per host
// frame and only reads guest memory. USA game only for now: the addresses are not in the version
// map yet.
//
// Message windows (fn_800433B4 draws them): four slots, each with a 0x200 byte text buffer and a
// state word (0 closed, 1 opening, 2 writing, 4 waiting, 6 closing). The whole text is in the buffer
// as soon as a window opens, so it is spoken at once instead of following the letters.
//
// Choices (fn_80043F14 sets one up, fn_80043FA8 moves the cursor): four 0x80 byte text buffers and
// state words (4 = offered), and the selected index.
#include "recomp/access.h"

#include "port/install.h"
#include "recomp/guest_util.h"
#include "recomp/hle.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace recomp {
u32 copyDispCount();  // hle_gx.cpp
}

namespace recomp::access {
namespace {

constexpr u32 kText = 0x803CE7D0;        // + slot * 0x200
constexpr u32 kChoiceText = 0x803CEFD0;  // + index * 0x80
constexpr u32 kState = 0x803CF4C8;       // + slot * 4
constexpr u32 kChoiceState = 0x803CF4D8;  // + index * 4
constexpr u32 kChoiceSelected = 0x804D850C;  // r13+0x5AC
constexpr u32 kStateClosing = 6, kChoiceOffered = 4;

// Menu object of the file screen (bytes).
constexpr u32 kFileCursor = 1;     // 0-2 files, 3 Options
constexpr u32 kOptionsCursor = 4;  // Sound, Rumble, Controls, Brightness, OK, Pikmin movie
constexpr u32 kSound_ = 5;         // 0 Mono, 1 Stereo, 2 Surround
constexpr u32 kRumble = 6;
constexpr u32 kControls = 7;  // 0 Standard, 1 Sidestep
constexpr u32 kMenuScreen = 8;
constexpr u32 kScreenFiles = 0, kScreenOptions = 0x14;

// Menu object of the pause screen.
constexpr u32 kPauseCursor = 0x11A;  // byte: 1 Continue, 2 Quit, 3 No, 4 Yes
constexpr u32 kPauseStyle = 0x124;   // word: 1 Standard, 2 Sidestep
constexpr u32 kStyleValue = 4;       // control style screen, word: 1 Standard, 2 Sidestep

constexpr u32 kActorSlots = 0x803D48A0;  // slot 0 is Luigi
constexpr u32 kHealth = 0xFC, kCounters = 0x12F4, kCounterCount = 10;

using clock = std::chrono::steady_clock;

// The text without the game's control codes: ESC, two letters and an optional [argument], such as
// the colour codes ESC CC[ffffffff]. Line breaks are only layout.
std::string gameText(u32 addr, u32 len) {
    std::string out;
    for (u32 i = 0; i < len; i++) {
        const u8 b = rd8(addr + i);
        if (b == 0) break;
        if (b == 0x1B) {
            i += 2;
            if (i + 1 < len && rd8(addr + i + 1) == '[')
                while (i + 1 < len && rd8(addr + i) != ']' && rd8(addr + i) != 0) i++;
            continue;
        }
        if (b == '\n' || b == '\r') {
            if (!out.empty() && out.back() != ' ') out += ' ';
        } else if (b >= 0x20 && b < 0x7F) {
            out += char(b);
        }
    }
    return out;
}

void pollMessages() {
    static std::string sLast[4];
    for (u32 slot = 0; slot < 4; slot++) {
        const u32 state = rd32(kState + slot * 4);
        if (state == 0) {
            sLast[slot].clear();
            continue;
        }
        if (state == kStateClosing) continue;
        const std::string text = gameText(kText + slot * 0x200, 0x200);
        if (text == sLast[slot]) continue;
        sLast[slot] = text;
        say(text);
    }
}

void pollChoices() {
    static std::string sLast;
    std::string choices[4];
    u32 count = 0;
    for (u32 i = 0; i < 4 && rd32(kChoiceState + i * 4) == kChoiceOffered; i++)
        choices[count++] = gameText(kChoiceText + i * 0x80, 0x80);
    const u32 selected = rd32(kChoiceSelected);
    if (count == 0) {
        sLast.clear();
        return;
    }
    if (selected >= count) return;
    const std::string item = choices[selected] + ", " + std::to_string(selected + 1) + " of " + std::to_string(count);
    if (item == sLast) return;
    const bool first = sLast.empty();
    sLast = item;
    say(item, !first);  // the first one follows the question
}

// A screen's update function ran within the last moments. Hooks count calls on the guest thread.
struct ScreenRun {
    std::atomic<u32> calls{0};
    u32 seen = 0;
    clock::time_point last{};
    bool running() {
        const u32 now = calls;
        if (now != seen) {
            seen = now;
            last = clock::now();
        }
        return clock::now() - last < std::chrono::milliseconds(300);
    }
};
ScreenRun sTitle, sFileMenu, sPause, sStyle;
std::atomic<u32> sFileMenuAddr{0}, sPauseAddr{0}, sStyleAddr{0};

void pollTitle() {
    static bool sSaid = false;
    const bool on = sTitle.running();
    if (on && !sSaid) say("Luigi's Mansion. Press Start.");
    sSaid = on;
}

std::string optionsRow(u32 menu) {
    static const char* const kSound[] = {"Mono", "Stereo", "Surround"};
    switch (rd8(menu + kOptionsCursor)) {
    case 0: return std::string("Sound: ") + kSound[std::min<u32>(rd8(menu + kSound_), 2)];
    case 1: return std::string("Rumble: ") + (rd8(menu + kRumble) ? "On" : "Off");
    case 2: return std::string("Controls: ") + (rd8(menu + kControls) ? "Sidestep" : "Standard");
    case 3: return "Brightness";
    case 4: return "OK";
    default: return "View a Pikmin Movie";
    }
}

// The file screen and its Options screen (fn_8004A004). Text there is pictures, so the words come
// from here.
void pollFileMenu() {
    static std::string sLast;
    static u32 sLastScreen = ~0u;
    const u32 menu = sFileMenuAddr;
    if (!sFileMenu.running() || !menu) {
        sLast.clear();
        sLastScreen = ~0u;
        return;
    }
    const u32 screen = rd8(menu + kMenuScreen);
    std::string item;
    if (screen == kScreenFiles) {
        const u32 cursor = rd8(menu + kFileCursor);
        item = cursor < 3 ? "File " + std::to_string(cursor + 1) : std::string("Options");
    } else if (screen == kScreenOptions) {
        item = optionsRow(menu);
    } else {
        return;  // a screen not mapped yet
    }
    if (screen != sLastScreen) {
        sLastScreen = screen;
        say(screen == kScreenFiles ? "Which file would you like to load?" : "Options");
        sLast.clear();
    }
    if (item == sLast) return;
    const bool first = sLast.empty();
    sLast = item;
    say(item, !first);
}

// The pause screen (fn_80038744), also pictures.
void pollPause() {
    static std::string sLast, sLastStyle;
    const u32 menu = sPauseAddr;
    if (!sPause.running() || !menu) {
        sLast.clear();
        sLastStyle.clear();
        return;
    }
    const u32 cursor = rd8(menu + kPauseCursor);
    const std::string style = rd32(menu + kPauseStyle) == 2 ? "Sidestep" : "Standard";
    if (cursor < 1 || cursor > 4) return;
    static const char* const kItems[] = {"Continue", "Quit", "No", "Yes"};
    const std::string item = kItems[cursor - 1];
    if (sLast.empty()) say("Paused. Control style " + style + ".");
    else if (style != sLastStyle) say("Control style " + style);
    sLastStyle = style;
    if (item == sLast) return;
    const bool first = sLast.empty();
    const bool quitAsked = cursor == 3 && sLast == "Quit";
    sLast = item;
    if (quitAsked) say("Do you want to quit?");
    say(item, !first && !quitAsked);
}

// Luigi's health and money, spoken when they change. Actor +0xFC is health, +0x12F4 onwards the
// treasure counters.
void pollStatus() {
    const u32 luigi = rd32(kActorSlots);
    if (!gu::inMem1(luigi)) return;
    static s32 sHealth = -1;
    const s32 health = s16(rd16(luigi + kHealth));
    if (health != sHealth) {
        if (sHealth >= 0 && health >= 0) say("Health " + std::to_string(health), false);
        sHealth = health;
    }
    // Money. A burst of coins gives one total: speak once the counters have been still briefly.
    // Coins, bills, gold bars; the rest are gems. They are reloaded when Luigi's actor changes, and
    // that is not a pickup.
    static u32 sActor = 0;
    static s32 sCounters[kCounterCount] = {}, sSpoken[kCounterCount] = {};
    static clock::time_point sChanged{};
    static bool sPending = false;
    const bool fresh = luigi != sActor;
    sActor = luigi;
    for (u32 i = 0; i < kCounterCount; i++) {
        const s32 v = s32(rd32(luigi + kCounters + 4 * i));
        if (fresh) sCounters[i] = sSpoken[i] = v;
        if (v == sCounters[i]) continue;
        sCounters[i] = v;
        sChanged = clock::now();
        sPending = true;
    }
    if (!sPending || clock::now() - sChanged < std::chrono::milliseconds(500)) return;
    sPending = false;
    std::string text;
    s32 gems = 0, spokenGems = 0;
    for (u32 i = 0; i < kCounterCount; i++) {
        if (i >= 3) {
            gems += sCounters[i];
            spokenGems += sSpoken[i];
        } else if (sCounters[i] != sSpoken[i]) {
            static const char* const kNames[] = {"Coins ", "Bills ", "Gold bars "};
            text += kNames[i] + std::to_string(sCounters[i]) + ". ";
        }
        sSpoken[i] = sCounters[i];
    }
    if (gems != spokenGems) text += "Gems " + std::to_string(gems) + ". ";
    say(text, false);
}

// The control style screen in E. Gadd's first talk (fn_80051380).
void pollControlStyle() {
    static std::string sLast;
    const u32 menu = sStyleAddr;
    if (!sStyle.running() || !menu) {
        sLast.clear();
        return;
    }
    const std::string style = rd32(menu + kStyleValue) == 2 ? "Sidestep" : "Standard";
    if (style == sLast) return;
    if (sLast.empty()) say("Choose your control style. Left and right change it, A confirms.", false);
    sLast = style;
    say(style, false);
}

// Test aid LM_ACCESS_WATCH=<address>,<bytes>: logs guest memory whenever it changes, to find what a
// screen keeps its state in.
void watchMemory() {
    static u32 sAddr = 0, sLen = 0;
    static std::string sLast;
    static const bool sOn = [] {
        const char* v = std::getenv("LM_ACCESS_WATCH");
        return v && std::sscanf(v, "%x,%u", &sAddr, &sLen) == 2;
    }();
    if (!sOn) return;
    std::string now;
    for (u32 i = 0; i < sLen; i++) {
        char hex[4];
        std::snprintf(hex, sizeof(hex), "%02X ", rd8(sAddr + i));
        now += hex;
    }
    if (now == sLast) return;
    sLast = now;
    std::printf("[watch] frame %u: %s\n", copyDispCount(), now.c_str());
    std::fflush(stdout);
}

}  // namespace

bool gamePaused() { return sPause.running(); }

void pollGame() {
    if (port::usCodeFeature() != port::Avail::Yes) return;
    watchMemory();
    if (!active()) return;
    pollTitle();
    pollFileMenu();
    pollPause();
    pollControlStyle();
    pollStatus();
    pollMessages();
    pollChoices();
}

}  // namespace recomp::access

namespace recomp {

// Title screen update (TitleScreenPane), per frame while "Press Start" shows.
HLE(fn_8003A6FC) {
    access::sTitle.calls++;
    callOriginal(c, "fn_8003A6FC");
}

// Control style screen update: r3 is its object.
HLE(fn_80051380) {
    access::sStyleAddr = c.r[3];
    access::sStyle.calls++;
    callOriginal(c, "fn_80051380");
}

// Pause screen update: r3 is its menu object.
HLE(fn_80038744) {
    access::sPauseAddr = c.r[3];
    access::sPause.calls++;
    callOriginal(c, "fn_80038744");
}

// File screen and Options update: r3 is its menu object.
HLE(fn_8004A004) {
    access::sFileMenuAddr = c.r[3];
    access::sFileMenu.calls++;
    callOriginal(c, "fn_8004A004");
}

}  // namespace recomp
