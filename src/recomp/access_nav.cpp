// Screen reader support: the Go to menu and Luigi's automatic walking
// (docs/accessibility/README.md, "Go to menu"). USA game only for now.
//
// Pressing the left stick in opens a menu of what access_world.cpp found in Luigi's room. Picking
// an object plots a route with the map's own collision and steers Luigi along it by replacing the
// stick input, so he moves the way a player would move him. Doors and chests get an A press on
// arrival; everything else is collected by touch. Moving the stick takes control back.
//
// The map's layout is built once when the map starts, and covers it whole: every floor over every
// spot, stairs included, so room size and height make no difference to a route. Routes are searched
// on their own threads, and Luigi stands still for that moment.
//
// Collision (col.mp in /Map/mapN.szp): big endian. 0x24 vertices (3 floats), 0x28 normals (3
// floats), 0x2C triangles (0x18 bytes: u16 vertices a, b, c and normal), 0x30 what follows them.
// Static furniture is in it. Moved furniture, chests and ghosts are not: chests and Toads are routed
// round, and when Luigi stops making progress the spot ahead of him is marked blocked and the route
// is plotted again. Stairs are real steps about 30 units high, joined within kStepUp.
//
// Test aids: LM_ACCESS_WORLD_LOG=1 logs routes and progress, LM_ACCESS_NAV_AUDIT=1 plots between the
// doors of every room of the map and logs what it could not reach.
#include "recomp/access_nav.h"

#include "recomp/access.h"
#include "recomp/hle.h"
#include "recomp/menu.h"

#include <dolphin/pad.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

namespace recomp::unused2 {
std::vector<u8> readDiscItem(const char* disc, const char* inner);  // gallery.cpp
}

