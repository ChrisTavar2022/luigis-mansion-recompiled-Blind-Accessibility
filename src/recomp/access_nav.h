// Screen reader support: the Go to menu and Luigi's automatic walking (src/recomp/access_nav.cpp).
#pragma once

#include <cstdint>
#include <vector>

namespace recomp::access::nav {

enum class Category { Doors, Money, Hearts, Keys, Chests, Treasure, Toads, Count };

struct Object {
    Category category;
    float x, y, z;
    std::uint32_t id;  // actor or door id, 0 for spilled money
    float reach;       // above 0: Luigi stops this far away and presses A
};

// From access_world.cpp each input read, while Luigi is free to move.
// minX..maxZ: Luigi's room. Routes stay inside it.
void publish(const std::vector<Object>& objects, float x, float y, float z, std::uint32_t map, std::uint32_t room, float minX, float minZ,
             float maxX, float maxZ);
// Steers Luigi while he walks. rightX / rightZ: the camera's right axis. True while walking; the
// player moving the stick stops it.
bool drive(float rightX, float rightZ, std::uint16_t& button, std::int8_t& stickX, std::int8_t& stickY);
void cancel(const char* reason);  // an event started, or similar
void doorLocked();                // the door Luigi tried is locked: the walk ends

void toggleMenu();
bool menuOpen();
void drawMenu();  // once per frame, inside the ImGui frame

}  // namespace recomp::access::nav
