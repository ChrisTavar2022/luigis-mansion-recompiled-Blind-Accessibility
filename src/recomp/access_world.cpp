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
constexpr u32 kTypeBanana = 168;
constexpr u32 kRoomTable = 0x804D8170;
constexpr u32 kTypeTable = 0x8033B590, kTypeSize = 0x1C, kTypeFlags = 0x08, kTypeFlagGhost = 8, kTypeCount = 0x29C8 / 0x1C;
constexpr u32 kCamera = 0x803A3820, kCamViewMtx = 0x18C;
constexpr u32 kEvent = 0x803D2D40, kEventCursor = 0x0C;
constexpr u32 kMenuOff = 0x804D8088;
constexpr u32 kTrainingMap = 3;
constexpr u32 kSearchView = 0x804D8160;  // byte: the Game Boy Horror's search view is up
constexpr u32 kWindowState = 0x803CF4C8;  // + slot * 4: a message window
constexpr float kFocusCone = 0.10f;  // radians off the middle of the view, about 6 degrees
constexpr float kFocusHeightWeight = 0.05f;
constexpr float kFocusTilt = 0.30f;  // radians of height off the view before it is said, about 17 degrees
constexpr int kFocusSettle = 6;  // input reads on one object before it is named, so a sweep stays quiet
constexpr u32 kCurMap = 0x804D80C8, kMapTable = 0x802FF218, kMapDoors = 0x18;
constexpr u32 kDoorSize = 0x1C, kDoorX = 0x08, kDoorY = 0x0C, kDoorZ = 0x10, kDoorRoomA = 0x1A, kDoorRoomB = 0x1B;
constexpr u32 kDoorMax = 128, kAuditRooms = 74;

enum class Kind { Chest, Key, Heart, Treasure, Money, Door, Ghost, Banana, Toad, GhostAimed };  // as access::Cue

bool kindOf(u32 type, Kind& k) {
    if (type == 122 || (type >= 373 && type <= 377)) k = Kind::Chest;
    else if (type >= 86 && type <= 91) k = Kind::Key;
    else if (type == 205 || type == 206) k = Kind::Heart;
    else if (type >= 194 && type <= 203) k = Kind::Treasure;  // pearls, gems, diamonds, gold
    else if (type == 42 || type == 43) k = Kind::Toad;
    else return false;
    return true;
}

bool isGhost(u32 type) { return type < kTypeCount && (rd16(kTypeTable + type * kTypeSize + kTypeFlags) & kTypeFlagGhost); }