namespace recomp::access::nav {
namespace {

constexpr float kCell = 20.0f;
constexpr float kBodyHigh = 110.0f;  // the top of his body: a wall between a step and this blocks him
constexpr float kBodyRadius = 45.0f;
constexpr float kWaypointReach = 30.0f, kTouchReach = 28.0f;
constexpr float kRoomMargin = 110.0f;
constexpr float kPlanMargin = 110.0f;  // a route may end this much beyond the press distance
constexpr float kItemAllowed = 200.0f;  // he goes this close to an item out of reach, such as one on a table
constexpr int kStuckReads = 40;   // input reads without progress
constexpr int kMaxReplans = 6;
constexpr int kArrivedReads = 12;     // stopped this long near a door, chest or Toad: he has arrived
constexpr float kArrivedMargin = 60.0f;
constexpr int kMaxPresses = 3;
constexpr int kDoorWaitReads = 1800;  // a door takes about 13 seconds to open, at 60 reads a second
constexpr s8 kMoveDeadZone = 40;  // the player's stick above this takes over
constexpr int kLayers = 8;            // floors stacked over one cell
constexpr float kStepUp = 45.0f;      // a stair step: lower than this does not block him
constexpr float kLevelGap = 300.0f;   // more height than this is another level: doors stand 150 or 275 above their floor, floors are 550 apart
constexpr float kStartHeight = 60.0f;  // the floor Luigi stands on is this close to him
constexpr float kDoorHeight = 275.0f;  // a door's y above its floor, at most
constexpr float kFurnitureMargin = 75.0f;  // he has arrived this far from furniture's edge
constexpr float kTouchFurniture = 90.0f, kFacingFurniture = 0.4f;  // A names what is this close, in front of him

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

// Words for the furniture table's model names, by prefix, first match. The names are short forms,
// many Japanese (isu chair, tubo vase, tana shelf, yoroi armor), so these are read from the names and
// the rooms they stand in. A model not listed says "Furniture". An empty word leaves it out: markers
// and floor pieces, nothing a player would look for.
struct ModelWord {
    const char* prefix;
    const char* word;
};
constexpr ModelWord kModelWords[] = {
    {"omark", ""}, {"flrd", ""}, {"yuka", ""}, {"tesu", ""}, {"a_tesu", ""}, {"kabe", ""}, {"(null)", ""},
    {"booka", "Book"}, {"bookb", "Book"}, {"book", "Bookshelf"}, {"o_hon", "Books"}, {"o_syo", "Books"},
    {"gakub", "Picture"}, {"opic", "Picture"}, {"tpic", "Picture"}, {"pic", "Picture"}, {"oe", "Picture"}, {"o_e", "Picture"},
    {"papa", "Toilet paper"}, {"otoire", "Toilet"}, {"tank", "Toilet tank"}, {"bath", "Bathtub"}, {"senmen", "Sink"}, {"sen", "Sink"}, {"wash", "Sink"},
    {"bed", "Bed"}, {"bil", "Billiard table"}, {"bin", "Bottle"}, {"drink", "Drink"}, {"ocup", "Cup"}, {"food", "Food"}, {"a_groc", "Groceries"}, {"sara", "Plate"}, {"o_sara", "Plate"},
    {"box", "Box"}, {"obox", "Box"}, {"oldbox", "Box"}, {"wbox", "Box"}, {"a_wbox", "Box"}, {"trsbx", "Box"}, {"ohako", "Box"},
    {"a_barr", "Barrel"}, {"a_drum", "Drum"}, {"o_taiko", "Drum"}, {"o_piano", "Piano"}, {"o_harpu", "Harp"}, {"o_mokin", "Xylophone"}, {"o_rapa", "Horn"},
    {"bake", "Bucket"}, {"obaketu", "Bucket"}, {"a_buck", "Bucket"}, {"hoki", "Broom"}, {"oiron", "Ironing board"},
    {"car", "Toy car"}, {"heri", "Toy helicopter"}, {"toy", "Toy"}, {"dol", "Doll"}, {"o_nin", "Doll"}, {"moku", "Rocking horse"}, {"o_uma", "Horse"},
    {"chest", "Chest of drawers"}, {"dres", "Dresser"}, {"tans", "Wardrobe"},
    {"desk", "Desk"}, {"otukue", "Desk"}, {"tuku", "Desk"}, {"o_tuku", "Desk"},
    {"a_tabl", "Table"}, {"otebl", "Table"},
    {"isu", "Chair"}, {"oisu", "Chair"}, {"o_isu", "Chair"}, {"oais", "Chair"}, {"o_sofa", "Sofa"}, {"a_sofa", "Sofa"},
    {"drt", "Dartboard"}, {"fan", "Ceiling fan"}, {"fuku", "Clothes"}, {"kutu", "Shoes"}, {"hana", "Flowers"}, {"hasira", "Pillar"}, {"a_pill", "Pillar"},
    {"htana", "Shelf"}, {"otana", "Shelf"}, {"otan", "Shelf"}, {"stana", "Shelf"}, {"tana", "Shelf"}, {"shelf", "Shelf"}, {"a_shel", "Shelf"}, {"a_rack", "Rack"},
    {"mirr", "Mirror"}, {"o_sika", "Deer head"}, {"a_lion", "Lion head"}, {"o_tora", "Tiger rug"}, {"o_ken", "Sword"}, {"yoroi", "Suit of armor"},
    {"o_kusa", "Plant"}, {"a_plan", "Plant"}, {"ueki", "Plant"}, {"oha", "Plant"},
    {"o_misin", "Sewing machine"}, {"omisi", "Sewing machine"}, {"reizo", "Refrigerator"}, {"suiso", "Fish tank"}, {"trophy", "Trophy"},
    {"o_syan", "Chandelier"}, {"osyan", "Chandelier"}, {"syan", "Chandelier"}, {"shan", "Chandelier"},
    {"a_lamp", "Lamp"}, {"a_ligh", "Light"}, {"ohikari", "Lamp"}, {"o_hika", "Lamp"},
    {"rou", "Candle stand"}, {"orou", "Candle stand"}, {"o_rou", "Candle stand"}, {"osoku", "Candle stand"}, {"a_cand", "Candle stand"},
    {"torch", "Torch"}, {"a_torc", "Torch"}, {"a_tomb", "Tombstone"}, {"a_tree", "Tree"}, {"a_foun", "Fountain"}, {"a_dog", "Dog house"}, {"a_bird", "Birdbath"},
    {"tubo", "Vase"}, {"otubo", "Vase"}, {"sk13", "Screen"}, {"sp13", "Speaker"}, {"break", "Breaker box"}, {"a_ladd", "Ladder"},
};

const char* modelWord(const std::string& model) {
    for (const ModelWord& w : kModelWords)
        if (model.rfind(w.prefix, 0) == 0) return w.word;
    return "Furniture";
}

// A piece of the map's furniture table (jmp/furnitureinfo), fixed for the map.
struct Furniture {
    float x, y, z;
    float halfX, halfZ;  // its footprint, turned as it stands
    float tall;
    u32 room;
    const char* word;
};

u32 be32(const u8* p) { return u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3]; }
u32 be16(const u8* p) { return u32(p[0]) << 8 | p[1]; }
float beFloat(const u8* p) {
    const u32 v = be32(p);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

// The whole map's walkable layout, built once per map from col.mp on a thread of its own: each cell
// of a grid over the map holds the floors above it and whether Luigi's body fits on them. It never
// changes afterwards, so route searches read it from their own threads without a lock.
struct NavMap {
    int map = -1;
    float originX = 0, originZ = 0;
    int w = 0, h = 0;
    std::vector<float> height;  // w * h * kLayers
    std::vector<u8> count;      // floors over each cell
    std::vector<u8> blocked;    // w * h * kLayers
    std::vector<Furniture> furniture;
    bool inside(int x, int z) const { return x >= 0 && z >= 0 && x < w && z < h; }
    int cellX(float x) const { return int(std::floor((x - originX) / kCell)); }
    int cellZ(float z) const { return int(std::floor((z - originZ) / kCell)); }
    Vec2 centre(int x, int z) const { return {originX + (float(x) + 0.5f) * kCell, originZ + (float(z) + 0.5f) * kCell}; }
    size_t cell(int x, int z) const { return size_t(z) * w + x; }
    size_t node(int x, int z, int layer) const { return cell(x, z) * kLayers + layer; }
};

// The furniture table. Fields are found by the hash of their names, as the game finds them.
std::vector<Furniture> readFurniture(const std::string& path) {
    std::vector<Furniture> out;
    const std::vector<u8> t = unused2::readDiscItem(path.c_str(), "jmp/furnitureinfo");
    if (t.size() < 0x10) return out;
    const u32 rows = be32(&t[0]), fields = be32(&t[4]), data = be32(&t[8]), size = be32(&t[12]);
    if (0x10 + size_t(fields) * 12 > t.size() || data + size_t(rows) * size > t.size()) return out;
    const auto field = [&](u32 hash) -> int {
        for (u32 i = 0; i < fields; i++)
            if (be32(&t[0x10 + i * 12]) == hash) return int(be16(&t[0x10 + i * 12 + 8]));
        return -1;
    };
    const int model = field(0x9BC8CD), x = field(0x17BEFD9), y = field(0x17BEFDA), z = field(0x17BEFDB), dirY = field(0x17A0565);
    const int room = field(0xC9952D), sizeX = field(0x1545CAA), sizeY = field(0x1545CAB), sizeZ = field(0x1545CAC);
    if (model < 0 || x < 0 || y < 0 || z < 0 || dirY < 0 || room < 0 || sizeX < 0 || sizeY < 0 || sizeZ < 0) return out;
    for (u32 r = 0; r < rows; r++) {
        const u8* row = &t[data + r * size];
        const std::string name(reinterpret_cast<const char*>(row + model), strnlen(reinterpret_cast<const char*>(row + model), 32));
        const u32 wide = be32(row + sizeX), deep = be32(row + sizeZ);
        const char* word = modelWord(name);
        if (!*word || wide == 0 || deep == 0) continue;
        // A quarter turn swaps its width and depth.
        const bool turned = std::abs(std::remainder(beFloat(row + dirY), 180.0f)) > 45.0f;
        out.push_back({beFloat(row + x), beFloat(row + y), beFloat(row + z), float(turned ? deep : wide) * 0.5f, float(turned ? wide : deep) * 0.5f, float(be32(row + sizeY)), be32(row + room), word});
    }
    return out;
}

std::shared_ptr<const NavMap> buildNav(int map) {
    const std::string path = "/Map/map" + std::to_string(map) + ".szp";
    const std::vector<u8> d = unused2::readDiscItem(path.c_str(), "col.mp");
    if (logOn()) std::printf("[nav] %s col.mp: %zu bytes\n", path.c_str(), d.size());
    if (d.size() < 0x40) return nullptr;
    const u32 verts = be32(&d[0x24]), normals = be32(&d[0x28]), tris = be32(&d[0x2C]), end = be32(&d[0x30]);
    if (verts >= normals || normals >= tris || tris >= end || end > d.size()) return nullptr;
    const auto vertex = [&](u32 i) {
        const u8* p = &d[verts + i * 12];
        return std::array<float, 3>{beFloat(p), beFloat(p + 4), beFloat(p + 8)};
    };
    std::vector<Tri> walls, floors;
    const u32 vertexCount = (normals - verts) / 12, normalCount = (tris - normals) / 12;
    for (u32 t = tris; t + 0x18 <= end; t += 0x18) {
        const u32 a = be16(&d[t]), b = be16(&d[t + 2]), c = be16(&d[t + 4]), n = be16(&d[t + 6]);
        if (a >= vertexCount || b >= vertexCount || c >= vertexCount || n >= normalCount) continue;
        const float up = beFloat(&d[normals + n * 12 + 4]);
        if (up > 0.7f) floors.push_back({vertex(a), vertex(b), vertex(c)});
        else if (std::abs(up) <= 0.7f) walls.push_back({vertex(a), vertex(b), vertex(c)});  // ceilings do not block
    }
    if (floors.empty()) return nullptr;
    auto nav = std::make_shared<NavMap>();
    NavMap& m = *nav;
    m.map = map;
    float minX = 1e30f, minZ = 1e30f, maxX = -1e30f, maxZ = -1e30f;
    for (const Tri& t : floors)
        for (const auto* v : {&t.a, &t.b, &t.c}) {
            minX = std::min(minX, (*v)[0]);
            minZ = std::min(minZ, (*v)[2]);
            maxX = std::max(maxX, (*v)[0]);
            maxZ = std::max(maxZ, (*v)[2]);
        }
    m.originX = minX - kCell;
    m.originZ = minZ - kCell;
    m.w = int((maxX - minX) / kCell) + 3;
    m.h = int((maxZ - minZ) / kCell) + 3;
    const size_t cells = size_t(m.w) * m.h;
    m.height.assign(cells * kLayers, 0.0f);
    m.count.assign(cells, 0);
    m.blocked.assign(cells * kLayers, 0);
    for (const Tri& t : floors) {
        const auto [minTX, maxTX] = std::minmax({t.a[0], t.b[0], t.c[0]});
        const auto [minTZ, maxTZ] = std::minmax({t.a[2], t.b[2], t.c[2]});
        const int x0 = std::max(m.cellX(minTX), 0), x1 = std::min(m.cellX(maxTX), m.w - 1);
        const int z0 = std::max(m.cellZ(minTZ), 0), z1 = std::min(m.cellZ(maxTZ), m.h - 1);
        const float det = (t.b[2] - t.c[2]) * (t.a[0] - t.c[0]) + (t.c[0] - t.b[0]) * (t.a[2] - t.c[2]);
        if (std::abs(det) < 1e-3f) continue;
        for (int cz = z0; cz <= z1; cz++)
            for (int cx = x0; cx <= x1; cx++) {
                const Vec2 c = m.centre(cx, cz);
                const float u = ((t.b[2] - t.c[2]) * (c.x - t.c[0]) + (t.c[0] - t.b[0]) * (c.z - t.c[2])) / det;
                const float v = ((t.c[2] - t.a[2]) * (c.x - t.c[0]) + (t.a[0] - t.c[0]) * (c.z - t.c[2])) / det;
                const float w = 1.0f - u - v;
                if (u < 0.0f || v < 0.0f || w < 0.0f) continue;
                const float h = t.a[1] * u + t.b[1] * v + t.c[1] * w;
                u8& n = m.count[m.cell(cx, cz)];
                bool known = false;
                for (int l = 0; l < n; l++) known |= std::abs(m.height[m.node(cx, cz, l)] - h) < 5.0f;
                if (!known && n < kLayers) m.height[m.node(cx, cz, n++)] = h;
            }
    }
    // A floor is closed where a wall stands between a step and the top of Luigi's body, within his
    // radius. Each wall sample marks the cells around it once per cell: the offsets are worked out once.
    const int reach = int(std::ceil(kBodyRadius / kCell));
    std::vector<std::pair<int, int>> disc;
    for (int dz = -reach; dz <= reach; dz++)
        for (int dx = -reach; dx <= reach; dx++)
            if (std::hypot(float(dx), float(dz)) * kCell <= kBodyRadius) disc.push_back({dx, dz});
    for (const Tri& t : walls) {
        const auto [minY, maxY] = std::minmax({t.a[1], t.b[1], t.c[1]});
        const auto [minTX, maxTX] = std::minmax({t.a[0], t.b[0], t.c[0]});
        const auto [minTZ, maxTZ] = std::minmax({t.a[2], t.b[2], t.c[2]});
        // The face of a stair step, with a floor at its top, is no wall: on steep stairs the face two
        // steps ahead is within his radius and would close the stairs. A low piece of a tiled wall has
        // no floor at its top, and stays.
        if (maxY - minY <= kStepUp) {
            const int mx = m.cellX((minTX + maxTX) * 0.5f), mz = m.cellZ((minTZ + maxTZ) * 0.5f);
            bool stepFace = false;
            for (int dz = -1; dz <= 1 && !stepFace; dz++)
                for (int dx = -1; dx <= 1 && !stepFace; dx++) {
                    if (!m.inside(mx + dx, mz + dz)) continue;
                    for (int l = 0; l < m.count[m.cell(mx + dx, mz + dz)]; l++) stepFace |= std::abs(m.height[m.node(mx + dx, mz + dz, l)] - maxY) < 8.0f;
                }
            if (stepFace) continue;
        }
        const float size = std::max({maxTX - minTX, maxTZ - minTZ, maxY - minY});
        const int steps = std::clamp(int(size / (kCell * 0.5f)) + 1, 1, 400);
        for (int i = 0; i <= steps; i++)
            for (int j = 0; j <= steps - i; j++) {
                const float u = float(i) / float(steps), v = float(j) / float(steps), w = 1.0f - u - v;
                const float py = t.a[1] * w + t.b[1] * u + t.c[1] * v;
                const int sx = m.cellX(t.a[0] * w + t.b[0] * u + t.c[0] * v), sz = m.cellZ(t.a[2] * w + t.b[2] * u + t.c[2] * v);
                for (const auto& [dx, dz] : disc) {
                    const int cx = sx + dx, cz = sz + dz;
                    if (!m.inside(cx, cz)) continue;
                    const int layers = m.count[m.cell(cx, cz)];
                    for (int l = 0; l < layers; l++) {
                        const float above = py - m.height[m.node(cx, cz, l)];
                        if (above > kStepUp && above < kBodyHigh) m.blocked[m.node(cx, cz, l)] = 1;
                    }
                }
            }
    }
    m.furniture = readFurniture(path);
    if (logOn()) std::printf("[nav] map %d layout: %d by %d cells, %zu floors, %zu walls, %zu furniture\n", map, m.w, m.h, floors.size(), walls.size(), m.furniture.size());
    return nav;
}

struct Block {
    float x, y, z, radius;
};

// One route to find: the search area is a room's box, and blocks are what the map's layout does not
// know about, such as chests and the spots Luigi got stuck at.
struct Request {
    float from[3], to[3];
    float allowed;
    float box[4];  // min x, min z, max x, max z
    float startReach = kStartHeight;
    bool orClosest = false;  // nothing within allowed: go to the closest spot anyway
    std::vector<Block> blocks;
};

// The layout plus one request's blocks.
struct View {
    const NavMap& m;
    std::vector<u8> blocked;
    // The open floor on this cell within a step of height y, or -1.
    int layerNear(int x, int z, float y, float reach) const {
        if (!m.inside(x, z)) return -1;
        int found = -1;
        float best = reach;
        for (int l = 0; l < m.count[m.cell(x, z)]; l++) {
            const float gap = std::abs(m.height[m.node(x, z, l)] - y);
            if (gap <= best && !blocked[m.node(x, z, l)]) {
                best = gap;
                found = l;
            }
        }
        return found;
    }
};

// A straight walk from a at height ay stays on open floor, step by step, and ends at height by.
bool lineFree(const View& f, Vec2 a, float ay, Vec2 b, float by) {
    const float dist = std::hypot(b.x - a.x, b.z - a.z);
    const int steps = int(dist / (kCell * 0.5f)) + 1;
    float y = ay;
    for (int i = 1; i <= steps; i++) {
        const float t = float(i) / float(steps);
        const int x = f.m.cellX(a.x + (b.x - a.x) * t), z = f.m.cellZ(a.z + (b.z - a.z) * t);
        const int l = f.layerNear(x, z, y, kStepUp);
        if (l < 0) return false;
        y = f.m.height[f.m.node(x, z, l)];
    }
    return std::abs(y - by) <= kStepUp;
}

// Everywhere Luigi can reach from the floor under him, then the spot closest to the target on the
// target's level: a door or a chest is inside something solid, so the target itself is often closed,
// and a nearby open cell can be behind a wall. The route is then straightened: a waypoint stays only
// where a straight walk would hit something.
bool search(const NavMap& m, const Request& r, std::vector<Vec2>& out, float* closest = nullptr, bool* partial = nullptr) {
    View f{m, m.blocked};
    for (const Block& b : r.blocks) {
        const int sx = m.cellX(b.x), sz = m.cellZ(b.z), span = int(std::ceil(b.radius / kCell));
        for (int cz = sz - span; cz <= sz + span; cz++)
            for (int cx = sx - span; cx <= sx + span; cx++) {
                if (!m.inside(cx, cz) || std::hypot(float(cx - sx), float(cz - sz)) * kCell > b.radius) continue;
                for (int l = 0; l < m.count[m.cell(cx, cz)]; l++)
                    if (std::abs(m.height[m.node(cx, cz, l)] - b.y) < kLevelGap) f.blocked[m.node(cx, cz, l)] = 1;
            }
    }
    const int minCX = std::max(m.cellX(r.box[0]), 0), minCZ = std::max(m.cellZ(r.box[1]), 0);
    const int maxCX = std::min(m.cellX(r.box[2]), m.w - 1), maxCZ = std::min(m.cellZ(r.box[3]), m.h - 1);
    const auto inBox = [&](int x, int z) { return x >= minCX && z >= minCZ && x <= maxCX && z <= maxCZ; };
    int sx = m.cellX(r.from[0]), sz = m.cellZ(r.from[2]), sl = f.layerNear(sx, sz, r.from[1], r.startReach);
    for (int span = 1; span <= 6 && sl < 0; span++)
        for (int dz = -span; dz <= span && sl < 0; dz++)
            for (int dx = -span; dx <= span && sl < 0; dx++)
                if ((sl = f.layerNear(sx + dx, sz + dz, r.from[1], r.startReach)) >= 0) {
                    sx += dx;
                    sz += dz;
                }
    if (sl < 0) {
        if (logOn()) std::printf("[nav] no open floor near %.0f,%.0f,%.0f\n", r.from[0], r.from[1], r.from[2]);
        return false;
    }
    const size_t count = m.blocked.size();
    std::vector<float> cost(count, 1e30f);
    std::vector<int> parent(count, -1);
    using Node = std::pair<float, int>;
    std::priority_queue<Node, std::vector<Node>, std::greater<Node>> open;
    const int start = int(m.node(sx, sz, sl));
    cost[size_t(start)] = 0.0f;
    open.push({0.0f, start});
    int goal = -1;
    float best = 1e30f;
    while (!open.empty()) {
        const auto [c0, cur] = open.top();
        open.pop();
        if (c0 > cost[size_t(cur)]) continue;
        const int cell = cur / kLayers, cx = cell % m.w, cz = cell / m.w;
        const float h = m.height[size_t(cur)];
        const Vec2 c = m.centre(cx, cz);
        if (std::abs(h - r.to[1]) <= kLevelGap) {
            const float near = std::hypot(c.x - r.to[0], c.z - r.to[2]);
            if (near < best) {
                best = near;
                goal = cur;
            }
        }
        for (int dz = -1; dz <= 1; dz++)
            for (int dx = -1; dx <= 1; dx++) {
                if ((dx == 0 && dz == 0) || !inBox(cx + dx, cz + dz)) continue;
                if (dx != 0 && dz != 0 && (f.layerNear(cx + dx, cz, h, kStepUp) < 0 || f.layerNear(cx, cz + dz, h, kStepUp) < 0)) continue;  // no cutting corners
                // Every floor within a step: the closest alone can miss the way up a stair.
                for (int l = 0; l < m.count[m.cell(cx + dx, cz + dz)]; l++) {
                    const int n = int(m.node(cx + dx, cz + dz, l));
                    if (f.blocked[size_t(n)] || std::abs(m.height[size_t(n)] - h) > kStepUp) continue;
                    const float step = c0 + (dx != 0 && dz != 0 ? 1.414f : 1.0f);
                    if (step >= cost[size_t(n)]) continue;
                    cost[size_t(n)] = step;
                    parent[size_t(n)] = cur;
                    open.push({step, n});
                }
            }
    }
    if (closest) *closest = goal < 0 ? -1.0f : best;
    if (goal < 0 || best > r.allowed) {
        if (logOn()) std::printf("[nav] closest spot on the target's level is %.0f from it\n", goal < 0 ? -1.0f : best);
        if (goal < 0 || !r.orClosest) return false;
        if (partial) *partial = true;
    }
    struct Spot {
        Vec2 at;
        float y;
    };
    std::vector<Spot> raw;
    for (int n = goal; n != -1; n = parent[size_t(n)]) {
        const int cell = n / kLayers;
        raw.push_back({m.centre(cell % m.w, cell / m.w), m.height[size_t(n)]});
    }
    std::reverse(raw.begin(), raw.end());
    out.clear();
    size_t i = 0;
    while (i + 1 < raw.size()) {
        size_t next = raw.size() - 1;
        while (next > i + 1 && !lineFree(f, raw[i].at, raw[i].y, raw[next].at, raw[next].y)) next--;
        out.push_back(raw[next].at);
        i = next;
    }
    if (out.empty()) out.push_back(raw.back().at);
    if (logOn()) std::printf("[nav] route: %zu waypoints, from height %.0f to %.0f\n", out.size(), raw.front().y, raw.back().y);
    return true;
}

// The layout of the map in play: built by one thread, waited for by route searches.
std::mutex sNavM;
std::condition_variable sNavCv;
std::shared_ptr<const NavMap> sNav;
int sNavRequested = -1;
bool sNavDone = false;

void requestNav(int map) {
    {
        std::lock_guard lk(sNavM);
        if (map == sNavRequested) return;
        sNavRequested = map;
        sNavDone = false;
        sNav = nullptr;
    }
    std::thread([map] {
        const auto start = std::chrono::steady_clock::now();
        std::shared_ptr<const NavMap> nav = buildNav(map);
        if (logOn()) std::printf("[nav] layout took %lld ms\n", static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count()));
        std::lock_guard lk(sNavM);
        if (map != sNavRequested) return;  // the player moved on
        sNav = std::move(nav);
        sNavDone = true;
        sNavCv.notify_all();
    }).detach();
}

std::shared_ptr<const NavMap> waitNav() {
    std::unique_lock lk(sNavM);
    sNavCv.wait_for(lk, std::chrono::seconds(30), [] { return sNavDone; });
    return sNav;
}

// A search under way on its own thread, so the game never waits for it.
struct Job {
    Request request;
    size_t solids = 0;  // the first blocks: chests and Toads, dropped if they close the only way
    std::atomic<bool> done{false};
    bool found = false, partial = false;
    std::vector<Vec2> route;
};

void runJob(const std::shared_ptr<Job>& job) {
    std::thread([job] {
        if (const std::shared_ptr<const NavMap> nav = waitNav()) {
            job->found = search(*nav, job->request, job->route);
            if (!job->found && job->solids > 0) {
                job->request.blocks.erase(job->request.blocks.begin(), job->request.blocks.begin() + std::ptrdiff_t(job->solids));
                job->solids = 0;
                job->found = search(*nav, job->request, job->route);
            }
            if (!job->found) {
                job->request.orClosest = true;
                job->found = search(*nav, job->request, job->route, nullptr, &job->partial);
            }
        }
        job->done = true;
    }).detach();
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
    std::shared_ptr<Job> job;  // the search under way
    bool partial = false;      // the route ends short of the target
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
    case Category::Furniture: return "Furniture";
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
    case Category::Furniture: return "furniture";
    default: return "treasure";
    }
}

