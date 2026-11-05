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
#include <cstring>
#include <iterator>
#include <mutex>
#include <set>
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
constexpr u32 kHealth = 0xFC, kCounters = 0x12F4, kCounterCount = 10, kMoneyCounters = 12;  // spoken ones; all that fn_8004D688 adds up
constexpr u32 kGhostTypes = 0x8033B590, kGhostTypeSize = 0x1C, kGhostTypeName = 0x18, kGhostTypeCount = 0x29C8 / 0x1C;
constexpr u32 kGhostTypeFlags = 0x08, kGhostTypeFlag = 8;
constexpr s32 kCounterMax = 99999, kHealthMax = 999;
constexpr auto kMapSettle = std::chrono::seconds(3);  // a new map's actor fills in stages
constexpr u32 kActPos = 0x44, kActRoom = 0xB4;
constexpr u32 kRoomTable = 0x804D8170, kRoomSize = 0x30, kRoomCount = 74;
constexpr u32 kMapTable = 0x802FF218, kMapDoors = 0x18;
constexpr u32 kRoomInfo = 0x80C, kRoomNameIndex = 0x20;  // Luigi +0x80C (fn_800B1B34), read by fn_800458D0
constexpr u32 kCurMap = 0x804D80C8, kMansionMap = 2, kLabMap = 1, kTrainingMap = 3, kGalleryMap = 6;
constexpr u32 kEvent = 0x803D2D40, kEventCursor = 0x0C, kMenuOff = 0x804D8088;
constexpr u32 kRoomLights = 0x804D8178, kRoomLightSize = 0x14C, kRoomLightByte = 6, kRoomLightBit = 0x80;  // set when a room is cleared
constexpr u32 kElement = 0x1184, kElementFire = 2;
constexpr auto kStatusRestart = std::chrono::seconds(5);
constexpr u32 kCurScene = 0x804CFF98, kSceneOpening = 0x8021DBD8, kSceneMain = 0x8021DB68;
constexpr u32 kOpeningNext = 0x804CFFAC, kOpeningIsDemo = 1;  // the title screen's demo is the same scene
constexpr u32 kOpeningScript = 0x804CFFA8;  // fn_8000CE58: 76 the forest opening, 0 the short walk-up from the lab once flag 7 is set
constexpr u32 kLuigiStopped = 0x10;  // +0x40, <LUIGISTOP>
constexpr int kFoyerEvent = 48;
constexpr u32 kFlagBoosOut = 73, kFlagBlackout = 61;  // game flags (fn_80065FA4; 61 as fn_80037894 reads it)
bool gameFlag(u32 flag) { return (rd8(kEvent + 0x659 + flag / 8) >> (flag % 8)) & 1; }
constexpr u32 kMapFloor = 0x804D8314;  // r13+0x3B4, byte, signed: -1 basement, 0 first floor
constexpr u32 kRoomVisited = 0x803CDF50;  // u16 per room, bit 0

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

clock::time_point sDescribing{};  // a cutscene description lasts about this long

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
        // A description still being read is not cut off: the line follows it.
        say(text, clock::now() >= sDescribing);
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
        // Regular ghosts: internal names matched by the rooms jmp/enemyinfo puts them in against the
        // Super Mario Wiki's lists. Flash and Blue Blaze are by being the second of their kind, the
        // variants the wiki names; both grabber colours say Grabbing Ghost.
        {"yapoo2", "Temper Terror"},
        {"mapoo1", "Purple Puncher"},
        {"mapoo2", "Flash"},
        {"mopoo1", "Blue Twirler"},
        {"mopoo2", "Blue Blaze"},
        {"topoo2", "Mirror Ghost"},
        {"topoo3", "Cinema Ghost"},
        {"topoo", "Grabbing Ghost"},
        {"putcher", "Bowling Ghost"},
        {"tenjyo2", "Purple Bomber"},
        {"skul", "Mr. Bones"},
        {"eskul", "Mr. Bones"},
        {"bmtelesa", "Boo"},
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
        {"heypo", "Shy Guy Ghost"},
        {"tenjyo", "Ceiling Surprise"},
        {"banaoba", "Garbage Can Ghost"},
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
ScreenRun sTitle, sFileMenu, sPause, sStyle, sSave, sAccount, sGbhMap;
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

