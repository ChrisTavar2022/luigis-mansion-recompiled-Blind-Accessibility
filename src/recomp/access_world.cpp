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
// the floor get a fourth, under the object sounds setting, with a clashing warning tone.
//
// The tones have their own audio streams on purpose. Starting the game's own sounds from here takes
// sounds from JAudio's fixed lists; in a busy moment (coins bouncing) the game's next start then
// finds a list empty and writes through a null pointer (fn_8018FF88), which crashed the game.
//
// Test aid: LM_ACCESS_WORLD_LOG=1 logs the targets.
#include "recomp/access.h"

#include "port/install.h"
#include "port/settings.h"
#include "recomp/guest_util.h"
#include "recomp/hle.h"

#include <dolphin/pad.h>
#include <SDL3/SDL.h>

#include <algorithm>
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
constexpr u32 kActType = 0x3C, kActFlags = 0x40, kActPos = 0x44, kActRoom = 0xB4, kActAlpha = 0xE3;
constexpr u32 kActHidden = 0x6100;  // +0x40 bits the game's ghost draw pass skips
constexpr u32 kTypeBanana = 168;
constexpr u32 kTypeTable = 0x8033B590, kTypeSize = 0x1C, kTypeFlags = 0x08, kTypeFlagGhost = 8, kTypeCount = 0x29C8 / 0x1C;
constexpr u32 kCamera = 0x803A3820, kCamViewMtx = 0x18C;
constexpr u32 kEvent = 0x803D2D40, kEventCursor = 0x0C;
constexpr u32 kMenuOff = 0x804D8088;
constexpr u32 kCurMap = 0x804D80C8, kMapTable = 0x802FF218, kMapDoors = 0x18;
constexpr u32 kDoorSize = 0x1C, kDoorX = 0x08, kDoorY = 0x0C, kDoorZ = 0x10, kDoorRoomA = 0x1A, kDoorRoomB = 0x1B;
constexpr u32 kDoorMax = 128;

enum class Kind { Chest, Key, Heart, Treasure, Money, Door, Ghost, Banana };

bool kindOf(u32 type, Kind& k) {
    if (type == 122 || (type >= 373 && type <= 377)) k = Kind::Chest;
    else if (type >= 86 && type <= 91) k = Kind::Key;
    else if (type == 205 || type == 206) k = Kind::Heart;
    else if (type >= 194 && type <= 203) k = Kind::Treasure;  // pearls, gems, diamonds, gold
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
};

float rdf(u32 a) { return gu::rdf(a); }
Vec rdVec(u32 a) { return {rdf(a), rdf(a + 4), rdf(a + 8)}; }
float flatDistance(const Vec& a, const Vec& b) { return std::hypot(a.x - b.x, a.z - b.z); }

// Spilled coins and bills are not actors: their update hooks report them here.
std::mutex sMoneyM;
std::vector<Vec> sMoney;

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
        for (const Vec& p : sMoney) out.push_back({Kind::Money, p, 0});
        sMoney.clear();
    }
    const u32 map = rd32(kMapTable + 4 * rd32(kCurMap));
    const u32 doors = gu::inMem1(map) ? rd32(map + kMapDoors) : 0;
    if (gu::inMem1(doors)) {
        for (u32 i = 0; i < kDoorMax; i++) {
            const u32 d = doors + i * kDoorSize;
            if (rd8(d) == 0) break;
            const Vec p{float(s32(rd32(d + kDoorX))), float(s32(rd32(d + kDoorY))), float(s32(rd32(d + kDoorZ)))};
            if (rd8(d + kDoorRoomA) == room || rd8(d + kDoorRoomB) == room) out.push_back({Kind::Door, p, (i + 1) << 16});
        }
    }
    return out;
}

// Guest memory for one sound: the game reads the position while it plays and keeps its handle.
// A channel's pulses play on its own stream, so an item and a door can sound together.
struct Channel {
    int wait = 0;
    SDL_AudioStream* stream = nullptr;
};
Channel sItem, sDoor, sGhost, sBanana;
u32 sPlayed = 0;

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
    case Kind::Ghost: return {311.0f, 233.0f, 210};
    case Kind::Banana: return {1480.0f, 1397.0f, 90};  // a clash: something to avoid
    default: return {196.0f, 0.0f, 110};  // door
    }
}