std::string targetName(const Object& o) { return o.name.empty() ? singleName(o.category) : o.name; }

std::string lowerFirst(std::string s) {
    if (!s.empty()) s[0] = char(std::tolower(static_cast<unsigned char>(s[0])));
    return s;
}

// The furniture of Luigi's room on his level, as menu entries. Lamps on the ceiling are not on it.
std::vector<Object> furnitureObjects() {
    std::vector<Object> out;
    std::shared_ptr<const NavMap> nav;
    {
        std::lock_guard lk(sNavM);
        nav = sNav;
    }
    if (!nav || nav->map != int(sMap)) return out;
    for (const Furniture& p : nav->furniture)
        if (p.room == sRoomNumber && std::abs(p.y - sLuigi[1]) <= kLevelGap)
            out.push_back({Category::Furniture, p.x, p.y, p.z, 0, std::max(p.halfX, p.halfZ) + kFurnitureMargin, p.word});
    return out;
}

// Where it is from Luigi, in the camera's frame: stick up is forward.
std::string describe(const Object& o) {
    const float dx = o.x - sLuigi[0], dz = o.z - sLuigi[2];
    const float dist = std::hypot(dx, dz);
    std::string where = dz < -dist * 0.4f ? "ahead" : dz > dist * 0.4f ? "behind" : "";
    if (dx > dist * 0.4f) where += where.empty() ? "right" : " right";
    else if (dx < -dist * 0.4f) where += where.empty() ? "left" : " left";
    if (where.empty()) where = "here";
    // Two doors can stand one above the other, as in the Foyer.
    const float up = o.y - sLuigi[1];
    return where + (dist < 250.0f ? ", near" : dist < 700.0f ? "" : ", far") + (up > kLevelGap ? ", upstairs" : up < -kLevelGap - kDoorHeight ? ", downstairs" : "");
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
    say("Going to " + targetName(o));
}

