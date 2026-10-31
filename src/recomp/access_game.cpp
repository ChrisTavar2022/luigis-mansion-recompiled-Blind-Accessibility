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
#include "recomp/access_nav.h"

#include "port/install.h"
#include "port/settings.h"
#include "recomp/guest_util.h"
#include "recomp/hle.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

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
constexpr u32 kCopyCursor = 3;   // 1 File 2, 2 File 3: only files that are free
constexpr u32 kEraseCursor = 2;  // 0 Yes, 1 No
constexpr u32 kScreenFiles = 0, kScreenOptions = 0x14;
constexpr u32 kScreenCopyTo = 3, kScreenCopySaving = 6, kScreenCopied = 4;
constexpr u32 kScreenErase = 7, kScreenEraseSaving = 0x0A, kScreenErased = 8;

// Save records in memory, one per file (fn_801859E0). fn_80049818 shows them on the file screen.
constexpr u32 kFileRecords = 0x803ED36C, kFileRecordSize = 0x3B0, kFileCount = 3;
constexpr u32 kFileSavedBit = 1;      // u16 at +0
constexpr u32 kFileShowsBoos = 0x0B;  // byte, bit 8
constexpr u32 kFileFlags = 0x338, kFileBoos = 0x368, kFileBooBits = 48;
constexpr u32 kFileRank = 0x3A7, kFileMoney = 0x3A8;
constexpr u32 kAreaNames = 0x8033455C;  // pointers, indexed by areaReached()

// Account object of the area-clear screens (bytes). States run 0, 3, 4, 5, 6, 7.
constexpr u32 kAccountState = 1, kAccountCaught = 3, kAccountTotal = 5, kAccountMoney = 6, kAccountDone = 7;
constexpr u32 kAccountRow = 0x7AC, kAccountRowSize = 6, kAccountRows = 23;

// Menu object of the save screen (bytes).
constexpr u32 kSaveState = 1;   // 0 the question, 0x0B saving, 0x0C saved, waits for A
constexpr u32 kSaveDone = 0x0C;
constexpr u32 kSaveCursor = 2;  // 0 Save, 1 Never mind

// Menu object of the pause screen.
constexpr u32 kPauseCursor = 0x11A;  // byte: 1 Continue, 2 Quit, 3 No, 4 Yes
constexpr u32 kPauseStyle = 0x124;   // word: 1 Standard, 2 Sidestep
constexpr u32 kStyleValue = 4;       // control style screen, word: 1 Standard, 2 Sidestep

constexpr u32 kActorSlots = 0x803D48A0;  // slot 0 is Luigi
constexpr u32 kHealth = 0xFC, kCounters = 0x12F4, kCounterCount = 10;
constexpr u32 kCaughtType = 0x68;  // in Luigi's capture message, the type the game adds to his list (+0x11F0)
constexpr u32 kGhostTypes = 0x8033B590, kGhostTypeSize = 0x1C, kGhostTypeName = 0x18, kGhostTypeCount = 0x29C8 / 0x1C;
constexpr u32 kGhostTypeFlags = 0x08, kGhostTypeFlag = 8;
constexpr s32 kCounterMax = 99999, kHealthMax = 999;
constexpr auto kMapSettle = std::chrono::seconds(3);  // a new map's actor fills in stages
constexpr u32 kActPos = 0x44, kActRoom = 0xB4;
constexpr u32 kRoomTable = 0x804D8170, kRoomSize = 0x30, kRoomCount = 74;
constexpr u32 kMapTable = 0x802FF218, kMapDoors = 0x18;
constexpr u32 kRoomInfo = 0x80C, kRoomNameIndex = 0x20;  // Luigi +0x80C (fn_800B1B34), read by fn_800458D0
constexpr u32 kCurMap = 0x804D80C8, kMansionMap = 2;

using clock = std::chrono::steady_clock;

