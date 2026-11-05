// Screen reader support: object sounds and auto interact (docs/accessibility/README.md). USA game
// only for now. Runs from PADRead, once per game input read.
//
// One scanner lists the important objects: chests, keys, hearts, treasure, spilled money and doors.
// Both features use only that list, so auto interact cannot reach anything else, such as furniture.
//
// Object sounds repeat a short tone for the nearest item and the nearest door in Luigi's room. The
// repeat gets faster as Luigi gets closer, the stereo position is the object's place left or right
// of Luigi along the camera's axis, and an object behind him sounds lower. Ghosts that show get the
// same treatment on a third channel, with their own setting and a quicker repeat. Banana peels on
// the floor get a fourth, under the object sounds setting, with a clashing warning tone, and Toad a
// fifth, so that nothing nearer can hide him.
//
// The tones have their own audio streams on purpose. Starting the game's own sounds from here takes
// sounds from JAudio's fixed lists; in a busy moment (coins bouncing) the game's next start then
// finds a list empty and writes through a null pointer (fn_8018FF88), which crashed the game.
//
// Test aid: LM_ACCESS_WORLD_LOG=1 logs the targets.



#include "recomp/access.h"
#include "recomp/access_nav.h"

#include "port/install.h"
#include "port/settings.h"
#include "recomp/guest_util.h"
#include "recomp/hle.h"

#include <dolphin/pad.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>
#include <vector>