// pan: -1 left to 1 right. pitch scales the tone.
void playTone(Channel& ch, Kind kind, float pan, float pitch) {
    if (!ch.stream) {
        const SDL_AudioSpec spec{SDL_AUDIO_F32, 2, kToneRate};
        ch.stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (!ch.stream) return;
        SDL_ResumeAudioStreamDevice(ch.stream);
    }
    if (SDL_GetAudioStreamQueued(ch.stream) > 1024) return;  // still playing: the resampler keeps a small tail
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
}
// Luigi stops this far from the centre of a chest or a door: they are solid.
constexpr float kSolidReach = 140.0f;
constexpr int kMaxAutoPresses = 3;  // per object: an opened chest stays where it is
bool solid(Kind k) { return k == Kind::Chest || k == Kind::Door; }
std::map<u32, int> sAutoPresses;

std::set<u32> sReached;  // objects Luigi got to: silent until he changes room
u32 sLastRoom = ~0u;

void playFor(Channel& ch, const Target& t, const Vec& luigi) {
    // The camera's right axis (first row of its view matrix) and forward on the floor.
    const Vec right{rdf(kCamera + kCamViewMtx), rdf(kCamera + kCamViewMtx + 4), rdf(kCamera + kCamViewMtx + 8)};
    const float dx = t.pos.x - luigi.x, dz = t.pos.z - luigi.z;
    const float side = dx * right.x + dz * right.z;
    const float ahead = dx * right.z - dz * right.x;  // stick up is forward
    const float dist = std::max(std::hypot(dx, dz), 1.0f);
    playTone(ch, t.kind, std::clamp(side / dist * 1.4f, -1.0f, 1.0f), ahead < -dist * 0.2f ? 0.7f : 1.0f);
}

void pulse(Channel& ch, const Target* t, const Vec& luigi, int fastest, int slowest) {
    if (!t) {
        ch.wait = 0;
        return;
    }
    if (ch.wait > 0) {
        ch.wait--;
        return;
    }
    playFor(ch, *t, luigi);
    ch.wait = std::clamp(int(flatDistance(t->pos, luigi) / 25.0f), fastest, slowest);
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

void noteMoney(float x, float y, float z) {
    if (!active()) return;
    std::lock_guard lk(sMoneyM);
    if (sMoney.size() < 256) sMoney.push_back({x, y, z});
}

void worldFrame(std::uint16_t& button, std::int8_t stickX, std::int8_t stickY) {
    const auto& set = port::settings().accessibility;
    if (!active() || port::usCodeFeature() != port::Avail::Yes || (!set.objectSounds && !set.ghostSounds && !set.autoInteract)) return;
    const u32 luigi = rd32(kActorSlots);
    if (!gu::inMem1(luigi)) return;
    if (rd32(kEvent + kEventCursor) != 0 || rd32(kMenuOff) != 0 || gamePaused()) return;  // Luigi is not free to move

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
    const std::vector<Target> targets = scan(luigi);
    for (const Target& t : targets) {
        const float dist = flatDistance(t.pos, l);
        if (t.kind != Kind::Ghost && t.kind != Kind::Banana) {
            if (t.id && dist < (solid(t.kind) ? kSolidReach : 70.0f)) sReached.insert(t.id);
            if (t.id && sReached.count(t.id)) continue;
        }
        const Target*& best = t.kind == Kind::Door ? door : t.kind == Kind::Ghost ? ghost : t.kind == Kind::Banana ? banana : item;
        if (!best || dist < flatDistance(best->pos, l)) best = &t;
    }
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
        pulse(sBanana, banana, l, 5, 30);  // a hazard: stays on however close Luigi gets
    }
    if (set.ghostSounds) pulse(sGhost, ghost, l, 6, 30);  // they move: keep up with them
    if (!set.autoInteract) return;
    // Chests and keys open with A when close. A door only when Luigi walks at it, so walking past
    // one never leaves the room. Everything else is collected by touching it.
    u32 press = 0;
    for (const Target& t : targets) {
        const float dist = flatDistance(t.pos, l);
        if ((t.kind == Kind::Chest || t.kind == Kind::Key) && dist < kSolidReach) press = t.id;
        if (t.kind == Kind::Door && dist < kSolidReach && (stickX != 0 || stickY != 0)) {
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