// Starts the search for the current walk. Chests and Toads are not in the collision, and walking
// into one costs a stall: routes go round them unless that leaves no way.
void startPlan() {
    auto job = std::make_shared<Job>();
    Request& r = job->request;
    std::copy(sLuigi, sLuigi + 3, r.from);
    r.to[0] = sWalk.target.x;
    r.to[1] = sWalk.target.y;
    r.to[2] = sWalk.target.z;
    r.allowed = sWalk.target.reach > 0.0f ? sWalk.target.reach + kPlanMargin : kItemAllowed;
    // Routes stay in the room: a doorway is open in the collision, and must not lead Luigi out. The
    // room table's box stops short of the walls, so the margin lets him step into a doorway.
    r.box[0] = sRoom[0] - kRoomMargin;
    r.box[1] = sRoom[1] - kRoomMargin;
    r.box[2] = sRoom[2] + kRoomMargin;
    r.box[3] = sRoom[3] + kRoomMargin;
    for (const Object& o : sObjects)
        if ((o.category == Category::Chests || o.category == Category::Toads) && o.id != sWalk.target.id) r.blocks.push_back({o.x, o.y, o.z, o.reach - 15.0f});
    job->solids = r.blocks.size();
    for (const Vec2& p : sWalk.extraBlocks) r.blocks.push_back({p.x, sLuigi[1], p.z, kBodyRadius + 15.0f});
    sWalk.job = job;
    runJob(job);
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
    requestNav(int(map));
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
        startPlan();
    }
    if (sWalk.job) {
        stickX = stickY = 0;
        if (!sWalk.job->done) return true;
        const std::shared_ptr<Job> job = std::move(sWalk.job);
        sWalk.job = nullptr;
        if (!job->found) {
            finish("I can't find a way there");
            return false;
        }
        sWalk.route = std::move(job->route);
        sWalk.partial = job->partial;
        sWalk.next = 0;
        sWalk.stuck = 0;
        sWalk.lastPos = {sLuigi[0], sLuigi[2]};
        if (logOn()) std::printf("[nav] walking: %zu waypoints from %.0f,%.0f to %.0f,%.0f\n", sWalk.route.size(), sLuigi[0], sLuigi[2], sWalk.target.x, sWalk.target.z);
    }
    const Vec2 luigi{sLuigi[0], sLuigi[2]};
    // The target may be moving, or gone: an actor that left the list was collected.
    bool present = sWalk.target.id == 0;  // furniture and spots stay
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
    if (sWalk.partial && sWalk.next >= sWalk.route.size()) {
        finish("As close as I can get to the " + targetName(sWalk.target));
        return false;
    }
    if (sWalk.target.reach > 0.0f && (toTarget < sWalk.target.reach || sWalk.next >= sWalk.route.size() || sWalk.arrived)) {
        stickX = stickY = 0;
        if (sWalk.target.category == Category::Furniture) {
            finish("At the " + lowerFirst(targetName(sWalk.target)));
            return false;
        }
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
        sWalk.wantPlan = true;
        stickX = stickY = 0;
        return true;
    }
    // World direction to stick: stick right is the camera's right axis, stick up is forward on the
    // floor (screen up), which is that axis turned a quarter.
    const float forwardX = rightZ, forwardZ = -rightX;
    const float sx = (dx * rightX + dz * rightZ) / len, sy = (dx * forwardX + dz * forwardZ) / len;
    stickX = s8(std::clamp(sx * 110.0f, -110.0f, 110.0f));
    stickY = s8(std::clamp(sy * 110.0f, -110.0f, 110.0f));
    return true;
}