namespace recomp::access {
namespace {

constexpr u32 kActorSlots = 0x803D48A0, kActorSlotCount = 128;
constexpr u32 kFacing = 0xF48;  // Luigi's facing: float x, y, z
constexpr float kLevelBand = 0.38f, kPitchUp = 1.33f, kPitchDown = 0.75f;  // 22.5 degrees: eight directions, a fourth apart
constexpr float kAimCosine = 0.94f, kAimReach = 400.0f;  // about 20 degrees either side; MaxLightBindRange
constexpr u32 kActType = 0x3C, kActFlags = 0x40, kActPos = 0x44, kActRoom = 0xB4, kActAlpha = 0xE3;
constexpr u32 kActHidden = 0x6100;  // +0x40 bits the game's ghost draw pass skips
constexpr u32 kTypeBanana = 168, kTypeGhostHeart = 62;
constexpr u32 kTypeFireSpirit = 114, kTypeIceSpirit = 115, kTypeWaterSpirit = 116;
constexpr float kFakeDoorWarn = 220.0f;
constexpr float kKeyTaken = 250.0f;
constexpr u32 kRoomTable = 0x804D8170;
constexpr u32 kRoomLights = 0x804D8178, kRoomLightSize = 0x14C, kRoomLightByte = 6, kRoomLightBit = 0x80;  // set when a room is cleared
constexpr u32 kTypeTable = 0x8033B590, kTypeSize = 0x1C, kTypeFlags = 0x08, kTypeFlagGhost = 8, kTypeCount = 0x29C8 / 0x1C;
constexpr u32 kCamera = 0x803A3820, kCamViewMtx = 0x18C;
constexpr u32 kEvent = 0x803D2D40, kEventCursor = 0x0C;
constexpr u32 kMenuOff = 0x804D8088;
constexpr u32 kTrainingMap = 3, kMansionMap = 2, kBowserMap = 9, kBogmireMap = 13;
constexpr float kThrownHeight = 200.0f;  // higher than this it is out of play for now: balls and horses wait up there
constexpr u32 kHealth = 0xFC, kHealthMost = 0xFFC;
constexpr int kRefillDelay = 150, kRefillStep = 30;  // input reads, about 30 a second
constexpr u32 kBoolossusMap = 11;
constexpr float kBossBounce = 120.0f;  // sum of Chauncey's recent rises and falls
constexpr u32 kNozzle = 0x7A4;
constexpr u32 kBooCount = 0x5B8, kBooRecords = 0x18, kBooRecordSize = 12, kBooMax = 120, kNoBooRoom = 0xFF;
constexpr u32 kCandleLit = 1, kCandleFlicker = 2, kCandleOut = 4;
constexpr u32 kParlor = 35;  // name index: its observer brings the ghosts when every candle is out (cond_type 10)
constexpr float kParlorPortrait = -2500.0f;  // z: the two stands by the portrait lie beyond this, and only they matter
constexpr float kStandGap = 160.0f, kCandleReach = 230.0f;
constexpr u32 kParticleMgr = 0x803D990C, kMgrGroup = 0xAB4, kGroupList = 0x2A0, kEmNext = 0x20C, kEmOwner = 0xC4;
constexpr int kShotMs = 3000;
constexpr float kShotReach = 900.0f, kShotStep = 45.0f;  // near this, it moves this far each read
constexpr float kLockShotReach = 5000.0f;  // with the lock on: the whole arena
constexpr u32 kNozzleDir = 0x10FC;  // Luigi: where the nozzle points, while the vacuum runs; what a thrown object flies along
constexpr u32 kAiPlayer = 0x950, kAiActor = 0x808;  // an AI object: which player holds it, its actor
constexpr float kBossStop = 350.0f;  // Go to stops this far from a boss
constexpr u32 kLuigiElement = 0x1184, kElementFire = 2;  // Luigi +0x1184: 2 fire, 3 water, 4 ice
constexpr float kShiversStop = 220.0f;  // close enough for the fire to reach his candle: a guess, not measured
constexpr float kLockCatch = 900.0f;  // a ball nearer than this comes before the boss
constexpr u64 kLockSayMs = 8000;
constexpr u32 kAiStrategy = 0x800;
constexpr float kBossHook = 350.0f;  // an open boss hooks from about here: 427 failed, 365 worked
constexpr int kLockRegrip = 40, kLockRegripOff = 5;  // reads between restarts of the vacuum, and reads off
constexpr int kBossTilt = 100;
constexpr float kChaunceyFrontX = 188.0f, kChaunceyFrontZ = 313.0f;  // from him, where hooks landed; he faces +z
constexpr int kLockIdle = 20;  // the left stick counts as left alone under this
constexpr int kLockGrab = 40;  // the player's own right stick past this takes over
constexpr int kLockStill = 20;  // reads of a full turn with no movement before trying the other stick
constexpr float kLockReach = 300.0f;  // from the nozzle
constexpr int kLockLost = 45;  // input reads with nothing in reach
constexpr u8 kTriggerPressed = 100;
constexpr float kThrowCosine = 0.94f, kThrowReach = 1200.0f;  // about 20 degrees either side
constexpr float kThrownNotice = 900.0f;  // what is thrown from further off is not news yet
constexpr float kPlatformMoving = 0.6f, kPlatformJump = 200.0f;  // units per input read
constexpr int kPlatformReads = 20, kPlatformLeave = 30, kStickAtRest = 6;
constexpr u32 kBallRoom = 10, kRecRoom = 22, kNursery = 24;  // name indices
constexpr u32 kFishedGhost = 0x11A0;  // Luigi: the ghost on the vacuum, which drags him
constexpr u32 kGrainTimer = 0x48, kHeartLife = 0x1AC;
constexpr s32 kKeepTimer = 300;  // game frames: above where an item starts to fade
constexpr u32 kSearchView = 0x804D8160;  // byte: the Game Boy Horror's search view is up
constexpr u32 kWindowState = 0x803CF4C8;  // + slot * 4: a message window
constexpr float kFocusCone = 0.10f;  // radians off the middle of the view, about 6 degrees
constexpr float kFocusHeightWeight = 0.05f;
constexpr float kFocusTilt = 0.30f;  // radians of height off the view before it is said, about 17 degrees
constexpr int kFocusSettle = 6;  // input reads on one object before it is named, so a sweep stays quiet
constexpr u32 kCurMap = 0x804D80C8, kMapTable = 0x802FF218, kMapDoors = 0x18;
constexpr u32 kDoorSize = 0x1C, kDoorX = 0x08, kDoorY = 0x0C, kDoorZ = 0x10, kDoorRoomA = 0x1A, kDoorRoomB = 0x1B;
constexpr u32 kDoorMax = 128, kAuditRooms = 74;
constexpr u32 kDoorId = 7, kNoDoor = 0xFF;  // an open boundary between two zones, as at the top of the Foyer stairs

enum class Kind { Chest, Key, Heart, Treasure, Money, Door, Ghost, Banana, Toad, GhostAimed, Thrown, SceneStart, SceneEnd, DialogueEnd, Npc, GhostLit, GhostHooked, Boss, Holding, CanThrow, GhostHeart, Boo, Candle, Medal, Spirit };  // as access::Cue

bool kindOf(u32 type, Kind& k) {
    if (type == 122 || (type >= 373 && type <= 377)) k = Kind::Chest;
    else if (type >= 86 && type <= 91) k = Kind::Key;
    else if (type == 205 || type == 206) k = Kind::Heart;
    else if (type >= 194 && type <= 203) k = Kind::Treasure;  // pearls, gems, diamonds, gold
    else if (type == 42 || type == 43) k = Kind::Toad;
    else if (type >= 39 && type <= 41) k = Kind::Npc;  // the professor: lohakase, dhakase, dhakase2
    else return false;
    return true;
}

// What ghosts and bosses throw, or what the player throws back. By the type table's names: 65 babyball
// and 68 bball (Chauncey), 66 and 67 his rocking horses, 69 telball, 149 and 150 books, 155 car, 156
// plane, 157 bone, 158 to 161 billiard balls, 167 wind-up toy, 170 to 172 yarn, 211 and 212 Bogmire's
// shadows, 215 and 216 icicles, 219 ice, 231 Bowser's bomb.
bool isThrown(u32 type) {
    switch (type) {
    case 65: case 66: case 67: case 68: case 69: case 149: case 150: case 155: case 156: case 157:
    case 158: case 159: case 160: case 161: case 167: case 170: case 171: case 172: case 211: case 212: case 215: case 216: case 219: case 231:
        return true;
    default:
        return false;
    }
}

const char* thrownWord(u32 type) {
    switch (type) {
    case 66: case 67: return "Rocking horse";
    case 149: case 150: return "Book";
    case 155: return "Toy car";
    case 156: return "Toy plane";
    case 157: return "Bone";
    case 158: case 159: case 160: case 161: return "Billiard ball";
    case 167: return "Wind-up toy";
    case 170: case 171: case 172: return "Yarn ball";
    case 211: case 212: return "Shadow";
    case 215: case 216: return "Icicle";
    case 219: return "Ice";
    case 231: return "Spiked bomb";
    default: return "Ball";
    }
}

const char* bossWord(u32 type) {
    switch (type) {
    case 63: case 64: return "Chauncey";
    case 213: return "Bogmire";
    case 220: return "Boolossus";
    case 221: return "King Boo";
    default: return "Bowser";
    }
}

// The bosses' own bodies: 63 Chauncey in the Nursery, 64 Chauncey on his bed, 213 Bogmire, 220 Boolossus, 221 King Boo, 229 and 230 the Bowser suit.
bool isBoss(u32 type) { return type == 63 || type == 64 || type == 213 || type == 220 || type == 221 || type == 229 || type == 230; }

bool isGhost(u32 type) { return type < kTypeCount && (rd16(kTypeTable + type * kTypeSize + kTypeFlags) & kTypeFlagGhost); }

struct Vec {
    float x, y, z;
};
struct Target {
    Kind kind;
    Vec pos;
    u32 id;  // actor address, or door number + 1 in the high half
    u32 leadsTo = 0;  // a door: the room on its other side
    u32 type = 0;     // thrown things and bosses: the actor type
    bool open = false;  // a door record with no door: the edge of a zone, nothing to press
};

float rdf(u32 a) { return gu::rdf(a); }
Vec rdVec(u32 a) { return {rdf(a), rdf(a + 4), rdf(a + 8)}; }
float flatDistance(const Vec& a, const Vec& b) { return std::hypot(a.x - b.x, a.z - b.z); }

// Spilled coins and bills are not actors: their update hooks report them here.
std::mutex sMoneyM;
std::vector<Vec> sMoney;

// The room table (0x30 per room): int min x, y, z at +8 and max at +0x14. box: min x, min z, max x, max z.
bool roomBox(u32 room, float box[4]) {
    const u32 rooms = rd32(kRoomTable);
    if (!gu::inMem1(rooms)) return false;
    const u32 b = rooms + room * 0x30;
    const s32 minX = s32(rd32(b + 0x08)), minZ = s32(rd32(b + 0x10)), maxX = s32(rd32(b + 0x14)), maxZ = s32(rd32(b + 0x1C));
    if (maxX <= minX || maxZ <= minZ) return false;
    box[0] = float(minX);
    box[1] = float(minZ);
    box[2] = float(maxX);
    box[3] = float(maxZ);
    return true;
}

bool sHeartNow = false;  // a ghost is showing its heart
std::vector<std::pair<u32, Vec>> sHearts;  // each one showing
u32 sFakeDoorNear = 0;  // types 225 to 227: they fall on Luigi if opened
bool sBabyWaits = false;  // the Nursery's ball is here: Chauncey has not been dealt with

// The element medals: game flags as fn_800D27E0 gives them for the medal items, and the rooms the
// treasure table puts them in (elffst, elwfst, elifst), by name index.
constexpr struct {
    u32 element, flag, room;
    const char* word;
} kMedals[] = {{2, 43, 4, "Fire"}, {3, 44, 8, "Water"}, {4, 45, 47, "Ice"}};
bool flagOn(u32 flag) { return (rd8(kEvent + 0x659 + flag / 8) >> (flag % 8)) & 1; }
const u32 kMedalChest = 0x4D45;  // Target::type of a chest that holds a medal

constexpr u32 kMapRoomBits = 0x803CDF50;  // + room * 2, u16: bit 0 the room is known to the map
// Rooms Luigi has stood in, this run.
std::set<u32> sBeenIn;
void noteRoom(u32 room) {
    static u32 sRun = 0;
    if (runCount() != sRun) {  // a new game has visited nothing
        sRun = runCount();
        sBeenIn.clear();
    }
    sBeenIn.insert(rd32(kCurMap) << 8 | room);
}
bool beenIn(u32 room) { return sBeenIn.count(rd32(kCurMap) << 8 | room) != 0; }

constexpr u32 kTypeMelody = 38;  // pianist
constexpr float kSeatSlack = 60.0f;
constexpr u32 kTypeShivers = 14, kShiversHall = 17, kButlersRoom = 0;  // room number; name index
bool sShiversHere = false;
u32 sShiversRoom = 0, sShiversId = 0;
Vec sShiversPos{};

std::vector<Target> scan(u32 luigi) {
    std::vector<Target> out;
    const u32 room = rd32(luigi + kActRoom) >> 24;
    const float luigiY = rdf(luigi + kActPos + 4);
    float here[4];
    const bool hasBox = roomBox(room, here);
    sBabyWaits = false;
    sHeartNow = false;
    sHearts.clear();
    sFakeDoorNear = 0;
    sShiversHere = false;
    // With LM_ACCESS_WORLD_LOG: every actor of a room Luigi walks into, once, with its name and state.
    static const bool sRoomLog = std::getenv("LM_ACCESS_WORLD_LOG") != nullptr;
    static u32 sLoggedRoom = ~0u;
    static std::set<u32> sLoggedActors;
    if (room != sLoggedRoom) sLoggedActors.clear();
    sLoggedRoom = room;
    for (u32 i = 1; i < kActorSlotCount; i++) {
        const u32 a = rd32(kActorSlots + 4 * i);
        if (!gu::inMem1(a)) continue;
        const u32 type = rd32(a + kActType);
        // Outside the mansion, thrown things and the professor need not carry Luigi's room.
        const Vec p = rdVec(a + kActPos);
        if (sRoomLog && (rd32(a + kActRoom) >> 24 == room || flatDistance(p, rdVec(luigi + kActPos)) < 1500.0f) && sLoggedActors.insert(a).second)
            std::printf("[world] room %u actor type %u %s, its room %u, at %.0f,%.0f,%.0f flags %08X alpha %u ghost %d\n", room, type, ghostWord(type).c_str(), rd32(a + kActRoom) >> 24, p.x, p.y, p.z, rd32(a + kActFlags), rd8(a + kActAlpha), isGhost(type));
        if (type == kTypeShivers && p.x > -20000.0f) {  // wherever he is, once out of the park: he has his own speech
            sShiversHere = true;
            sShiversRoom = rd32(a + kActRoom) >> 24;
            if (sShiversId != a) log("[access] Shivers is out at %.0f,%.0f,%.0f, room %u", p.x, p.y, p.z, rd32(a + kActRoom) >> 24);
            sShiversId = a;
            sShiversPos = p;
        }
        // A stunned ghost's heart (made by fn_800C0B18). It carries no room of its own.
        if (type == kTypeGhostHeart) {
            if (p.x > -20000.0f && !(rd32(a + kActFlags) & kActHidden)) {
                sHeartNow = true;
                sHearts.push_back({a, p});
            }
            continue;
        }
        // In the mansion a thrown thing counts by where it is: Chauncey's balls are not in the Nursery's list.
        const bool inBox = hasBox && p.x >= here[0] && p.z >= here[1] && p.x <= here[2] && p.z <= here[3];
        const bool anyRoom = rd32(kCurMap) != kMansionMap ? isThrown(type) || isBoss(type) || (type >= 39 && type <= 41) : isThrown(type) && inBox;
        if (type == 68 && inBox) sBabyWaits = true;  // his ball waits in the Nursery; he comes when the horse rocks
        if (rd32(a + kActRoom) >> 24 != room && !anyRoom) continue;
        if (p.x < -20000.0f) continue;  // parked: not in play
        // A peel in a ghost's hand is not on the floor yet: only one near Luigi's height counts.
        if (type >= 225 && type <= 227 && !(rd32(a + kActFlags) & kActHidden) && flatDistance(p, rdVec(luigi + kActPos)) < kFakeDoorWarn) sFakeDoorNear = a;
        if (type == kTypeBanana) {
            if (!(rd32(a + kActFlags) & kActHidden) && std::abs(p.y - luigiY) < 60.0f) out.push_back({Kind::Banana, p, a});
            continue;
        }
        if (isThrown(type)) {
            if (!(rd32(a + kActFlags) & kActHidden) && p.y - luigiY < kThrownHeight) out.push_back({Kind::Thrown, p, a, 0, type});
            continue;
        }
        if (isBoss(type)) {  // the Bowser suit has no ghost alpha
            if (!(rd32(a + kActFlags) & kActHidden) && (rd8(a + kActAlpha) != 0 || type == 229 || type == 230)) out.push_back({Kind::Boss, p, a, 0, type});
            continue;
        }
        Kind k;
        // A spirit is no use without its medal, and has its own beacon with one. They have no ghost flag.
        if (type >= kTypeFireSpirit && type <= kTypeWaterSpirit) {
            const u32 element = type == kTypeFireSpirit ? 2 : type == kTypeIceSpirit ? 4 : 3;
            if (!(rd32(a + kActFlags) & kActHidden) && hasMedal(element)) out.push_back({Kind::Spirit, p, a, 0, type});
            continue;
        }
        // A ghost counts only while it shows: not hidden in furniture, not faded out.
        // Melody sits at the piano, unseen until every instrument has played. Seen and still in her seat,
        // she is someone to talk to: the person beacon, and People in Go to, where A starts her quiz.
        // Off her seat she is a ghost again.
        if (type == kTypeMelody) {
            static u32 sMelody = 0;
            static Vec sSeat{};
            if (a != sMelody) {
                sMelody = a;
                sSeat = p;
            }
            // Test aid LM_ACCESS_TEST_MELODY=1 shows her without the instruments.
            static const bool sTestShow = std::getenv("LM_ACCESS_TEST_MELODY") != nullptr;
            if (sTestShow) wr8(a + kActAlpha, 255);
            const bool seen = rd8(a + kActAlpha) != 0 && !(rd32(a + kActFlags) & kActHidden);
            if (!seen) sSeat = p;  // unseen, she is in her seat
            static u32 sN = 0;
            if (sRoomLog && sN++ % 60 == 0) std::printf("[world] Melody: alpha %u flags %08X at %.0f,%.0f,%.0f, %.0f from her seat\n", rd8(a + kActAlpha), rd32(a + kActFlags), p.x, p.y, p.z, flatDistance(p, sSeat));
            if (seen && flatDistance(p, sSeat) < kSeatSlack) {
                out.push_back({Kind::Npc, p, a, 0, type});
                continue;
            }
        }
        // Test aid LM_ACCESS_TEST_CHEST_YAW=<hex>: turns every chest to that heading (+0x88, 0x10000 a
        // turn), and with LM_ACCESS_WORLD_LOG logs each chest's heading and Luigi's flags.
        if (type == 122 || (type >= 373 && type <= 377)) {
            static const char* sYaw = std::getenv("LM_ACCESS_TEST_CHEST_YAW");
            if (sYaw) {
                const u16 v = u16(std::strtoul(sYaw, nullptr, 16));
                wr16(a + 0x88, v);
                wr16(a + 0x8A, v);
            }
            // Which of its words change, to find its open state.
            static std::map<u32, std::vector<u32>> sWords;
            if (sRoomLog) {
                std::vector<u32>& w = sWords[a];
                std::string changed;
                for (u32 o = 0; o < 0x400; o += 4) {
                    const u32 v = rd32(a + o);
                    if (w.size() == 0x100 && w[o / 4] != v && o != 0x44 && o != 0x48 && o != 0x4C) {
                        char b[40];
                        std::snprintf(b, sizeof b, " +%X %08X>%08X", o, w[o / 4], v);
                        changed += b;
                    }
                    if (w.size() < 0x100) w.push_back(v);
                    else w[o / 4] = v;
                }
                if (!changed.empty() && changed.size() < 600) std::printf("[world] chest %08X changed:%s\n", a, changed.c_str());
            }
            static u32 sN = 0;
            if (sRoomLog && sN++ % 30 == 0) std::printf("[world] chest %08X at %.0f,%.0f,%.0f heading %04X %04X, Luigi at %.0f,%.0f flags %08X\n", a, p.x, p.y, p.z, rd16(a + 0x88), rd16(a + 0x8A), rdf(luigi + kActPos), rdf(luigi + kActPos + 8), rd32(luigi + kActFlags));
        }
        // Shivers fades as he paces, and the player has to follow him down a long hall: his tone stays on.
        if (isGhost(type) && !(rd32(a + kActFlags) & kActHidden) && (rd8(a + kActAlpha) != 0 || type == kTypeShivers)) {
            out.push_back({Kind::Ghost, p, a, 0, type});
        } else if (kindOf(type, k)) {
            out.push_back({k, p, a});
            if (k == Kind::Chest && rd32(kCurMap) == kMansionMap) {
                const u32 rooms = rd32(kRoomTable);
                const u32 index = gu::inMem1(rooms) ? rd8(rooms + room * 0x30 + 3) : ~0u;
                for (const auto& m : kMedals)
                    if (m.room == index && !flagOn(m.flag)) out.back().type = kMedalChest;
            }
        }
    }
    {
        std::lock_guard lk(sMoneyM);
        // Spilled money has no room of its own: it counts when it lies inside Luigi's.
        float box[4];
        const bool known = roomBox(room, box);
        for (const Vec& p : sMoney)
            if (known && p.x >= box[0] && p.z >= box[1] && p.x <= box[2] && p.z <= box[3]) out.push_back({Kind::Money, p, 0});
        sMoney.clear();
    }
    const u32 map = rd32(kMapTable + 4 * rd32(kCurMap));
    const u32 doors = gu::inMem1(map) ? rd32(map + kMapDoors) : 0;
    if (gu::inMem1(doors)) {
        for (u32 i = 0; i < kDoorMax; i++) {
            const u32 d = doors + i * kDoorSize;
            if (rd8(d) == 0) break;
            const Vec p{float(s32(rd32(d + kDoorX))), float(s32(rd32(d + kDoorY))), float(s32(rd32(d + kDoorZ)))};
            const u32 a = rd8(d + kDoorRoomA), b = rd8(d + kDoorRoomB);
            if (a == room || b == room) out.push_back({Kind::Door, p, (i + 1) << 16, a == room ? b : a, 0, rd8(d + kDoorId) == kNoDoor});
        }
    }
    return out;
}

// The Game Boy Horror's search view: says what is in the middle of the view as the player looks
// around. The game names something only when A is pressed, and that text is spoken as dialogue.
void searchView(u32 luigi) {
    static bool sOn = false;
    static std::string sSaid, sSeen;
    static int sSeenFor = 0;
    if (rd8(kSearchView) == 0) {
        if (sOn) say("Search view closed");
        sOn = false;
        return;
    }
    if (!sOn) {
        sOn = true;
        sSaid.clear();
        sSeen.clear();
        say("Search view. The stick looks around, A searches, B returns.");
    }
    for (u32 slot = 0; slot < 4; slot++)
        if (rd32(kWindowState + slot * 4) != 0) return;  // the answer is being read out
    // The view matrix's rows are the camera's axes: it looks down minus the third, and the eye is
    // minus the fourth column turned back by them.
    float m[3][4];
    for (u32 r = 0; r < 3; r++)
        for (u32 k = 0; k < 4; k++) m[r][k] = rdf(kCamera + kCamViewMtx + r * 16 + k * 4);
    const float dir[3] = {-m[2][0], -m[2][1], -m[2][2]};
    float eye[3];
    for (int k = 0; k < 3; k++) eye[k] = -(m[0][k] * m[0][3] + m[1][k] * m[1][3] + m[2][k] * m[2][3]);
    // Left and right decide what is in focus, since the view starts tilted down and height is hard to
    // judge without seeing. Height is then said in words, so the player can tilt to it before A.
    const float flat = std::max(std::hypot(dir[0], dir[2]), 1e-3f);
    const float pitch = std::atan2(dir[1], flat);
    float angle = kFocusCone + kFocusHeightWeight, height = 0.0f;
    std::string word = nav::aimedFurniture(eye, dir, angle, height, kFocusHeightWeight);
    for (const Target& t : scan(luigi)) {
        const char* name = t.kind == Kind::Ghost ? "Ghost" : t.kind == Kind::Chest ? "Chest" : t.kind == Kind::Key ? "Key" : t.kind == Kind::Heart ? "Heart"
                         : t.kind == Kind::Treasure ? "Treasure" : t.kind == Kind::Toad ? "Toad" : t.kind == Kind::Banana ? "Banana peel" : t.kind == Kind::Door ? "Door" : t.kind == Kind::Thrown ? thrownWord(t.type) : t.kind == Kind::Boss ? bossWord(t.type) : t.kind == Kind::Npc ? (t.type == kTypeMelody ? "Melody Pianissima" : "Professor") : nullptr;
        if (!name) continue;
        const float dx = t.pos.x - eye[0], dz = t.pos.z - eye[2], dist = std::max(std::hypot(dx, dz), 1.0f);
        const float ahead = (dx * dir[0] + dz * dir[2]) / (dist * flat);
        if (ahead <= 0.0f) continue;
        const float up = std::atan2(t.pos.y - eye[1], dist);
        // A wider target than furniture. Of two in line, such as doors one above the other, the one
        // nearer the tilt of the view.
        const float off = std::acos(std::min(ahead, 1.0f)) - std::atan(80.0f / dist) + kFocusHeightWeight * std::abs(up - pitch);
        if (off >= angle) continue;
        angle = off;
        height = up;
        const std::string door = t.kind == Kind::Door ? doorName(t.leadsTo) : std::string();
        word = door.empty() ? name : door;
    }
    if (!word.empty()) {
        const float tilt = height - pitch;
        if (tilt > kFocusTilt) word += ", look up";
        else if (tilt < -kFocusTilt) word += ", look down";
    }
    static const bool sLog = std::getenv("LM_ACCESS_WORLD_LOG") != nullptr;
    if (sLog) {
        static u32 sN = 0;
        if (sN++ % 30 == 0) std::printf("[world] search view eye %.0f,%.0f,%.0f look %.2f,%.2f,%.2f: %s\n", eye[0], eye[1], eye[2], dir[0], dir[1], dir[2], word.c_str());
    }
    if (word != sSeen) {
        sSeen = word;
        sSeenFor = 0;
    }
    if (++sSeenFor < kFocusSettle || word == sSaid) return;
    sSaid = word;  // nothing in the middle stays silent, and a return to the same thing is named again
    if (!word.empty()) say(word);
}

// Health comes back a point at a time, some while after the last hurt, at input reads. Written
// directly: the setter fn_800B9488 plays a reaction, and needs a guest call. The speech for a
// single point is held back in pollStatus.
void refillHealth(u32 luigi) {
    static s32 sLast = 0;
    static int sWait = kRefillDelay;
    const s32 health = s16(rd16(luigi + kHealth)), most = s16(rd16(luigi + kHealthMost));
    const bool hurt = health < sLast;
    sLast = health;
    if (!port::settings().accessibility.healthRefill || health <= 0 || most <= 0 || most > 999 || health >= most || hurt) {
        sWait = kRefillDelay;
        return;
    }
    if (--sWait > 0) return;
    sWait = kRefillStep;
    wr16(luigi + kHealth, u32(health + 1));
    sLast = health + 1;
    if (health + 1 == most) say("Health full", false);
}

// A moving platform: Luigi travels while nobody is moving him. Knockback is over in a moment, so it
// takes a while to count. The game has no one marker for these, so this watches the result, and only
// where the guides name a moving floor: the Ball Room's and the Rec Room's treadmill. A light push of
// the stick walks Luigi too, so anything past its rest position counts as steering.
void noticePlatform(const Vec& l, s8 stickX, s8 stickY, bool walking, u32 nameIndex, bool towed) {
    static Vec sWas{};
    static int sCarried = 0, sStill = 0, sSteered = 0;
    static bool sOn = false;
    const float moved = std::sqrt((l.x - sWas.x) * (l.x - sWas.x) + (l.y - sWas.y) * (l.y - sWas.y) + (l.z - sWas.z) * (l.z - sWas.z));
    sWas = l;
    if (moved > kPlatformJump) {  // a door or a warp
        sCarried = sStill = sSteered = 0;
        sOn = false;
        return;
    }
    if (nameIndex != kBallRoom && nameIndex != kRecRoom) {
        sCarried = sStill = sSteered = 0;
        sOn = false;
        return;
    }
    const bool steering = walking || towed || std::abs(stickX) > kStickAtRest || std::abs(stickY) > kStickAtRest;
    sSteered = steering ? sSteered + 1 : 0;
    if (!steering && moved > kPlatformMoving) {
        sCarried++;
        sStill = 0;
    } else if (!steering) {
        sCarried = 0;
        sStill++;
    }
    if (!sOn && sCarried >= kPlatformReads) {
        sOn = true;
        say("On a moving platform", false);
    }
    // Stopped, or the player has walked Luigi away.
    if (sOn && (sStill >= kPlatformReads || sSteered >= kPlatformLeave)) {
        sOn = false;
        sCarried = 0;
        say("Off the platform", false);
    }
}

// A heart's life runs down at +0x1AC (fn_800D1704), and it goes at 0.
void keepHearts() {
    if (!port::settings().accessibility.itemsStay) return;
    for (u32 i = 1; i < kActorSlotCount; i++) {
        const u32 a = rd32(kActorSlots + 4 * i);
        if (!gu::inMem1(a)) continue;
        const u32 type = rd32(a + kActType);
        if (type != 205 && type != 206) continue;
        const s32 left = s32(rd32(a + kHeartLife));
        if (left > 0 && left < kKeepTimer) wr32(a + kHeartLife, u32(kKeepTimer));
    }
}

// What is going on in a boss fight, from what the game shows: health, whether the boss can be seen,
// what is being thrown, and the Boos left. The numbers are the game's own.
std::mutex sBossM;
std::string sBossLine;
std::atomic<bool> sBossNear{false};

}  // namespace
bool bossOpen(const Target* boss);  // defined below, outside this namespace
namespace {

// One fight, one boss: Bowser's suit (229 head, 230 body) and King Boo (221) who comes out of it count as
// the same fight. Chauncey is 63 in the Nursery and 64 on his bed, two different scenes.
u32 bossFamily(u32 type) { return type == 229 || type == 230 || type == 221 ? 230u : type; }

// The boss the lock, the Go to menu and the speech follow: one that can be vacuumed now first, then the
// suit's body over its head (they swap as the nearer), then the nearest. King Boo is listed only while
// he shows, which is when he is out of the suit, and then he is the one to catch.
const Target* pickBoss(const std::vector<Target>& targets, const Vec& l) {
    const Target* best = nullptr;
    int bestRank = -1;
    for (const Target& t : targets) {
        if (t.kind != Kind::Boss) continue;
        const int rank = bossOpen(&t) ? 3 : t.type == 221 ? 2 : t.type == 229 ? 0 : 1;
        if (rank > bestRank || (rank == bestRank && flatDistance(t.pos, l) < flatDistance(best->pos, l))) {
            best = &t;
            bestRank = rank;
        }
    }
    return best;
}

void bossSpeech(const std::vector<Target>& targets, const Vec& l, bool cleared) {
    static const bool sLogPhases = std::getenv("LM_ACCESS_WORLD_LOG") != nullptr;
    using ms = std::chrono::milliseconds;
    using clk = std::chrono::steady_clock;
    static u32 sType = 0;
    static s32 sTop = 0, sStep = 0;
    static bool sSeen = false, sLowSaid = false;
    static clk::time_point sLastSeen{}, sLastBoos{};
    static int sBoos = -1, sBoosSaid = -1;
    const auto now = clk::now();
    static u32 sNamed = 0;  // the bodies of this fight already named: 1 the suit, 2 King Boo
    const Target* boss = pickBoss(targets, l);
    int boos = 0;
    for (const Target& t : targets)
        if (t.kind == Kind::Ghost && t.pos.y > -500.0f && (rd32(t.id + kActType) == 222 || rd32(t.id + kActType) == 214)) boos++;
    if (boss) {
        const char* name = bossWord(boss->type);
        const bool suit = boss->type == 229 || boss->type == 230;
        const s32 hp = suit ? 0 : s16(rd16(boss->id + 0xFC));
        const u32 body = boss->type == 221 ? 2u : 1u;
        if (bossFamily(boss->type) != sType) {
            sType = bossFamily(boss->type);
            sTop = hp;
            sStep = 4;
            sSeen = true;
            sLowSaid = false;
            sNamed = body;
            say(hp <= 0 ? std::string(name) : std::string(name) + ", health " + std::to_string(hp));  // the suit and Boolossus show none
        } else if (!(sNamed & body)) {
            // King Boo out of the suit for the first time in this fight: his health starts the quarters.
            sNamed |= body;
            sSeen = true;
            if (hp > 0 && sTop <= 0) {
                sTop = hp;
                sStep = 4;
            }
            say(hp <= 0 ? std::string(name) : std::string(name) + ", health " + std::to_string(hp));
        } else if (!sSeen) {
            sSeen = true;
            say(std::string(name) + " is back");
        }
        sLastSeen = now;
        // A quarter of its health at a time.
        const s32 step = sTop > 0 ? (hp * 4 + sTop - 1) / sTop : 0;
        if (hp > 0 && step < sStep) {
            sStep = step;
            say(std::string(name) + ", health " + std::to_string(hp), false);
        }
        if (boss->type == 221 && hp > 0 && hp < 200 && !sLowSaid) {
            sLowSaid = true;
            say("King Boo is weak. Bowser's head will go back on the wrong way round, and the suit will run wild.", false);
        }
        std::lock_guard lk(sBossM);
        sBossLine = hp <= 0 ? std::string(name) : std::string(name) + ", health " + std::to_string(hp);
        sBossNear = true;
    } else if (sSeen && now - sLastSeen > ms(1500)) {
        sSeen = false;
        say(std::string(bossWord(sType)) + " has vanished");
    }
    // In the mansion a boss that is gone from the room is gone: no phases go on being read out.
    if (!boss && sType != 0 && rd32(kCurMap) == kMansionMap && now - sLastSeen > ms(1500)) {
        sType = 0;
        std::lock_guard lk(sBossM);
        sBossLine.clear();
        sBossNear = false;
    }
    if (sType != 0 && now - sLastSeen > ms(30000)) {
        sType = 0;
        std::lock_guard lk(sBossM);
        sBossLine.clear();
        sBossNear = false;
    }
    // Small Boos, in the Boolossus fight: only once the count holds still.
    if (rd32(kCurMap) == kBoolossusMap) {
        if (boos != sBoos) {
            sBoos = boos;
            sLastBoos = now;
        } else if (boos != sBoosSaid && boos > 0 && now - sLastBoos > ms(800)) {
            sBoosSaid = boos;
            say(std::to_string(boos) + (boos == 1 ? " Boo left" : " Boos left"), false);
        }
    }
    // The Nursery before the fight: the guides' first step, once each visit.
    static bool sWakeSaid = false;
    if (sBabyWaits && !cleared && !boss && !sWakeSaid) {
        sWakeSaid = true;
        say("This is Chauncey's room. Vacuum the rocking horse to wake him. It is in the Go to menu, under Furniture.", false);
    }
    if (!sBabyWaits) sWakeSaid = false;
    // The phase of the fight, from what is in play. Each boss's phases are from the guides listed in
    // docs/accessibility/boss-notes.md; the game does not name them.
    if (sType == 0) return;
    int horses = 0, balls = 0, bombs = 0, shadows = 0;
    bool kingBoo = false;
    for (const Target& t : targets) {
        if (t.kind == Kind::Boss && t.type == 221) kingBoo = true;
        if (t.kind != Kind::Thrown) continue;
        if (t.type == 66 || t.type == 67) horses++;
        else if (t.type == 65 || t.type == 68) balls++;
        else if (t.type == 231) bombs++;
        else if (t.type == 211 || t.type == 212) shadows++;
    }
    // He floats high above the bed at rest, so his height says nothing: rising and falling does.
    static float sBossY = 0.0f, sBounce = 0.0f;
    if (boss) {
        sBounce = sBounce * 0.9f + std::abs(boss->pos.y - sBossY);
        sBossY = boss->pos.y;
    }
    const float bounce = sBounce;
    // Bouncing balls and passing horses go in and out of reach, so a phase outlasts its last sighting.
    // The ball that stays is the one that has lain alone for a while.
    static clk::time_point sLastHorse{}, sLastBall{}, sBallSince{};
    static u32 sBallId = 0;
    if (horses > 0) sLastHorse = now;
    if (balls > 0) sLastBall = now;
    u32 ball = 0;
    for (const Target& t : targets)
        if (t.kind == Kind::Thrown && (t.type == 65 || t.type == 68)) ball = t.id;
    if (balls != 1 || ball != sBallId) {
        sBallId = balls == 1 ? ball : 0;
        sBallSince = now;
    }
    const char* phase = "";
    switch (sType) {
    case 63:  // Chauncey in the Nursery: dodge what he throws, then throw a ball back
        if (balls > 0) phase = "Chauncey is throwing balls. Dodge them, then vacuum one and throw it at him.";
        break;
    case 64:  // Chauncey: horses, balls until one stays, then he jumps about
        if (bounce > kBossBounce) phase = "Chauncey is jumping. Keep moving so he cannot land on you.";
        else if (now - sLastHorse < ms(3000)) phase = "Rocking horses are charging. Dodge to the side.";
        else if (balls == 1 && now - sBallSince > ms(3000)) phase = "One ball is left. Vacuum it, then throw it at Chauncey.";
        else if (now - sLastBall < ms(3000)) phase = "Balls are bouncing. Keep clear of them.";
        break;
    case 213:  // Bogmire
        if (shadows > 0) phase = "Shadow coming. Vacuum it until it is big, then throw it at Bogmire.";
        break;
    case 220:  // Boolossus: pulled onto a horn, split, frozen, re-formed
        phase = boos > 0 ? "Boolossus has split. Freeze the small Boos with ice, then vacuum them."
                         : "Pull Boolossus onto the horn of an ice statue to split it.";
        break;
    case 221:  // King Boo out of the suit
    case 230:  // Bowser
        if (kingBoo) phase = "Bowser's head is off and King Boo is out. Vacuum him.";
        else if (bombs > 0) phase = "Spiked bomb. Vacuum it, then throw it into Bowser's mouth when he inhales.";
        break;
    default: break;
    }
    static std::string sPhase, sPhaseSaid;
    static clk::time_point sPhaseSince{};
    if (phase != sPhase) {
        sPhase = phase;
        sPhaseSince = now;
        if (sLogPhases) std::printf("[world] phase: %s (horses %d, balls %d, bombs %d, shadows %d, boos %d, boss height %.0f, bounce %.0f)\n", phase, horses, balls, bombs, shadows, boos, boss ? boss->pos.y - l.y : -1.0f, bounce);
    }
    // A phase must hold a moment: things pass in and out of play.
    if (*phase && sPhaseSaid != phase && now - sPhaseSince > ms(900)) {
        sPhaseSaid = phase;
        say(phase, false);
    }
}

// The game's candles: objects behind particle emitters (docs/RENDERING.md, "World lights follow the
// game's flames"). +0x10 state: 1 lit, 2 flickering, 3 being blown, 4 out until fire, 5 out until the
// room's light comes on.
struct Candle {
    u32 id;
    Vec pos;
    u32 state, room;
};
std::vector<Candle> candles() {
    std::vector<Candle> out;
    const u32 group = rd32(kParticleMgr + kMgrGroup);
    const u32 sentinel = gu::inMem1(group) ? rd32(group + kGroupList) : 0;
    u32 n = 0;
    for (u32 e = gu::inMem1(sentinel) ? rd32(sentinel + kEmNext) : 0; gu::inMem1(e) && e != sentinel && n < 512; e = rd32(e + kEmNext), n++) {
        const u32 owner = rd32(e + kEmOwner);
        if (!gu::inMem1(owner) || rd32(owner + 0x30) != e) continue;
        out.push_back({owner, rdVec(owner + 0x14), rd8(owner + 0x10), rd32(owner + 0x20) >> 24});
    }
    return out;
}

std::atomic<bool> sCandlesOut{false};
std::atomic<u32> sCandlesRoom{~0u};  // the room sCandlesOut is for, by room number

// One stand's flames, by how close they are: fire lights them one at a time. key: its first flame.
struct Stand {
    u32 key;
    Vec pos;
    int flames = 0, out = 0, lit = 0;
};
std::vector<Stand> candleStands(u32 room) {
    std::vector<Stand> out;
    // Test aid LM_ACCESS_TEST_LIGHT=<input reads>: lights one flame of Luigi's room that often.
    static const int sTestEvery = [] {
        const char* v = std::getenv("LM_ACCESS_TEST_LIGHT");
        return v ? std::atoi(v) : 0;
    }();
    static int sTestWait = 0;
    bool testLight = sTestEvery > 0 && ++sTestWait >= sTestEvery;
    // Test aid LM_ACCESS_TEST_SNUFF=<input reads>: puts out one flame of Luigi's room that often.
    static const int sSnuffEvery = [] {
        const char* v = std::getenv("LM_ACCESS_TEST_SNUFF");
        return v ? std::atoi(v) : 0;
    }();
    static int sSnuffWait = 0;
    bool testSnuff = sSnuffEvery > 0 && ++sSnuffWait >= sSnuffEvery;
    for (const Candle& c : candles()) {
        if (c.room != room) continue;
        if (testSnuff && c.state == kCandleLit && c.pos.z < kParlorPortrait) {
            wr8(c.id + 0x10, kCandleOut);
            testSnuff = false;
            sSnuffWait = 0;
        }
        if (testLight && c.state == kCandleOut) {
            wr8(c.id + 0x10, 1);
            testLight = false;
            sTestWait = 0;
        }
        Stand* found = nullptr;
        for (Stand& s : out)
            if (flatDistance(s.pos, c.pos) < kStandGap && std::abs(s.pos.y - c.pos.y) < kStandGap) found = &s;
        if (!found) {
            out.push_back({c.id, c.pos});
            found = &out.back();
        }
        found->key = std::min(found->key, c.id);
        found->flames++;
        // Its middle, by the running average of its flames.
        found->pos.x += (c.pos.x - found->pos.x) / float(found->flames);
        found->pos.z += (c.pos.z - found->pos.z) / float(found->flames);
        found->out += c.state == kCandleOut;
        found->lit += c.state == kCandleLit || c.state == kCandleFlicker;
    }
    return out;
}

// What lighting all of a room's candles does, by name index, from the guides.
const char* candleResult(u32 nameIndex) {
    switch (nameIndex) {
    case 4: return "The seal is broken. The door to the Fortune-teller's Room is open.";
    case 3: return "A key appears on top of the brass column beside the booth.";
    case 27: case 40: return "Ghosts are coming.";
    default: return "";
    }
}

// Says when a stand has taken the flame and needs no more. Only what Luigi lights counts: a room's
// candles coming on with its lights are never in state 4. True when a stand has just finished.
bool watchCandles(u32 room, u32 nameIndex, const std::vector<Stand>& stands) {
    bool done = false;
    static std::map<u32, int> sOut;
    static std::set<u32> sLit;
    static u32 sRoom = ~0u;
    if (room != sRoom) {
        sRoom = room;
        sOut.clear();
        sLit.clear();
    }
    sCandlesRoom = room;
    // The Parlor runs the other way: its candles start lit, and the vacuum puts them out.
    if (nameIndex == kParlor) {
        sCandlesOut = false;
        static std::map<u32, int> sWasLit;
        static u32 sLitRoom = ~0u;
        if (room != sLitRoom) {
            sLitRoom = room;
            sWasLit.clear();
        }
        int still = 0;
        for (const Stand& s : stands) still += s.lit > 0 && s.pos.z < kParlorPortrait;
        for (const Stand& s : stands) {
            if (s.pos.z >= kParlorPortrait) continue;
            const auto was = sWasLit.find(s.key);
            if (was != sWasLit.end() && was->second > 0 && s.lit == 0) {
                log("[access] candle stand at %.0f,%.0f is out", s.pos.x, s.pos.z);
                done = true;
                if (still > 0) say("One candle stand left.", false);  // the last one starts a scene, which speaks for itself
            }
            sWasLit[s.key] = s.lit;
        }
        return done;
    }
    int left = 0;
    for (const Stand& s : stands) left += s.out > 0;
    sCandlesOut = left > 0;
    for (const Stand& s : stands) {
        const auto was = sOut.find(s.key);
        const int before = was == sOut.end() ? s.out : was->second;
        sOut[s.key] = s.out;
        if (s.out == before) continue;
        log("[access] candle stand at %.0f,%.0f: %d of %d out", s.pos.x, s.pos.z, s.out, s.flames);
        if (s.out == 0) {
            sLit.insert(s.key);
            done = true;
            if (left > 0) say("Candle stand lit. It needs no more fire. " + std::to_string(left) + " more to light.", false);
            else say(std::string("All the candles are lit. ") + candleResult(nameIndex), false);
        } else if (s.out < before) {
            say("Flame lit. " + std::to_string(s.out) + " more on this stand.", false);
        } else if (before == 0 && sLit.count(s.key)) {
            sLit.erase(s.key);
            say("A candle stand has gone out. It needs fire again.", false);
        }
    }
    return done;
}

std::atomic<u32> sBooManager{0};

bool blackout() { return rd32(kCurMap) == kMansionMap && ((rd8(kEvent + 0x659 + 61 / 8) >> (61 % 8)) & 1); }

// Whether a room counts as lit, by name index. The room table is in room number order.
bool nameLit(u32 nameIndex) {
    const u32 rooms = rd32(kRoomTable), lights = rd32(kRoomLights);
    if (!gu::inMem1(rooms) || !gu::inMem1(lights) || blackout()) return false;
    for (u32 i = 0; i < 74; i++)
        if (rd8(rooms + i * 0x30 + 3) == nameIndex) return (rd8(lights + i * kRoomLightSize + kRoomLightByte) & kRoomLightBit) != 0;
    return false;
}

// The Boo in Luigi's room. The game's manager holds one record per Boo: +0 the room's name index,
// +2 the furniture table row it hides in, -1 for none, +4 must be 0 to hide, +6 its health, +8 its
// body once out. Every Boo has a row from the start, dark room or not, but only fn_800C4240 puts one
// in play, and only while the room is lit (fn_80018D78: cleared, and no blackout). So lit gates all
// of it. True and spot filled while one is hidden here. The manager exists once the Boos are let out.
bool watchBoos(u32 nameIndex, bool lit, Target& spot) {
    static u32 sRoom = ~0u, sFollowing = 0, sFollowRoom = ~0u;  // the Boo that came out, and the room it came out in
    static bool sSaid = false, sOut = false, sTaught = false;
    if (nameIndex != sRoom) {
        sRoom = nameIndex;
        sSaid = sOut = false;
    }
    const u32 manager = sBooManager;
    // A Boo followed in another map, another game, or caught: its record no longer says where it went.
    static u32 sManager = 0, sRun = 0;
    if (manager != sManager || runCount() != sRun) sFollowing = 0;
    sManager = manager;
    sRun = runCount();
    if (sFollowing != 0 && s16(rd16(sFollowing + 6)) <= 0) sFollowing = 0;
    if (!gu::inMem1(manager)) return false;
    const u32 count = rd32(manager + kBooCount);
    if (count == 0 || count > kBooMax) return false;
    if (sFollowing != 0) {  // it came out, and its record has moved to another room: Luigi walking away is not that
        const u32 now = rd8(sFollowing);
        if (now != sFollowRoom) {
            const std::string where = now == kNoBooRoom ? std::string() : roomWord(now);
            if (!where.empty()) say("The Boo has fled to the " + where + (nameLit(now) ? ". Follow it." : ". That room is still dark, so the Boo cannot be found there until its ghosts are cleared."), false);
            sFollowing = 0;
        }
    }
    u32 record = 0;
    for (u32 i = 0; i < count && !record; i++) {
        const u32 r = manager + kBooRecords + i * kBooRecordSize;
        if (rd8(r) == nameIndex && s16(rd16(r + 6)) > 0) record = r;
    }
    if (!record) return false;
    static const bool sBooLog = std::getenv("LM_ACCESS_WORLD_LOG") != nullptr;
    if (sBooLog) {
        static u32 sN = 0;
        if (sN++ % 300 == 0) std::printf("[world] Boo record room %u: row %d, wait %d, health %d, body %08X, lit %d\n", nameIndex, s16(rd16(record + 2)), s8(rd8(record + 4)), s16(rd16(record + 6)), rd32(record + 8), lit);
    }
    const s32 row = s16(rd16(record + 2));
    if (rd32(record + 8) != 0) {
        if (!sOut) {
            sOut = true;
            sFollowing = record;
            sFollowRoom = nameIndex;
            say("The Boo is out. Vacuum it now, no flashlight needed. If it gets away, it flees to another room.");
        }
        return false;
    }
    if (row < 0 || !lit || s8(rd8(record + 4)) != 0) return false;
    if (!sSaid) {
        sSaid = true;
        const std::string health = std::to_string(s16(rd16(record + 6)));
        if (sTaught) say("A Boo is hiding here, health " + health + ".", false);
        else say("A Boo is hiding in the furniture here, health " + health + ". Follow the Boo tone to the piece it is in, then press A at it or shake it with the vacuum. When the Boo comes out, vacuum it at once. The room is lit, so no flashlight is needed.", false);
        sTaught = true;
        log("[access] a Boo hides in row %d of room %u", row, nameIndex);
    }
    float at[3];
    if (!nav::furnitureRow(u32(row), at)) return false;
    spot = {Kind::Boo, {at[0], at[1], at[2]}, record};
    return true;
}

// Shivers from appear flag 35 until the Butler's Room is lit. He paces the north hallway and needs
// fire on his candle, so the player has to be told where he is and where he runs. Heard once a run,
// and again each time he changes where he is.
void shiversSpeech(u32 room) {
    static bool sTold = false, sToldHere = false, sRan = false;
    static u32 sRun = 0;
    if (runCount() != sRun) {
        sRun = runCount();
        sTold = sToldHere = sRan = false;
    }
    if (rd32(kCurMap) != kMansionMap || !sShiversHere || nameLit(kButlersRoom)) return;
    if (!sTold) {
        sTold = true;
        say("Shivers the butler is out. He paces the north hallway on the first floor with a candle.", false);
    }
    if (room == kShiversHall && sShiversRoom == kShiversHall && !sToldHere) {
        sToldHere = true;
        say("Shivers paces this hallway. Follow the ghost tone until it rises, then hold the right stick up and spray fire at his candle with the left trigger.", false);
    }
    const bool ran = sShiversRoom != kShiversHall;
    if (ran != sRan) {
        sRan = ran;
        say(ran ? "Shivers is running from the fire. Go to the Butler's Room and wait for him." : "Shivers is back in the north hallway.", false);
    }
}

// Test aid LM_ACCESS_NAV_AUDIT=1: once per map, tries the routes between every room's doors.
void auditRooms() {
    static const bool sOn = std::getenv("LM_ACCESS_NAV_AUDIT") != nullptr;
    static u32 sDone = ~0u;
    if (!sOn || rd32(kCurMap) == sDone) return;
    sDone = rd32(kCurMap);
    const u32 map = rd32(kMapTable + 4 * sDone);
    const u32 doors = gu::inMem1(map) ? rd32(map + kMapDoors) : 0;
    std::vector<nav::AuditRoom> rooms;
    for (u32 room = 0; room < kAuditRooms; room++) {
        float box[4];
        if (!roomBox(room, box)) continue;
        nav::AuditRoom r{room, box[0], box[1], box[2], box[3], {}};
        for (u32 i = 0; gu::inMem1(doors) && i < kDoorMax; i++) {
            const u32 d = doors + i * kDoorSize;
            if (rd8(d) == 0) break;
            const u32 a = rd8(d + kDoorRoomA), b = rd8(d + kDoorRoomB);
            if (a == room || b == room) r.doors.push_back({float(s32(rd32(d + kDoorX))), float(s32(rd32(d + kDoorY))), float(s32(rd32(d + kDoorZ))), a == room ? b : a});
        }
        rooms.push_back(std::move(r));
    }
    nav::audit(int(sDone), std::move(rooms));
}

// Guest memory for one sound: the game reads the position while it plays and keeps its handle.
// A channel's pulses play on its own stream, so an item and a door can sound together.
struct Channel {
    int wait = 0;
    SDL_AudioStream* stream = nullptr;
};
Channel sItem, sDoor, sGhost, sBanana, sToad, sThrown, sNotice, sNpc, sCombat, sBoss, sBoo, sMedal, sSpirit;
u32 sPlayed = 0;
std::mutex sNoticeM;  // the queue is filled from the input thread too (candles)
std::vector<Kind> sNoticeWaiting;
std::atomic<bool> sSwallowPress{false};

constexpr int kToneRate = 48000;

// One tone per kind: frequency, a second tone that follows it (0 none) and the length in ms.
struct Tone {
    float freq, second;
    int ms;
};
Tone toneFor(Kind k) {
    switch (k) {
    case Kind::Chest: return {523.0f, 784.0f, 150};
    case Kind::Key: return {1568.0f, 2093.0f, 110};
    case Kind::Heart: return {392.0f, 330.0f, 170};
    case Kind::Treasure: return {1319.0f, 0.0f, 90};
    case Kind::Money: return {988.0f, 0.0f, 60};
    case Kind::Ghost: return {311.0f, 233.0f, 110};
    case Kind::GhostAimed: return {622.0f, 932.0f, 110};  // bright and rising: the light is on it
    case Kind::Banana: return {1480.0f, 1397.0f, 90};  // a clash: something to avoid
    case Kind::Toad: return {659.0f, 988.0f, 140};
    case Kind::Thrown: return {466.0f, 932.0f, 80};
    case Kind::Npc: return {554.0f, 415.0f, 170};
    case Kind::GhostLit: return {880.0f, 1175.0f, 90};
    case Kind::GhostHooked: return {294.0f, 587.0f, 180};
    case Kind::Boss: return {147.0f, 220.0f, 170};
    case Kind::Holding: return {1047.0f, 1319.0f, 90};
    case Kind::CanThrow: return {1760.0f, 2349.0f, 70};
    case Kind::GhostHeart: return {1175.0f, 1568.0f, 130};
    case Kind::Boo: return {740.0f, 831.0f, 120};
    case Kind::Medal: return {988.0f, 1480.0f, 170};
    case Kind::Spirit: return {622.0f, 466.0f, 140};
    case Kind::Candle: return {587.0f, 880.0f, 200};
    case Kind::SceneStart: return {220.0f, 330.0f, 260};
    case Kind::SceneEnd: return {330.0f, 220.0f, 260};
    case Kind::DialogueEnd: return {587.0f, 0.0f, 120};
    default: return {196.0f, 0.0f, 110};  // door
    }
}

// pan: -1 left to 1 right. pitch scales the tone. False while the last tone still plays.
bool playTone(Channel& ch, Kind kind, float pan, float pitch) {
    if (!ch.stream) {
        const SDL_AudioSpec spec{SDL_AUDIO_F32, 2, kToneRate};
        ch.stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (!ch.stream) return true;
        SDL_ResumeAudioStreamDevice(ch.stream);
    }
    if (SDL_GetAudioStreamQueued(ch.stream) > 1024) return false;  // the resampler keeps a small tail
    const Tone tone = toneFor(kind);
    const int frames = kToneRate * tone.ms / 1000;
    const float angle = (pan + 1.0f) * 0.25f * 3.14159265f;
    const float left = std::cos(angle), right = std::sin(angle);
    const float volume = 0.3f * port::settings().audio.masterVolume * float(port::settings().accessibility.cueVolume) / 100.0f;
    std::vector<float> out(size_t(frames) * 2);
    float phase = 0.0f;
    for (int i = 0; i < frames; i++) {
        const float t = float(i) / float(frames);
        const float freq = (tone.second > 0.0f && t >= 0.5f ? tone.second : tone.freq) * pitch;
        phase += 6.2831853f * freq / float(kToneRate);
        const float envelope = std::min(t * 20.0f, 1.0f) * (1.0f - t);
        const float s = std::sin(phase) * envelope * volume;
        out[size_t(i) * 2] = s * left;
        out[size_t(i) * 2 + 1] = s * right;
    }
    SDL_PutAudioStreamData(ch.stream, out.data(), int(out.size() * sizeof(float)));
    sPlayed++;
    return true;
}
bool cueOn(Kind k) {
    const auto& a = port::settings().accessibility;
    switch (k) {
    case Kind::Chest: return a.cueChest;
    case Kind::Key: return a.cueKey;
    case Kind::Heart: return a.cueHeart;
    case Kind::Treasure: return a.cueTreasure;
    case Kind::Money: return a.cueMoney;
    case Kind::Door: return a.cueDoor;
    case Kind::Ghost: return a.cueGhost;
    case Kind::GhostAimed: return a.cueGhostAimed;
    case Kind::Banana: return a.cueBanana;
    case Kind::Toad: return a.cueToad;
    case Kind::Thrown: return a.cueThrown;
    case Kind::Npc: return a.cueNpc;
    case Kind::GhostLit: return a.cueGhostLit;
    case Kind::GhostHooked: return a.cueGhostHooked;
    case Kind::Boss: return a.cueBoss;
    case Kind::Holding: return a.cueHolding;
    case Kind::CanThrow: return a.cueCanThrow;
    case Kind::GhostHeart: return a.cueGhostHeart;
    case Kind::Boo: return a.cueBoo;
    case Kind::Medal: return a.cueMedal;
    case Kind::Spirit: return a.cueSpirit;
    case Kind::Candle: return a.cueCandle;
    case Kind::SceneStart: return a.cueSceneStart;
    case Kind::SceneEnd: return a.cueSceneEnd;
    case Kind::DialogueEnd: return a.cueDialogueEnd;
    }
    return true;
}

// What the vacuum is locked on to: it stays quiet until let go. What was thrown, for a few moments.
u32 sHeldId = 0, sShotId = 0;
std::chrono::steady_clock::time_point sShotAt{};

// Easier boss hits: an object just thrown, once near the boss, goes straight to it. The game's own
// collision then does the rest, so a hit counts as any other. The boss's height no longer matters.
// The boss the lock works on, nearest first; kept for the moment a throw leaves the nozzle.
Vec sLockBoss{};
bool sLockBossSet = false;

void steerShot(const Target* boss, const std::vector<Target>& targets) {
    if (sShotId == 0) return;
    if (!port::settings().accessibility.easierBossHits || std::chrono::steady_clock::now() - sShotAt > std::chrono::milliseconds(kShotMs)) {
        sShotId = 0;
        return;
    }
    if (!boss) return;
    for (const Target& t : targets) {
        if (t.id != sShotId) continue;
        const float dx = boss->pos.x - t.pos.x, dy = boss->pos.y - t.pos.y, dz = boss->pos.z - t.pos.z;
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d > (port::settings().accessibility.bossLock ? kLockShotReach : kShotReach) || d < 1.0f) return;
        const float step = std::min(kShotStep, d) / d;
        wr32(t.id + kActPos, std::bit_cast<u32>(t.pos.x + dx * step));
        wr32(t.id + kActPos + 4, std::bit_cast<u32>(t.pos.y + dy * step));
        wr32(t.id + kActPos + 8, std::bit_cast<u32>(t.pos.z + dz * step));
        static u32 sLogged = 0;
        if (sLogged != sShotId) {
            sLogged = sShotId;
            log("[access] steering a shot at the boss, %.0f away", d);
        }
        return;
    }
}

// Keeps the vacuum running for the player. No value in the game says a ball is held, so this does not
// ask: pressing the right trigger with a thrown object in reach locks the vacuum on, until the left
// trigger. The game lets the right trigger win over the left, so it must let go at once for the throw.
void holdObject(u32 luigi, const Vec& l, const Target* boss, const std::vector<Target>& targets, bool noLock, u16& button, u8& triggerLeft, u8& triggerRight) {
    const auto& a = port::settings().accessibility;
    static bool sLocked = false;
    static int sAway = 0, sPulse = 0;
    static u32 sType = 0;
    const u32 nozzle = rd32(luigi + kNozzle);
    if (!a.holdObjects || !gu::inMem1(nozzle)) {
        sLocked = false;
        sHeldId = 0;
        return;
    }
    const Vec n = rdVec(nozzle + kActPos);
    float nearest = 1e30f;
    u32 type = 0, id = 0;
    for (const Target& t : targets) {
        if (t.kind != Kind::Thrown || t.type == 66 || t.type == 67) continue;  // horses cannot be held
        const float d = std::sqrt((t.pos.x - n.x) * (t.pos.x - n.x) + (t.pos.y - n.y) * (t.pos.y - n.y) + (t.pos.z - n.z) * (t.pos.z - n.z));
        if (d < nearest) {
            nearest = d;
            type = t.type;
            id = t.id;
        }
    }
    const bool right = (button & PAD_TRIGGER_R) != 0 || triggerRight > kTriggerPressed;
    const bool left = (button & PAD_TRIGGER_L) != 0 || triggerLeft > kTriggerPressed;
    static const bool sLog = std::getenv("LM_ACCESS_WORLD_LOG") != nullptr;
    if (!sLocked) {
        if (!right || nearest > kLockReach || left) return;
        if (noLock) return;
        sLocked = true;
        sAway = 0;
        sType = type;
        sHeldId = id;
        if (a.cueHolding) playTone(sCombat, Kind::Holding, 0.0f, 1.0f);
        if (!boss) say(std::string(thrownWord(type)) + " in reach. Vacuum locked on. Left trigger throws.");
        log("[access] vacuum locked on, %s at %.0f", thrownWord(type), nearest);
    }
    if (left) {
        sLocked = false;
        if (rd32(luigi + 0x10EC) != 0) {  // only what is truly on the nozzle counts as a shot
            sShotId = sHeldId;
            sShotAt = std::chrono::steady_clock::now();
        }
        sHeldId = 0;
        button &= u16(~PAD_TRIGGER_R);
        triggerRight = 0;
        if (sLog) std::printf("[world] vacuum let go for the throw\n");
        return;
    }
    // What it had is gone, or was thrown some other way.
    sAway = nearest > kLockReach ? sAway + 1 : 0;
    if (sAway > kLockLost) {
        sLocked = false;
        sHeldId = 0;
        say("Vacuum released");
        return;
    }
    button |= PAD_TRIGGER_R;
    triggerRight = 255;
    // Ready to throw: the boss is ahead, about as near as a shot needs.
    if (!boss || !a.cueCanThrow) return;
    // The nozzle, not Luigi's body: it is what the throw follows.
    const float fx = rdf(luigi + kNozzleDir), fz = rdf(luigi + kNozzleDir + 8), facing = std::hypot(fx, fz);
    const float dx = boss->pos.x - l.x, dz = boss->pos.z - l.z, dist = std::max(std::hypot(dx, dz), 1.0f);
    if (facing < 1e-3f || (fx * dx + fz * dz) / (facing * dist) < kThrowCosine || (dist > kThrowReach && !a.bossLock)) return;
    if (sPulse > 0) {
        sPulse--;
        return;
    }
    sPulse = 8;
    playTone(sCombat, Kind::CanThrow, 0.0f, 1.0f);
}

// Luigi can stop 113 from a chest's centre and 95 from a door's: they are solid.
constexpr float kThrownReach = 130.0f;  // the vacuum reaches this far
constexpr float kDoorPress = 105.0f, kChestPress = 125.0f, kToadPress = 110.0f;
constexpr float kSolidReach = 140.0f;  // silent from here: Luigi has arrived
constexpr int kMaxAutoPresses = 3;  // per object: an opened chest stays where it is
bool solid(Kind k) { return k == Kind::Chest || k == Kind::Door || k == Kind::Toad || k == Kind::Npc; }
std::map<u32, int> sAutoPresses;

std::set<u32> sReached;  // objects Luigi got to: silent until he changes room
u32 sLastRoom = ~0u;
bool sAHeld = false;  // the player's A, before a walk adds its own presses

bool playFor(Channel& ch, const Target& t, const Vec& luigi) {
    // The camera's right axis (first row of its view matrix) and forward on the floor.
    const Vec right{rdf(kCamera + kCamViewMtx), rdf(kCamera + kCamViewMtx + 4), rdf(kCamera + kCamViewMtx + 8)};
    const float dx = t.pos.x - luigi.x, dz = t.pos.z - luigi.z;
    const float side = dx * right.x + dz * right.z;
    const float ahead = dx * right.z - dz * right.x;  // stick up is forward
    const float dist = std::max(std::hypot(dx, dz), 1.0f);
    // Three pitches: level for straight left or right, higher for up and its diagonals, lower for down.
    const float pitch = ahead > dist * kLevelBand ? kPitchUp : ahead < -dist * kLevelBand ? kPitchDown : 1.0f;
    return playTone(ch, t.kind, std::clamp(side / dist * 1.4f, -1.0f, 1.0f), pitch);
}

void pulse(Channel& ch, const Target* t, const Vec& luigi, int fastest, int slowest) {
    if (!t) {
        ch.wait = 0;
        return;
    }
    const int wait = std::clamp(int(flatDistance(t->pos, luigi) / 25.0f), fastest, slowest);
    ch.wait = std::min(ch.wait, wait);  // something rushing closer must not wait out a slow beat
    if (ch.wait > 0) {
        ch.wait--;
        return;
    }
    if (!playFor(ch, *t, luigi)) return;  // try again on the next read, or close sounds slower than far
    ch.wait = wait;
}

// A short A press, then a pause, so the game sees separate presses.
void pressA(u16& button, u32 id) {
    static int sTimer = 0;
    if (sTimer == 0) {
        if (sAutoPresses[id] >= kMaxAutoPresses) return;
        sAutoPresses[id]++;
        sTimer = 24;
    }
    if (sTimer-- > 20) button |= PAD_BUTTON_A;
}

}  // namespace

