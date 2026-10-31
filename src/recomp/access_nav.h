// Screen reader support: the Go to menu and Luigi's automatic walking (src/recomp/access_nav.cpp).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace recomp::access::nav {

enum class Category { Doors, Money, Hearts, Keys, Chests, Treasure, Toads, Furniture, Count };

struct Object {
    Category category;
    float x, y, z;
    std::uint32_t id;  // actor or door id, 0 for spilled money
    float reach;       // above 0: Luigi stops this far away and presses A
    std::string name;  // empty: the category's word
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
// The Game Boy Horror's search view: the furniture of Luigi's room closest to the middle of the view.
// angle starts as the widest allowed and comes back as how far off centre it is, with its size taken
// off, counted left and right only. height comes back as the angle up to it. Empty: nothing nearer
// the middle than angle.
std::string aimedFurniture(const float eye[3], const float dir[3], float& angle, float& height, float heightWeight);
// The player pressed A: says what furniture Luigi is facing, if any.
void pressedA(float x, float z, float facingX, float facingZ);

// Test aid LM_ACCESS_NAV_AUDIT: plots between the doors of each room and logs the failures.
struct AuditDoor {
    float x, y, z;
    std::uint32_t leadsTo;
};
struct AuditRoom {
    std::uint32_t number;
    float minX, minZ, maxX, maxZ;
    std::vector<AuditDoor> doors;
};
void audit(int map, std::vector<AuditRoom> rooms);

void toggleMenu();
bool menuOpen();
void drawMenu();  // once per frame, inside the ImGui frame

}  // namespace recomp::access::nav