// Every game menu says its items the same way: "Save, 1 of 2". index counts from 0.
std::string positioned(const std::string& text, u32 index, u32 count) {
    return text + ", " + std::to_string(index + 1) + " of " + std::to_string(count);
}

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
    const std::string item = positioned(choices[selected], selected, count);
    if (item == sLast) return;
    const bool first = sLast.empty();
    sLast = item;
    say(item, !first);  // the first one follows the question
}

// The words for a capture, by the start of the game's type name. Portrait ghosts carry the names on
// the game's own plaques (e_ad_ghostname_NN.bti). Types left out are not known for certain.
const char* ghostName(const std::string& type) {
    static const struct {
        const char* type;
        const char* name;
    } kNames[] = {
        {"father", "Neville"},
        {"mother", "Lydia"},
        {"baby", "Chauncey"},
        {"bbaby", "Chauncey"},
        {"eater", "Mr. Luggs"},
        {"dancer", "The Floating Whirlindas"},
        {"pianist", "Melody Pianissima"},
        {"situji", "Shivers"},
        {"uranai", "Madame Clairvoya"},
        {"dog01", "Spooky"},
        {"bfire", "Bogmire"},
        {"builder", "Biff Atlas"},
        {"obaasan", "Nana"},
        {"fat", "Miss Petunia"},
        {"hustler", "Slim Bankshot"},
        {"boy", "Henry and Orville"},
        {"denwa", "Uncle Grimmly"},
        {"btelesa", "Boolossus"},
        {"tubone", "Jarvis"},
        {"htubone", "Jarvis"},
        {"gaka", "Vincent Van Gore"},
        {"girl", "Sue Pea"},
        {"doll", "Clockwork Soldier"},
        {"snowman", "Sir Weston"},
        {"mgyapoo", "Gold Ghost"},
        {"yapoo", "Gold Ghost"},
        {"iyapoo", "Speedy Spirit"},
        {"heypo", "Shy Guy"},
        {"tenjyo", "Ceiling Surprise"},
        {"banaoba", "Banana Ghost"},
        {"waiter", "Waiter Ghost"},
        {"telesa", "Boo"},
        {"goldrat", "Gold Mouse"},
        {"rat", "Mouse"},
        {"bat", "Bat"},
    };
    for (const auto& n : kNames)
        if (type.rfind(n.type, 0) == 0) return n.name;
    return "Ghost";
}

// Captures, gathered so several at once are one announcement. Filled on the guest thread.
std::mutex sCaughtM;
std::vector<std::pair<std::string, u32>> sCaught;
clock::time_point sCaughtAt{};