void swallowPadPress() { sSwallowPress = true; }

// The gallery's portraits, in plaque order, matched to the pictures in /Nakamura/up/obaNNgp1.mdl. The
// titles are the English game's. The descriptions are of those pictures.
namespace {
constexpr u32 kPlaqueGhost = 0x80389A74;  // + plaque * 4: the ghost's number in the record below
constexpr u32 kPortraitGrades = 0x803CFA84;  // two bits per ghost, as fn_800555EC reads them: 0 not caught
constexpr const char* kFrames[] = {"", "Bronze frame", "Silver frame", "Gold frame"};
constexpr struct {
    const char* name;
    const char* title;
    const char* picture;
} kPortraits[] = {
    {"Neville", "the Bookish Father", "A gaunt blue-grey man with a tall orange quiff and a big orange moustache, mouth open, in a blue patterned robe, against purple."},
    {"Lydia", "the Mirror-Gazing Mother", "A blue-skinned woman with short curly blonde hair and a red robe, combing her hair, against dark gold."},
    {"Nana", "the Scarf-Knitting Granny", "A stooped old woman with her hair in a round bun, a bell-shaped figure, all in brown like an old photograph."},
    {"Chauncey", "the Spoiled Baby", "A huge round blue baby with a green tuft of hair and a blue ring at his mouth, crawling over a green and yellow blanket."},
    {"Melody Pianissima", "the Beautiful Pianist", "A slim pale woman with long blonde hair and a dark red gown, her hands raised as if over the keys, against cream."},
    {"Miss Petunia", "the Bathing Beauty", "A very large round pink woman speckled with foam and flowers, against speckled red and white."},
    {"Madame Clairvoya", "the Freaky Fortune-Teller", "A woman in a purple turban with a veil over her lower face and gold earrings, in a red robe, against yellow fading to black."},
    {"Shivers", "the Wandering Butler", "A thin grey butler with a bald head in a black tailcoat, holding up a lit candle, against purple fading to black."},
    {"Sue Pea", "the Dozing Girl", "A girl with long black and yellow striped pigtails and green eyes, shown upside down in bold flat colours, against purple."},
    {"The Floating Whirlindas", "the Dancing Couple", "A tall man in a red jacket holding a woman with curly yellow hair in a sparkling green gown, in vivid unreal colours, against purple and orange."},
    {"Mr. Luggs", "the Glutton", "An enormous round purple-faced man with thick lips and a white bib over a red robe, a knife and fork raised, against dark yellow."},
    {"Jarvis", "the Jar Collector", "A bright pink ghost with a small worried face, rising like smoke out of a little white jar, against dark purple cloud."},
    {"Spooky", "the Hungry Guard Dog", "A fat lilac bulldog with a huge open red mouth and white fangs, against green fading to black."},
    {"Henry and Orville", "the Twin Brothers", "Two identical blond boys with ghostly tails, in purple and red waistcoats, side by side, against green fading to purple."},
    {"Clockwork Soldiers", "the Toy Platoon", "Three wooden toy soldiers in tall black hats and bright jackets, standing in a row, against dark blue."},
    {"Biff Atlas", "the Bodybuilder", "A muscular blond man in a red vest, gold belt and purple trousers, thrusting a fist forward, against green."},
    {"Slim Bankshot", "the Lonely Poolshark", "A thin pale man in a purple cap and black waistcoat, leaning forward along a pool cue, against orange."},
    {"Sir Weston", "the Chilly Climber", "A climber in a brown fur-trimmed coat with a red pack, leaning into a blizzard, half lost in grey and white snow."},
    {"Bogmire", "the Cemetery Shadow", "A tall pointed purple shadow with yellow eyes and a screaming mouth, melting into a pool at its base, against dark blue."},
    {"Uncle Grimmly", "Hermit of the Darkness", "A bald long-faced old man with pale eyes, in a brown suit and purple tie with a red flower, against gold fading to black."},
    {"Vincent Van Gore", "the Starving Artist", "A thin blue painter with a red beret and green jacket, holding up a brush and palette, against blue."},
    {"Boolossus", "the Jumbo Ghost", "A vast white Boo seen from the side, eyes closed and long red tongue hanging out, against dark blue."},
    {"King Boo and Bowser", "the Dastardly Duo", "Bowser, red-haired and spike-collared, reaching forward with flames behind him, and King Boo in his crown floating over his shoulder."},
};

int portraitGrade(int plaque) {
    const int ghost = s32(rd32(kPlaqueGhost + u32(plaque) * 4)) - 1;
    if (ghost < 0 || ghost >= 23) return 0;
    return (rd8(kPortraitGrades + u32(ghost / 4)) >> (ghost % 4 * 2)) & 3;
}
}  // namespace