struct Vec {
    float x, y, z;
};
struct Target {
    Kind kind;
    Vec pos;
    u32 id;  // actor address, or door number + 1 in the high half
    u32 leadsTo = 0;  // a door: the room on its other side
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

std::vector<Target> scan(u32 luigi) {
    std::vector<Target> out;
    const u32 room = rd32(luigi + kActRoom) >> 24;
    const float luigiY = rdf(luigi + kActPos + 4);
    for (u32 i = 1; i < kActorSlotCount; i++) {
        const u32 a = rd32(kActorSlots + 4 * i);
        if (!gu::inMem1(a) || rd32(a + kActRoom) >> 24 != room) continue;
        const u32 type = rd32(a + kActType);
        const Vec p = rdVec(a + kActPos);
        if (p.x < -20000.0f) continue;  // parked: not in play
        // A peel in a ghost's hand is not on the floor yet: only one near Luigi's height counts.
        if (type == kTypeBanana) {
            if (!(rd32(a + kActFlags) & kActHidden) && std::abs(p.y - luigiY) < 60.0f) out.push_back({Kind::Banana, p, a});
            continue;
        }
        Kind k;
        // A ghost counts only while it shows: not hidden in furniture, not faded out.
        if (isGhost(type) && !(rd32(a + kActFlags) & kActHidden) && rd8(a + kActAlpha) != 0) out.push_back({Kind::Ghost, p, a});
        else if (kindOf(type, k)) out.push_back({k, p, a});
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
            if (a == room || b == room) out.push_back({Kind::Door, p, (i + 1) << 16, a == room ? b : a});
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
                         : t.kind == Kind::Treasure ? "Treasure" : t.kind == Kind::Toad ? "Toad" : t.kind == Kind::Banana ? "Banana peel" : t.kind == Kind::Door ? "Door" : nullptr;
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
Channel sItem, sDoor, sGhost, sBanana, sToad;
u32 sPlayed = 0;
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
    const float volume = 0.3f * port::settings().audio.masterVolume;
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
    }
    return true;
}

// Luigi can stop 113 from a chest's centre and 95 from a door's: they are solid.
constexpr float kDoorPress = 105.0f, kChestPress = 125.0f, kToadPress = 110.0f;
constexpr float kSolidReach = 140.0f;  // silent from here: Luigi has arrived
constexpr int kMaxAutoPresses = 3;  // per object: an opened chest stays where it is
bool solid(Kind k) { return k == Kind::Chest || k == Kind::Door || k == Kind::Toad; }
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

void playCue(Cue cue) { playTone(sItem, Kind(int(cue)), 0.0f, 1.0f); }

void noteMoney(float x, float y, float z) {
    if (!active()) return;
    std::lock_guard lk(sMoneyM);
    if (sMoney.size() < 256) sMoney.push_back({x, y, z});
}

void worldFrame(std::uint16_t& button, std::int8_t& stickX, std::int8_t& stickY) {
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
    searchView(luigi);
    // The training room is one long event script, with Luigi free through it.
    const bool scripted = rd32(kCurMap) != kTrainingMap && (rd32(kEvent + kEventCursor) != 0 || rd32(kMenuOff) != 0);
    if (scripted || gamePaused()) {  // Luigi is not free to move
        nav::cancel("event or pause");
        return;
    }

    const u32 room = rd32(luigi + kActRoom) >> 24;
    if (room != sLastRoom) {
        sLastRoom = room;
        sReached.clear();
        sAutoPresses.clear();
    }
    const Vec l = rdVec(luigi + kActPos);
    const Target* item = nullptr;
    const Target* door = nullptr;
    const Target* ghost = nullptr;
    const Target* banana = nullptr;
    const Target* toad = nullptr;
    const std::vector<Target> targets = scan(luigi);
    for (const Target& t : targets) {
        const float dist = flatDistance(t.pos, l);
        if (t.kind != Kind::Ghost && !cueOn(t.kind)) continue;  // the ghost's own cue is chosen below
        if (t.kind != Kind::Ghost && t.kind != Kind::Banana) {
            if (t.id && dist < (solid(t.kind) ? kSolidReach : 70.0f)) sReached.insert(t.id);
            if (t.id && sReached.count(t.id)) continue;
        }
        const Target*& best = t.kind == Kind::Door ? door : t.kind == Kind::Ghost ? ghost : t.kind == Kind::Banana ? banana : t.kind == Kind::Toad ? toad : item;
        if (!best || dist < flatDistance(best->pos, l)) best = &t;
    }
    // What the Go to menu offers, and the walk in progress.
    std::vector<nav::Object> objects;
    for (const Target& t : targets) {
        nav::Category category;
        switch (t.kind) {
        case Kind::Door: category = nav::Category::Doors; break;
        case Kind::Money: category = nav::Category::Money; break;
        case Kind::Heart: category = nav::Category::Hearts; break;
        case Kind::Key: category = nav::Category::Keys; break;
        case Kind::Chest: category = nav::Category::Chests; break;
        case Kind::Treasure: category = nav::Category::Treasure; break;
        case Kind::Toad: category = nav::Category::Toads; break;
        default: continue;
        }
        objects.push_back({category, t.pos.x, t.pos.y, t.pos.z, t.id, t.kind == Kind::Door ? kDoorPress : t.kind == Kind::Chest ? kChestPress : t.kind == Kind::Toad ? kToadPress : 0.0f, t.kind == Kind::Door ? doorName(t.leadsTo) : std::string()});
    }
    float box[4] = {-1e9f, -1e9f, 1e9f, 1e9f};
    roomBox(room, box);
    auditRooms();
    nav::publish(objects, l.x, l.y, l.z, rd32(kCurMap), room, box[0], box[1], box[2], box[3]);
    const bool wasPressed = sAHeld;
    sAHeld = (button & PAD_BUTTON_A) != 0;
    if (sAHeld && !wasPressed) nav::pressedA(l.x, l.z, rdf(luigi + kFacing), rdf(luigi + kFacing + 8));
    const bool walking = nav::drive(rdf(kCamera + kCamViewMtx), rdf(kCamera + kCamViewMtx + 8), button, stickX, stickY);

    static const bool sLog = std::getenv("LM_ACCESS_WORLD_LOG") != nullptr;
    if (sLog) {
        static u32 sN = 0;
        if (sN++ % 60 == 0)
            std::printf("[world] luigi %.0f,%.0f,%.0f room %u: %zu targets, item %.0f, door %.0f, ghost %.0f, banana %.0f, tones %u\n", l.x, l.y, l.z, room,
                        targets.size(), item ? flatDistance(item->pos, l) : -1.0f, door ? flatDistance(door->pos, l) : -1.0f, ghost ? flatDistance(ghost->pos, l) : -1.0f, banana ? flatDistance(banana->pos, l) : -1.0f, sPlayed);
    }
    if (set.objectSounds) {
        pulse(sItem, item, l, 8, 45);
        pulse(sDoor, door, l, 8, 45);
        pulse(sToad, toad, l, 8, 45);
        pulse(sBanana, banana, l, 5, 30);  // a hazard: stays on however close Luigi gets
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
    }
    if (!set.autoInteract || walking) return;
    // Chests and keys open with A when close. A door only when Luigi walks at it, so walking past
    // one never leaves the room. Everything else is collected by touching it.
    u32 press = 0;
    for (const Target& t : targets) {
        const float dist = flatDistance(t.pos, l);
        if ((t.kind == Kind::Chest && dist < kChestPress) || (t.kind == Kind::Key && dist < 60.0f)) press = t.id;
        if (t.kind == Kind::Door && dist < kDoorPress && (stickX != 0 || stickY != 0)) {
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

}  // namespace recomp::access
