// Screen reader support: the Go to menu and Luigi's automatic walking
// (docs/accessibility/README.md, "Go to menu"). USA game only for now.
//
// Pressing the left stick in opens a menu of what access_world.cpp found in Luigi's room. Picking
// an object plots a route with the map's own collision and steers Luigi along it by replacing the
// stick input, so he moves the way a player would move him. Doors and chests get an A press on
// arrival; everything else is collected by touch. Moving the stick takes control back.
//
// Collision (col.mp in /Map/mapN.szp): big endian. 0x24 vertices (3 floats), 0x28 normals (3
// floats), 0x2C triangles (0x18 bytes: u16 vertices a, b, c and normal), 0x30 what follows them.
// Static furniture is in it. Moved furniture, chests and ghosts are not: when Luigi stops making
// progress the spot ahead of him is marked blocked and the route is plotted again.
//
// A target on Luigi's level gets a flat route at his height. One on another level gets a route over
// the floors themselves, each cell holding one node per floor above it: stairs are real steps about
// 30 units high, walls to the flat route, and walkable here.
//
// Test aid: LM_ACCESS_WORLD_LOG=1 logs routes and progress.
#include "recomp/access_nav.h"

#include "recomp/access.h"
#include "recomp/hle.h"
#include "recomp/menu.h"

#include <dolphin/pad.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <queue>
#include <string>

namespace recomp::unused2 {
std::vector<u8> readDiscItem(const char* disc, const char* inner);  // gallery.cpp
}