// What a room asks of the player, by name index, from the guides. Own words, kept short. Each is
// spoken once in a run, when the room reaches that state: a boss or portrait ghost's advice only
// while it is there, what comes after it once the lights are on. docs/accessibility/guide-notes.md
// has more.
// Dark: its ghosts are there. Cleared: they are gone. JustCleared: they went while Luigi watched, for
// a job that comes straight after and cannot be seen to be done later. Fire: Luigi holds fire and the
// room's candles are out. Seal: cleared, candles out. BoosIn: cleared, Boos not yet let out. Blackout:
// the power is off.
enum class When { Always, Dark, Cleared, JustCleared, Fire, Seal, BoosIn, Blackout };
struct RoomHint {
    u32 room;
    When when;
    const char* text;
};
constexpr RoomHint kRoomHints[] = {
    {35, When::Dark, "Vacuum the candles around the portrait on the back wall to bring out the ghosts. The Go to menu lists them under Candles."},
    {34, When::Dark, "Neville hides while you face him. Stand beside him with your back turned until he yawns and his heart shows, then turn and vacuum."},
    {33, When::Dark, "Lydia cannot be stunned while she combs her hair. Vacuum the right curtain to let in a draft, and catch her when she gets up to close it."},
    {38, When::Dark, "Two ghosts hide in the closets. Press A at them. Later, turn your back to the mirror to find a ghost."},
    {20, When::Dark, "Ghosts here grab Luigi. Wiggle the stick to shake them off."},
    {10, When::Dark, "Vacuum the masks off the ghosts, flash them, then vacuum them. Then ride the spinning floor with your back to the dancers until the heart shows."},
    {14, When::BoosIn, "Press the red wall button opposite the mirror. A wall rolls back. Further in, vacuum the Danger poster off the wall to find another button. Pressing that one lets the Boos out, and the story goes on from there."},
    {17, When::Dark, "Vacuum the toilet."},
    {3, When::Dark, "Flash the crystal ball to bring out Madame Clairvoya, then press A at her. She wants to hear about each of Mario's five items."},
    {3, When::Fire, "Light the candles in the four corners with the left trigger. A key then appears on top of the brass column beside the booth. The Go to menu lists them under Candles."},
    {4, When::Dark, "The ghosts here show only in the mirror, and you cannot see them in the room. They come at you, so keep flashing and vacuuming."},
    {4, When::Seal, "The door back to the Fortune-teller's Room is sealed, and it will not open until the seal is broken. Open the chest for the fire medal. Then vacuum up a fire spirit by the coals, and light the candle stands at each end of the room with the left trigger. The Go to menu lists them under Candles, and the spirits under Elements."},
    {5, When::JustCleared, "Vacuum the washing machine."},
    {0, When::Dark, "Shivers sits down here after fire scares him, and the candle burns him. Vacuum him while he is seated."},
    {0, When::JustCleared, "There is a mouse hole in the back right corner: press A at it in the search view."},
    {21, When::Dark, "Press A at each instrument except the piano. The drums are three. Then go to the piano. Answer water, or Super Mario Bros. 3. Vacuum up the sheet music."},
    {9, When::Dark, "Vacuum the bananas first. Light the candelabras beside Mr. Luggs with fire, catch his two servers, and vacuum up his food. Dodge his fireballs until he tires."},
    {8, When::Dark, "Vacuum the fridge door open. Shoot the ice ghost with fire."},
    {8, When::JustCleared, "Take water from the sink to put out the burning door."},
    {11, When::Dark, "Water the sprout by the faucet, and again after each boss. Dodge Spooky three times to bring out Mr. Bones. Catch him, and his bone draws Spooky out with his heart showing."},
    {16, When::Dark, "Shake the tombstones to bring out three skeletons. After them, go to the big tombstone."},
    {23, When::Always, "Press A at the birdhouse, and at the shack from its right side. There is a well to climb down."},
    {22, When::Dark, "Press A at a punching bag to swing it at Biff Atlas, then get clear. Three hits show his heart."},
    {22, When::JustCleared, "Walk on the treadmill for a few seconds, and a key appears."},
    {47, When::Always, "Plates fly here. The emblem in the near right corner puts you on the ceiling, and another drops you on the table, where the white chest is."},
    {40, When::Dark, "Light all the candelabras with fire to bring out the ghosts."},
    {41, When::Dark, "Press A at the telescope in the search view. Then vacuum up the meteor and throw it at the moon."},
    {46, When::Dark, "Knock over the yarn basket, then vacuum a ball and throw it at Nana. Three hits show her heart."},
    {45, When::Dark, "Pull back the curtain from the left wall, and dodge her water. Spray her with ice, then vacuum."},
    {25, When::Dark, "Spin the helicopter mobile on the ceiling with the vacuum. Play hide and seek: leave, come back, and vacuum the boxes. The ones that shake hold the twins. Vacuum the car and plane first."},
    {12, When::Dark, "Slim shoots balls. Vacuum one and throw it at him, three times, to show his heart. Look for his floating cue."},
    {13, When::Dark, "Press A at the projector. The ghosts are invisible: watch for puffs of smoke. Seven of them."},
    {52, When::Dark, "Shake the animal heads to bring out water ghosts. Spray them with ice."},
    {67, When::Blackout, "Press A at the generator's red switch in the right corner to bring back the power."},
    {63, When::Dark, "Vacuum the dirt off the floor before you fight, and shake the barrels to bring out the ghosts."},
    {50, When::Dark, "The ghosts hide in the trunks. Press A at them."},
    {56, When::Dark, "Shake every clock to start them. Vacuum the keys out of the soldiers' backs, then vacuum the soldiers."},
    {56, When::Cleared, "The door between the small clock towers is the lift to the roof."},
    {48, When::Dark, "Shake the boxes, then each suit of armor. Keep clear of the ones holding maces."},
    {55, When::Dark, "Press A at the blue vase and play. Jarvis pops out of vases: hit him with ice each time."},
    {36, When::Always, "Vacuum the cloth off the mirror, then press A at it in the search view to go back to the Foyer."},
    {27, When::Dark, "Light the two candles beside the painting with fire. Ghosts come in waves, then fire ghosts, so take water."},
    {28, When::Dark, "Spray Sue Pea with water three times to wake her. Vacuum her dolls first."},
    {66, When::Always, "Freeze the sewage with ice, walk across, and turn the wheel."},
    {61, When::Dark, "Take fire and light the two fires in the corner, then melt Sir Weston's ice block. If he freezes you, shake the stick."},
    {57, When::Dark, "Each painting sends out three ghosts, seven times over. After them the painter shows his heart."},
};