// The list screen (fn_8005232C, object at r13+0x6E0): +1 what shows, 0 the Item page, 1 the Ghost page,
// 2 a ghost's profile, +2 the cursor on the Ghost page, +0 nonzero opens every tile. Tiles in order:
// the game's table at 0x8033804C holds each one's actor type, and a bit by type in the bytes at
// [Luigi +0x86C] +4 says it was caught. These are the plaques of the same ghosts. King Boo has no tile.
constexpr struct {
    u32 type;
    int plaque;
} kTiles[] = {{46, 1}, {47, 2}, {64, 4}, {210, 10}, {14, 8}, {38, 5}, {186, 7}, {44, 11}, {120, 13}, {213, 19}, {54, 16}, {45, 6}, {48, 3}, {55, 17}, {49, 14}, {220, 22}, {37, 12}, {173, 9}, {53, 21}, {187, 20}, {57, 15}, {217, 18}};

namespace {
bool tileCaught(u32 screen, u32 type) {
    if (rd8(screen) != 0) return true;
    const u32 luigi = rd32(kActorSlots);
    const u32 record = gu::inMem1(luigi) ? rd32(luigi + 0x86C) : 0;
    const u32 bits = gu::inMem1(record) ? rd32(record + 4) : 0;
    return gu::inMem1(bits) && ((rd8(bits + type / 8) >> (type % 8)) & 1) != 0;
}

std::string treasureList() {
    static const char* const kTreasure[] = {"coins", "bills", "gold bars", "sapphires", "emeralds", "rubies", "diamonds", "pink diamonds", "gold diamonds", "small pearls", "medium pearls", "large pearls"};
    const u32 luigi = rd32(kActorSlots);
    if (!gu::inMem1(luigi)) return "";
    std::string out;
    for (u32 i = 0; i < std::size(kTreasure); i++) {
        const s32 n = s32(rd32(luigi + 0x12F4 + i * 4));
        if (n > 0) out += (out.empty() ? "" : ", ") + std::to_string(n) + " " + kTreasure[i];
    }
    return out.empty() ? "No treasure yet." : out + ".";
}
}  // namespace