namespace recomp::access::nav {
namespace {

// The route grid: a square around Luigi at his floor height.
constexpr int kCells = 240;
constexpr float kCell = 20.0f;
constexpr float kBodyLow = 15.0f, kBodyHigh = 110.0f;  // heights above his feet that block him
constexpr float kBodyRadius = 45.0f;
constexpr float kWaypointReach = 30.0f, kTouchReach = 28.0f;
constexpr float kRoomMargin = 110.0f;
constexpr float kPlanMargin = 110.0f;  // a route may end this much beyond the press distance
constexpr float kItemAllowed = 90.0f;  // an item this close to a reachable spot can be touched or vacuumed
constexpr int kStuckReads = 40;   // input reads without progress
constexpr int kMaxReplans = 4;
constexpr int kArrivedReads = 12;     // stopped this long near a door, chest or Toad: he has arrived
constexpr float kArrivedMargin = 60.0f;
constexpr int kMaxPresses = 3;
constexpr int kDoorWaitReads = 1800;  // a door takes about 13 seconds to open, at 60 reads a second
constexpr s8 kMoveDeadZone = 40;  // the player's stick above this takes over
constexpr int kLayers = 6;            // floors stacked over one cell
constexpr float kStepUp = 45.0f;      // a stair step: lower than this does not block him
constexpr float kLevelGap = 200.0f;   // more height than this is another level; a door's y is above its floor
constexpr float kStartHeight = 60.0f;  // the floor Luigi stands on is this close to him

bool logOn() {
    static const bool sLog = std::getenv("LM_ACCESS_WORLD_LOG") != nullptr;
    return sLog;
}

struct Vec2 {
    float x, z;
};
struct Tri {
    std::array<float, 3> a, b, c;
};

std::vector<Tri> sTris;  // steep triangles of the loaded map
std::vector<Tri> sFloorTris;  // floors of the loaded map, stairs included
int sLoadedMap = -1;

u32 be32(const u8* p) { return u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3]; }
u32 be16(const u8* p) { return u32(p[0]) << 8 | p[1]; }
float beFloat(const u8* p) {
    const u32 v = be32(p);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

bool loadCollision(int map) {
    if (map == sLoadedMap) return !sTris.empty();
    sLoadedMap = map;
    sTris.clear();
    sFloorTris.clear();
    const std::string path = "/Map/map" + std::to_string(map) + ".szp";
    const std::vector<u8> d = unused2::readDiscItem(path.c_str(), "col.mp");
    if (logOn()) std::printf("[nav] %s col.mp: %zu bytes\n", path.c_str(), d.size());
    if (d.size() < 0x40) return false;
    const u32 verts = be32(&d[0x24]), normals = be32(&d[0x28]), tris = be32(&d[0x2C]), end = be32(&d[0x30]);
    if (verts >= normals || normals >= tris || tris >= end || end > d.size()) return false;
    const auto vertex = [&](u32 i) {
        const u8* p = &d[verts + i * 12];
        return std::array<float, 3>{beFloat(p), beFloat(p + 4), beFloat(p + 8)};
    };
    const u32 vertexCount = (normals - verts) / 12, normalCount = (tris - normals) / 12;
    for (u32 t = tris; t + 0x18 <= end; t += 0x18) {
        const u32 a = be16(&d[t]), b = be16(&d[t + 2]), c = be16(&d[t + 4]), n = be16(&d[t + 6]);
        if (a >= vertexCount || b >= vertexCount || c >= vertexCount || n >= normalCount) continue;
        const float up = beFloat(&d[normals + n * 12 + 4]);
        if (up > 0.7f) sFloorTris.push_back({vertex(a), vertex(b), vertex(c)});
        if (std::abs(up) > 0.7f) continue;  // floors and ceilings do not block
        sTris.push_back({vertex(a), vertex(b), vertex(c)});
    }
    if (logOn()) std::printf("[nav] %zu blocking triangles\n", sTris.size());
    return !sTris.empty();
}

struct Grid {
    float originX = 0, originZ = 0;
    std::vector<u8> blocked = std::vector<u8>(kCells * kCells, 0);
    u8& at(int x, int z) { return blocked[size_t(z) * kCells + x]; }
    bool inside(int x, int z) const { return x >= 0 && z >= 0 && x < kCells && z < kCells; }
    int cellX(float x) const { return int(std::floor((x - originX) / kCell)); }
    int cellZ(float z) const { return int(std::floor((z - originZ) / kCell)); }
    Vec2 centre(int x, int z) const { return {originX + (float(x) + 0.5f) * kCell, originZ + (float(z) + 0.5f) * kCell}; }
    void mark(float wx, float wz, float radius) {
        const int cx = cellX(wx), cz = cellZ(wz), r = int(std::ceil(radius / kCell));
        for (int z = cz - r; z <= cz + r; z++)
            for (int x = cx - r; x <= cx + r; x++)
                if (inside(x, z) && std::hypot(float(x - cx), float(z - cz)) * kCell <= radius) at(x, z) = 1;
    }
};

// Blocks what Luigi's body would touch at height y, around (x, z).
Grid buildGrid(float x, float y, float z, const std::vector<Vec2>& extra) {
    Grid g;
    g.originX = x - kCells * kCell / 2.0f;
    g.originZ = z - kCells * kCell / 2.0f;
    const float low = y + kBodyLow, high = y + kBodyHigh;
    const float maxX = g.originX + kCells * kCell, maxZ = g.originZ + kCells * kCell;
    std::vector<u8> solid(kCells * kCells, 0);
    for (const Tri& t : sTris) {
        const auto [minY, maxY] = std::minmax({t.a[1], t.b[1], t.c[1]});
        if (maxY < low || minY > high) continue;
        const auto [minTX, maxTX] = std::minmax({t.a[0], t.b[0], t.c[0]});
        const auto [minTZ, maxTZ] = std::minmax({t.a[2], t.b[2], t.c[2]});
        if (maxTX < g.originX || minTX > maxX || maxTZ < g.originZ || minTZ > maxZ) continue;
        const float size = std::max({maxTX - minTX, maxTZ - minTZ, maxY - minY});
        const int steps = std::clamp(int(size / (kCell * 0.5f)) + 1, 1, 400);
        for (int i = 0; i <= steps; i++)
            for (int j = 0; j <= steps - i; j++) {
                const float u = float(i) / float(steps), v = float(j) / float(steps), w = 1.0f - u - v;
                const float py = t.a[1] * w + t.b[1] * u + t.c[1] * v;
                if (py < low || py > high) continue;
                const int cx = g.cellX(t.a[0] * w + t.b[0] * u + t.c[0] * v);
                const int cz = g.cellZ(t.a[2] * w + t.b[2] * u + t.c[2] * v);
                if (g.inside(cx, cz)) solid[size_t(cz) * kCells + cx] = 1;
            }
    }
    for (int cz = 0; cz < kCells; cz++)
        for (int cx = 0; cx < kCells; cx++)
            if (solid[size_t(cz) * kCells + cx]) {
                const Vec2 c = g.centre(cx, cz);
                g.mark(c.x, c.z, kBodyRadius);
            }
    for (const Vec2& p : extra) g.mark(p.x, p.z, kBodyRadius + 15.0f);
    return g;
}

bool nearestFree(Grid& g, int& x, int& z, int range) {
    if (g.inside(x, z) && !g.at(x, z)) return true;
    for (int r = 1; r <= range; r++)
        for (int dz = -r; dz <= r; dz++)
            for (int dx = -r; dx <= r; dx++) {
                if (std::max(std::abs(dx), std::abs(dz)) != r || !g.inside(x + dx, z + dz) || g.at(x + dx, z + dz)) continue;
                x += dx;
                z += dz;
                return true;
            }
    return false;
}

bool lineFree(Grid& g, Vec2 a, Vec2 b) {
    const float dist = std::hypot(b.x - a.x, b.z - a.z);
    const int steps = int(dist / (kCell * 0.5f)) + 1;
    for (int i = 0; i <= steps; i++) {
        const float t = float(i) / float(steps);
        const int x = g.cellX(a.x + (b.x - a.x) * t), z = g.cellZ(a.z + (b.z - a.z) * t);
        if (!g.inside(x, z) || g.at(x, z)) return false;
    }
    return true;
}

// Everywhere Luigi can reach from where he stands, then the reachable cell closest to the target:
// a door or a chest is inside something solid, so the target itself is often blocked, and a nearby
// free cell can be behind a wall. The route is then straightened: a waypoint stays only where a
// straight walk would hit something.
bool plan(Grid& g, Vec2 from, Vec2 to, float allowed, std::vector<Vec2>& out) {
    int sx = g.cellX(from.x), sz = g.cellZ(from.z);
    if (!nearestFree(g, sx, sz, 6)) return false;
    const int tx = g.cellX(to.x), tz = g.cellZ(to.z);
    const int count = kCells * kCells;
    std::vector<float> cost(size_t(count), 1e30f);
    std::vector<int> parent(size_t(count), -1);
    using Node = std::pair<float, int>;
    std::priority_queue<Node, std::vector<Node>, std::greater<Node>> open;
    const auto index = [](int x, int z) { return z * kCells + x; };
    cost[size_t(index(sx, sz))] = 0.0f;
    open.push({0.0f, index(sx, sz)});
    int goal = index(sx, sz);
    float best = std::hypot(float(sx - tx), float(sz - tz));
    while (!open.empty()) {
        const auto [c0, cur] = open.top();
        open.pop();
        if (c0 > cost[size_t(cur)]) continue;
        const int cx = cur % kCells, cz = cur / kCells;
        const float near = std::hypot(float(cx - tx), float(cz - tz));
        if (near < best) {
            best = near;
            goal = cur;
        }
        for (int dz = -1; dz <= 1; dz++)
            for (int dx = -1; dx <= 1; dx++) {
                const int nx = cx + dx, nz = cz + dz;
                if ((dx == 0 && dz == 0) || !g.inside(nx, nz) || g.at(nx, nz)) continue;
                if (dx != 0 && dz != 0 && (g.at(cx + dx, cz) || g.at(cx, cz + dz))) continue;  // no cutting corners
                const float c = c0 + (dx != 0 && dz != 0 ? 1.414f : 1.0f);
                const int n = index(nx, nz);
                if (c >= cost[size_t(n)]) continue;
                cost[size_t(n)] = c;
                parent[size_t(n)] = cur;
                open.push({c, n});
            }
    }
    if (best * kCell > allowed) {
        if (logOn()) std::printf("[nav] closest reachable spot is %.0f from the target\n", best * kCell);
        return false;
    }
    std::vector<Vec2> raw;
    for (int n = goal; n != -1; n = parent[size_t(n)]) raw.push_back(g.centre(n % kCells, n / kCells));
    std::reverse(raw.begin(), raw.end());
    out.clear();
    size_t i = 0;
    while (i + 1 < raw.size()) {
        size_t next = raw.size() - 1;
        while (next > i + 1 && !lineFree(g, raw[i], raw[next])) next--;
        out.push_back(raw[next]);
        i = next;
    }
    if (out.empty()) out.push_back(raw.back());
    return true;
}

// The floors around (x, z): every cell holds the heights of the floors over it, and which of them
// Luigi's body fits on. Only cells inside the given box count.
struct Floors {
    Grid grid;  // only its coordinates
    std::vector<float> height = std::vector<float>(size_t(kCells) * kCells * kLayers, 0.0f);
    std::vector<u8> count = std::vector<u8>(size_t(kCells) * kCells, 0);
    std::vector<u8> blocked = std::vector<u8>(size_t(kCells) * kCells * kLayers, 0);
    static size_t cell(int x, int z) { return size_t(z) * kCells + x; }
    static size_t node(int x, int z, int layer) { return cell(x, z) * kLayers + layer; }
    // The open floor on this cell within a step of height y, or -1.
    int layerNear(int x, int z, float y, float reach) const {
        if (!grid.inside(x, z)) return -1;
        int found = -1;
        float best = reach;
        for (int l = 0; l < count[cell(x, z)]; l++) {
            const float gap = std::abs(height[node(x, z, l)] - y);
            if (gap <= best && !blocked[node(x, z, l)]) {
                best = gap;
                found = l;
            }
        }
        return found;
    }
};

Floors buildFloors(float x, float y, float z, const float box[4], const std::vector<Vec2>& extra) {
    Floors f;
    f.grid.originX = x - kCells * kCell / 2.0f;
    f.grid.originZ = z - kCells * kCell / 2.0f;
    const Grid& g = f.grid;
    for (const Tri& t : sFloorTris) {
        const auto [minTX, maxTX] = std::minmax({t.a[0], t.b[0], t.c[0]});
        const auto [minTZ, maxTZ] = std::minmax({t.a[2], t.b[2], t.c[2]});
        const int x0 = std::max(g.cellX(std::max(minTX, box[0])), 0), x1 = std::min(g.cellX(std::min(maxTX, box[2])), kCells - 1);
        const int z0 = std::max(g.cellZ(std::max(minTZ, box[1])), 0), z1 = std::min(g.cellZ(std::min(maxTZ, box[3])), kCells - 1);
        const float det = (t.b[2] - t.c[2]) * (t.a[0] - t.c[0]) + (t.c[0] - t.b[0]) * (t.a[2] - t.c[2]);
        if (std::abs(det) < 1e-3f) continue;
        for (int cz = z0; cz <= z1; cz++)
            for (int cx = x0; cx <= x1; cx++) {
                const Vec2 c = g.centre(cx, cz);
                const float u = ((t.b[2] - t.c[2]) * (c.x - t.c[0]) + (t.c[0] - t.b[0]) * (c.z - t.c[2])) / det;
                const float v = ((t.c[2] - t.a[2]) * (c.x - t.c[0]) + (t.a[0] - t.c[0]) * (c.z - t.c[2])) / det;
                const float w = 1.0f - u - v;
                if (u < 0.0f || v < 0.0f || w < 0.0f) continue;
                const float h = t.a[1] * u + t.b[1] * v + t.c[1] * w;
                u8& n = f.count[Floors::cell(cx, cz)];
                bool known = false;
                for (int l = 0; l < n; l++) known |= std::abs(f.height[Floors::node(cx, cz, l)] - h) < 5.0f;
                if (!known && n < kLayers) f.height[Floors::node(cx, cz, n++)] = h;
            }
    }
    const int reach = int(std::ceil(kBodyRadius / kCell));
    for (const Tri& t : sTris) {
        const auto [minTX, maxTX] = std::minmax({t.a[0], t.b[0], t.c[0]});
        const auto [minTZ, maxTZ] = std::minmax({t.a[2], t.b[2], t.c[2]});
        const auto [minY, maxY] = std::minmax({t.a[1], t.b[1], t.c[1]});
        if (maxTX < box[0] - kBodyRadius || minTX > box[2] + kBodyRadius || maxTZ < box[1] - kBodyRadius || minTZ > box[3] + kBodyRadius) continue;
        const float size = std::max({maxTX - minTX, maxTZ - minTZ, maxY - minY});
        const int steps = std::clamp(int(size / (kCell * 0.5f)) + 1, 1, 400);
        for (int i = 0; i <= steps; i++)
            for (int j = 0; j <= steps - i; j++) {
                const float u = float(i) / float(steps), v = float(j) / float(steps), w = 1.0f - u - v;
                const float py = t.a[1] * w + t.b[1] * u + t.c[1] * v;
                const int sx = g.cellX(t.a[0] * w + t.b[0] * u + t.c[0] * v), sz = g.cellZ(t.a[2] * w + t.b[2] * u + t.c[2] * v);
                for (int cz = sz - reach; cz <= sz + reach; cz++)
                    for (int cx = sx - reach; cx <= sx + reach; cx++) {
                        if (!g.inside(cx, cz) || std::hypot(float(cx - sx), float(cz - sz)) * kCell > kBodyRadius) continue;
                        for (int l = 0; l < f.count[Floors::cell(cx, cz)]; l++) {
                            const float above = py - f.height[Floors::node(cx, cz, l)];
                            if (above > kStepUp && above < kBodyHigh) f.blocked[Floors::node(cx, cz, l)] = 1;
                        }
                    }
            }
    }
    // Spots he got stuck at are at his height: another floor over the same cell stays open.
    for (const Vec2& p : extra) {
        const int sx = g.cellX(p.x), sz = g.cellZ(p.z), blockReach = reach + 1;
        for (int cz = sz - blockReach; cz <= sz + blockReach; cz++)
            for (int cx = sx - blockReach; cx <= sx + blockReach; cx++) {
                if (!g.inside(cx, cz) || std::hypot(float(cx - sx), float(cz - sz)) * kCell > kBodyRadius + 15.0f) continue;
                for (int l = 0; l < f.count[Floors::cell(cx, cz)]; l++)
                    if (std::abs(f.height[Floors::node(cx, cz, l)] - y) < kLevelGap) f.blocked[Floors::node(cx, cz, l)] = 1;
            }
    }
    return f;
}

// A straight walk from a at height ay stays on open floor, step by step, and ends at height by.
bool floorLineFree(const Floors& f, Vec2 a, float ay, Vec2 b, float by) {
    const float dist = std::hypot(b.x - a.x, b.z - a.z);
    const int steps = int(dist / (kCell * 0.5f)) + 1;
    float y = ay;
    for (int i = 1; i <= steps; i++) {
        const float t = float(i) / float(steps);
        const int x = f.grid.cellX(a.x + (b.x - a.x) * t), z = f.grid.cellZ(a.z + (b.z - a.z) * t);
        const int l = f.layerNear(x, z, y, kStepUp);
        if (l < 0) return false;
        y = f.height[Floors::node(x, z, l)];
    }
    return std::abs(y - by) <= kStepUp;
}

// As plan(), over the floors: the search runs from the floor under Luigi to the reachable node
// closest to the target on the target's level.
bool planFloors(const Floors& f, float fromX, float fromY, float fromZ, float toX, float toY, float toZ, float allowed, std::vector<Vec2>& out) {
    int sx = f.grid.cellX(fromX), sz = f.grid.cellZ(fromZ), sl = f.layerNear(sx, sz, fromY, kStartHeight);
    for (int r = 1; r <= 6 && sl < 0; r++)
        for (int dz = -r; dz <= r && sl < 0; dz++)
            for (int dx = -r; dx <= r && sl < 0; dx++)
                if ((sl = f.layerNear(sx + dx, sz + dz, fromY, kStartHeight)) >= 0) {
                    sx += dx;
                    sz += dz;
                }
    if (sl < 0) return false;
    const size_t count = size_t(kCells) * kCells * kLayers;
    std::vector<float> cost(count, 1e30f);
    std::vector<int> parent(count, -1);
    using Node = std::pair<float, int>;
    std::priority_queue<Node, std::vector<Node>, std::greater<Node>> open;
    const int start = int(Floors::node(sx, sz, sl));
    cost[size_t(start)] = 0.0f;
    open.push({0.0f, start});
    int goal = -1;
    float best = 1e30f;
    while (!open.empty()) {
        const auto [c0, cur] = open.top();
        open.pop();
        if (c0 > cost[size_t(cur)]) continue;
        const int cell = cur / kLayers, cx = cell % kCells, cz = cell / kCells;
        const float h = f.height[size_t(cur)];
        const Vec2 c = f.grid.centre(cx, cz);
        if (std::abs(h - toY) <= kLevelGap) {
            const float near = std::hypot(c.x - toX, c.z - toZ);
            if (near < best) {
                best = near;
                goal = cur;
            }
        }
        for (int dz = -1; dz <= 1; dz++)
            for (int dx = -1; dx <= 1; dx++) {
                if (dx == 0 && dz == 0) continue;
                const int l = f.layerNear(cx + dx, cz + dz, h, kStepUp);
                if (l < 0) continue;
                if (dx != 0 && dz != 0 && (f.layerNear(cx + dx, cz, h, kStepUp) < 0 || f.layerNear(cx, cz + dz, h, kStepUp) < 0)) continue;
                const float step = c0 + (dx != 0 && dz != 0 ? 1.414f : 1.0f);
                const int n = int(Floors::node(cx + dx, cz + dz, l));
                if (step >= cost[size_t(n)]) continue;
                cost[size_t(n)] = step;
                parent[size_t(n)] = cur;
                open.push({step, n});
            }
    }
    if (goal < 0 || best > allowed) {
        if (logOn()) std::printf("[nav] closest spot on the target's level is %.0f from it\n", goal < 0 ? -1.0f : best);
        return false;
    }
    struct Spot {
        Vec2 at;
        float y;
    };
    std::vector<Spot> raw;
    for (int n = goal; n != -1; n = parent[size_t(n)]) {
        const int cell = n / kLayers;
        raw.push_back({f.grid.centre(cell % kCells, cell / kCells), f.height[size_t(n)]});
    }
    std::reverse(raw.begin(), raw.end());
    out.clear();
    size_t i = 0;
    while (i + 1 < raw.size()) {
        size_t next = raw.size() - 1;
        while (next > i + 1 && !floorLineFree(f, raw[i].at, raw[i].y, raw[next].at, raw[next].y)) next--;
        out.push_back(raw[next].at);
        i = next;
    }
    if (out.empty()) out.push_back(raw.back().at);
    if (logOn()) std::printf("[nav] floor route: %zu waypoints, from height %.0f to %.0f\n", out.size(), raw.front().y, raw.back().y);
    return true;
}

// ---- Shared state -----------------------------------------------------------------------------

std::mutex sM;  // the menu runs on the host thread, the walking in PADRead
std::vector<Object> sObjects;
float sLuigi[3] = {};
float sRoom[4] = {};  // Luigi's room: min x, min z, max x, max z
u32 sRoomNumber = 0;
u32 sMap = 0;

struct Walk {
    bool on = false;
    Object target{};
    std::vector<Vec2> route;
    size_t next = 0;
    std::vector<Vec2> extraBlocks;
    int replans = 0, stuck = 0, presses = 0, pressTimer = 0;
    bool arrived = false;  // stopped by the solid thing he was walking to
    int doorWait = 0;  // input reads left to see whether the door let him through
    u32 startRoom = 0;
    Vec2 lastPos{};
    bool wantPlan = false;
} sWalk;

bool sOpen = false, sFocus = false;

const char* categoryName(Category c) {
    switch (c) {
    case Category::Doors: return "Doors";
    case Category::Money: return "Coins and money";
    case Category::Hearts: return "Hearts";
    case Category::Keys: return "Keys";
    case Category::Chests: return "Chests";
    case Category::Toads: return "Toads";
    default: return "Treasure";
    }
}
const char* singleName(Category c) {
    switch (c) {
    case Category::Doors: return "door";
    case Category::Money: return "money";
    case Category::Hearts: return "heart";
    case Category::Keys: return "key";
    case Category::Chests: return "chest";
    case Category::Toads: return "Toad";
    default: return "treasure";
    }
}

std::string targetName(const Object& o) { return o.name.empty() ? singleName(o.category) : o.name; }

// Where it is from Luigi, in the camera's frame: stick up is forward.
std::string describe(const Object& o) {
    const float dx = o.x - sLuigi[0], dz = o.z - sLuigi[2];
    const float dist = std::hypot(dx, dz);
    std::string where = dz < -dist * 0.4f ? "ahead" : dz > dist * 0.4f ? "behind" : "";
    if (dx > dist * 0.4f) where += where.empty() ? "right" : " right";
    else if (dx < -dist * 0.4f) where += where.empty() ? "left" : " left";
    if (where.empty()) where = "here";
    return where + (dist < 250.0f ? ", near" : dist < 700.0f ? "" : ", far");
}

void startWalk(const Object& o) {
    sWalk = Walk{};
    sWalk.on = true;
    sWalk.target = o;
    // Test aid LM_ACCESS_GOTO=x,y,z: any pick walks there instead.
    if (const char* v = std::getenv("LM_ACCESS_GOTO")) {
        Object spot{};
        spot.category = Category::Treasure;
        if (std::sscanf(v, "%f,%f,%f", &spot.x, &spot.y, &spot.z) == 3) sWalk.target = spot;
    }
    sWalk.wantPlan = true;
    sWalk.lastPos = {sLuigi[0], sLuigi[2]};
    loadCollision(int(sMap));  // the menu still holds the game: the disc read costs nothing here
    say("Going to " + targetName(o));
}

// PADRead, on the guest thread: the search takes a moment, so a route is not replanned lightly.
bool makeRoute() {
    if (!loadCollision(int(sMap))) return false;
    const float allowed = sWalk.target.reach > 0.0f ? sWalk.target.reach + kPlanMargin : kItemAllowed;
    std::vector<Vec2> route;
    const auto useRoute = [&] {
        sWalk.route = std::move(route);
        sWalk.next = 0;
        sWalk.stuck = 0;
        if (logOn()) std::printf("[nav] route: %zu waypoints from %.0f,%.0f to %.0f,%.0f\n", sWalk.route.size(), sLuigi[0], sLuigi[2], sWalk.target.x, sWalk.target.z);
        return true;
    };
    const auto overFloors = [&] {
        const float box[4] = {sRoom[0] - kRoomMargin, sRoom[1] - kRoomMargin, sRoom[2] + kRoomMargin, sRoom[3] + kRoomMargin};
        const Floors f = buildFloors(sLuigi[0], sLuigi[1], sLuigi[2], box, sWalk.extraBlocks);
        return planFloors(f, sLuigi[0], sLuigi[1], sLuigi[2], sWalk.target.x, sWalk.target.y, sWalk.target.z, allowed, route);
    };
    if (std::abs(sWalk.target.y - sLuigi[1]) > kLevelGap) return overFloors() && useRoute();
    Grid g = buildGrid(sLuigi[0], sLuigi[1], sLuigi[2], sWalk.extraBlocks);
    // Routes stay in the room: a doorway is open in the collision, and must not lead Luigi out. The
    // room table's box stops short of the walls, so the margin lets him step into a doorway.
    for (int cz = 0; cz < kCells; cz++)
        for (int cx = 0; cx < kCells; cx++) {
            const Vec2 c = g.centre(cx, cz);
            if (c.x < sRoom[0] - kRoomMargin || c.z < sRoom[1] - kRoomMargin || c.x > sRoom[2] + kRoomMargin || c.z > sRoom[3] + kRoomMargin)
                g.at(cx, cz) = 1;
        }
    if (plan(g, {sLuigi[0], sLuigi[2]}, {sWalk.target.x, sWalk.target.z}, allowed, route)) return useRoute();
    return overFloors() && useRoute();  // the same level, but only by way of stairs
}

void finish(const std::string& text, bool interrupt = true) {
    if (logOn()) std::printf("[nav] end: %s\n", text.c_str());
    sWalk.on = false;
    say(text, interrupt);
}

}  // namespace

void publish(const std::vector<Object>& objects, float x, float y, float z, u32 map, u32 room, float minX, float minZ,
             float maxX, float maxZ) {
    std::lock_guard lk(sM);
    sRoomNumber = room;
    sRoom[0] = minX;
    sRoom[1] = minZ;
    sRoom[2] = maxX;
    sRoom[3] = maxZ;
    sObjects = objects;
    sLuigi[0] = x;
    sLuigi[1] = y;
    sLuigi[2] = z;
    sMap = map;
}

void doorLocked() {
    std::lock_guard lk(sM);
    if (logOn() && sWalk.on) std::printf("[nav] the door is locked\n");
    sWalk.on = false;
}

void cancel(const char* reason) {
    std::lock_guard lk(sM);
    sObjects.clear();  // nothing is reachable until Luigi is free again
    if (!sWalk.on) return;
    if (logOn()) std::printf("[nav] cancelled: %s\n", reason);
    // A press already made is what started this: a door or chest scene, not an interruption.
    if (sWalk.presses == 0) finish("Walk interrupted", false);
    sWalk.on = false;
}

bool drive(float rightX, float rightZ, u16& button, s8& stickX, s8& stickY) {
    std::lock_guard lk(sM);
    if (!sWalk.on) return false;
    if (std::abs(stickX) > kMoveDeadZone || std::abs(stickY) > kMoveDeadZone) {
        finish("Stopped");
        return false;
    }
    // The door was pressed: its animation plays, then Luigi is through it. A locked door is announced
    // by access_game.cpp and ends the walk through doorLocked().
    if (sWalk.doorWait > 0) {
        stickX = stickY = 0;
        if (sRoomNumber != sWalk.startRoom) finish("Through the door", false);  // after the room's name
        else if (--sWalk.doorWait == 0) finish("The door did not open");
        return sWalk.on;
    }
    if (sWalk.wantPlan) {
        sWalk.wantPlan = false;
        if (!makeRoute()) {
            finish("I can't find a way there");
            return false;
        }
    }
    const Vec2 luigi{sLuigi[0], sLuigi[2]};
    // The target may be moving, or gone: an actor that left the list was collected.
    bool present = sWalk.target.id == 0;
    for (const Object& o : sObjects)
        if (o.id == sWalk.target.id && o.id != 0) {
            present = true;
            sWalk.target.x = o.x;
            sWalk.target.y = o.y;
            sWalk.target.z = o.z;
        }
    if (!present) {
        finish("Got it");
        return false;
    }
    // Standing under a landing is not being at the door on it.
    const float toTarget = std::abs(sWalk.target.y - sLuigi[1]) > kLevelGap ? 1e6f : std::hypot(sWalk.target.x - luigi.x, sWalk.target.z - luigi.z);
    if (logOn()) {
        static u32 sN = 0;
        if (sN++ % 10 == 0) std::printf("[nav] at %.0f,%.0f, %.0f to go, waypoint %zu of %zu, stuck %d, presses %d, replans %d\n", luigi.x, luigi.z, toTarget, sWalk.next, sWalk.route.size(), sWalk.stuck, sWalk.presses, sWalk.replans);
    }
    // At a door or chest: close enough, or at the end of the route, the closest reachable spot.
    if (sWalk.next < sWalk.route.size() && std::hypot(sWalk.route[sWalk.next].x - luigi.x, sWalk.route[sWalk.next].z - luigi.z) < kWaypointReach) sWalk.next++;
    if (sWalk.target.reach > 0.0f && (toTarget < sWalk.target.reach || sWalk.next >= sWalk.route.size() || sWalk.arrived)) {
        stickX = stickY = 0;
        if (sWalk.pressTimer == 0) {
            if (sWalk.target.category == Category::Doors && sWalk.presses >= 1) {
                sWalk.doorWait = kDoorWaitReads;
                sWalk.startRoom = sRoomNumber;
                return true;
            }
            if (sWalk.presses >= (sWalk.target.category == Category::Toads ? 1 : kMaxPresses)) {  // a second A would skip his words
                finish("At the " + targetName(sWalk.target));
                return false;
            }
            sWalk.presses++;
            sWalk.pressTimer = 24;
        }
        if (sWalk.pressTimer-- > 20) button |= PAD_BUTTON_A;
        return true;
    }
    if (sWalk.target.reach <= 0.0f && toTarget < kTouchReach) {
        finish(sWalk.target.id == 0 ? "Got it" : "Here");
        return false;
    }
    while (sWalk.next < sWalk.route.size() &&
           std::hypot(sWalk.route[sWalk.next].x - luigi.x, sWalk.route[sWalk.next].z - luigi.z) < kWaypointReach)
        sWalk.next++;
    if (sWalk.next >= sWalk.route.size() && toTarget < kItemAllowed + kCell && sWalk.target.reach <= 0.0f) {
        finish("As close as I can get to the " + targetName(sWalk.target));
        return false;
    }
    Vec2 aim{sWalk.target.x, sWalk.target.z};
    if (sWalk.next < sWalk.route.size()) aim = sWalk.route[sWalk.next];
    const float dx = aim.x - luigi.x, dz = aim.z - luigi.z, len = std::max(std::hypot(dx, dz), 1.0f);

    // No progress: mark the spot ahead as blocked and plot again.
    if (std::hypot(luigi.x - sWalk.lastPos.x, luigi.z - sWalk.lastPos.z) > 8.0f) {
        sWalk.lastPos = luigi;
        sWalk.stuck = 0;
    } else if (sWalk.target.reach > 0.0f && toTarget < sWalk.target.reach + kArrivedMargin && ++sWalk.stuck > kArrivedReads) {
        sWalk.arrived = true;  // the thing he came to is what stopped him
    } else if (++sWalk.stuck > kStuckReads) {
        if (++sWalk.replans > kMaxReplans) {
            finish("Blocked. I can't reach the " + targetName(sWalk.target));
            return false;
        }
        sWalk.extraBlocks.push_back({luigi.x + dx / len * 60.0f, luigi.z + dz / len * 60.0f});
        if (!makeRoute()) {
            finish("I can't find a way there");
            return false;
        }
    }
    // World direction to stick: stick right is the camera's right axis, stick up is forward on the
    // floor (screen up), which is that axis turned a quarter.
    const float forwardX = rightZ, forwardZ = -rightX;
    const float sx = (dx * rightX + dz * rightZ) / len, sy = (dx * forwardX + dz * forwardZ) / len;
    stickX = s8(std::clamp(sx * 110.0f, -110.0f, 110.0f));
    stickY = s8(std::clamp(sy * 110.0f, -110.0f, 110.0f));
    return true;
}

void toggleMenu() {
    sOpen = !sOpen;
    sFocus = sOpen;
    if (!sOpen) swallowPadPress();
    if (sOpen) {
        std::lock_guard lk(sM);
        sWalk.on = false;
    }
    say(sOpen ? "Go to menu" : "Go to menu closed");
}

bool menuOpen() { return sOpen; }

void drawMenu() {
    if (!sOpen) return;
    std::vector<Object> objects;
    {
        std::lock_guard lk(sM);
        objects = sObjects;
    }
    std::sort(objects.begin(), objects.end(), [](const Object& a, const Object& b) {
        return std::hypot(a.x - sLuigi[0], a.z - sLuigi[2]) < std::hypot(b.x - sLuigi[0], b.z - sLuigi[2]);
    });
    // Categories that have something, in order. Left and right change category anywhere in the menu.
    std::vector<Category> present;
    for (int c = 0; c < int(Category::Count); c++)
        for (const Object& o : objects)
            if (o.category == Category(c)) {
                present.push_back(Category(c));
                break;
            }
    static int sCategory = 0;
    const auto countOf = [&](Category c) {
        int n = 0;
        for (const Object& o : objects) n += o.category == c;
        return n;
    };
    const auto sayCategory = [&] {
        const Category c = present[size_t(sCategory)];
        say(std::string(categoryName(c)) + ", " + std::to_string(countOf(c)) + ". Left and right change category.", false);
    };
    if (sCategory >= int(present.size())) sCategory = 0;
    if (!present.empty()) {
        const int count = int(present.size());
        // The stick counts only when pushed sideways: moving down the list is often a little diagonal.
        const float across = std::max(ImGui::GetKeyData(ImGuiKey_GamepadLStickLeft)->AnalogValue, ImGui::GetKeyData(ImGuiKey_GamepadLStickRight)->AnalogValue);
        const float along = std::max(ImGui::GetKeyData(ImGuiKey_GamepadLStickUp)->AnalogValue, ImGui::GetKeyData(ImGuiKey_GamepadLStickDown)->AnalogValue);
        const bool sideways = across > along * 2.0f;
        const bool left = ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft, false) || (sideways && ImGui::IsKeyPressed(ImGuiKey_GamepadLStickLeft, false));
        const bool right = ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, false) || (sideways && ImGui::IsKeyPressed(ImGuiKey_GamepadLStickRight, false));
        if (left || right) {
            sCategory = (sCategory + (right ? 1 : count - 1)) % count;
            sFocus = true;
            sayCategory();
        } else if (sFocus) {
            sayCategory();
        }
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(520, 380), ImGuiCond_FirstUseEver);
    if (sFocus) ImGui::SetNextWindowFocus();
    if (ImGui::Begin("Go to", nullptr, ImGuiWindowFlags_NoCollapse)) {
        if (present.empty()) {
            ImGui::TextUnformatted("Nothing to go to in this room.");
            if (sFocus) say("Nothing to go to in this room", false);
            sFocus = false;
        } else {
            const Category category = present[size_t(sCategory)];
            ImGui::TextUnformatted(categoryName(category));
            ImGui::Separator();
            if (sFocus) ImGui::SetKeyboardFocusHere();
            sFocus = false;
            int n = 0;
            for (const Object& o : objects) {
                if (o.category != category) continue;
                n++;
                char label[96];
                std::snprintf(label, sizeof(label), "%s, %s##%d", targetName(o).c_str(), describe(o).c_str(), n);
                if (ImGui::Selectable(label)) {
                    std::lock_guard lk(sM);
                    startWalk(o);
                    sOpen = false;
                    swallowPadPress();
                }
            }
        }
    }
    ImGui::End();
}

}  // namespace recomp::access::nav