// What the key found in a room opens, by the room's name index, from the guides. Two rooms give a
// second key late in the game, and the mod does not know which it is, so those name both.
struct RoomKey {
    u32 room;
    const char* opens;
};
constexpr RoomKey kRoomKeys[] = {
    {35, "the Anteroom"},
    {38, "the second floor hallway to the west, or late in the game, the Breaker Room in the basement"},
    {34, "the Master Bedroom"},
    {33, "the Nursery"},
    {24, "the double doors on the Foyer's ground floor"},
    {20, "the Ball Room"},
    {10, "the Storage Room"},
    {17, "the Fortune-teller's Room"},
    {3, "the Laundry Room, or late in the game, the Safari Room on the third floor"},
    {0, "the Conservatory"},
    {21, "the Dining Room"},
    {16, "the Courtyard"},
    {69, "the Rec Room"},
    {22, "a first floor hallway door, a shortcut through the house"},
    {46, "the Twins' Room"},
    {45, "the Billiards Room"},
    {52, "the Balcony"},
    {59, "the west side of the third floor"},
    {67, "the Cellar"},
    {63, "the Clockwork Room"},
    {48, "the Pipe Room"},
    {36, "the Sitting Room"},
    {66, "Cold Storage"},
    {61, "the Artist's Studio"},
    {57, "the Secret Altar"},
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

std::string roomWord(u32 nameIndex) { return nameIndex < std::size(kRoomNames) ? kRoomNames[nameIndex] : ""; }

std::string doorName(u32 room) {
    if (rd32(kCurMap) != kMansionMap || room >= kRoomCount) return "";
    const char* name = roomName(room);
    return std::string(*name ? name : "Hallway") + " door";
}

namespace {

// What a locked door leads to: the nearest door of Luigi's room, the room on its other side.
constexpr u32 kFlagFirstKey = 3, kFlagFirstKeyGone = 14;
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
    // The Mirror Room's seal: its candles are the only ones that open a door (candleResult in
    // access_world.cpp). Other rooms' candles bring a key or ghosts, and their locked doors are just locked.
    constexpr u32 kMirrorRoom = 4;
    const u32 lights = rd32(kRoomLights);
    if ((rd32(luigi + kRoomInfo + kRoomNameIndex) & 0xFF) == kMirrorRoom && candlesUnlit() && gu::inMem1(lights) &&
        (rd8(lights + room * kRoomLightSize + kRoomLightByte) & kRoomLightBit))
        return (*name ? std::string("The ") + name + " door" : std::string("This door")) + " is sealed. It will not open until the candles here are lit with fire.";
    // The first key: flag 3 brings it down in the Foyer (jmp/keyinfo, demo_key, door 34), and flag 14
    // takes it away. Trying the Parlor's door is what starts that.
    if (!gameFlag(kFlagFirstKeyGone) && (gameFlag(kFlagFirstKey) || std::strcmp(name, "Parlor") == 0))
        return (*name ? std::string(name) + " is locked. " : std::string("This door is locked. ")) + "A key has appeared in the Foyer, back the way you came. It opens the Parlor.";
    return *name ? std::string(name) + " is locked" : std::string("This door is locked");
}

// The hints that fit the room as it is now. A room that clears while Luigi stands in it gets its
// next hint then, once any scene is over.
void roomHints(u32 luigi, u32 room) {
    if (!port::settings().accessibility.roomHints || rd32(kEvent + kEventCursor) != 0 || rd32(kMenuOff) != 0) return;
    for (u32 slot = 0; slot < 4; slot++)
        if (rd32(kState + slot * 4) != 0) return;
    const u32 lights = rd32(kRoomLights);
    if (!gu::inMem1(lights)) return;
    const bool cleared = (rd8(lights + (rd32(luigi + kActRoom) >> 24) * kRoomLightSize + kRoomLightByte) & kRoomLightBit) != 0;
    // A room seen dark and then clear, on this visit.
    static u32 sDarkRoom = ~0u;
    static bool sJustCleared = false;
    if (room != sDarkRoom) sJustCleared = false;
    if (!cleared) sDarkRoom = room;
    else if (sDarkRoom == room) sJustCleared = true;
    const bool blackout = gameFlag(kFlagBlackout);
    static std::set<const RoomHint*> sSaid;
    static u32 sRun = 0;
    if (runCount() != sRun) {  // a new game: every hint again
        sRun = runCount();
        sSaid.clear();
    }
    for (const RoomHint& hint : kRoomHints) {
        if (hint.room != room || sSaid.count(&hint)) continue;
        bool fits = false;
        switch (hint.when) {
        case When::Always: fits = true; break;
        case When::Dark: fits = !cleared && !blackout; break;
        case When::Cleared: fits = cleared; break;
        case When::JustCleared: fits = sJustCleared; break;
        case When::Fire: fits = rd32(luigi + kElement) == kElementFire && candlesUnlit(); break;
        case When::Seal: fits = cleared && candlesUnlit(); break;
        case When::BoosIn: fits = cleared && !gameFlag(kFlagBoosOut); break;
        case When::Blackout: fits = blackout; break;
        }
        if (!fits) continue;
        sSaid.insert(&hint);
        say(hint.text, false);
    }
}

// The room Luigi walks into, then the floor when that changed too. Only the mansion has these names.
void pollRoom(u32 luigi) {
    static u32 sRoom = ~0u, sFloor = ~0u;
    if (!port::settings().accessibility.announceRooms || rd32(kCurMap) != kMansionMap) {
        sRoom = sFloor = ~0u;
        return;
    }
    const u32 room = rd32(luigi + kRoomInfo + kRoomNameIndex) & 0xFF;
    if (room == sRoom) {
        roomHints(luigi, room);
        return;
    }
    const bool first = sRoom == ~0u;
    sRoom = room;
    const char* name = room < std::size(kRoomNames) ? kRoomNames[room] : "";
    // A room already won says so on the way in, in the name's sentence.
    const u32 lights = rd32(kRoomLights);
    const bool clear = *name && gu::inMem1(lights) && !gameFlag(kFlagBlackout) && (rd8(lights + (rd32(luigi + kActRoom) >> 24) * kRoomLightSize + kRoomLightByte) & kRoomLightBit);
    say(*name ? (std::string(name) + (clear ? ", clear" : "")) : std::string("Hallway"), false);
    roomHints(luigi, room);
    // The room table's own floor, once Luigi's room number is known.
    const u32 rooms = rd32(kRoomTable);
    if (!gu::inMem1(rooms)) return;
    const u32 floor = rd8(rooms + (rd32(luigi + kActRoom) >> 24) * kRoomSize + 1);
    if (floor != sFloor && !first) say(floorName(floor), false);
    sFloor = floor;
}

// What the game's treasure counters are worth, in order: the table at 0x80335B78, as added up by
// fn_8004D688. The first three counters are coins, bills and gold bars.
constexpr s64 kCounterValues[kMoneyCounters] = {5000, 20000, 100000, 500000, 800000, 1000000, 2000000, 5000, 20000000, 50000, 100000, 1000000};

// Luigi's health and money, spoken when they change.
// Actor +0xFC is health, +0x12F4 onwards the treasure counters.
// The element in the Poltergust and the medals. A spirit fills the meter to the game's own limit
// (int at 0x804D2C2C, fn_800AEEA8), and the element is 1 once the meter runs out.
constexpr u32 kElementMeter = 0x1188, kElementMax = 0x804D2C2C;
constexpr struct {
    u32 element, flag;
    const char* word;
    const char* lower;
} kElements[] = {{2, 43, "Fire", "fire"}, {3, 44, "Water", "water"}, {4, 45, "Ice", "ice"}};
const char* elementWord(u32 element) {
    for (const auto& e : kElements)
        if (e.element == element) return e.word;
    return nullptr;
}
float elementMax() {
    const s32 max = s32(rd32(kElementMax));
    return max > 0 && max < 100000 ? float(max) : 100.0f;
}

void pollElement(u32 luigi, bool settled) {
    static u32 sElement = 0;
    static bool sFull = false, sMedals[3] = {};
    // Test aid LM_ACCESS_TEST_ELEMENT=<read>: fire at half, then full, then gone, 150 reads apart.
    static const int sTest = [] {
        const char* v = std::getenv("LM_ACCESS_TEST_ELEMENT");
        return v ? std::atoi(v) : 0;
    }();
    static int sReads = 0;
    if (sTest && ++sReads >= sTest) {
        const int step = (sReads - sTest) / 150;
        if (step == 0) wr32(luigi + kElement, 2), wr32(luigi + kElementMeter, std::bit_cast<u32>(elementMax() / 2));
        else if (step == 1) wr32(luigi + kElementMeter, std::bit_cast<u32>(elementMax()));
        else if (step == 2) wr32(luigi + kElement, 1), wr32(luigi + kElementMeter, 0);
    }
    const u32 element = rd32(luigi + kElement);
    if (element > 4) return;  // junk during a load
    const float meter = gu::rdf(luigi + kElementMeter);
    const bool full = meter >= elementMax() - 0.5f;
    if (settled) {
        for (u32 i = 0; i < std::size(kElements); i++)
            if (gameFlag(kElements[i].flag) && !sMedals[i])
                say(std::string(kElements[i].word) + " Medal collected. You can now vacuum up " + kElements[i].lower + " spirits, and each one fills your element meter.", false);
        const char* word = elementWord(element);
        if (element != sElement) {
            if (word) say(std::string(word) + " collected." + (full ? " Meter full." : ""), false);
            else if (const char* was = elementWord(sElement)) say(std::string(was) + " used up.", false);
        } else if (word && full && !sFull) {
            say(std::string(word) + " meter full.", false);
        }
    }
    for (u32 i = 0; i < std::size(kElements); i++) sMedals[i] = gameFlag(kElements[i].flag);
    sElement = element;
    sFull = full;
}

// "Fire, meter 80 percent. Medals: fire, water", or empty before the first medal.
std::string elementStatus(u32 luigi) {
    std::string medals;
    for (const auto& e : kElements)
        if (gameFlag(e.flag)) medals += (medals.empty() ? "" : ", ") + std::string(e.lower);
    const char* word = elementWord(rd32(luigi + kElement));
    if (medals.empty() && !word) return "";
    std::string out = word ? std::string(word) + ", meter " + std::to_string(int(gu::rdf(luigi + kElementMeter) * 100.0f / elementMax() + 0.5f)) + " percent." : std::string("No element held.");
    if (!medals.empty()) out += " Medals: " + medals;
    return out;
}

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
        // A point at a time is the refill setting, which says when it is full.
        const bool refilling = health == sHealth + 1 && port::settings().accessibility.healthRefill;
        if (sHealth > 0 && health > 0 && settled && !refilling) say("Health " + std::to_string(health), false);  // 0: luigiDown
        sHealth = health;
    }
    if (sound && health > 0) pollRoom(luigi);
    if (sound) pollElement(luigi, settled);
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
            if (std::getenv("LM_ACCESS_WORLD_LOG")) log("[access] counter %u is out of range: %d", i, v);
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