void listScreen(u32 screen) {
    if (!active() || port::usCodeFeature() != port::Avail::Yes || !gu::inMem1(screen)) return;
    static u64 sLast = 0;
    static int sShown = -1, sCursor = -1;
    const u64 now = SDL_GetTicks();
    const bool fresh = now - sLast > 500;
    sLast = now;
    const int shown = rd8(screen + 1), cursor = rd8(screen + 2);
    if (!fresh && shown == sShown && cursor == sCursor) return;
    const bool newPage = fresh || shown != sShown;
    sShown = shown;
    sCursor = cursor;
    const std::string open = fresh ? "Game Boy Horror list. The left stick moves, A checks, B or Z goes back. " : "";
    if (shown == 0) {
        say(open + "Items. " + treasureList() + " Right for ghosts.");
    } else if ((shown == 1 || shown == 2) && cursor < int(std::size(kTiles))) {
        const auto& tile = kTiles[size_t(cursor)];
        const auto& p = kPortraits[size_t(tile.plaque - 1)];
        const bool caught = tileCaught(screen, tile.type);
        const std::string where = std::to_string(cursor + 1) + " of " + std::to_string(std::size(kTiles));
        if (shown == 2) say(std::string(p.name) + ", " + p.title + ". " + p.picture);
        else say(open + (newPage ? "Ghosts. " : "") + (caught ? std::string(p.name) + ", caught, " : std::string("Not caught yet, ")) + where);
    }
    log("[access] list screen: shows %d, cursor %d", shown, cursor);
}