void pollCaught() {
    std::lock_guard lk(sCaughtM);
    if (sCaught.empty() || clock::now() - sCaughtAt < std::chrono::seconds(1)) return;
    for (const auto& [name, count] : sCaught) say((count > 1 ? std::to_string(count) + " " : std::string()) + name + " caught", false);
    sCaught.clear();
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
ScreenRun sTitle, sFileMenu, sPause, sStyle, sSave, sAccount;
std::atomic<u32> sFileMenuAddr{0}, sPauseAddr{0}, sStyleAddr{0}, sSaveAddr{0}, sAccountAddr{0};

void pollTitle() {
    static bool sSaid = false;
    const bool on = sTitle.running();
    if (on && !sSaid) say("Luigi's Mansion. Press Start.");
    sSaid = on;
}

std::string optionsRow(u32 menu) {
    static const char* const kSound[] = {"Mono", "Stereo", "Surround"};
    const u32 row = rd8(menu + kOptionsCursor);
    switch (row) {
    case 0: return positioned(std::string("Sound: ") + kSound[std::min<u32>(rd8(menu + kSound_), 2)], row, 6);
    case 1: return positioned(std::string("Rumble: ") + (rd8(menu + kRumble) ? "On" : "Off"), row, 6);
    case 2: return positioned(std::string("Controls: ") + (rd8(menu + kControls) ? "Sidestep" : "Standard"), row, 6);
    case 3: return positioned("Brightness", row, 6);
    case 4: return positioned("OK", row, 6);
    default: return positioned("View a Pikmin Movie", 5, 6);
    }
}

bool fileSaved(u32 file) { return rd16(kFileRecords + file * kFileRecordSize) & kFileSavedBit; }

// The same flags fn_80049818 tests to choose the file's progress text.
u32 areaReached(u32 record) {
    const u32 flags = record + kFileFlags;
    if (rd8(flags + 0x1B) & 0x10) return 3;
    if (rd8(flags + 0x1A) & 0x20) return 2;
    return rd8(flags + 8) & 1;
}

// What the file screen shows for a file, as pictures and digits.
std::string fileSummary(u32 file) {
    std::string out = "File " + std::to_string(file + 1);
    if (!fileSaved(file)) return out + ", empty";
    const u32 record = kFileRecords + file * kFileRecordSize;
    const std::string area = gameText(rd32(kAreaNames + areaReached(record) * 4), 32);
    if (!area.empty()) out += ", " + area;
    out += ", money " + std::to_string(rd32(record + kFileMoney));
    if (rd8(record + kFileShowsBoos) & 8) {
        u32 boos = 0;
        for (u32 i = 0; i < kFileBooBits; i++) boos += (rd8(record + kFileBoos + i / 8) >> (i % 8)) & 1;
        out += ", Boos " + std::to_string(boos);
    }
    const u32 rank = rd8(record + kFileRank);  // 1 H, the worst, to 8 A
    if (rank >= 1 && rank <= 8) out += std::string(", rank ") + "HGFEDCBA"[rank - 1];
    return out;
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
    std::string heading, item;
    switch (screen) {
    case kScreenFiles: {
        const u32 cursor = rd8(menu + kFileCursor);
        heading = "Which file would you like to load? On a saved file, Y copies, and B on an Xbox controller erases.";
        item = positioned(cursor < kFileCount ? fileSummary(cursor) : std::string("Options"), cursor, 4);
        break;
    }
    case kScreenOptions:
        heading = "Options";
        item = optionsRow(menu);
        break;
    case kScreenCopyTo: {
        heading = "Which file would you like to copy to?";
        const u32 target = rd8(menu + kCopyCursor);
        u32 index = 0, count = 0;  // only free files can be chosen
        for (u32 file = 0; file < kFileCount; file++) {
            if (fileSaved(file)) continue;
            if (file < target) index++;
            count++;
        }
        item = positioned("File " + std::to_string(target + 1), index, count);
        break;
    }
    case kScreenErase:
        heading = "Do you really want to erase this file?";
        item = positioned(rd8(menu + kEraseCursor) == 0 ? "Yes" : "No", rd8(menu + kEraseCursor), 2);
        break;
    case kScreenCopySaving:
    case kScreenEraseSaving:
        heading = "Saving. Do not turn off the game.";
        break;
    case kScreenCopied:
        heading = "The file has been copied.";
        break;
    case kScreenErased:
        heading = "The file has been erased.";
        break;
    default:
        return;  // a screen not mapped yet
    }
    if (screen != sLastScreen) {
        sLastScreen = screen;
        say(heading);
        sLast.clear();
    }
    if (item.empty() || item == sLast) return;
    const bool first = sLast.empty();
    sLast = item;
    say(item, !first);
}

// The area-clear result screens (fn_800501EC), pictures too. Account object +1 is the state.
void pollAccount() {
    static u32 sLast = ~0u;
    const u32 account = sAccountAddr;
    if (!sAccount.running() || !account) {
        sLast = ~0u;
        return;
    }
    const u32 state = rd8(account + kAccountState);
    if (state == sLast) return;
    sLast = state;
    switch (state) {
    case kAccountCaught: {
        u32 caught = 0;  // the game's own count
        for (u32 i = 0; i < kAccountRows; i++) caught += rd8(account + kAccountRow + i * kAccountRowSize) != 0;
        say("Results. Now you've caught " + std::to_string(caught) + (caught == 1 ? " ghost." : " ghosts."));
        break;
    }
    case kAccountTotal: say("Your total catch. Press A to continue."); break;
    case kAccountMoney: say("Now the treasure you've found. Press A until the total."); break;
    case kAccountDone: {
        std::string text;
        const u32 luigi = rd32(kActorSlots);
        if (gu::inMem1(luigi)) {
            s32 gems = 0;
            for (u32 i = 3; i < kCounterCount; i++) gems += s32(rd32(luigi + kCounters + 4 * i));
            const s32 coins = s32(rd32(luigi + kCounters)), bills = s32(rd32(luigi + kCounters + 4)), bars = s32(rd32(luigi + kCounters + 8));
            if (coins >= 0 && bills >= 0 && bars >= 0 && gems >= 0)
                text = "Coins " + std::to_string(coins) + ". Bills " + std::to_string(bills) + ". Gold bars " + std::to_string(bars) + ". Gems " + std::to_string(gems) + ". ";
        }
        say(text + "Press A to continue.");
        break;
    }
    default: break;
    }
}

// The save screen (fn_80054508), also pictures.
void pollSave() {
    static std::string sLast;
    static bool sSaving = false, sSaved = false;
    const u32 menu = sSaveAddr;
    if (!sSave.running() || !menu) {
        sSaving = sSaved = false;
        sLast.clear();
        return;
    }
    if (rd8(menu + kSaveState) == kSaveDone) {
        if (!sSaved) say("The game was saved. Press A to continue.");
        sSaved = true;
        return;
    }
    if (rd8(menu + kSaveState) != 0) {
        if (sLast.rfind("Save", 0) == 0 && !sSaving) {
            sSaving = true;
            say("Saving. Do not turn off the game.");
        }
        return;
    }
    const u32 cursor = rd8(menu + kSaveCursor);
    const std::string item = positioned(cursor == 0 ? "Save" : "Never mind", cursor, 2);
    if (item == sLast) return;
    if (sLast.empty()) say("Would you like to save up to this point?");
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
    const std::string item = cursor <= 2 ? positioned(kItems[cursor - 1], cursor - 1, 2) : positioned(kItems[cursor - 1], cursor - 3, 2);
    if (sLast.empty()) say("Paused. Control style " + style + ".");
    else if (style != sLastStyle) say("Control style " + style);
    sLastStyle = style;
    if (item == sLast) return;
    const bool first = sLast.empty();
    const bool quitAsked = cursor == 3 && sLast.rfind("Quit", 0) == 0;
    sLast = item;
    if (quitAsked) say("Do you want to quit?");
    say(item, !first && !quitAsked);
}

// Room names, the words on the game's name cards (the pictures in kawano/roomname, table at
// 0x80305F20 in the same order). Empty: a room with no card, which is a hallway.
constexpr const char* kRoomNames[] = {
    "Butler's Room", "Hidden Room", "Foyer", "Fortune-teller's Room", "Mirror Room", "Laundry Room", "", "",
    "Kitchen", "Dining Room", "Ball Room", "Boneyard", "Billiards Room", "Projection Room", "Storage Room", "",
    "Graveyard", "Washroom", "", "", "Bathroom", "Conservatory", "Rec Room", "Courtyard",
    "Nursery", "The Twins' Room", "", "Sitting Room", "Guest Room", "", "", "",
    "", "Master Bedroom", "Study", "Parlor", "Sealed Room", "", "Wardrobe Room", "Anteroom",
    "Astral Hall", "Observatory", "Washroom", "", "", "Bathroom", "Nana's Room", "Tea Room",
    "Armory", "", "Telephone Room", "", "Safari Room", "", "", "Ceramics Studio",
    "Clockwork Room", "The Artist's Studio", "", "Balcony", "", "Cold Storage", "", "Cellar",
    "", "", "Pipe Room", "Breaker Room", "", "Bottom of the Well", "Secret Altar", "",
};

const char* floorName(u32 floor) {
    switch (floor) {
    case 0: return "First floor";
    case 1: return "Second floor";
    case 2: return "Third floor";
    case 3: return "Roof";
    default: return "Basement";
    }
}

// A room's name by its room number (door records use those): the table's first word holds the
// number, the floor and the name index. Empty for a hallway.
const char* roomName(u32 number) {
    const u32 rooms = rd32(kRoomTable);
    if (rd32(kCurMap) != kMansionMap || number >= kRoomCount || !gu::inMem1(rooms)) return "";
    const u32 index = rd8(rooms + number * kRoomSize + 3);
    return index < std::size(kRoomNames) ? kRoomNames[index] : "";
}

}  // namespace