void audit(int map, std::vector<AuditRoom> rooms) {
    requestNav(map);
    std::thread([rooms = std::move(rooms)] {
        const std::shared_ptr<const NavMap> nav = waitNav();
        if (!nav) {
            std::printf("[nav-audit] no layout\n");
            return;
        }
        int pairs = 0, failed = 0;
        for (const AuditRoom& room : rooms) {
            std::printf("[nav-audit] room %u: %.0f by %.0f, %zu doors\n", room.number, room.maxX - room.minX, room.maxZ - room.minZ, room.doors.size());
            for (size_t a = 0; a < room.doors.size(); a++)
                for (size_t b = 0; b < room.doors.size(); b++) {
                    if (a == b) continue;
                    const AuditDoor& from = room.doors[a];
                    const AuditDoor& to = room.doors[b];
                    Request r;
                    // Inside the room: a door stands in its wall, between two rooms.
                    r.from[0] = std::clamp(from.x, room.minX + 60.0f, room.maxX - 60.0f);
                    r.from[1] = from.y - 150.0f;  // the door's middle: its floor is closest to this
                    r.from[2] = std::clamp(from.z, room.minZ + 60.0f, room.maxZ - 60.0f);
                    r.to[0] = to.x;
                    r.to[1] = to.y;
                    r.to[2] = to.z;
                    r.allowed = 105.0f + kPlanMargin;
                    r.box[0] = room.minX - kRoomMargin;
                    r.box[1] = room.minZ - kRoomMargin;
                    r.box[2] = room.maxX + kRoomMargin;
                    r.box[3] = room.maxZ + kRoomMargin;
                    r.startReach = kLevelGap;  // a door's y is above its floor
                    std::vector<Vec2> route;
                    float closest = 0.0f;
                    pairs++;
                    if (search(*nav, r, route, &closest)) continue;
                    failed++;
                    std::printf("[nav-audit] room %u FAILED: door to room %u (%.0f,%.0f,%.0f) to door to room %u (%.0f,%.0f,%.0f), closest %.0f\n", room.number, from.leadsTo, from.x, from.y, from.z, to.leadsTo, to.x, to.y, to.z, closest);
                }
        }
        std::printf("[nav-audit] %d routes tried, %d failed\n", pairs, failed);
        std::fflush(stdout);
    }).detach();
}