std::string plaqueName(int plaque) {
    if (plaque < 1 || plaque > int(std::size(kPortraits)) || port::usCodeFeature() != port::Avail::Yes) return "Portrait";
    return portraitGrade(plaque) ? kPortraits[plaque - 1].name : "Blank canvas";
}

void galleryPlaque(int plaque) {
    if (!active() || plaque < 1 || plaque > int(std::size(kPortraits)) || port::usCodeFeature() != port::Avail::Yes) return;
    const int grade = portraitGrade(plaque);
    const auto& p = kPortraits[plaque - 1];
    log("[access] plaque %d, grade %d", plaque, grade);
    say(grade ? std::string(p.name) + ", " + p.title + ". " + kFrames[grade] + ". " + p.picture : std::string("A blank canvas. This ghost is not caught yet."));
}

// fn_800B9680's callers, USA: each kind of damage that is not a hit. The floor is named in
// docs/HIDDEN_MANSION.md. The 15 point one matches Bowser's inhale in the guides.
bool damageCancelled(u32 lr, u32 callerLr) {
    if (!active() || port::usCodeFeature() != port::Avail::Yes || lr != 0x800B96A8u) return false;
    const auto& a = port::settings().accessibility;
    const auto in = [&](u32 fn, u32 size) { return callerLr >= fn && callerLr < fn + size; };
    if (std::getenv("LM_ACCESS_WORLD_LOG")) std::printf("[world] damage called from %08X\n", callerLr);
    const bool cancel = in(0x800B41E0u, 0x5D0) ? a.noFloorDamage
                      : in(0x800B47B0u, 0x4F8) || in(0x800B4F2Cu, 0x1E4) ? a.noLingeringDamage
                      : in(0x800B5110u, 0x214) ? a.noGhostDamage : false;
    if (cancel) log("[access] damage from %08X cancelled", callerLr);
    return cancel;
}

bool hitCancelled(u32 hit) {
    if (!active() || port::usCodeFeature() != port::Avail::Yes || !gu::inMem1(hit)) return false;
    const s32 id = s32(rd32(hit + 0x14));
    // Bogmire's shadows, and it seems Bowser's flames and spiked balls, name no attacker.
    if (id < 0 && (rd32(kCurMap) == kBowserMap || rd32(kCurMap) == kBogmireMap) && port::settings().accessibility.noThrownDamage) {
        static u32 sCount = 0;
        if (sCount++ % 200 == 0) log("[access] hit from a boss arena, damage %d, cancelled, %u so far", s32(rd32(hit + 0x18)), sCount);
        return true;
    }
    const u32 attacker = id >= 0 && u32(id) < kActorSlotCount ? rd32(kActorSlots + 4 * u32(id)) : 0;
    const u32 type = gu::inMem1(attacker) ? rd32(attacker + kActType) : ~0u;
    const auto& a = port::settings().accessibility;
    const bool thrown = isThrown(type);  // Bogmire's shadows are ghosts by flag, and count as thrown
    const bool cancel = thrown ? a.noThrownDamage : isBoss(type) || isGhost(type) ? a.noGhostDamage : a.noHazardDamage;
    // In last_run.log: which attacks come from which type, for the bosses not played in testing.
    // A cancelled hit lands again every frame while Luigi stays in contact.
    static s32 sLastType = -2;
    static std::chrono::steady_clock::time_point sLastHit{};
    const auto hitNow = std::chrono::steady_clock::now();
    if (s32(type) != sLastType || hitNow - sLastHit > std::chrono::seconds(2)) log("[access] hit from type %d, damage %d, kind %d%s", s32(type), s32(rd32(hit + 0x18)), s32(rd32(hit + 0x20)), cancel ? ", cancelled" : "");
    sLastType = s32(type);
    sLastHit = hitNow;
    return cancel;
}

void playCue(Cue cue) { playTone(sItem, Kind(int(cue)), 0.0f, 1.0f); }

// Two can fall due together, and a channel plays one tone at a time: the later one waits its turn.
void noticeCue(Cue cue) {
    if (!cueOn(Kind(int(cue)))) return;
    std::lock_guard lk(sNoticeM);
    sNoticeWaiting.push_back(Kind(int(cue)));
}

void noticeTick() {
    std::lock_guard lk(sNoticeM);
    if (sNoticeWaiting.empty() || !playTone(sNotice, sNoticeWaiting.front(), 0.0f, 1.0f)) return;
    if (std::getenv("LM_ACCESS_WORLD_LOG")) std::printf("[world] notice cue %d\n", int(sNoticeWaiting.front()));
    sNoticeWaiting.erase(sNoticeWaiting.begin());
}

std::string bossStatus() {
    std::lock_guard lk(sBossM);
    return sBossLine;
}

void ghostMessage(u32 msg) {
    // What a boss is told, so a report from a fight can be read back: 2 and 21 come every frame.
    // 1, 2, 12, 20, 26, 27 and 28 run all the time; the rest mark something happening.
    // 21 also comes every frame: without it, and with a repeat logged at most every 2 seconds, a long
    // fight does not fill last_run.log.
    if (sBossNear && msg != 1 && msg != 2 && msg != 12 && msg != 20 && msg != 21 && msg != 26 && msg != 27 && msg != 28) {
        static u32 sLastMsg = ~0u;
        static u64 sLastAt = 0;
        const u64 at = SDL_GetTicks();
        if (msg != sLastMsg || at - sLastAt > 2000) log("[access] a ghost near the boss got message %u", msg);
        sLastMsg = msg;
        sLastAt = at;
    }
    if ((msg != 2 && msg != 9) || !active() || !port::settings().accessibility.ghostSounds || port::usCodeFeature() != port::Avail::Yes) return;
    // The beam message comes every frame the light touches: a repeat at most this often.
    static u64 sLastLit = 0;
    const u64 now = SDL_GetTicks();
    if (msg == 2 && now - sLastLit < 350) return;
    if (msg == 2) sLastLit = now;
    const Kind kind = msg == 2 ? Kind::GhostLit : Kind::GhostHooked;
    if (!cueOn(kind)) return;
    playTone(sCombat, kind, 0.0f, 1.0f);
    if (msg == 9 && sBossNear && port::settings().accessibility.bossSpeech) say("Hooked. Pull away.");
    if (std::getenv("LM_ACCESS_WORLD_LOG")) std::printf("[world] ghost message %u\n", msg);
}

void keepMoney(u32 grain) {
    if (!active() || !port::settings().accessibility.itemsStay || port::usCodeFeature() != port::Avail::Yes) return;
    const s32 left = s16(rd16(grain + kGrainTimer));
    if (left > 0 && left < kKeepTimer) wr16(grain + kGrainTimer, u32(kKeepTimer));
}

void noteMoney(float x, float y, float z) {
    if (!active()) return;
    std::lock_guard lk(sMoneyM);
    if (sMoney.size() < 256) sMoney.push_back({x, y, z});
}

// With the vacuum on in a boss fight, turns the nozzle for the player: at the boss with a ball held or
// its heart out, otherwise at the nearest ball. The nozzle is what counts, not Luigi's body: the right
// stick turns it ahead of him, which the old facing test never saw. The stick that turns him depends on
// the control style, and no value for that was found, so the lock watches: if a full turn moves
// nothing it tries the other stick.
// Which AI object runs each boss: actors hold no pointer back to it, but every state change names it.
std::map<u32, u32> sBossThoughts;
u32 thoughtOf(u32 actor) {
    const auto it = sBossThoughts.find(actor);
    return it != sBossThoughts.end() && rd32(it->second + kAiActor) == actor ? it->second : 0;
}

// True while the boss can be vacuumed: its AI's own state, from the boss test bot in coop_test.cpp.
// This outlasts the heart by far.
bool bossOpen(const Target* boss) {
    if (!boss) return false;
    const u32 thought = thoughtOf(boss->id);
    const u32 strategy = thought ? rd32(thought + kAiStrategy) : 0;
    if (!gu::inMem1(strategy)) return false;
    const u32 state = rd16(strategy + 0xE);
    switch (boss->type) {
    case 64: return state == 0xC;
    case 213: return state == 5;
    case 220: return state == 7;
    case 221: return state >= 1 && state <= 3;
    default: return false;
    }
}