// The Game Boy Horror's map (Y), fn_80029C38. The floor changes in fn_8002A364, the rooms come from
// the room table, and a room's visited bit is what the map uses to draw it (fn_8002A260).
std::string mapFloorText(s32 floor) {
    std::string text = floorName(u32(floor));
    const u32 rooms = rd32(kRoomTable);
    if (rd32(kCurMap) != kMansionMap || !gu::inMem1(rooms)) return text;
    std::string list;
    std::vector<std::string> seen;
    for (u32 n = 0; n < kRoomCount; n++) {
        if (rd8(rooms + n * kRoomSize + 1) != u8(floor) || !(rd16(kRoomVisited + n * 2) & 1)) continue;
        const char* name = roomName(n);
        if (!*name || std::find(seen.begin(), seen.end(), name) != seen.end()) continue;
        seen.push_back(name);
        list += (list.empty() ? "" : ", ") + std::string(name);
    }
    return text + (list.empty() ? ". No rooms visited here yet." : ". Rooms visited: " + list + ".");
}

void pollGbhMap() {
    static s32 sFloor = 1000;
    if (!sGbhMap.running()) {
        sFloor = 1000;
        return;
    }
    const s32 floor = s8(rd8(kMapFloor));
    if (sFloor == 1000) {
        const u32 luigi = rd32(kActorSlots);
        std::string here;
        if (gu::inMem1(luigi) && rd32(kCurMap) == kMansionMap) {
            const u32 index = rd32(luigi + kRoomInfo + kRoomNameIndex) & 0xFF;
            const char* name = index < std::size(kRoomNames) ? kRoomNames[index] : "";
            here = std::string(" You are in ") + (*name ? name : "a hallway") + ".";
        }
        say("Map. The stick up and down changes floor, A zooms, B returns." + here);
        say(mapFloorText(floor), false);
    } else if (floor != sFloor) {
        say(mapFloorText(floor));
    }
    sFloor = floor;
}