std::string doorName(u32 room) {
    if (rd32(kCurMap) != kMansionMap || room >= kRoomCount) return "";
    const char* name = roomName(room);
    return std::string(*name ? name : "Hallway") + " door";
}

namespace {

// What a locked door leads to: the nearest door of Luigi's room, the room on its other side.
std::string lockedDoorText() {
    const u32 luigi = rd32(kActorSlots);
    const u32 map = rd32(kMapTable + 4 * rd32(kCurMap));
    const u32 doors = gu::inMem1(map) ? rd32(map + kMapDoors) : 0;
    if (!gu::inMem1(luigi) || !gu::inMem1(doors) || rd32(kCurMap) != kMansionMap) return "This door is locked";
    const u32 room = rd32(luigi + kActRoom) >> 24;
    const float x = gu::rdf(luigi + kActPos), z = gu::rdf(luigi + kActPos + 8);
    float best = 1e30f;
    u32 other = ~0u;
    for (u32 i = 0; i < 128; i++) {
        const u32 d = doors + i * 0x1C;
        if (rd8(d) == 0) break;
        const u32 a = rd8(d + 0x1A), b = rd8(d + 0x1B);
        if (a != room && b != room) continue;
        const float dist = std::hypot(float(s32(rd32(d + 8))) - x, float(s32(rd32(d + 0x10))) - z);
        if (dist < best) {
            best = dist;
            other = a == room ? b : a;
        }
    }
    const char* name = other == ~0u ? "" : roomName(other);
    return *name ? std::string(name) + " is locked" : std::string("This door is locked");
}

// The room Luigi walks into, then the floor when that changed too. Only the mansion has these names.
void pollRoom(u32 luigi) {
    static u32 sRoom = ~0u, sFloor = ~0u;
    if (!port::settings().accessibility.announceRooms || rd32(kCurMap) != kMansionMap) {
        sRoom = sFloor = ~0u;
        return;
    }
    const u32 room = rd32(luigi + kRoomInfo + kRoomNameIndex) & 0xFF;
    if (room == sRoom) return;
    const bool first = sRoom == ~0u;
    sRoom = room;
    const char* name = room < std::size(kRoomNames) ? kRoomNames[room] : "";
    say(*name ? name : "Hallway", false);
    // The room table's own floor, once Luigi's room number is known.
    const u32 rooms = rd32(kRoomTable);
    if (!gu::inMem1(rooms)) return;
    const u32 floor = rd8(rooms + (rd32(luigi + kActRoom) >> 24) * kRoomSize + 1);
    if (floor != sFloor && !first) say(floorName(floor), false);
    sFloor = floor;
}

// Luigi's health and money, spoken when they change.
// Actor +0xFC is health, +0x12F4 onwards the treasure counters.
void pollStatus() {
    const u32 luigi = rd32(kActorSlots);
    if (!gu::inMem1(luigi)) return;
    static s32 sHealth = -1;
    static u32 sHealthMap = ~0u;
    static clock::time_point sMapStart{};
    const s32 health = s16(rd16(luigi + kHealth));
    // The actor holds junk during a map load, and a new map's first value is no change.
    const bool sound = health >= 0 && health <= kHealthMax;
    if (rd32(kCurMap) != sHealthMap) sMapStart = clock::now();
    if (!sound || rd32(kCurMap) != sHealthMap) {
        sHealth = -1;
        sHealthMap = rd32(kCurMap);
    }
    const bool settled = clock::now() - sMapStart > kMapSettle;
    if (sound && health != sHealth) {
        if (sHealth > 0 && health > 0 && settled) say("Health " + std::to_string(health), false);  // 0: luigiDown
        sHealth = health;
    }
    if (sound && health > 0) pollRoom(luigi);
    // Money. A burst of coins gives one total: speak once the counters have been still briefly.
    // Coins, bills, gold bars; the rest are gems. They are reloaded when Luigi's actor changes, and
    // that is not a pickup.
    static u32 sActor = 0, sMap = ~0u;
    static s32 sCounters[kCounterCount] = {}, sSpoken[kCounterCount] = {};
    static clock::time_point sChanged{};
    static bool sPending = false;
    // A map load keeps the actor's address and fills it in stages, with junk between: start over
    // from the first sound values.
    for (u32 i = 0; i < kCounterCount; i++) {
        const s32 v = s32(rd32(luigi + kCounters + 4 * i));
        if (v < 0 || v > kCounterMax) {
            sActor = 0;
            sPending = false;
            return;
        }
    }
    const bool fresh = luigi != sActor || rd32(kCurMap) != sMap || !settled;
    sActor = luigi;
    sMap = rd32(kCurMap);
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
    const bool sidestep = rd32(menu + kStyleValue) == 2;
    const std::string style = positioned(sidestep ? "Sidestep" : "Standard", sidestep ? 1 : 0, 2);
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

void luigiDown(u32 actor) {
    if (port::usCodeFeature() == port::Avail::Yes && actor == rd32(kActorSlots)) say("Game over", false);
}

void ghostCaught(u32 data) {
    if (port::usCodeFeature() != port::Avail::Yes || !gu::inMem1(data)) return;
    // Keys, hearts and other items come through the same message: the game's own text covers those.
    const u32 type = rd32(data + kCaughtType);
    if (type >= kGhostTypeCount || !(rd16(kGhostTypes + type * kGhostTypeSize + kGhostTypeFlags) & kGhostTypeFlag)) return;
    const std::string name = gameText(rd32(kGhostTypes + type * kGhostTypeSize + kGhostTypeName), 16);
    std::lock_guard lk(sCaughtM);
    sCaughtAt = clock::now();
    for (auto& [caught, count] : sCaught)
        if (caught == ghostName(name)) {
            count++;
            return;
        }
    sCaught.push_back({ghostName(name), 1});
}

void pollGame() {
    if (port::usCodeFeature() != port::Avail::Yes) return;
    watchMemory();
    // Test aid LM_ACCESS_POKE=<frame>:<address>:<value>: one guest word, to open a screen.
    if (const char* v = std::getenv("LM_ACCESS_POKE")) {
        unsigned frame = 0, addr = 0, value = 0;
        static bool sDone = false;
        if (!sDone && std::sscanf(v, "%u:%x:%x", &frame, &addr, &value) == 3 && copyDispCount() >= frame) {
            sDone = true;
            wr32(addr, value);
        }
    }
    if (!active()) return;
    pollTitle();
    pollFileMenu();
    pollPause();
    pollSave();
    pollAccount();
    pollControlStyle();
    pollStatus();
    pollCaught();
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

// The door scenes: r3 is 1 for the locked door's scene. Every attempt on a door comes here, by hand or
// by the Go to menu.
HLE(fn_8011496C) {
    if (c.r[3] == 1) {
        access::say(access::lockedDoorText());
        access::nav::doorLocked();
    }
    callOriginal(c, "fn_8011496C");
}

// Save screen update: r3 is its menu object.
HLE(fn_80054508) {
    access::sSaveAddr = c.r[3];
    access::sSave.calls++;
    callOriginal(c, "fn_80054508");
}

// Pause screen update: r3 is its menu object.
HLE(fn_80038744) {
    access::sPauseAddr = c.r[3];
    access::sPause.calls++;
    callOriginal(c, "fn_80038744");
}

// Area-clear result screens update: r3 is the account object.
HLE(fn_800501EC) {
    access::sAccountAddr = c.r[3];
    access::sAccount.calls++;
    callOriginal(c, "fn_800501EC");
}

// File screen and Options update: r3 is its menu object.
HLE(fn_8004A004) {
    access::sFileMenuAddr = c.r[3];
    access::sFileMenu.calls++;
    callOriginal(c, "fn_8004A004");
}

}  // namespace recomp