std::string aimedFurniture(const float eye[3], const float dir[3], float& angle, float& height, float heightWeight) {
    std::shared_ptr<const NavMap> nav;
    {
        std::lock_guard lk(sNavM);
        nav = sNav;
    }
    if (!nav || nav->map != int(sMap)) return {};
    const char* best = nullptr;
    const float flat = std::max(std::hypot(dir[0], dir[2]), 1e-3f), pitch = std::atan2(dir[1], flat);
    for (const Furniture& p : nav->furniture) {
        if (p.room != sRoomNumber || std::abs(p.y - sLuigi[1]) > kLevelGap + kBodyHigh) continue;
        const float dx = p.x - eye[0], dz = p.z - eye[2], dist = std::max(std::hypot(dx, dz), 1.0f);
        const float ahead = (dx * dir[0] + dz * dir[2]) / (dist * flat);
        if (ahead <= 0.0f) continue;
        const float up = std::atan2(p.y + p.tall * 0.5f - eye[1], dist);
        const float off = std::acos(std::min(ahead, 1.0f)) - std::atan(std::max(p.halfX, p.halfZ) / dist) + heightWeight * std::abs(up - pitch);
        if (off < angle) {
            angle = off;
            height = up;
            best = p.word;
        }
    }
    return best ? best : "";
}

void pressedA(float x, float z, float facingX, float facingZ) {
    std::shared_ptr<const NavMap> nav;
    {
        std::lock_guard lk(sNavM);
        nav = sNav;
    }
    if (!nav || nav->map != int(sMap)) return;
    const float facing = std::hypot(facingX, facingZ);
    if (facing < 1e-3f) return;
    const Furniture* best = nullptr;
    float bestGap = kTouchFurniture;
    for (const Furniture& p : nav->furniture) {
        if (p.room != sRoomNumber || std::abs(p.y - sLuigi[1]) > kLevelGap) continue;
        const float gap = std::hypot(std::max(std::abs(p.x - x) - p.halfX, 0.0f), std::max(std::abs(p.z - z) - p.halfZ, 0.0f));
        const float dx = p.x - x, dz = p.z - z, dist = std::max(std::hypot(dx, dz), 1.0f);
        if (gap < bestGap && (dx * facingX + dz * facingZ) / (dist * facing) > kFacingFurniture) {
            bestGap = gap;
            best = &p;
        }
    }
    if (best) say(best->word, false);
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
        const std::vector<Object> furniture = furnitureObjects();
        objects.insert(objects.end(), furniture.begin(), furniture.end());
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