// Cutscene descriptions: what a scene shows, spoken between its lines. event is the number of the
// script (/Event/eventNN.szp), or kOpening. at is seconds into the scene. Written from frames of each
// scene, so the wording is a draft for the user to review.
constexpr int kOpening = -1, kReturnWalk = -2;
struct SceneLine {
    int event;
    float at;
    const char* text;
};
constexpr SceneLine kSceneLines[] = {
    {kOpening, 1.0f, "Night. Luigi walks alone through a dark forest of twisted, moss-covered trees, a flashlight lighting the way."},
    {kOpening, 20.0f, "The trees open onto a huge, decaying mansion on a hill, under a storm of green-black cloud. Two windows glow orange. A broken carriage lies by the path, and a small crooked shed glows beside it."},
    {kOpening, 34.0f, "Luigi climbs the wide stone steps to the mansion's arched double doors."},
    {kOpening, 40.0f, "The doors fill the view: dark wood and iron scrollwork, with a small skull at their centre."},
    {kReturnWalk, 0.5f, "Lightning over the mansion on its hill. Down by his lab, E. Gadd waves Luigi off."},
    {kReturnWalk, 7.0f, "Through the gate, and the view follows close behind Luigi, the Poltergust on his back, up to the front door."},
    {kFoyerEvent, 0.5f, "Inside is the Foyer, a tall dark hall. A carpeted staircase rises to a landing, under a chandelier and a large clock."},
    {kFoyerEvent, 8.0f, "The view moves to a door beneath the clock. It is sealed by a glowing blue web."},
    {1, 1.0f, "A small old man in a white coat fights a ghost with a vacuum on his back. It flings him across the floor at Luigi's feet."},
    {2, 1.0f, "Back in the lab, the professor stands by a big machine, the Ghost Portrificationizer. It draws in the captured ghosts, and a framed painting comes out."},
    {7, 0.5f, "A key appears in the room."},
    {8, 1.0f, "E. Gadd's lab, a cluttered workshop of machines and pipes. The little professor faces Luigi."},
    {11, 1.0f, "The Game Boy Horror crackles, and the professor's face appears on its screen."},
    {12, 1.0f, "The Game Boy Horror rings, and the professor's face appears on its screen."},
    {15, 1.0f, "The Game Boy Horror rings, and the professor's face appears on its screen."},
    {30, 0.5f, "Luigi steps onto a lift, and it carries him up to the roof."},
    {40, 0.5f, "The lift carries Luigi back down."},
    {42, 1.0f, "The Game Boy Horror crackles, and the professor's face appears on its screen."},
    {44, 0.5f, "Lightning strikes outside. Every light in the mansion goes out."},
    {46, 1.0f, "Far below in the well, Luigi sees Mario, trapped and crying out for help."},
    {50, 0.5f, "Chauncey, a giant ghost baby, sits up in his crib."},
    {54, 1.0f, "Luigi holds up a medal, and the professor's face appears on the Game Boy Horror."},
    {61, 0.5f, "The candles blaze up, and something in the room turns on Luigi."},
    {64, 0.5f, "Luigi, shrunk tiny, lands on Chauncey's enormous bed. The huge baby towers over him."},
    {65, 0.5f, "The glowing grave draws Luigi down into the dark."},
    {66, 0.5f, "A round, shadowy arena among the graves. Bogmire, a black ghost, rises out of the shadows."},
    {67, 0.5f, "Luigi is back in the graveyard."},
    {68, 0.5f, "Luigi looks through the telescope at the night sky."},
    {69, 0.5f, "A swarm of Boos bursts out, cackling, and scatters through the mansion."},
    {70, 1.0f, "Luigi holds up a medal, and the professor's face appears on the Game Boy Horror."},
    {71, 0.5f, "On the moonlit balcony, a ring of Boos circles and laughs at Luigi."},
    {72, 0.5f, "The Boos pile together and swell into one giant Boo, Boolossus."},
    {73, 0.5f, "A dim altar room. A huge painting of Mario hangs on the wall, and a small glowing figure floats before it: King Boo, wearing a crown."},
    {74, 0.5f, "King Boo gazes at his painting. Then he swaps it for one of Bowser, and it drags Luigi inside."},
    {75, 0.5f, "Luigi lands on a rooftop arena under a dark sky. A giant Bowser stomps towards him."},
    {76, 0.5f, "The strange room clears, and the way ahead opens."},
    {80, 1.0f, "The Game Boy Horror crackles, and the professor's face appears on its screen."},
    {82, 0.5f, "The defeated painter's ghost begins to fade away."},
    {88, 1.0f, "The professor's face appears on the Game Boy Horror, and Luigi lifts the great painting of Mario down."},
    {90, 0.5f, "The giant Bowser falls apart, and Luigi drops back into the altar room, beside the painting of Mario."},
    {93, 0.5f, "The telephone rings, and Luigi picks it up."},
    {94, 0.5f, "The telephone rings, and Luigi picks it up."},
};