void lockAim(u32 luigi, const Vec& l, const Target* boss, bool open, const std::vector<Target>& targets, u16& button, u8& triggerRight, s8& stickX, s8& stickY, s8& substickX, s8& substickY) {
    static bool sOn = false, sOtherStick = false;
    static int sStill = 0;
    static float sBearing = 0.0f;
    const bool right = (button & PAD_TRIGGER_R) != 0 || triggerRight > kTriggerPressed;
    static const bool sBot = std::getenv("LM_BOSS_DRIVE") != nullptr;  // the test bot's right stick is not a player taking over
    const bool yours = !sBot && (std::abs(int(substickX)) > kLockGrab || std::abs(int(substickY)) > kLockGrab);
    if (!port::settings().accessibility.bossLock || !boss || !right || yours || rd32(luigi + kFishedGhost) != 0) {
        sOn = false;
        return;
    }
    const bool onBall = rd32(luigi + 0x10EC) != 0 || rd32(luigi + 0x10F0) != 0 || rd32(luigi + 0x10F4) != 0 || rd32(luigi + 0x10F8) != 0;
    const Target* aim = boss;
    if (!onBall && !open && !sHeartNow) {
        float nearest = kLockCatch;
        for (const Target& t : targets) {
            if (t.kind != Kind::Thrown || t.type == 66 || t.type == 67) continue;  // horses cannot be held
            const float d = flatDistance(t.pos, l);
            if (d < nearest) {
                nearest = d;
                aim = &t;
            }
        }
    }
    // The nozzle's value is only kept fresh with something on it. Until then his body shows the aim.
    const u32 pointing = onBall ? kNozzleDir : kFacing;
    float fx = rdf(luigi + pointing), fz = rdf(luigi + pointing + 8);
    const float ny = rdf(luigi + pointing + 4), facing = std::hypot(fx, fz);
    if (facing < 1e-3f) return;
    fx /= facing;
    fz /= facing;
    const float dx = aim->pos.x - l.x, dz = aim->pos.z - l.z, dist = std::max(std::hypot(dx, dz), 1.0f);
    const float dot = (fx * dx + fz * dz) / dist, cross = (fx * dz - fz * dx) / dist;
    const float turn = dot < 0.0f ? (cross < 0.0f ? -127.0f : 127.0f) : std::clamp(cross * 400.0f, -127.0f, 127.0f);
    // An open boss hooks only from close by, and not from right under it. With the vacuum held and the
    // left stick left alone, Luigi walks into the distance that works.
    const bool idle = std::abs(int(stickX)) + std::abs(int(stickY)) < kLockIdle;
    // Chauncey hooked only from his front: from his side the same distance and aim did nothing.
    const bool front = boss->type == 64;
    const float standX = front ? aim->pos.x + (l.x < aim->pos.x ? -kChaunceyFrontX : kChaunceyFrontX) : l.x, standZ = front ? aim->pos.z + kChaunceyFrontZ : l.z;
    const float toX = front ? standX - l.x : dx, toZ = front ? standZ - l.z : dz, far = std::max(std::hypot(toX, toZ), 1.0f);
    if (open && aim == boss && idle && (front ? far > 25.0f : dist > kBossHook + 20.0f || dist < kBossHook - 90.0f)) {
        const Vec camRight{rdf(kCamera + kCamViewMtx), 0.0f, rdf(kCamera + kCamViewMtx + 8)};
        const float way = (front || dist > kBossHook ? 90.0f : -90.0f) / far;
        stickX = s8(std::clamp((toX * camRight.x + toZ * camRight.z) * way, -127.0f, 127.0f));
        stickY = s8(std::clamp((toX * camRight.z - toZ * camRight.x) * way, -127.0f, 127.0f));
    }
    // Every hook seen came as the vacuum started, never long after: while parked in place it starts again.
    static int sParked = 0;
    sParked = open && aim == boss && !onBall && dist < kBossHook + 150.0f ? (sParked + 1) % kLockRegrip : 0;
    if (sParked > kLockRegrip - kLockRegripOff) {
        button &= u16(~PAD_TRIGGER_R);
        triggerRight = 0;
    }
    if (sOtherStick && idle) stickX = s8(turn);
    else substickX = s8(turn);
    // How far up the stick tilts the nozzle, not how fast: full tilt goes over an open boss's body, 100 is what hooks.
    substickY = aim == boss && open ? s8(kBossTilt) : onBall ? s8(0) : s8(std::clamp((aim->pos.y - l.y - 100.0f) / dist * 500.0f, -127.0f, 127.0f));
    const float bearing = std::atan2(fz, fx);
    float moved = std::abs(bearing - sBearing);
    if (moved > 3.14159265f) moved = 6.2831853f - moved;
    sBearing = bearing;
    sStill = std::abs(turn) > 60.0f && moved < 0.004f ? sStill + 1 : 0;
    if (sStill > kLockStill) {
        sStill = 0;
        sOtherStick = !sOtherStick;
        log("[access] lock: no turn, trying the %s stick", sOtherStick ? "left" : "right");
    }
    // Said on a change of what it follows, and never so often that it talks over the fight.
    static u64 sSaidAt = 0;
    static u32 sSaidType = 0;
    const u64 now = SDL_GetTicks();
    if (!sOn && aim->kind == Kind::Boss && (aim->type != sSaidType || now - sSaidAt > kLockSayMs)) {
        sSaidAt = now;
        sSaidType = aim->type;
        say("Locked on " + std::string(aim->kind == Kind::Boss ? bossWord(aim->type) : thrownWord(aim->type)), false);
    }
    sOn = true;
    static const bool sLog = std::getenv("LM_ACCESS_WORLD_LOG") != nullptr;
    static u32 sN = 0;
    if (sLog && sN++ % 15 == 0) std::printf("[world] lock on type %u: dist %.0f, dot %.2f, on ball %d, slots %08X %08X, flags %u %u, y %.2f, c %d,%d, stick %d\n", aim->type, dist, dot, onBall, rd32(luigi + 0x10EC), rd32(luigi + 0x10F0), rd8(luigi + 0x10E9), rd8(luigi + 0x10EA), ny, substickX, substickY, stickX);
}