std::atomic<int> sEventFile{-1};  // the last event script loaded
std::atomic<u32> sRunCount{0};

// When a scene starts and ends, when dialogue ends, and each scene's descriptions.
void pollScene() {
    const u32 scene = rd32(kCurScene);
    const u32 luigi = rd32(kActorSlots);
    const bool stopped = gu::inMem1(luigi) && (rd32(luigi + 0x40) & kLuigiStopped);
    const bool event = rd32(kEvent + kEventCursor) != 0 && rd32(kCurMap) != kTrainingMap;
    const bool demo = scene == kSceneOpening && rd32(kOpeningNext) == kOpeningIsDemo;
    const bool inScene = (scene == kSceneOpening && !demo) || (scene == kSceneMain && event && (rd32(kMenuOff) != 0 || stopped));

    // Short gaps between a scene's parts are the same scene.
    static bool sShown = false;
    static clock::time_point sChanged{}, sStart{};
    static bool sLast = false;
    static int sKey = -3;
    static size_t sNext = 0;
    if (inScene != sLast) {
        sLast = inScene;
        sChanged = clock::now();
    }
    // Longer to call a scene over: one part often hands straight to the next, as the opening does to
    // the Foyer, and "you have control" would be false.
    if (inScene != sShown && clock::now() - sChanged > std::chrono::milliseconds(inScene ? 400 : 2000)) {
        sShown = inScene;
        noticeCue(inScene ? Cue::SceneStart : Cue::SceneEnd);
    }
    // Descriptions follow any script, including ones that leave Luigi free, and play once per run:
    // the lab's scripts come round on every visit, and the title demo loops. The real opening always
    // plays them, whatever the demo already said.
    static bool sRunning = false;
    static std::vector<bool> sSpoken(std::size(kSceneLines), false);
    if (scene != kSceneOpening && scene != kSceneMain) sSpoken.assign(std::size(kSceneLines), false);  // a new start
    {
        static bool sWasOut = true;
        const bool out = scene != kSceneOpening && scene != kSceneMain;
        if (out && !sWasOut) {
            sRunCount++;
            log("[access] back at the title: run %u", sRunCount.load());
        }
        sWasOut = out;
    }
    const bool running = scene == kSceneOpening || (scene == kSceneMain && rd32(kEvent + kEventCursor) != 0);
    // One script can follow another with no gap.
    const int key = scene == kSceneOpening ? (rd32(kOpeningScript) == 0 ? kReturnWalk : kOpening) : sEventFile.load();
    if (running && (!sRunning || key != sKey)) {
        sKey = key;
        sStart = clock::now();
        sNext = 0;
        log("[access] scene starts: event %d%s", sKey, demo ? ", title demo" : "");
    }
    sRunning = running;
    if (running && port::settings().accessibility.sceneDescriptions) {
        const float at = std::chrono::duration<float>(clock::now() - sStart).count();
        for (; sNext < std::size(kSceneLines); sNext++) {
            const SceneLine& line = kSceneLines[sNext];
            if (line.event != sKey) continue;
            if (at < line.at) break;
            if (!sSpoken[sNext] || (sKey == kOpening && !demo)) {
                say(line.text, false);
                sDescribing = clock::now() + std::chrono::milliseconds(70 * std::strlen(line.text));  // about 14 letters a second
            }
            sSpoken[sNext] = true;
        }
    }

    bool window = false;
    for (u32 slot = 0; slot < 4; slot++) window |= rd32(kState + slot * 4) != 0;
    static bool sWindow = false;
    static clock::time_point sClosed{};
    if (window) {
        sWindow = true;
        sClosed = clock::now();
    } else if (sWindow && clock::now() - sClosed > std::chrono::milliseconds(500)) {
        sWindow = false;
        if (sShown || !inScene) noticeCue(Cue::DialogueEnd);  // a line that follows at once is the same talk
    }
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
}

}  // namespace

bool gamePaused() { return sPause.running(); }

void keyCollected(u32 room) {
    if (port::usCodeFeature() != port::Avail::Yes) return;
    for (const RoomKey& key : kRoomKeys)
        if (key.room == room) {
            say(std::string("This key opens ") + key.opens + ".", false);
            return;
        }
    say("Key collected. The map shows locked doors.", false);
}

bool sayStatus() {
    if (port::usCodeFeature() != port::Avail::Yes || rd32(kCurScene) != kSceneMain) return false;
    const u32 luigi = rd32(kActorSlots);
    if (!gu::inMem1(luigi)) return false;
    // In a menu, a cutscene or a conversation the stick repeats what was said instead.
    if (sTitle.running() || sFileMenu.running() || sPause.running() || sSave.running() || sAccount.running() || sStyle.running() || sGbhMap.running()) return false;
    if (rd32(kCurMap) != kTrainingMap && (rd32(kEvent + kEventCursor) != 0 || rd32(kMenuOff) != 0)) return false;
    for (u32 slot = 0; slot < 4; slot++)
        if (rd32(kState + slot * 4) != 0) return false;
    const s32 health = s16(rd16(luigi + kHealth));
    if (health < 0 || health > kHealthMax) return false;
    // Each press says the next thing. After a pause the next press starts over, so the first thing
    // is always the same one press away.
    static int sNext = 0;
    static clock::time_point sLastPress{};
    if (clock::now() - sLastPress > kStatusRestart) sNext = 0;
    sLastPress = clock::now();
    // Further stops: the element and medals once there are any, and a boss while one is about.
    std::vector<std::string> extra;
    if (const std::string element = elementStatus(luigi); !element.empty()) extra.push_back(element);
    if (const std::string boss = bossStatus(); !boss.empty()) extra.push_back(boss);
    const int stops = 3 + int(extra.size());
    if (sNext >= stops) sNext = 0;
    const int item = sNext;
    sNext = (sNext + 1) % stops;
    if (item >= 3) {
        say(extra[item - 3]);
    } else if (item == 0) {
        say("Health " + std::to_string(health));
    } else if (item == 1) {
        s64 total = 0;
        for (u32 i = 0; i < kMoneyCounters; i++) {
            const s32 count = s32(rd32(luigi + kCounters + 4 * i));
            if (count > 0 && count <= kCounterMax) total += count * kCounterValues[i];
        }
        say("Total " + std::to_string(total) + " dollars");
    } else if (rd32(kCurMap) == kMansionMap) {
        const u32 index = rd32(luigi + kRoomInfo + kRoomNameIndex) & 0xFF;
        const char* name = index < std::size(kRoomNames) ? kRoomNames[index] : "";
        const u32 rooms = rd32(kRoomTable);
        std::string where = *name ? name : "Hallway";
        if (gu::inMem1(rooms)) where += std::string(", ") + floorName(rd8(rooms + (rd32(luigi + kActRoom) >> 24) * kRoomSize + 1));
        say(where);
    } else {
        say(rd32(kCurMap) == kLabMap ? "E. Gadd's Lab" : rd32(kCurMap) == kTrainingMap ? "Training room" : rd32(kCurMap) == kGalleryMap ? "Portrait gallery" : "Outside the mansion");
    }
    return true;
}

void luigiDown(u32 actor) {
    if (port::usCodeFeature() == port::Avail::Yes && actor == rd32(kActorSlots)) say("Game over", false);
}