void worldFrame(std::uint16_t& button, std::int8_t& stickX, std::int8_t& stickY, std::int8_t& substickX, std::int8_t& substickY, std::uint8_t& triggerLeft, std::uint8_t& triggerRight) {
    const auto& set = port::settings().accessibility;
    if (!active()) return;
    if (sSwallowPress) {
        if (button & (PAD_BUTTON_A | PAD_BUTTON_B)) {
            button &= u16(~(PAD_BUTTON_A | PAD_BUTTON_B));
            if (std::getenv("LM_ACCESS_WORLD_LOG")) std::printf("[world] held press kept from the game\n");
        }
        else sSwallowPress = false;
    }
    if (port::usCodeFeature() != port::Avail::Yes) return;
    const u32 luigi = rd32(kActorSlots);
    if (!gu::inMem1(luigi)) {
        nav::cancel("no Luigi");
        return;
    }
    keepHearts();
    searchView(luigi);
    // The training room is one long event script, with Luigi free through it.
    const bool scripted = rd32(kCurMap) != kTrainingMap && (rd32(kEvent + kEventCursor) != 0 || rd32(kMenuOff) != 0);
    if (scripted || gamePaused()) {  // Luigi is not free to move
        nav::cancel("event or pause");
        return;
    }

    refillHealth(luigi);
    const u32 room = rd32(luigi + kActRoom) >> 24;
    noteRoom(room);
    if (room != sLastRoom) {
        sLastRoom = room;
        sReached.clear();
        sAutoPresses.clear();
    }
    const Vec l = rdVec(luigi + kActPos);
    const u32 roomTable = rd32(kRoomTable);
    const u32 nameIndex = gu::inMem1(roomTable) ? rd8(roomTable + room * 0x30 + 3) : room;
    const u32 lights = rd32(kRoomLights);
    // Test aid LM_ACCESS_TEST_CLEAR=1: every room Luigi enters counts as cleared.
    static const bool sTestClear = std::getenv("LM_ACCESS_TEST_CLEAR") != nullptr;
    if (sTestClear && gu::inMem1(lights)) wr8(lights + room * kRoomLightSize + kRoomLightByte, rd8(lights + room * kRoomLightSize + kRoomLightByte) | kRoomLightBit);
    const bool cleared = gu::inMem1(lights) && (rd8(lights + room * kRoomLightSize + kRoomLightByte) & kRoomLightBit) != 0;
    const Target* item = nullptr;
    const Target* door = nullptr;
    const Target* ghost = nullptr;
    const Target* banana = nullptr;
    const Target* toad = nullptr;
    const Target* thrown = nullptr;
    const Target* npc = nullptr;
    const Target* bossTarget = nullptr;
    const Target* medal = nullptr;
    const Target* spirit = nullptr;
    const std::vector<Target> targets = scan(luigi);
    for (const Target& t : targets) {
        const float dist = flatDistance(t.pos, l);
        // The chest with a medal keeps its own beacon until the medal is taken.
        if (t.kind == Kind::Chest && t.type == kMedalChest) {
            if (set.cueMedal && (!medal || dist < flatDistance(medal->pos, l))) medal = &t;
            continue;
        }
        if (t.kind == Kind::Spirit) {
            if (set.cueSpirit && (!spirit || dist < flatDistance(spirit->pos, l))) spirit = &t;
            continue;
        }
        if (t.kind != Kind::Ghost && !cueOn(t.kind)) continue;  // the ghost's own cue is chosen below
        if (t.kind == Kind::Thrown && t.id == sHeldId) continue;  // in the vacuum: no need to find it
        if (t.kind != Kind::Ghost && t.kind != Kind::Banana && t.kind != Kind::Thrown && t.kind != Kind::Boss) {
            if (t.id && dist < (solid(t.kind) ? kSolidReach : 70.0f)) sReached.insert(t.id);
            if (t.id && sReached.count(t.id)) continue;
        }
        const Target*& best = t.kind == Kind::Door ? door : t.kind == Kind::Ghost ? ghost : t.kind == Kind::Banana ? banana : t.kind == Kind::Toad ? toad : t.kind == Kind::Thrown ? thrown : t.kind == Kind::Npc ? npc : t.kind == Kind::Boss ? bossTarget : item;
        if (!best || dist < flatDistance(best->pos, l)) best = &t;
    }
    // The boss, whatever its cue is set to: the walk and the lock do not depend on hearing it.
    const Target* lockBoss = pickBoss(targets, l);
    sLockBossSet = lockBoss != nullptr;
    const bool open = bossOpen(lockBoss);
    {
        // The boss that was open, by name: in Bowser's fight the pick moves back to the suit when King
        // Boo closes, and it is King Boo who has recovered.
        static bool sWasOpen = false;
        static u32 sOpenType = 0;
        if (open && !sWasOpen) log("[access] the boss can be vacuumed");
        if (sWasOpen && !open && rd32(luigi + kFishedGhost) == 0 && sOpenType != 0) say(std::string(bossWord(sOpenType)) + " has recovered.", false);
        if (open) sOpenType = lockBoss->type;
        sWasOpen = open;
    }
    if (lockBoss) sLockBoss = lockBoss->pos;
    // What the Go to menu offers, and the walk in progress.
    std::vector<nav::Object> objects;
    if (lockBoss) objects.push_back({nav::Category::Bosses, lockBoss->pos.x, l.y, lockBoss->pos.z, lockBoss->id, kBossStop, bossWord(lockBoss->type)});  // it may be in the air: the walk is on the floor
    // Shivers, in Bosses while he paces Luigi's hallway: the walk follows him, and says what to do there.
    if (!lockBoss && sShiversHere && sShiversRoom == room && rd32(kCurMap) == kMansionMap && !nameLit(kButlersRoom)) {
        nav::Object o{nav::Category::Bosses, sShiversPos.x, l.y, sShiversPos.z, sShiversId, kShiversStop, "Shivers"};
        o.arrive = rd32(luigi + kLuigiElement) == kElementFire
                       ? "Turn until the aim tone rises, then hold the right stick up and spray fire at his candle with the left trigger."
                       : "He needs fire on his candle, and you have no fire now.";
        objects.push_back(o);
    }
    const std::vector<Stand> stands = candleStands(room);
    if (watchCandles(room, nameIndex, stands)) noticeCue(Cue::Candle);  // the notice channel is played from the render thread
    for (const Stand& s : stands)
        if (nameIndex == kParlor ? !cleared && s.lit > 0 && s.pos.z < kParlorPortrait : s.out > 0) objects.push_back({nav::Category::Candles, s.pos.x, l.y, s.pos.z, s.key, kCandleReach, nameIndex == kParlor ? "Lit candle stand" : "Unlit candle stand"});  // the flames are high up; the walk is on the floor
    for (const Target& t : targets) {
        if (t.kind == Kind::Spirit) {
            objects.push_back({nav::Category::Elements, t.pos.x, t.pos.y, t.pos.z, t.id, kThrownReach, t.type == kTypeFireSpirit ? "Fire spirit" : t.type == kTypeIceSpirit ? "Ice spirit" : "Water spirit"});
            continue;
        }
        nav::Category category;
        switch (t.kind) {
        case Kind::Door: category = nav::Category::Doors; break;
        case Kind::Money: category = nav::Category::Money; break;
        case Kind::Heart: category = nav::Category::Hearts; break;
        case Kind::Key: category = nav::Category::Keys; break;
        case Kind::Chest: category = nav::Category::Chests; break;
        case Kind::Treasure: category = nav::Category::Treasure; break;
        case Kind::Toad: category = nav::Category::Toads; break;
        case Kind::Npc: category = nav::Category::People; break;
        case Kind::Thrown: category = nav::Category::Thrown; break;
        default: continue;
        }
        const std::string note = t.kind == Kind::Door && beenIn(t.leadsTo) ? "visited" : "";
        std::string name = t.kind == Kind::Chest && t.type == kMedalChest ? "Element medal chest" : t.kind == Kind::Door ? doorName(t.leadsTo) : t.kind == Kind::Npc ? (t.type == kTypeMelody ? "Melody Pianissima" : "Professor") : t.kind == Kind::Thrown ? thrownWord(t.type) : std::string();
        if (t.open) name = name.size() > 5 ? "Way to " + name.substr(0, name.size() - 5) : "Way through";  // "Hallway door"
        objects.push_back({category, t.pos.x, t.pos.y, t.pos.z, t.id, t.open ? 0.0f : t.kind == Kind::Door ? kDoorPress : t.kind == Kind::Chest ? kChestPress : t.kind == Kind::Toad || t.kind == Kind::Npc ? kToadPress : t.kind == Kind::Thrown ? kThrownReach : 0.0f, name, t.open, note});
        // A chest's front, from its heading (+0x88, 0x10000 a turn), so Go to tries that side first.
        if (t.kind == Kind::Chest && t.id) {
            const float turn = float(rd16(t.id + 0x88)) * (6.2831853f / 65536.0f);
            objects.back().frontX = std::sin(turn);
            objects.back().frontZ = std::cos(turn);
        }
    }
    float box[4] = {-1e9f, -1e9f, 1e9f, 1e9f};
    roomBox(room, box);
    auditRooms();
    const u32 rooms = rd32(kRoomTable);
    nav::publish(objects, l.x, l.y, l.z, rd32(kCurMap), room, gu::inMem1(rooms) ? rd8(rooms + room * 0x30 + 3) : room, box[0], box[1], box[2], box[3]);
    const bool wasPressed = sAHeld;
    sAHeld = (button & PAD_BUTTON_A) != 0;
    if (sAHeld && !wasPressed) {
        nav::pressedA(l.x, l.z, rdf(luigi + kFacing), rdf(luigi + kFacing + 8), !cleared);
        for (const Target& t : targets)
            if (t.kind == Kind::Chest && flatDistance(t.pos, l) < kChestPress) nav::noteVisited(t.id);
    }
    const bool walking = nav::drive(rdf(kCamera + kCamViewMtx), rdf(kCamera + kCamViewMtx + 8), button, stickX, stickY);

    static const bool sLog = std::getenv("LM_ACCESS_WORLD_LOG") != nullptr;
    if (sLog) {
        static u32 sN = 0;
        if (sN++ % 60 == 0)
            std::printf("[world] medal %.0f, spirit %.0f\n", medal ? flatDistance(medal->pos, l) : -1.0f, spirit ? flatDistance(spirit->pos, l) : -1.0f),
            std::printf("[world] luigi %.0f,%.0f,%.0f room %u: %zu targets, item %.0f, door %.0f, ghost %.0f, banana %.0f, thrown %.0f, npc %.0f, boss %.0f, tones %u\n", l.x, l.y, l.z, room,
                        targets.size(), item ? flatDistance(item->pos, l) : -1.0f, door ? flatDistance(door->pos, l) : -1.0f, ghost ? flatDistance(ghost->pos, l) : -1.0f, banana ? flatDistance(banana->pos, l) : -1.0f, thrown ? flatDistance(thrown->pos, l) : -1.0f, npc ? flatDistance(npc->pos, l) : -1.0f, bossTarget ? flatDistance(bossTarget->pos, l) : -1.0f, sPlayed);
    }
    if (set.objectSounds) {
        pulse(sItem, item, l, 8, 45);
        pulse(sDoor, door, l, 8, 45);
        pulse(sToad, toad, l, 8, 45);
        pulse(sNpc, npc, l, 8, 45);
        pulse(sBanana, banana, l, 5, 30);  // a hazard: stays on however close Luigi gets
        Target medalTone, spiritTone;
        if (medal) (medalTone = *medal).kind = Kind::Medal;
        if (spirit) spiritTone = *spirit;
        pulse(sMedal, medal ? &medalTone : nullptr, l, 6, 40);
        pulse(sSpirit, spirit ? &spiritTone : nullptr, l, 4, 30);  // they drift
    }
    // A key that goes from beside Luigi is one he took: nothing else removes them.
    {
        static u32 sKey = 0, sKeyRoom = ~0u;
        u32 near = 0;
        for (const Target& t : targets)
            if (t.kind == Kind::Key && flatDistance(t.pos, l) < kKeyTaken) near = t.id;
        bool here = false;
        for (const Target& t : targets) here |= t.kind == Kind::Key && t.id == sKey;
        if (sKey != 0 && !here && room == sKeyRoom) {
            const u32 rooms = rd32(kRoomTable);
            keyCollected(gu::inMem1(rooms) ? rd8(rooms + room * 0x30 + 3) : room);
            log("[access] key taken in room %u", room);
        }
        sKey = near;
        sKeyRoom = room;
    }
    if (set.announcePlatforms) noticePlatform(l, stickX, stickY, walking, nameIndex, rd32(luigi + kFishedGhost) != 0);
    // A fake door looks like any other, and hurts.
    static u32 sFakeSaid = 0;
    if (sFakeDoorNear && sFakeDoorNear != sFakeSaid) say("Fake door here. Do not open it.", false);
    if (sFakeDoorNear) sFakeSaid = sFakeDoorNear;
    // The heart on a ghost: the moment it can be vacuumed, repeated while it stays.
    static bool sHeartWas = false;
    static int sHeartBeat = 0;
    if (sHeartNow && !sHeartWas) {
        log("[access] a ghost's heart shows at %llu ms", static_cast<unsigned long long>(SDL_GetTicks()));
        sHeartBeat = 0;
    }
    // Each stunned ghost by name, once: its heart hangs over it, so the nearest ghost to that is it.
    {
        static std::set<u32> sStunned;
        std::set<u32> now;
        bool first = true;
        for (const auto& [id, at] : sHearts) {
            now.insert(id);
            if (sStunned.count(id) || !set.cueGhostHeart) continue;
            const Target* who = nullptr;
            for (const Target& t : targets)
                if ((t.kind == Kind::Ghost || t.kind == Kind::Boss) && (!who || flatDistance(t.pos, at) < flatDistance(who->pos, at))) who = &t;
            const std::string name = !who ? "Ghost" : who->kind == Kind::Boss ? bossWord(who->type) : ghostWord(who->type);
            say(name + " stunned. Vacuum now.", first);
            first = false;
        }
        sStunned = std::move(now);
    }
    if (!sHeartNow && sHeartWas) log("[access] the heart has gone at %llu ms", static_cast<unsigned long long>(SDL_GetTicks()));
    sHeartWas = sHeartNow;
    if ((sHeartNow || open) && set.cueGhostHeart && --sHeartBeat <= 0) {
        playTone(sCombat, Kind::GhostHeart, 0.0f, 1.0f);
        sHeartBeat = 12;
    }
    if (set.bossSpeech) bossSpeech(targets, l, cleared);
    if (set.roomHints) shiversSpeech(room);
    {
        static const bool sDoorLog = std::getenv("LM_ACCESS_SHIVERS_LOG") != nullptr;
        static u32 sDone = ~0u;
        if (sDoorLog && room != sDone) {
            sDone = room;
            for (const Target& t : targets)
                if (t.kind == Kind::Door) std::printf("[doors] room %u index %u: door at %.0f,%.0f,%.0f leads to %u (%s), map bits %u\n", room, nameIndex, t.pos.x, t.pos.y, t.pos.z, t.leadsTo, doorName(t.leadsTo).c_str(), rd16(kMapRoomBits + t.leadsTo * 2));
            for (const Stand& s : stands) std::printf("[doors] candle stand at %.0f,%.0f: %d flames, %d lit, %d out\n", s.pos.x, s.pos.z, s.flames, s.lit, s.out);
        }
    }
    // Test aid LM_ACCESS_TEST_THROW=<reads>: presses the left trigger once a ball has been on the nozzle that long.
    {
        static const int sTestThrow = std::getenv("LM_ACCESS_TEST_THROW") ? std::atoi(std::getenv("LM_ACCESS_TEST_THROW")) : 0;
        static int sHeldReads = 0;
        const bool held = rd32(luigi + 0x10EC) != 0;
        sHeldReads = held ? sHeldReads + 1 : 0;
        if (sTestThrow > 0 && sHeldReads > sTestThrow) {
            button |= PAD_TRIGGER_L;
            triggerLeft = 255;
        }
    }
    // The moment something is truly on the nozzle, which the game's own slots show.
    {
        static bool sWasOn = false;
        const bool on = rd32(luigi + 0x10EC) != 0;
        if (on && !sWasOn && lockBoss) say("Got it. Left trigger throws at " + std::string(bossWord(lockBoss->type)) + ".");
        sWasOn = on;
    }
    // The Nursery's ball needs no lock until Chauncey is up and fighting.
    holdObject(luigi, l, lockBoss, targets, rd32(kCurMap) == kMansionMap && nameIndex == kNursery && !lockBoss, button, triggerLeft, triggerRight);
    steerShot(lockBoss, targets);
    if (!walking) lockAim(luigi, l, lockBoss, open, targets, button, triggerRight, stickX, stickY, substickX, substickY);
    if (rd32(kCurMap) != kMansionMap) sBooManager = 0;  // it goes with the map
    {
        static Target sBooSpot;
        const bool hiding = watchBoos(nameIndex, cleared && !blackout(), sBooSpot);
        if (set.objectSounds && set.cueBoo) pulse(sBoo, hiding ? &sBooSpot : nullptr, l, 4, 30);
    }
    if (set.ghostSounds) {  // they move: keep up with them
        Target aimed;
        if (ghost) {
            const float fx = rdf(luigi + kFacing), fz = rdf(luigi + kFacing + 8), facing = std::hypot(fx, fz);
            const float dx = ghost->pos.x - l.x, dz = ghost->pos.z - l.z, dist = std::max(std::hypot(dx, dz), 1.0f);
            aimed = *ghost;
            if (facing > 1e-3f && (fx * dx + fz * dz) / (facing * dist) > kAimCosine && dist < kAimReach) aimed.kind = Kind::GhostAimed;
            if (sLog) {
                static u32 sReads = 0;
                if (sReads++ % 15 == 0)
                    std::printf("[world] ghost at %llu ms: dist %.0f, cos %.2f, %s, tones %u\n", static_cast<unsigned long long>(SDL_GetTicks()), dist, (fx * dx + fz * dz) / std::max(facing * dist, 1e-3f),
                                aimed.kind == Kind::GhostAimed ? "aimed" : "not aimed", sPlayed);
            }
        }
        pulse(sGhost, ghost && cueOn(aimed.kind) ? &aimed : nullptr, l, 3, 30);
        pulse(sThrown, thrown, l, 2, 20);  // the fastest cue: these come at Luigi
        pulse(sBoss, bossTarget, l, 3, 25);
    }
    if (!set.autoInteract || walking) return;
    // Chests and keys open with A when close. A door only when Luigi walks at it, so walking past
    // one never leaves the room. Everything else is collected by touching it.
    u32 press = 0;
    for (const Target& t : targets) {
        const float dist = flatDistance(t.pos, l);
        if ((t.kind == Kind::Chest && dist < kChestPress) || (t.kind == Kind::Key && dist < 60.0f)) press = t.id;
        if (t.kind == Kind::Door && !t.open && dist < kDoorPress && (stickX != 0 || stickY != 0)) {
            const Vec right{rdf(kCamera + kCamViewMtx), 0.0f, rdf(kCamera + kCamViewMtx + 8)};
            const Vec fwd{right.z, 0.0f, -right.x};  // stick up, on the floor
            const float sx = float(stickX), sy = float(stickY), len = std::hypot(sx, sy);
            const Vec go{(right.x * sx + fwd.x * sy) / len, 0.0f, (right.z * sx + fwd.z * sy) / len};
            const float dot = ((t.pos.x - l.x) * go.x + (t.pos.z - l.z) * go.z) / std::max(dist, 1.0f);
            if (dot > 0.7f) press = t.id;
        }
    }
    if (press) {
        pressA(button, press);
        if (sLog) std::printf("[world] auto interact, luigi %.0f,%.0f room %u\n", l.x, l.z, room);
    }
}

// An object is leaving the nozzle: it flies along the direction the game is about to read, so that is
// pointed at the boss, height and all. The throw then needs no aim, and the steering follows a boss
// who moves. The game writes the nozzle's direction afresh next frame.
void noteStrategy(u32 strategy) {
    if (!active() || !gu::inMem1(strategy)) return;
    const u32 thought = rd32(strategy + 4);
    const u32 actor = gu::inMem1(thought) ? rd32(thought + kAiActor) : 0;
    if (!gu::inMem1(actor) || !isBoss(rd32(actor + kActType))) return;
    if (sBossThoughts.size() > 16) sBossThoughts.clear();
    sBossThoughts[actor] = thought;
}

void aimThrow(u32 strategy) {
    if (std::getenv("LM_ACCESS_WORLD_LOG")) std::printf("[world] object leaving the nozzle, strategy %08X, boss known %d\n", strategy, sLockBossSet);
    if (!active() || !port::settings().accessibility.bossLock || !sLockBossSet || port::usCodeFeature() != port::Avail::Yes) return;
    const u32 luigi = rd32(kActorSlots);
    const u32 thought = gu::inMem1(strategy) ? rd32(strategy + 4) : 0;
    const u32 ball = gu::inMem1(thought) ? rd32(thought + kAiActor) : 0;
    if (!gu::inMem1(luigi) || !gu::inMem1(ball) || rd32(thought + kAiPlayer) != 0) return;
    const Vec from = rdVec(ball + kActPos);
    const float dx = sLockBoss.x - from.x, dy = sLockBoss.y - from.y, dz = sLockBoss.z - from.z;
    const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (d < 1.0f) return;
    wr32(luigi + kNozzleDir, std::bit_cast<u32>(dx / d));
    wr32(luigi + kNozzleDir + 4, std::bit_cast<u32>(dy / d));
    wr32(luigi + kNozzleDir + 8, std::bit_cast<u32>(dz / d));
    sShotId = ball;
    sShotAt = std::chrono::steady_clock::now();
    log("[access] throw locked on the boss, %.0f away, type %u", d, rd32(ball + kActType));
}

bool hasMedal(u32 element) {
    for (const auto& m : kMedals)
        if (m.element == element) return flagOn(m.flag);
    return false;
}

// Only for the room Luigi is in now: the flag is worked out on the input thread, and may still be
// the last room's just after he walks through a door.
bool candlesUnlit() {
    const u32 luigi = rd32(kActorSlots);
    return sCandlesOut && gu::inMem1(luigi) && sCandlesRoom == rd32(luigi + kActRoom) >> 24;
}
void noteBooManager(std::uint32_t manager) { sBooManager = manager; }

}  // namespace recomp::access