constexpr u32 kStunFrames = 29;
namespace {
void noteCaught(u32 type) {
    // Written to the log every time, so a report from play shows how far a catch got.
    const bool ghost = type < kGhostTypeCount && (rd16(kGhostTypes + type * kGhostTypeSize + kGhostTypeFlags) & kGhostTypeFlag);
    const std::string name = type < kGhostTypeCount ? gameText(rd32(kGhostTypes + type * kGhostTypeSize + kGhostTypeName), 16) : std::string();
    log("[access] message 21: type %u %s, %s", type, name.c_str(), ghost ? "a ghost" : "not a ghost");
    // Keys, hearts and other items come through the same message: the game's own text covers those.
    if (!ghost) return;
    std::lock_guard lk(sCaughtM);
    sCaughtAt = clock::now();
    for (auto& [caught, count] : sCaught)
        if (caught == ghostName(name)) {
            count++;
            return;
        }
    sCaught.push_back({ghostName(name), 1});
}
}  // namespace

std::string ghostWord(u32 type) {
    return type < kGhostTypeCount ? ghostName(gameText(rd32(kGhostTypes + type * kGhostTypeSize + kGhostTypeName), 16)) : "Ghost";
}

void ghostCaught(u32 type) {
    if (port::usCodeFeature() == port::Avail::Yes) noteCaught(type);
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
    // Test aid LM_ACCESS_TEST_CAUGHT=<type>@<frame>: a catch, as the message would report it.
    if (const char* v = std::getenv("LM_ACCESS_TEST_CAUGHT")) {
        unsigned type = 0, frame = 0;
        static bool sDone = false;
        if (!sDone && std::sscanf(v, "%u@%u", &type, &frame) == 2 && copyDispCount() >= frame) {
            sDone = true;
            noteCaught(type);
        }
    }
    pollTitle();
    pollFileMenu();
    pollPause();
    pollSave();
    pollAccount();
    pollControlStyle();
    pollGbhMap();
    pollScene();
    noticeTick();
    pollStatus();
    pollCaught();
    pollMessages();
    pollChoices();
}

u32 runCount() { return sRunCount.load(); }

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

// The Game Boy Horror's list screen, which Z opens: its update, with the screen object in r3.
HLE(fn_8005232C) {
    const u32 screen = c.r[3];
    callOriginal(c, "fn_8005232C");
    access::listScreen(screen);
}

// An object leaves the nozzle: this reads the nozzle's direction (Luigi +0x10FC) for its flight.
HLE(fn_800A3460) {
    access::aimThrow(c.r[3]);
    callOriginal(c, "fn_800A3460");
}
HLE(fn_8009D050) {
    access::aimThrow(c.r[3]);
    callOriginal(c, "fn_8009D050");
}
HLE(fn_800A95C0) {
    access::aimThrow(c.r[3]);
    callOriginal(c, "fn_800A95C0");
}

// How long a stunned ghost stays, by the setting. A stun is 29 game frames in the light-bind
// sub-strategy (strategy +0x504, pointed to by +0x590), state 3, and its counters do not end it: the
// way out is setNextState, held back here until the longer time is up. A ghost the vacuum has
// hooked goes on at once.
HLE(setNextState__13EnemyStrategyFUs) {
    const u32 sub = c.r[3];
    if (access::active() && port::usCodeFeature() == port::Avail::Yes && gu::inMem1(sub - 0x504) && rd16(sub + 0xE) == 3 && rd32(sub - 0x504 + 0x590) == sub) {
        static const char* sTest = std::getenv("LM_ACCESS_TEST_STUN");
        const u32 length = u32(std::clamp(sTest ? std::atoi(sTest) : port::settings().accessibility.stunLength, 0, 3));
        const u32 luigi = rd32(access::kActorSlots);
        const bool hooked = gu::inMem1(luigi) && rd32(luigi + 0x11A0) != 0;
        if (!hooked && rd32(sub + 0x10) < access::kStunFrames * (length + 1)) return;
    }
    access::noteStrategy(sub);
    callOriginal(c, "setNextState__13EnemyStrategyFUs");
}

// A hit on Luigi: the attacker is in the hit's data. A cancelled one returns as the handler's own
// checks do when no hit lands (0).
HLE(fn_800B9124) {
    if (access::hitCancelled(c.r[4])) {
        c.r[3] = 0;
        return;
    }
    callOriginal(c, "fn_800B9124");
}

// A file load by name: event scripts come through here.
HLE(fn_800067D0__FPCcPFPvP13LMDvdFileInfo_vP18LMDvdFileInfoBlockl) {
    int no = 0;
    if (const char* path = gstr(c.r[3]); path && std::sscanf(path, "/Event/event%d.szp", &no) == 1) access::sEventFile = no;
    callOriginal(c, "fn_800067D0__FPCcPFPvP13LMDvdFileInfo_vP18LMDvdFileInfoBlockl");
}

// The Boo manager's update. There is no global pointer to it.
HLE(fn_800C3B14) {
    access::noteBooManager(c.r[3]);
    callOriginal(c, "fn_800C3B14");
}

// Game Boy Horror map update.
HLE(fn_80029C38) {
    access::sGbhMap.calls++;
    callOriginal(c, "fn_80029C38");
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
