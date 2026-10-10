// Model viewer: draws disc models and whole maps with the game's own renderers (model_viewer.h).
//
// Host side (ImGui thread, guest parked): scans the whole disc with Aurora's disc reader
// (a few archives per frame), decompresses (Yay0/Yaz0) and expands RARC archives at any
// depth, and classifies every file by its contents:
//   .mdl  S3DModel character/object models (magic 04B40000)
//   .bin  room / furniture / door models of the room renderer (version byte 2)
//   .bmd  J3D models (effects, items, sky boxes)
// Maps come from the game's own map descriptors (lbl_802FF218: room files) and the map
// archive's jmp/furnitureinfo (furniture model, room, position, rotation, scale).
//
// Guest side (end of frame, LMChangeFrameBuffer hook): the files are copied into a private
// JKRExpHeap above MEM1 (0x82000000+, host-backed and unused by the game; the host window is
// 64 MiB, ctx.h) and set up and drawn with the game's code:
//   .mdl  relocation fn_80061B40 / .key fn_80061A24, S3DModel (__ct__8S3DModelFll), then as the
//         file-select screen draws Luigi (fn_800494E4): lighting fn_80057F90, calcJoints
//         fn_800589B0, calcView fn_80059710, opaque fn_80059C54 / translucent fn_80059F6C.
//         Ghosts: the game's ghost layer (fn_8000C2D0): ghost lighting fn_800581E8, the ghost's
//         alpha as destination alpha, the half-resolution layer copy fn_8005F498 and its
//         additive composite fn_8005F280 over the restored scene fn_8005F518/fn_8005F5B0.
//   .bin  graph object (0x7C, ctor fn_8001CF64), setup fn_8001D3F4 (relocation + node
//         matrices), placement through the node matrix functions fn_8001D96C / fn_8001CFB4,
//         drawn by fn_80015CF4 (passes 0/1 opaque, 2 translucent) with the view matrix in
//         lbl_803A4168, exactly as rooms (fn_8001798C) and furniture (fn_8001F098) are drawn,
//         but without the portal list (lbl_803A4470) and its scissor rectangles.
//   .bmd  J3DModelLoaderDataBase::load, J3DModel, two J3DDrawBuffers (fn_801BFF44), then
//         calc fn_801C7814 (which also enters the packets), viewCalc, J3DSys::drawInit and the draw
//         buffers (fn_801C043C), as the sky box (fn_8002FA34) and the effects draw them.
// Nothing is culled: no portal culling, no frustum or distance tests (the game has none in
// these paths), and the projection's near/far planes are fitted around the whole model.
#include "recomp/model_viewer.h"

#include "recomp/dev_tools.h"
#include "recomp/efb_copy.h"
#include "recomp/guest_util.h"
#include "recomp/gx_bridge.h"
#include "recomp/hle.h"
#include "recomp/runtime.h"
#include "recomp/version.h"

#include <dolphin/dvd.h>
#include <dolphin/gx/GXAurora.h>
#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace recomp::viewer {
namespace {

// ---- bytes, archives (host) ------------------------------------------------------------

u32 be32(const u8* p) { return u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3]; }
u16 be16(const u8* p) { return u16(p[0] << 8 | p[1]); }
float bef(const u8* p) { return std::bit_cast<float>(be32(p)); }

std::vector<u8> decompress(const std::vector<u8>& in) {
    if (in.size() < 16) return in;
    const bool yay0 = std::memcmp(in.data(), "Yay0", 4) == 0, yaz0 = std::memcmp(in.data(), "Yaz0", 4) == 0;
    if (!yay0 && !yaz0) return in;
    const u32 size = be32(&in[4]);
    std::vector<u8> out;
    out.reserve(size);
    auto at = [&](size_t i) -> u8 { return i < in.size() ? in[i] : 0; };
    if (yay0) {
        size_t linkOff = be32(&in[8]), chunkOff = be32(&in[12]), maskOff = 16;
        u32 mask = 0;
        int bits = 0;
        while (out.size() < size) {
            if (bits == 0) {
                mask = u32(at(maskOff)) << 24 | u32(at(maskOff + 1)) << 16 | u32(at(maskOff + 2)) << 8 | at(maskOff + 3);
                maskOff += 4;
                bits = 32;
            }
            if (mask & 0x80000000u) {
                out.push_back(at(chunkOff++));
            } else {
                const u16 link = u16(at(linkOff) << 8 | at(linkOff + 1));
                linkOff += 2;
                const size_t dist = (link & 0xFFF) + 1;
                size_t n = link >> 12;
                n = n == 0 ? at(chunkOff++) + 0x12 : n + 2;
                if (dist > out.size()) return {};
                for (size_t k = 0; k < n && out.size() < size; k++) out.push_back(out[out.size() - dist]);
            }
            mask <<= 1;
            bits--;
        }
    } else {
        size_t src = 16;
        while (out.size() < size && src < in.size()) {
            const u8 code = in[src++];
            for (int b = 0; b < 8 && out.size() < size; b++) {
                if (code & (0x80 >> b)) {
                    out.push_back(at(src++));
                } else {
                    const u8 b1 = at(src), b2 = at(src + 1);
                    src += 2;
                    const size_t dist = ((b1 & 0xF) << 8 | b2) + 1;
                    size_t n = b1 >> 4;
                    n = n == 0 ? at(src++) + 0x12 : n + 2;
                    if (dist > out.size()) return {};
                    for (size_t k = 0; k < n && out.size() < size; k++) out.push_back(out[out.size() - dist]);
                }
            }
        }
    }
    return out;
}

bool isRarc(const std::vector<u8>& d) { return d.size() >= 0x40 && std::memcmp(d.data(), "RARC", 4) == 0; }

struct Member {
    std::string path;  // "" for a loose file; "inner.arc/dir/file" inside nested archives
    std::vector<u8> data;
};

// Every file in a RARC archive; members that are archives themselves (compressed or not, any
// extension) are expanded in place, up to a few levels deep.
void rarcFiles(const std::vector<u8>& d, const std::string& prefix, std::vector<Member>& out, int depth) {
    if (!isRarc(d)) return;
    const u32 dataOff = be32(&d[0x0C]) + 0x20;
    const u32 nodeOff = be32(&d[0x24]) + 0x20, entryOff = be32(&d[0x2C]) + 0x20, strOff = be32(&d[0x34]) + 0x20;
    auto name = [&](u32 off) {
        std::string s;
        for (size_t i = strOff + off; i < d.size() && d[i]; i++) s += char(d[i]);
        return s;
    };
    auto walk = [&](auto&& self, u32 node, const std::string& dir, int guard) -> void {
        if (guard > 16 || nodeOff + node * 16 + 16 > d.size()) return;
        const u8* n = &d[nodeOff + node * 16];
        const u16 count = be16(n + 10);
        const u32 first = be32(n + 12);
        for (u32 i = first; i < first + count; i++) {
            if (entryOff + i * 20 + 20 > d.size()) return;
            const u8* e = &d[entryOff + i * 20];
            const u32 flagsName = be32(e + 4), a = be32(e + 8), size = be32(e + 12);
            const std::string en = name(flagsName & 0xFFFFFF);
            if (en == "." || en == "..") continue;
            if ((flagsName >> 24) & 0x02) {
                self(self, a, dir + en + "/", guard + 1);
            } else if (size_t(dataOff) + a + size <= d.size()) {
                std::vector<u8> bytes(d.begin() + dataOff + a, d.begin() + dataOff + a + size);
                const std::string p = prefix + dir + en;
                std::vector<u8> plain = decompress(bytes);
                if (depth < 4 && isRarc(plain)) {
                    rarcFiles(plain, p + "/", out, depth + 1);
                } else {
                    out.push_back({p, std::move(plain)});
                }
            }
        }
    };
    walk(walk, 0, "", 0);
}

// A disc file's contents: the members of an archive, or the file itself.
std::vector<Member> expandFile(const std::vector<u8>& raw) {
    std::vector<Member> out;
    std::vector<u8> d = decompress(raw);
    if (isRarc(d)) rarcFiles(d, "", out, 0);
    else out.push_back({"", std::move(d)});
    return out;
}

bool endsWith(const std::string& s, const char* suf) {
    const size_t n = std::strlen(suf);
    return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}
std::string baseName(const std::string& p) {
    const size_t i = p.rfind('/');
    return i == std::string::npos ? p : p.substr(i + 1);
}

std::vector<u8> readDisc(const std::string& path) {
    DVDFileInfo fi{};
    if (!DVDOpen(path.c_str(), &fi)) return {};
    std::vector<u8> buf((fi.length + 31) & ~31u);
    const s32 got = DVDReadPrio(&fi, buf.data(), s32(buf.size()), 0, 2);
    DVDClose(&fi);
    if (got < 0) return {};
    buf.resize(fi.length);
    return buf;
}

// ---- model kinds ------------------------------------------------------------------------

enum class Kind : u8 { Mdl, Graph, J3D };
const char* kindName(Kind k) { return k == Kind::Mdl ? "mdl" : k == Kind::Graph ? "bin" : "bmd"; }

bool isMdl(const std::vector<u8>& d) { return d.size() >= 0x80 && be32(d.data()) == 0x04B40000; }
bool isJ3D(const std::vector<u8>& d) {
    return d.size() >= 0x40 && std::memcmp(d.data(), "J3D2", 4) == 0 &&
           (std::memcmp(d.data() + 4, "bmd", 3) == 0 || std::memcmp(d.data() + 4, "bdl", 3) == 0);
}
// Room renderer model: version byte 2, a node table and sane section offsets.
bool isGraph(const std::string& name, const std::vector<u8>& d) {
    if (!endsWith(name, ".bin") || d.size() < 0x4C || d[0] != 2) return false;
    for (u32 h = 0x0C; h <= 0x44; h += 4)
        if (be32(&d[h]) >= d.size()) return false;
    return be32(&d[0x3C]) != 0 && be32(&d[0x14]) != 0;
}
bool classify(const std::string& name, const std::vector<u8>& d, Kind& k) {
    if (isMdl(d)) k = Kind::Mdl;
    else if (isJ3D(d)) k = Kind::J3D;
    else if (isGraph(name, d)) k = Kind::Graph;
    else return false;
    return true;
}

// ---- bounds (host, from the file data) -------------------------------------------------

struct Box {
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    bool valid() const { return lo[0] <= hi[0]; }
    void add(const float* v) {
        for (int k = 0; k < 3; k++)
            if (std::isfinite(v[k]) && std::fabs(v[k]) < 1e7f) {
                lo[k] = std::min(lo[k], v[k]);
                hi[k] = std::max(hi[k], v[k]);
            }
    }
    void add(const Box& b) {
        if (b.valid()) {
            add(b.lo);
            add(b.hi);
        }
    }
};

// Next section start after `start` among the given header offsets.
u32 nextSection(const std::vector<u8>& d, u32 start, u32 firstHdr, u32 lastHdr) {
    u32 next = u32(d.size());
    for (u32 h = firstHdr; h <= lastHdr; h += 4)
        if (const u32 o = be32(&d[h]); o > start && o < next) next = o;
    return next;
}

Box mdlBounds(const std::vector<u8>& m) {
    Box b;
    const u32 pos = be32(&m[0x48]);
    u32 end = u32(m.size());
    for (u32 off : {0x4Cu, 0x50u, 0x54u, 0x58u, 0x5Cu, 0x60u})
        if (const u32 o = be32(&m[off]); o > pos && o < end) end = o;
    for (u32 p = pos; p + 12 <= end; p += 12) {
        const float v[3] = {bef(&m[p]), bef(&m[p + 4]), bef(&m[p + 8])};
        b.add(v);
    }
    return b;
}

// Positions (s16, or f32 with header 0x48 bit 1) plus the node translations.
Box graphBounds(const std::vector<u8>& d, const float* mtx) {
    Box local;
    const u32 pos = be32(&d[0x14]);
    const bool flt = (be32(&d[0x48]) & 2) != 0;
    const u32 end = nextSection(d, pos, 0x0C, 0x44), stride = flt ? 12 : 6;
    for (u32 p = pos; p + stride <= end; p += stride) {
        float v[3];
        for (int k = 0; k < 3; k++) v[k] = flt ? bef(&d[p + 4 * k]) : float(s16(be16(&d[p + 2 * k])));
        local.add(v);
    }
    const u32 nodes = be32(&d[0x3C]), nodesEnd = nextSection(d, nodes, 0x0C, 0x44);
    float tmax = 0;
    for (u32 n = nodes; n + 0x8C <= nodesEnd; n += 0x8C)
        for (int k = 0; k < 3; k++) tmax = std::max(tmax, std::fabs(bef(&d[n + 0x24 + 4 * k])));
    if (!local.valid()) return local;
    for (int k = 0; k < 3; k++) {
        local.lo[k] -= tmax;
        local.hi[k] += tmax;
    }
    if (!mtx) return local;
    Box out;
    for (int i = 0; i < 8; i++) {
        const float c[3] = {(i & 1) ? local.hi[0] : local.lo[0], (i & 2) ? local.hi[1] : local.lo[1],
                            (i & 4) ? local.hi[2] : local.lo[2]};
        float w[3];
        for (int r = 0; r < 3; r++) w[r] = mtx[r * 4] * c[0] + mtx[r * 4 + 1] * c[1] + mtx[r * 4 + 2] * c[2] + mtx[r * 4 + 3];
        out.add(w);
    }
    return out;
}

// J3D: the VTX1 position array.
Box j3dBounds(const std::vector<u8>& d) {
    Box b;
    for (u32 off = 0x20; off + 8 <= d.size();) {
        const u32 size = be32(&d[off + 4]);
        if (size < 8 || off + size > d.size()) break;
        if (std::memcmp(&d[off], "VTX1", 4) == 0) {
            const u32 fmtOff = off + be32(&d[off + 8]);
            u32 type = 3, frac = 0, cnt = 1;
            bool found = false;
            for (u32 f = fmtOff; f + 16 <= off + size; f += 16) {
                const u32 attr = be32(&d[f]);
                if (attr == 0xFF) break;
                if (attr == 9) {
                    cnt = be32(&d[f + 4]);
                    type = be32(&d[f + 8]);
                    frac = d[f + 12];
                    found = true;
                }
            }
            const u32 data = be32(&d[off + 0x0C]);
            if (!found || !data) break;
            u32 end = size;
            for (u32 i = 1; i < 13; i++)
                if (const u32 o = be32(&d[off + 0x0C + 4 * i]); o > data && o < end) end = o;
            const u32 comps = cnt == 0 ? 2 : 3;
            const u32 csize = type == 4 ? 4 : type >= 2 ? 2 : 1;
            const float scale = 1.0f / float(1u << frac);
            for (u32 p = off + data; p + comps * csize <= off + end; p += comps * csize) {
                float v[3] = {0, 0, 0};
                for (u32 k = 0; k < comps; k++) {
                    const u8* q = &d[p + k * csize];
                    switch (type) {
                    case 0: v[k] = float(q[0]) * scale; break;
                    case 1: v[k] = float(s8(q[0])) * scale; break;
                    case 2: v[k] = float(be16(q)) * scale; break;
                    case 3: v[k] = float(s16(be16(q))) * scale; break;
                    default: v[k] = bef(q); break;
                    }
                }
                b.add(v);
            }
            break;
        }
        off += size;
    }
    return b;
}

Box boundsOf(Kind k, const std::vector<u8>& d) {
    switch (k) {
    case Kind::Mdl: return mdlBounds(d);
    case Kind::Graph: return graphBounds(d, nullptr);
    default: return j3dBounds(d);
    }
}

// ---- 3x4 matrices -----------------------------------------------------------------------

void mtxIdentity(float* m) {
    std::memset(m, 0, 12 * sizeof(float));
    m[0] = m[5] = m[10] = 1.0f;
}
void mtxMul(const float* a, const float* b, float* out) {
    float r[12];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++)
            r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + (j == 3 ? a[i * 4 + 3] : 0.0f);
    std::memcpy(out, r, sizeof(r));
}
// T * Rz * Ry * Rx * S (degrees), the room renderer's node convention (fn_8001D96C).
void mtxTRS(float* m, const float* t, const float* rdeg, const float* s) {
    const float k = 3.14159265f / 180.0f;
    const float cx = std::cos(rdeg[0] * k), sx = std::sin(rdeg[0] * k);
    const float cy = std::cos(rdeg[1] * k), sy = std::sin(rdeg[1] * k);
    const float cz = std::cos(rdeg[2] * k), sz = std::sin(rdeg[2] * k);
    const float rx[12] = {1, 0, 0, 0, 0, cx, -sx, 0, 0, sx, cx, 0};
    const float ry[12] = {cy, 0, sy, 0, 0, 1, 0, 0, -sy, 0, cy, 0};
    const float rz[12] = {cz, -sz, 0, 0, sz, cz, 0, 0, 0, 0, 1, 0};
    const float sc[12] = {s[0], 0, 0, 0, 0, s[1], 0, 0, 0, 0, s[2], 0};
    float a[12];
    mtxMul(rz, ry, a);
    mtxMul(a, rx, a);
    mtxMul(a, sc, m);
    m[3] = t[0];
    m[7] = t[1];
    m[11] = t[2];
}

// ---- catalogue (host) -------------------------------------------------------------------

struct ModelRef {
    std::string member;
    Kind kind;
};
struct Archive {
    std::string path;
    std::vector<ModelRef> models;
    bool ghost = false;  // a ghost actor's model archive (default for "Ghost effects")
};
std::vector<Archive> sCatalogue;
std::vector<std::string> sScanQueue;
size_t sScanPos = 0;
bool sScanListed = false, sScanDone = false;
int sScanModels = 0;
std::set<std::string> sGhostArchives;

void listDisc(const std::string& dir, int depth) {
    DVDDir d{};
    if (!DVDOpenDir(dir.c_str(), &d)) return;
    DVDDirEntry e{};
    std::vector<std::string> subdirs;
    while (DVDReadDir(&d, &e)) {
        if (!e.name) continue;
        const std::string p = (dir == "/" ? "/" : dir + "/") + e.name;
        if (e.isDir) {
            if (std::strcmp(e.name, "CVS") != 0) subdirs.push_back(p);
        } else {
            std::string low = p;
            for (char& ch : low) ch = char(std::tolower(u8(ch)));
            for (const char* ext : {".szp", ".arc", ".bin", ".mdl", ".bmd", ".bdl", ".rarc"})
                if (endsWith(low, ext)) sScanQueue.push_back(p);
        }
    }
    DVDCloseDir(&d);
    if (depth < 6)
        for (const auto& s : subdirs) listDisc(s, depth + 1);
}

// Ghost model archives, from the game's tables: actor types (lbl_8033B590, 0x1C each: model id,
// flags; flag 8 = drawn in the ghost layer, see fn_80060E70) and models (lbl_8034E46C, 0x34
// each, archive path first). Archives also used by non-ghost types (Luigi's) are left out.
void findGhostArchives() {
    if (!sGhostArchives.empty()) return;
    const u32 types = VA(0x8033B590), models = VA(0x8034E46C);
    if (!types || !models) return;
    std::vector<std::string> names;
    for (u32 i = 0; i < 400; i++) {
        const u32 p = rd32(models + i * 0x34);
        if (!gu::inMem1(p) || rd8(p) != '/') break;
        names.emplace_back(reinterpret_cast<const char*>(hostp(p)));
    }
    std::set<std::string> ghost, other;
    for (u32 i = 0; i < 400; i++) {
        const u32 e = types + i * 0x1C, name = rd32(e + 0x18);
        if (!gu::inMem1(name)) break;
        const u32 model = rd32(e);
        if (model >= names.size()) continue;
        ((rd32(e + 8) >> 16) & 8 ? ghost : other).insert(names[model]);
    }
    for (const auto& g : ghost)
        if (!other.count(g)) sGhostArchives.insert(g);
}

// Scans a few disc files per call (the first call lists the disc).
void scanStep(double budgetMs) {
    if (sScanDone) return;
    if (!sScanListed) {
        sScanListed = true;
        listDisc("/", 0);
        std::sort(sScanQueue.begin(), sScanQueue.end());
        sScanQueue.erase(std::unique(sScanQueue.begin(), sScanQueue.end()), sScanQueue.end());
        return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    while (sScanPos < sScanQueue.size()) {
        const std::string& path = sScanQueue[sScanPos++];
        Archive a{path, {}, sGhostArchives.count(path) != 0};
        for (const Member& m : expandFile(readDisc(path))) {
            Kind k;
            if (classify(m.path.empty() ? path : m.path, m.data, k)) a.models.push_back({m.path, k});
        }
        if (!a.models.empty()) {
            sScanModels += int(a.models.size());
            sCatalogue.push_back(std::move(a));
        }
        if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() > budgetMs) return;
    }
    sScanDone = true;
    log("[viewer] catalogue: %d models in %zu of %zu disc files", sScanModels, sCatalogue.size(), sScanQueue.size());
}

// ---- viewer state (host) ----------------------------------------------------------------

// Test aids: LM_DEV_VIEWER=<archive path>[#<model name part>][|<key name part>] opens a model;
// LM_DEV_VIEWER_MAP=<map>[:<room>] opens a map (room -1 or absent = every room).
const char* sEnvArchive = std::getenv("LM_DEV_VIEWER");
const char* sEnvMap = std::getenv("LM_DEV_VIEWER_MAP");
bool sOpen = sEnvArchive != nullptr || sEnvMap != nullptr || std::getenv("LM_DEV_VIEWER_SWEEP") != nullptr;

int sArchiveSel = -1;
std::vector<Member> sMembers;    // the selected archive's files
std::vector<int> sMdls, sKeys;   // indices into sMembers (models of any kind, .key animations)
std::vector<Kind> sMdlKinds;
int sMdlSel = -1, sKeySel = -1;
char sFilter[64] = "";
std::string sStatus;
std::string sLoadedInfo;
int sTab = 0;  // 0 models, 1 maps
bool sSelectMapsTab = false;

struct Camera {
    bool fly = false;
    float yaw = 0.6f, pitch = 0.25f, zoom = 1.0f;  // orbit
    bool spin = true;
    float pos[3] = {};                             // fly
    float speed = 30.0f;
} sCam = [] {
    // Test aid: LM_DEV_VIEWER_CAM=<yaw>,<pitch>,<distance> fixes the orbit camera (no spin).
    Camera cam;
    if (const char* v = std::getenv("LM_DEV_VIEWER_CAM")) {
        std::sscanf(v, "%f,%f,%f", &cam.yaw, &cam.pitch, &cam.zoom);
        cam.spin = false;
    }
    return cam;
}();
struct Anim {
    bool play = true;
    float speed = 1.0f;
} sAnim;

enum class Wire : int { Off, Overlay, Only };
Wire sWire = Wire::Off;
float sWireColor[4] = {1.0f, 0.85f, 0.2f, 1.0f};
bool sTextures = std::getenv("LM_DEV_VIEWER_NOTEX") == nullptr;  // test aid: LM_DEV_VIEWER_NOTEX=1
const bool sGhostFxDefault = std::getenv("LM_DEV_VIEWER_GHOSTFX") == nullptr || std::atoi(std::getenv("LM_DEV_VIEWER_GHOSTFX")) != 0;
bool sGhostFx = sGhostFxDefault;                                 // "Ghost effects" (ghost archives; test aid LM_DEV_VIEWER_GHOSTFX=0)
// Backdrop colour (test aid LM_DEV_VIEWER_BG=RRGGBB). Ghosts are added to what is behind them:
// on a dark backdrop they glow as in the dark mansion.
float sBackdrop[3] = {0x30 / 255.0f, 0x34 / 255.0f, 0x3C / 255.0f};
const bool sBackdropInit = [] {
    if (const char* v = std::getenv("LM_DEV_VIEWER_BG")) {
        const u32 rgb = u32(std::strtoul(v, nullptr, 16));
        for (int k = 0; k < 3; k++) sBackdrop[k] = float((rgb >> (16 - 8 * k)) & 0xFF) / 255.0f;
    }
    return true;
}();
bool sTwoSided = false;                                          // "Back faces": no back-face culling (Aurora patch 0011)
float sGhostColor[4] = {0x40 / 255.0f, 0x40 / 255.0f, 0x40 / 255.0f, 1.0f};  // ghost actor +0xE9 (ambient; 404040FF in play)
float sGhostAlpha = 1.0f;                                        // ghost actor +0xE3 (layer weight; FF when fully visible)
bool sIsGhost = false;                                           // loaded model is a ghost

// Maps.
int sMapSel = 2, sRoomSel = -1;  // -1 = all rooms
bool sFurniture = true;
struct RoomName {
    int index;
    std::string path;
};
std::vector<RoomName> sRooms;  // rooms of sMapSel (from the game's map descriptor)
int sRoomsOfMap = -1;

// Host -> guest load request (the guest is parked while the host runs: plain fields suffice).
struct LoadItem {
    Kind kind;
    std::vector<u8> data, key;
    float mtx[12];
    bool placed = false;
    std::string name;
};
struct Pending {
    bool load = false;
    std::vector<LoadItem> items;
    Box bounds;
    bool fly = false;
    std::string label;
} sPending;

// ---- guest side -------------------------------------------------------------------------

constexpr u32 kScratch = 0x81810000;  // matrices, colours
constexpr u32 kHeapObj = 0x81811000;  // JKRExpHeap object
// 0x81F00000-0x81FF0000 holds early Luigi's model copies (early_luigi.cpp): the heap starts
// above them, in the part of the 64 MiB host window that only the viewer uses.
constexpr u32 kHeapMem = 0x82000000;
constexpr u32 kHeapEnd = 0x83FF0000;  // host window: ctx.h kHostMemSize
static_assert(kHeapEnd <= 0x80000000u + kHostMemSize);
u32 sHeap = 0;

struct Drawable {
    Kind kind;
    u32 obj = 0;                // S3DModel / graph object / J3DModel
    u32 key = 0;                // S3DModel: .key animation
    u32 bufOpa = 0, bufXlu = 0; // J3D draw buffers
    u32 mtx = 0;                // guest copy of the placement matrix (graph root / J3D base)
    bool bindPose = false;      // S3DModel without animation: joints fixed at load
    std::string name;
};
std::vector<Drawable> sDraw;
u32 sNumFrames = 0;
float sFrame = 0.0f;
float sCenter[3] = {}, sRadius = 100.0f;
std::atomic<u32> sGuestFrames{0};
std::string sLoadResult;  // last load: "" = ok, else the failure

u32 sHeapUsed = 0;  // bytes allocated since the last freeAll
u32 heapAlloc(Ctx& c, u32 size) {
    const u32 a = gu::call(c, GU_SYM("alloc__7JKRHeapFUliP7JKRHeap"), {size, 32, sHeap});
    if (a) sHeapUsed += (size + 31) & ~31u;
    return a;
}

bool ensureHeap(Ctx& c) {
    if (sHeap) return true;
    const u32 kSystem = VA(0x804D94F0), kCurrent = VA(0x804D94F4), kRoot = VA(0x804D94F8);
    const u32 sys = rd32(kSystem), cur = rd32(kCurrent), root = rd32(kRoot);
    if (!gu::inMem1(root)) return false;
    std::memset(hostp(kHeapObj), 0, 0x1000);  // the heap object only (early Luigi lives below kHeapMem)
    gu::call(c, GU_SYM("__ct__10JKRExpHeapFPvUlP7JKRHeapb"), {kHeapObj, kHeapMem, kHeapEnd - kHeapMem, root, 0});
    wr32(kSystem, sys);  // the JKRHeap constructor may adopt a new heap as system/current heap
    wr32(kCurrent, cur);
    sHeap = kHeapObj;
    log("[viewer] model heap %08X: %u KiB", kHeapMem, (kHeapEnd - kHeapMem) / 1024);
    return true;
}

// Runs `f` with the viewer heap as JKRHeap::sCurrentHeap (operator new inside game code).
template <typename F> void withViewerHeap(F&& f) {
    const u32 kCurrent = VA(0x804D94F4);
    const u32 cur = rd32(kCurrent);
    wr32(kCurrent, sHeap);
    f();
    wr32(kCurrent, cur);
}

void putMtx(u32 at, const float* m, int n) {
    for (int i = 0; i < n; i++) gu::wrf(at + 4 * i, m[i]);
}

u32 copyIn(Ctx& c, const std::vector<u8>& bytes) {
    const u32 a = heapAlloc(c, u32(std::max<size_t>(bytes.size(), 32)));
    if (a) std::memcpy(hostp(a), bytes.data(), bytes.size());
    return a;
}

// A key with no channels; S3DModel draws nothing without a key (fn_80059C54 checks it).
std::vector<u8> emptyKey(u32 numJoints) {
    std::vector<u8> k(0x20 + std::max<u32>(numJoints * 0x24, 64) + 64, 0);
    auto put32 = [&](u32 off, u32 v) {
        for (int i = 0; i < 4; i++) k[off + i] = u8(v >> (24 - 8 * i));
    };
    put32(0x00, numJoints);
    k[0x07] = 1;  // one frame
    for (u32 off = 0x0C; off <= 0x1C; off += 4) put32(off, 0x20);
    return k;
}

bool loadMdl(Ctx& c, const LoadItem& it, Drawable& d, Box& bounds) {
    const u32 numJoints = be16(&it.data[0x08]);
    const bool anim = !it.key.empty();
    const std::vector<u8> key = anim ? it.key : emptyKey(numJoints);
    const u32 mdl = copyIn(c, it.data), k = copyIn(c, key), obj = heapAlloc(c, 0x114);
    if (!mdl || !k || !obj) return false;
    gu::call(c, GU_SYM("fn_80061B40"), {mdl});
    gu::call(c, GU_SYM("fn_80061A24"), {k});
    withViewerHeap([&] { gu::call(c, GU_SYM("__ct__8S3DModelFll"), {obj, mdl, 0}); });  // matrix arrays via operator new
    gu::call(c, GU_SYM("fn_8005AA3C"), {obj, k});
    const u32 joints = rd32(obj + 0x14), invBind = rd32(mdl + 0x38);
    if (anim) {
        gu::call(c, GU_SYM("fn_8005AA34"), {obj}, {0.0});
        gu::call(c, GU_SYM("fn_800589B0"), {obj, 0});
    } else {
        // No animation: the bind pose. Joint = inverse(inverse bind matrix), then the envelope
        // matrices from them (fn_80058850, the second half of calcJoints fn_800589B0); the
        // joints are not recalculated per frame.
        for (u32 j = 0; j < numJoints; j++) {
            const u32 m = joints + j * 0x30;
            if (invBind) gu::call(c, GU_SYM("PSMTXInverse"), {invBind + j * 0x30, m});
            else gu::call(c, GU_SYM("PSMTXIdentity"), {m});
            // Parts that are collapsed at rest and only scaled up by their animation (the
            // portrait ghosts' oba??g? effect meshes are bound at scale 1/100000): unit scale.
            for (int col = 0; col < 3; col++) {
                float v[3], len = 0;
                for (int r = 0; r < 3; r++) len += (v[r] = gu::rdf(m + (r * 4 + col) * 4)) * v[r];
                len = std::sqrt(len);
                if (len > 0.0f && len < 1e-3f)
                    for (int r = 0; r < 3; r++) gu::wrf(m + (r * 4 + col) * 4, v[r] / len);
            }
        }
        gu::call(c, GU_SYM("fn_80058850"), {obj});
        d.bindPose = true;
    }
    // Frame from the vertices and the joints at frame 0 (animated models are often posed far
    // from their rest positions, cutscene models in world space).
    for (u32 j = 0; j < numJoints; j++) {
        const float p[3] = {gu::rdf(joints + j * 0x30 + 0x0C), gu::rdf(joints + j * 0x30 + 0x1C), gu::rdf(joints + j * 0x30 + 0x2C)};
        bounds.add(p);
    }
    d.obj = obj;
    d.key = k;
    const u32 frames = anim ? std::max<u32>(1, be16(&key[6])) : 1;
    sNumFrames = std::max<u32>(sNumFrames, frames);
    sLoadedInfo = std::to_string(numJoints) + " joints, " + (anim ? std::to_string(frames) + " frames" : "bind pose");
    return true;
}

// Node matrices of a graph object under a root matrix, as fn_8001D3F4 does with identity.
void placeGraph(Ctx& c, u32 obj, u32 root) {
    const u32 node0 = rd32(obj + 0x3C);
    gu::call(c, GU_SYM("PSMTXCopy"), {root, node0 + 0x54});
    gu::call(c, GU_SYM("fn_8001D96C"), {obj, 0, node0 + 0x54});
    gu::call(c, GU_SYM("fn_8001CFB4"), {obj, 0, u32(s32(s16(rd16(node0 + 4)))), root});
    gu::call(c, GU_SYM("fn_8001CFB4"), {obj, 0, u32(s32(s16(rd16(node0 + 2)))), node0 + 0x54});
}

bool loadGraph(Ctx& c, const LoadItem& it, Drawable& d) {
    const u32 data = copyIn(c, it.data), obj = heapAlloc(c, 0x7C);
    if (!data || !obj) return false;
    std::memset(hostp(obj), 0, 0x7C);
    gu::call(c, GU_SYM("fn_8001CF64"), {obj});
    gu::call(c, GU_SYM("fn_8001D3F4"), {obj, data, u32(it.data.size())});
    if (rd8(obj) != 2) return false;  // fn_8001D3F4 marks a set-up model with state 2
    if (it.placed) {
        d.mtx = heapAlloc(c, 0x30);
        if (!d.mtx) return false;
        putMtx(d.mtx, it.mtx, 12);
        placeGraph(c, obj, d.mtx);
    }
    d.obj = obj;
    return true;
}

// Joints at rest scale 0 (effects grown only by their .bck: doki, lg_icestate, ftc): unit scale,
// as for .mdl parts bound at a vanishing scale. JNT1 entries are 0x40 bytes, scale at +4.
std::vector<u8> unitJointScales(std::vector<u8> d) {
    for (u32 off = 0x20; off + 0x10 <= d.size();) {
        const u32 size = be32(&d[off + 4]);
        if (size < 8 || off + size > d.size()) break;
        if (std::memcmp(&d[off], "JNT1", 4) == 0) {
            const u32 count = be16(&d[off + 8]), entries = off + be32(&d[off + 0x0C]);
            for (u32 i = 0; i < count && entries + i * 0x40 + 0x10 <= d.size(); i++)
                for (u32 k = 0; k < 3; k++) {
                    u8* p = &d[entries + i * 0x40 + 4 + 4 * k];
                    if (std::fabs(bef(p)) < 1e-3f) p[0] = 0x3F, p[1] = 0x80, p[2] = 0, p[3] = 0;  // 1.0f
                }
            break;
        }
        off += size;
    }
    return d;
}

bool loadJ3D(Ctx& c, const LoadItem& it, Drawable& d) {
    const u32 data = copyIn(c, unitJointScales(it.data));
    if (!data) return false;
    u32 modelData = 0, model = 0, opa = 0, xlu = 0;
    withViewerHeap([&] {
        // Same flags as the map effect models (fn_801730C4).
        modelData = gu::call(c, GU_SYM("load__22J3DModelLoaderDataBaseFPCvUl"), {data, 0x54020000});
        if (!modelData) return;
        model = heapAlloc(c, 0xA0);
        opa = heapAlloc(c, 0x24);
        xlu = heapAlloc(c, 0x24);
        if (!model || !opa || !xlu) return;
        gu::call(c, GU_SYM("__ct__8J3DModelFP12J3DModelDataUlUl"), {model, modelData, 0x10000});
        gu::call(c, GU_SYM("fn_801BFF44"), {opa, 0x400});
        gu::call(c, GU_SYM("fn_801BFF44"), {xlu, 0x400});
    });
    if (!modelData || !model || !opa || !xlu) return false;
    d.obj = model;
    d.bufOpa = opa;
    d.bufXlu = xlu;
    d.mtx = heapAlloc(c, 0x30);
    if (!d.mtx) return false;
    float ident[12];
    mtxIdentity(ident);
    putMtx(d.mtx, it.placed ? it.mtx : ident, 12);
    return true;
}

void loadPending(Ctx& c) {
    sPending.load = false;
    sDraw.clear();
    sNumFrames = 0;
    sFrame = 0.0f;
    sLoadResult.clear();
    if (!ensureHeap(c)) {
        sStatus = sLoadResult = "game heaps not ready";
        return;
    }
    gu::call(c, GU_SYM("freeAll__10JKRExpHeapFv"), {sHeap});
    sHeapUsed = 0;
    int failed = 0;
    std::string firstFail;
    for (const LoadItem& it : sPending.items) {
        Drawable d{it.kind};
        d.name = it.name;
        bool ok = false;
        switch (it.kind) {
        case Kind::Mdl: ok = loadMdl(c, it, d, sPending.bounds); break;
        case Kind::Graph: ok = loadGraph(c, it, d); break;
        case Kind::J3D: ok = loadJ3D(c, it, d); break;
        }
        if (ok) {
            sDraw.push_back(d);
        } else {
            if (!failed++) firstFail = it.name;
        }
    }
    sNumFrames = std::max<u32>(1, sNumFrames);
    const Box& b = sPending.bounds;
    if (b.valid()) {
        for (int k = 0; k < 3; k++) sCenter[k] = (b.lo[k] + b.hi[k]) * 0.5f;
        sRadius = std::max({b.hi[0] - b.lo[0], b.hi[1] - b.lo[1], b.hi[2] - b.lo[2]}) * 0.5f + 1.0f;
    } else {
        sCenter[0] = sCenter[1] = sCenter[2] = 0;
        sRadius = 100.0f;
    }
    if (failed) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%d of %zu failed to load (first: %s)", failed, sPending.items.size(), firstFail.c_str());
        sLoadResult = buf;
    }
    if (sPending.items.size() != 1) sLoadedInfo = std::to_string(sDraw.size()) + " models";
    sStatus = sLoadResult;
    log("[viewer] loaded %s%s: %zu/%zu models, heap %u KiB used, centre %.0f %.0f %.0f radius %.0f%s%s", sPending.label.c_str(),
        sIsGhost ? (sGhostFx ? " [ghost layer]" : " [ghost, plain]") : "",
        sDraw.size(), sPending.items.size(), sHeapUsed / 1024, sCenter[0], sCenter[1], sCenter[2], sRadius,
        sLoadResult.empty() ? "" : " - ", sLoadResult.c_str());
}

void drawBackdrop(Ctx& c) {
    // Orthographic full-screen fill exactly at the far plane (GX depth 0): clears the picture and
    // the depth under the model. (It used to sit at 99.9% of the depth range, which hid every
    // part of a model further than ~1000x the near plane: models "partly vanished" from afar.)
    const float ortho[16] = {2.0f / 640, 0, 0, -1, 0, -2.0f / 480, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1};
    const float ident[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    putMtx(kScratch + 0x000, ortho, 16);
    putMtx(kScratch + 0x040, ident, 12);
    wr32(kScratch + 0x070, u32(sBackdrop[0] * 255) << 24 | u32(sBackdrop[1] * 255) << 16 | u32(sBackdrop[2] * 255) << 8 | 0xFF);
    gu::call(c, GU_SYM("GXSetProjection"), {kScratch, 1});
    gu::call(c, GU_SYM("GXSetViewport"), {}, {0, 0, 640, 480, 0, 1});
    gu::call(c, GU_SYM("GXSetScissor"), {0, 0, 640, 480});
    gu::call(c, GU_SYM("GXLoadPosMtxImm"), {kScratch + 0x40, 0});
    gu::call(c, GU_SYM("GXSetCurrentMtx"), {0});
    gu::call(c, GU_SYM("GXSetNumChans"), {1});
    gu::call(c, GU_SYM("GXSetChanCtrl"), {4, 0, 0, 0, 0, 0, 2});
    gu::call(c, GU_SYM("GXSetChanMatColor"), {4, kScratch + 0x70});
    gu::call(c, GU_SYM("GXSetNumTexGens"), {0});
    gu::call(c, GU_SYM("GXSetNumTevStages"), {1});
    gu::call(c, GU_SYM("GXSetNumIndStages"), {0});
    gu::call(c, GU_SYM("GXSetTevDirect"), {0});
    gu::call(c, GU_SYM("GXSetTevOrder"), {0, 0xFF, 0xFF, 4});
    gu::call(c, GU_SYM("GXSetTevOp"), {0, 4});
    gu::call(c, GU_SYM("GXSetBlendMode"), {0, 1, 0, 0});
    gu::call(c, GU_SYM("GXSetAlphaCompare"), {7, 0, 0, 7, 0});
    gu::call(c, GU_SYM("GXSetZMode"), {1, 7, 1});
    gu::call(c, GU_SYM("GXSetCullMode"), {0});
    gu::call(c, GU_SYM("GXSetColorUpdate"), {1});
    gu::call(c, GU_SYM("GXSetAlphaUpdate"), {0});
    gu::call(c, GU_SYM("GXSetDstAlpha"), {0, 0});
    gu::call(c, GU_SYM("GXSetZCompLoc"), {1});
    gu::call(c, GU_SYM("GXSetFog"), {0, kScratch + 0x70}, {0, 0, 0, 0});
    gu::call(c, GU_SYM("GXClearVtxDesc"));
    gu::call(c, GU_SYM("GXSetVtxDesc"), {9, 1});
    gu::call(c, GU_SYM("GXSetVtxAttrFmt"), {0, 9, 1, 4, 0});
    gu::call(c, GU_SYM("GXBegin"), {0x80, 0, 4});
    const float quad[4][3] = {{0, 0, -0.999f}, {640, 0, -0.999f}, {640, 480, -0.999f}, {0, 480, -0.999f}};
    for (const auto& v : quad)
        for (float f : v) mmio_wr(0xCC008000, std::bit_cast<u32>(f), 4);
}

// View (kScratch+0x140) and projection (kScratch+0x100) for this frame.
void setupCamera(Ctx& c) {
    float eye[3], f[3];
    float nearZ, farZ;
    if (sCam.fly) {
        std::memcpy(eye, sCam.pos, sizeof(eye));
        f[0] = std::cos(sCam.pitch) * std::sin(sCam.yaw);
        f[1] = std::sin(sCam.pitch);
        f[2] = -std::cos(sCam.pitch) * std::cos(sCam.yaw);
        // Everything loaded stays inside the frustum's depth range from anywhere near it.
        float far2 = 0;
        for (int k = 0; k < 3; k++) far2 += (eye[k] - sCenter[k]) * (eye[k] - sCenter[k]);
        nearZ = 2.0f;
        farZ = std::max(20000.0f, (std::sqrt(far2) + sRadius) * 1.5f);
    } else {
        const float r = sRadius * 3.2f * sCam.zoom;
        const float cy = std::cos(sCam.yaw), sy = std::sin(sCam.yaw), cp = std::cos(sCam.pitch), sp = std::sin(sCam.pitch);
        eye[0] = sCenter[0] + r * cp * sy;
        eye[1] = sCenter[1] + r * sp;
        eye[2] = sCenter[2] + r * cp * cy;
        for (int k = 0; k < 3; k++) f[k] = sCenter[k] - eye[k];
        const float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
        for (float& v : f) v /= fl;
        // The model's bounds are only an estimate (joint transforms, node placement): keep
        // generous margins so that nothing is clipped at any distance.
        nearZ = std::max(0.5f, std::min(r - sRadius * 2.0f, sRadius * 0.05f));
        farZ = (r + sRadius) * 4.0f;
    }
    // side = normalize(forward x up), up = (0, 1, 0)
    const float sl = std::max(1e-6f, std::sqrt(f[0] * f[0] + f[2] * f[2]));
    const float s[3] = {-f[2] / sl, 0.0f, f[0] / sl};
    const float u[3] = {s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0]};
    const float view[12] = {
        s[0], s[1], s[2], -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]),
        u[0], u[1], u[2], -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]),
        -f[0], -f[1], -f[2], (f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2]),
    };
    const float t = 1.0f / std::tan(0.5f * 50.0f * 3.14159265f / 180.0f);
    const float aspect = 4.0f / 3.0f;
    const float proj[16] = {t / aspect, 0, 0, 0, 0, t, 0, 0, 0, 0, -nearZ / (farZ - nearZ), -(farZ * nearZ) / (farZ - nearZ),
                            0, 0, -1, 0};
    putMtx(kScratch + 0x100, proj, 16);
    putMtx(kScratch + 0x140, view, 12);
}

void loadProjection(Ctx& c) {
    gu::call(c, GU_SYM("GXSetProjection"), {kScratch + 0x100, 0});
    gu::call(c, GU_SYM("GXSetViewport"), {}, {0, 0, 640, 480, 0, 1});
    gu::call(c, GU_SYM("GXSetScissor"), {0, 0, 640, 480});
}

// S3DModel: joints, view matrices, then the opaque and translucent passes.
void drawMdl(Ctx& c, const Drawable& d, bool ghost) {
    if (!d.bindPose) {
        gu::call(c, GU_SYM("fn_8005AA34"), {d.obj}, {sFrame});
        gu::call(c, GU_SYM("fn_800589B0"), {d.obj, 0});
    }
    gu::call(c, GU_SYM("fn_80059710"), {d.obj, kScratch + 0x140});
    if (ghost) {
        // fn_80060E70 pass 0x30 (opaque) and 0x130 (translucent) for a ghost type: ghost
        // lighting with the actor's colour, its alpha as destination alpha.
        const u32 col = u32(sGhostColor[0] * 255) << 24 | u32(sGhostColor[1] * 255) << 16 | u32(sGhostColor[2] * 255) << 8 |
                        u32(sGhostColor[3] * 255);
        const u32 alpha = u32(std::clamp(sGhostAlpha, 0.0f, 1.0f) * 255.0f);
        wr32(kScratch + 0x74, col);
        gu::call(c, GU_SYM("fn_8005DB10"));
        gu::call(c, GU_SYM("fn_800581E8"), {kScratch + 0x74});
        gu::call(c, GU_SYM("GXSetDstAlpha"), {1, alpha});
        gu::call(c, GU_SYM("fn_8005832C"), {1});
        gu::call(c, GU_SYM("fn_80059C54"), {d.obj});
        gu::call(c, GU_SYM("fn_800581E8"), {kScratch + 0x74});
        gu::call(c, GU_SYM("GXSetDstAlpha"), {1, alpha});
        gu::call(c, GU_SYM("fn_8005832C"), {1});
        gu::call(c, GU_SYM("fn_80059F6C"), {d.obj});
        gu::call(c, GU_SYM("GXSetDstAlpha"), {1, 0xFF});
        return;
    }
    // Lighting as on the file-select screen (mode 0 of fn_80057F90).
    const u32 kLightMode = VA(0x804D07C8);
    const u32 savedMode = rd32(kLightMode);
    wr32(kLightMode, 0);
    gu::call(c, GU_SYM("fn_80057F90"), {0});
    gu::call(c, GU_SYM("fn_8005DB10"));
    gu::call(c, GU_SYM("fn_8005832C"), {1});
    gu::call(c, GU_SYM("fn_80059C54"), {d.obj});
    gu::call(c, GU_SYM("fn_8005832C"), {0});
    gu::call(c, GU_SYM("fn_80059F6C"), {d.obj});
    wr32(kLightMode, savedMode);
}

// Room renderer state that fn_80015844 reads (lbl_ variables in .sdata/.sbss).
struct GraphState {
    u32 view = VA(0x803A4168);       // camera matrix the node matrices are concatenated with
    u32 silhouette = VA(0x804D8230); // u8: flat silhouette pass (0 = normal)
    u32 mode = VA(0x804D81B4);       // s8: 0 = node flags 4/8/0x80 apply their overrides
    u32 portalFlag = VA(0x804D81B6); // u8: set per portal by fn_8001798C
};

// Room renderer lighting: its materials light channel 0 with the light mask in lbl_804D0044
// (GX_LIGHT0) and the ambient register, both set by the room light code in play. The viewer
// loads its own: an ambient level and a light at the camera.
float sRoomAmbient = 0.45f, sRoomLight = 0.8f;
void setupGraphLighting(Ctx& c) {
    const u32 kObj = kScratch + 0x200, kCol = kScratch + 0x78;
    const u8 a = u8(std::clamp(sRoomAmbient, 0.0f, 1.0f) * 255.0f), l = u8(std::clamp(sRoomLight, 0.0f, 1.0f) * 255.0f);
    wr32(kCol, u32(a) << 24 | u32(a) << 16 | u32(a) << 8 | 0xFF);
    gu::call(c, GU_SYM("GXSetChanAmbColor"), {4, kCol});
    wr32(kCol + 4, u32(l) << 24 | u32(l) << 16 | u32(l) << 8 | 0xFF);
    gu::call(c, GU_SYM("GXInitLightPos"), {kObj}, {0.0, 0.0, 0.0});
    gu::call(c, GU_SYM("GXInitLightColor"), {kObj, kCol + 4});
    gu::call(c, GU_SYM("GXInitLightAttn"), {kObj}, {1.0, 0.0, 0.0, 1.0, 0.0, 0.0});
    gu::call(c, GU_SYM("GXLoadLightObjImm"), {kObj, 1});
}

void drawGraphs(Ctx& c, int pass) {
    static const GraphState g;
    setupGraphLighting(c);
    u32 savedView[12];
    for (int i = 0; i < 12; i++) savedView[i] = rd32(g.view + 4 * i);
    const u8 sil = rd8(g.silhouette), mode = rd8(g.mode), portal = rd8(g.portalFlag);
    std::memcpy(hostp(g.view), hostp(kScratch + 0x140), 0x30);
    wr8(g.silhouette, 0);
    wr8(g.mode, 1);  // no state overrides (dark-room fades etc.): the model as authored
    wr8(g.portalFlag, 0);
    for (const Drawable& d : sDraw) {
        if (d.kind != Kind::Graph) continue;
        if (pass == 0) {
            gu::call(c, GU_SYM("fn_80015CF4"), {d.obj, 0, 0, 0xFF});
            gu::call(c, GU_SYM("fn_80015CF4"), {d.obj, 0, 1, 0xFF});
        } else {
            gu::call(c, GU_SYM("GXSetZMode"), {1, 3, 0});
            gu::call(c, GU_SYM("fn_80015CF4"), {d.obj, 0, 2, 0xFF});
            gu::call(c, GU_SYM("GXSetZMode"), {1, 3, 1});
        }
    }
    std::memcpy(hostp(g.view), savedView, 0x30);
    wr8(g.silhouette, sil);
    wr8(g.mode, mode);
    wr8(g.portalFlag, portal);
}

void drawJ3Ds(Ctx& c) {
    const u32 sys = VA(0x804C8898);  // j3dSys: +0 view matrix, +0x44/+0x48 draw buffers, +0x4C draw mode, +0x50 mode mask
    bool any = false;
    for (const Drawable& d : sDraw) any |= d.kind == Kind::J3D;
    if (!any) return;
    u32 saved[0x54 / 4];
    for (int i = 0; i < 0x54 / 4; i++) saved[i] = rd32(sys + 4 * i);
    // J3DMatPacket::draw only draws when (mode & mask) != 0; the game changes the mask per pass.
    wr32(sys + 0x50, 0xFFFFFFFF);
    for (const Drawable& d : sDraw) {
        if (d.kind != Kind::J3D) continue;
        gu::call(c, GU_SYM("J3DPSMtx34Copy__FPA4_fPA4_f"), {d.mtx, d.obj + 0x1C});
        gu::call(c, GU_SYM("J3DPSMtx34Copy__FPA4_fPA4_f"), {kScratch + 0x140, sys});
        wr32(sys + 0x44, d.bufOpa);
        wr32(sys + 0x48, d.bufXlu);
        gu::call(c, GU_SYM("fn_801C7814"), {d.obj});  // calc (joint calc enters the packets)
        gu::call(c, GU_SYM("viewCalc__8J3DModelFv"), {d.obj});
        gu::call(c, GU_SYM("drawInit__6J3DSysFv"), {sys});
        gu::call(c, GU_SYM("fn_800AAF5C"));  // the game's J3D light
        wr32(sys + 0x4C, 3);
        gu::call(c, GU_SYM("fn_801C043C"), {d.bufOpa});
        wr32(sys + 0x4C, 4);
        gu::call(c, GU_SYM("fn_801C043C"), {d.bufXlu});
        gu::call(c, GU_SYM("fn_801C0050"), {d.bufOpa});
        gu::call(c, GU_SYM("fn_801C0050"), {d.bufXlu});
    }
    for (int i = 0; i < 0x54 / 4; i++) wr32(sys + 4 * i, saved[i]);
    gu::call(c, GU_SYM("GXSetCurrentMtx"), {0});
}

// Every loaded model once (the wireframe pass repeats this).
void drawAll(Ctx& c, bool ghostLayerOnly) {
    loadProjection(c);
    gu::call(c, GU_SYM("GXSetZMode"), {1, 3, 1});
    gx::setUntextured(!sTextures);
    // The S3D lights (fn_80057CFC / fn_80057E40, also the ghost glow lights of fn_800581E8)
    // are placed relative to the game camera's view matrix (lbl_803A3820+0x18C): give them the
    // viewer's camera, or ghosts are lit from wherever the game's camera happens to be.
    const u32 gameView = VA(0x803A3820) + 0x18C;
    u8 savedView[0x30];
    std::memcpy(savedView, hostp(gameView), 0x30);
    std::memcpy(hostp(gameView), hostp(kScratch + 0x140), 0x30);
    for (const Drawable& d : sDraw)
        if (d.kind == Kind::Mdl) drawMdl(c, d, ghostLayerOnly);
    std::memcpy(hostp(gameView), savedView, 0x30);
    if (!ghostLayerOnly) {
        drawGraphs(c, 0);
        drawJ3Ds(c);
        drawGraphs(c, 1);
    }
    gx::setUntextured(false);
}

// Ghosts as in the game scene (fn_8000C2D0 + fn_8005F5B0/fn_8005F280): the ghost is drawn into
// a cleared EFB with its alpha as destination alpha, copied at half resolution, and added
// back over the restored scene weighted by that alpha: the soft glowing see-through look.
u32 sGhostLayer = 0, sGhostScene = 0;  // ghost layer buffers in the viewer heap (per load)

void drawScene(Ctx& c) {
    GXSetCullOverrideAurora(sTwoSided ? 1 : 0);
    bool anyMdl = false;
    for (const Drawable& d : sDraw) anyMdl |= d.kind == Kind::Mdl;
    const bool ghost = sGhostFx && sIsGhost && anyMdl && sGhostLayer && sGhostScene;
    if (sWire != Wire::Only) {
        if (sWire == Wire::Overlay) GX2SetPolygonOffset(8.0f, 2.0f, 8.0f, 2.0f, 0.0f);
        if (ghost) {
            const u32 kLayer = VA(0x804D86B8), kScene = VA(0x804D86BC);
            const u32 savedLayer = rd32(kLayer), savedScene = rd32(kScene);
            wr32(kLayer, sGhostLayer);
            wr32(kScene, sGhostScene);
            gu::call(c, GU_SYM("fn_8005F518"));  // scene -> copy, clear the EFB
            gu::call(c, GU_SYM("GXSetAlphaUpdate"), {1});
            gu::call(c, GU_SYM("fn_8005831C"), {1});
            gu::call(c, GU_SYM("fn_8005832C"), {1});
            drawAll(c, true);
            gu::call(c, GU_SYM("fn_8005DF84"));
            gu::call(c, GU_SYM("fn_8005831C"), {1});
            gu::call(c, GU_SYM("fn_8005832C"), {1});
            gu::call(c, GU_SYM("GXSetDstAlpha"), {0, 0});
            gu::call(c, GU_SYM("fn_8005F498"));  // half-resolution layer
            gu::call(c, GU_SYM("fn_8005F5B0"));  // scene back
            gu::call(c, GU_SYM("fn_8005F280"));  // + layer weighted by its alpha
            gu::call(c, GU_SYM("GXSetAlphaUpdate"), {0});
            wr32(kLayer, savedLayer);
            wr32(kScene, savedScene);
            // The other kinds (none in ghost archives) still draw normally.
            loadProjection(c);
            drawGraphs(c, 0);
            drawJ3Ds(c);
            drawGraphs(c, 1);
        } else {
            drawAll(c, false);
        }
        if (sWire == Wire::Overlay) GX2SetPolygonOffset(0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    }
    if (sWire != Wire::Off) {
        const u32 col = u32(sWireColor[0] * 255) << 24 | u32(sWireColor[1] * 255) << 16 | u32(sWireColor[2] * 255) << 8 |
                        u32(sWireColor[3] * 255);
        GXSetWireframeAurora(1, col);
        drawAll(c, false);
        GXSetWireframeAurora(0, col);
    }
    GXSetCullOverrideAurora(0);
}

// ---- sweep test (LM_DEV_VIEWER_SWEEP) ---------------------------------------------------
//
// LM_DEV_VIEWER_SWEEP=<path filter>[@<frames per model>] loads every catalogued model in turn
// and logs one line per model ("[viewer-sweep] ok|EMPTY|FAIL ..."): the load result and how
// much of the picture differs from the backdrop. LM_DEV_VIEWER_SHOTS=<dir> also writes a
// 160x120 thumbnail per model; LM_DEV_VIEWER_SWEEP_START=<n> resumes at model n;
// LM_DEV_VIEWER_SWEEP_EXIT=1 quits when done.
struct Sweep {
    bool on = false;
    std::string filter, shotDir;
    int frames = 8;
    std::vector<std::pair<int, int>> list;  // (archive, model)
    size_t pos = 0;
    enum { Idle, Loading, Drawing, Waiting } phase = Idle;
    u32 loadFrame = 0;
    int ok = 0, empty = 0, failed = 0;
    std::string current, result, info;
    double coverage = 0;
    bool bindPose = false;  // retrying an animated model in its bind pose
    bool twoSided = false;  // retrying with back faces (sky domes seen from outside)
    int shots = 0;          // pictures taken of the current load (new pipelines compile asynchronously)
    bool exitWhenDone = false;
} sSweep;
std::atomic<bool> sSweepShotDone{false};

void sweepInit() {
    const char* v = std::getenv("LM_DEV_VIEWER_SWEEP");
    if (!v) return;
    sSweep.on = true;
    std::string s = v;
    if (const size_t at = s.find('@'); at != std::string::npos) {
        sSweep.frames = std::max(3, std::atoi(s.c_str() + at + 1));
        s = s.substr(0, at);
    }
    sSweep.filter = s;
    if (const char* d = std::getenv("LM_DEV_VIEWER_SHOTS")) sSweep.shotDir = d;
    if (const char* st = std::getenv("LM_DEV_VIEWER_SWEEP_START")) sSweep.pos = size_t(std::max(0, std::atoi(st)));
    sSweep.exitWhenDone = std::getenv("LM_DEV_VIEWER_SWEEP_EXIT") != nullptr;
    efbcopy::requestNamedDump("", 1, nullptr);  // sets the readback buffers up before the first real shot
}

// ---- host-side selection --------------------------------------------------------------

void selectArchive(int idx) {
    sArchiveSel = idx;
    sMembers.clear();
    sMdls.clear();
    sMdlKinds.clear();
    sKeys.clear();
    sMdlSel = sKeySel = -1;
    if (idx < 0 || idx >= int(sCatalogue.size())) return;
    const Archive& a = sCatalogue[idx];
    sMembers = expandFile(readDisc(a.path));
    for (int i = 0; i < int(sMembers.size()); i++) {
        Kind k;
        if (classify(sMembers[i].path.empty() ? a.path : sMembers[i].path, sMembers[i].data, k)) {
            sMdls.push_back(i);
            sMdlKinds.push_back(k);
        } else if (endsWith(sMembers[i].path, ".key")) {
            sKeys.push_back(i);
        }
    }
    sStatus = sMdls.empty() ? "no models in this archive" : "";
    sGhostFx = sGhostFxDefault;
}

// Keys that belong with a model: same nested archive (prefix up to the last ".arc/").
bool keyMatches(int key, int mdl) {
    auto scope = [](const std::string& p) {
        const size_t i = p.rfind(".arc/");
        return i == std::string::npos ? std::string() : p.substr(0, i + 5);
    };
    return scope(sMembers[key].path) == scope(sMembers[mdl].path);
}

std::string memberLabel(int i) {
    const std::string& p = sMembers[i].path;
    return p.empty() ? baseName(sCatalogue[sArchiveSel].path) : p;
}

void requestLoad() {
    if (sMdlSel < 0) return;
    sPending = {};
    sPending.load = true;
    LoadItem it;
    it.kind = sMdlKinds[sMdlSel];
    it.data = sMembers[sMdls[sMdlSel]].data;
    it.name = sCatalogue[sArchiveSel].path + (sMembers[sMdls[sMdlSel]].path.empty() ? "" : ":" + sMembers[sMdls[sMdlSel]].path);
    if (it.kind == Kind::Mdl && sKeySel >= 0) it.key = sMembers[sKeys[sKeySel]].data;
    mtxIdentity(it.mtx);
    sPending.bounds = boundsOf(it.kind, it.data);
    sPending.label = it.name;
    sPending.items.push_back(std::move(it));
    sIsGhost = sCatalogue[sArchiveSel].ghost;
    sLoadedInfo.clear();
    if (sCam.fly) {
        sCam.fly = false;
        sCam.zoom = 1.0f;
    }
}

// Selects an archive, a model and an animation: the one containing `keyName` if given,
// else a "wait" pose, else the first key of the model's archive.
void openArchive(int idx, const char* keyName, const char* mdlName = nullptr, int mdlIndex = -1) {
    selectArchive(idx);
    if (sMdls.empty()) return;
    sMdlSel = mdlIndex >= 0 && mdlIndex < int(sMdls.size()) ? mdlIndex : 0;
    if (mdlName)
        for (int m = 0; m < int(sMdls.size()); m++)
            if (memberLabel(sMdls[m]).find(mdlName) != std::string::npos) {
                sMdlSel = m;
                break;
            }
    int first = -1, wait = -1, named = -1;
    if (sMdlKinds[sMdlSel] == Kind::Mdl)
        for (int k = 0; k < int(sKeys.size()); k++) {
            if (!keyMatches(sKeys[k], sMdls[sMdlSel])) continue;
            const std::string& p = sMembers[sKeys[k]].path;
            if (first < 0) first = k;
            if (wait < 0 && p.find("wait") != std::string::npos) wait = k;
            if (named < 0 && keyName && p.find(keyName) != std::string::npos) named = k;
        }
    sKeySel = named >= 0 ? named : wait >= 0 ? wait : first;
    if (keyName && std::strcmp(keyName, "-") == 0) sKeySel = -1;  // "|-": no animation
    requestLoad();
}

// ---- maps -------------------------------------------------------------------------------

// Room files of a map from the game's descriptor (lbl_802FF218[map]: u8 room count, +4 table
// of room file paths, indexed like jmp/furnitureinfo's room_no).
void listRooms(int map) {
    if (sRoomsOfMap == map) return;
    sRoomsOfMap = map;
    sRooms.clear();
    const u32 table = VA(0x802FF218);
    if (!table) return;
    const u32 desc = rd32(table + 4 * u32(map));
    if (!gu::inMem1(desc)) return;
    const u32 count = rd8(desc), names = rd32(desc + 4);
    if (!gu::inMem1(names)) return;
    for (u32 i = 0; i < count; i++) {
        const u32 p = rd32(names + 4 * i);
        if (!gu::inMem1(p)) continue;
        sRooms.push_back({int(i), reinterpret_cast<const char*>(hostp(p))});
    }
}

u32 jmpHash(const char* s) {
    u32 h = 0;
    for (; *s; s++) h = (u32(u8(*s)) + (h << 8)) % 0x1FFFFD9u;
    return h;
}

struct Furniture {
    std::string model;
    int room;
    float pos[3], dir[3], scale[3];
};

// jmp/furnitureinfo of /Map/map<N>.szp (fields located by their name hashes, tools/jmp_dump.py).
std::vector<Furniture> readFurniture(int map) {
    std::vector<Furniture> out;
    std::vector<u8> d;
    for (Member& m : expandFile(readDisc("/Map/map" + std::to_string(map) + ".szp")))
        if (m.path == "jmp/furnitureinfo") d = std::move(m.data);
    if (d.size() < 0x10) return out;
    const u32 n = be32(&d[0]), nf = be32(&d[4]), dataOff = be32(&d[8]), entrySize = be32(&d[12]);
    struct F {
        u32 off = ~0u;
        u8 type = 0;
    };
    auto field = [&](const char* name, u32 altHash = 0) {
        const u32 h = jmpHash(name);
        for (u32 i = 0; i < nf && 0x10 + i * 12 + 12 <= d.size(); i++) {
            const u8* f = &d[0x10 + i * 12];
            if (be32(f) == h || (altHash && be32(f) == altHash)) return F{be16(f + 8), f[11]};
        }
        return F{};
    };
    const F name = field("dmd_name"), room = field("room_no");
    const F pos[3] = {field("pos_x"), field("pos_y"), field("pos_z")};
    const F dir[3] = {field("dir_x"), field("dir_y"), field("dir_z")};
    const F scl[3] = {field("scale_x"), field("scale_y", 0x0E47510), field("scale_z", 0x0E47511)};
    if (name.off == ~0u || room.off == ~0u) return out;
    for (u32 e = 0; e < n; e++) {
        const u32 base = dataOff + e * entrySize;
        if (base + entrySize > d.size()) break;
        Furniture f;
        for (u32 i = base + name.off; i < d.size() && d[i] && i < base + name.off + 32; i++) f.model += char(d[i]);
        f.room = s32(be32(&d[base + room.off]));
        for (int k = 0; k < 3; k++) {
            f.pos[k] = pos[k].off != ~0u ? bef(&d[base + pos[k].off]) : 0.0f;
            f.dir[k] = dir[k].off != ~0u ? bef(&d[base + dir[k].off]) : 0.0f;
            f.scale[k] = scl[k].off != ~0u ? bef(&d[base + scl[k].off]) : 1.0f;
        }
        if (!f.model.empty() && f.model != "(null)") out.push_back(std::move(f));
    }
    return out;
}

// Loads rooms of a map (one, or all with room < 0) with their furniture, fly camera.
void requestMap(int map, int room) {
    listRooms(map);
    sPending = {};
    sPending.load = true;
    sPending.fly = true;
    const std::vector<Furniture> furniture = sFurniture ? readFurniture(map) : std::vector<Furniture>{};
    int missing = 0;
    for (const RoomName& r : sRooms) {
        if (room >= 0 && r.index != room) continue;
        std::vector<Member> files = expandFile(readDisc(r.path));
        const Member* shell = nullptr;
        for (const Member& m : files)
            if (m.path.empty() || m.path == "room.bin") shell = &m;
        if (!shell)  // archives without "room.bin": their first room model
            for (const Member& m : files) {
                Kind k;
                if (classify(m.path, m.data, k) && k == Kind::Graph) {
                    shell = &m;
                    break;
                }
            }
        if (shell && isGraph(shell->path.empty() ? r.path : shell->path, shell->data)) {
            LoadItem it{Kind::Graph, shell->data, {}, {}, false, r.path + (shell->path.empty() ? "" : ":" + shell->path)};
            mtxIdentity(it.mtx);
            sPending.bounds.add(graphBounds(it.data, nullptr));
            sPending.items.push_back(std::move(it));
        }
        for (const Furniture& f : furniture) {
            if (f.room != r.index) continue;
            const Member* m = nullptr;
            for (const Member& x : files)
                if (x.path == f.model + ".bin") m = &x;
            if (!m || !isGraph(m->path, m->data)) {
                missing++;
                continue;
            }
            LoadItem it{Kind::Graph, m->data, {}, {}, true, r.path + ":" + m->path};
            mtxTRS(it.mtx, f.pos, f.dir, f.scale);
            // Framed by the room shells only: some furniture is parked far away from its room.
            sPending.items.push_back(std::move(it));
        }
    }
    char label[96];
    std::snprintf(label, sizeof(label), "map %d %s%s", map, room < 0 ? "all rooms" : ("room " + std::to_string(room)).c_str(),
                  missing ? (" (" + std::to_string(missing) + " furniture models not found)").c_str() : "");
    sPending.label = label;
    sIsGhost = false;
    sLoadedInfo.clear();
    // Start outside the loaded area, looking at its centre.
    const Box& b = sPending.bounds;
    if (b.valid()) {
        float c[3], r = 0;
        for (int k = 0; k < 3; k++) {
            c[k] = (b.lo[k] + b.hi[k]) * 0.5f;
            r = std::max(r, (b.hi[k] - b.lo[k]) * 0.5f);
        }
        sCam.fly = true;
        sCam.spin = false;
        sCam.yaw = 0.0f;
        sCam.pitch = -0.45f;
        const float dist = r * 2.4f + 200.0f;
        sCam.pos[0] = c[0];
        sCam.pos[1] = c[1] + dist * std::sin(0.45f);
        sCam.pos[2] = c[2] + dist * std::cos(0.45f);
        sCam.speed = std::clamp(r / 60.0f, 5.0f, 300.0f);
    }
}

}  // namespace

bool isOpen() { return sOpen; }
void setOpen(bool open) { sOpen = open; }

void guestFrame(Ctx& c) {
    if (!sOpen) return;
    sGuestFrames++;
    if (sPending.load) {
        loadPending(c);
        sGhostLayer = sGhostScene = 0;
        if (sHeap && sIsGhost) {
            // Ghost layer buffers (fn_8005F098 sizes): RGB565 scene copy, half-res RGBA8 layer.
            sGhostScene = heapAlloc(c, 0x96000);
            sGhostLayer = heapAlloc(c, 0x4B000);
        }
        if (sSweep.on && sSweep.phase == Sweep::Loading) {
            sSweep.phase = Sweep::Drawing;
            sSweep.loadFrame = sGuestFrames;
        }
    }
    drawBackdrop(c);
    if (sDraw.empty()) {
        if (sSweep.on && sSweep.phase == Sweep::Drawing) sSweep.phase = Sweep::Waiting, sSweepShotDone = false;
        if (sSweep.on && sSweep.phase == Sweep::Waiting && !sSweepShotDone) {
            sSweep.result = sLoadResult.empty() ? "nothing loaded" : sLoadResult;
            sSweep.coverage = 0;
            sSweepShotDone = true;
        }
        return;
    }
    if (sAnim.play && sNumFrames > 1) {
        sFrame += sAnim.speed;
        if (sFrame >= float(sNumFrames)) sFrame = std::fmod(sFrame, float(sNumFrames));
    }
    if (sCam.spin && !sCam.fly) sCam.yaw = std::remainder(sCam.yaw + 0.01f, 6.2831853f);
    setupCamera(c);
    drawScene(c);
    // Sweep: grab the picture once the model has been drawn for a few frames.
    if (sSweep.on && sSweep.phase == Sweep::Drawing && s32(sGuestFrames - sSweep.loadFrame) + 1 >= sSweep.frames) {
        sSweep.phase = Sweep::Waiting;
        sSweepShotDone = false;
        char name[64];
        std::snprintf(name, sizeof(name), "%04zu", sSweep.pos);
        std::string path;
        if (!sSweep.shotDir.empty()) {
            std::string tag = sSweep.current;
            for (char& ch : tag)
                if (ch == '/' || ch == ':' || ch == '\\' || ch == ' ') ch = '_';
            path = sSweep.shotDir + "/" + name + tag + ".ppm";
        }
        sSweep.result = sLoadResult;
        sSweep.info = sLoadedInfo;
        efbcopy::requestNamedDump(path, 4, [](const u8* rgb, u32 w, u32 h) {
            size_t diff = 0;
            for (size_t i = 0; i < size_t(w) * h; i++) {
                const u8* p = rgb + i * 3;
                bool differs = false;
                for (int k = 0; k < 3; k++) differs |= std::abs(int(p[k]) - int(sBackdrop[k] * 255)) > 2;
                diff += differs;
            }
            sSweep.coverage = 100.0 * double(diff) / double(size_t(w) * h);
            sSweepShotDone = true;
        });
    }
}

void window() {
    static bool sInit = (sweepInit(), true);
    (void)sInit;
    if (!sOpen) return;
    findGhostArchives();
    scanStep(sSweep.on ? 200.0 : 12.0);
    if (sScanDone && sEnvArchive) {
        const std::string spec = sEnvArchive;
        sEnvArchive = nullptr;
        // <archive>[#<model name part>][|<animation name part>]
        const size_t bar = spec.find('|'), hash = spec.find('#');
        const std::string key = bar == std::string::npos ? "" : spec.substr(bar + 1);
        const std::string mdl = hash == std::string::npos ? "" : spec.substr(hash + 1, bar == std::string::npos ? std::string::npos : bar - hash - 1);
        for (int i = 0; i < int(sCatalogue.size()); i++)
            if (sCatalogue[i].path == spec.substr(0, std::min(bar, hash)))
                openArchive(i, key.empty() ? nullptr : key.c_str(), mdl.empty() ? nullptr : mdl.c_str());
    }
    if (sEnvMap && sScanListed) {
        int map = 2, room = -1;
        std::sscanf(sEnvMap, "%d:%d", &map, &room);
        sEnvMap = nullptr;
        sTab = 1;
        sSelectMapsTab = true;
        sMapSel = map;
        sRoomSel = room;
        requestMap(map, room);
    }
    if (const char* w = std::getenv("LM_DEV_VIEWER_WIRE"); w && sWire == Wire::Off) {  // test aid: 1 overlay, 2 only
        static bool sOnce = false;
        if (!sOnce) sWire = Wire(std::clamp(std::atoi(w), 0, 2));
        sOnce = true;
    }

    // Sweep driver.
    if (sSweep.on && sScanDone) {
        if (sSweep.list.empty()) {
            for (int a = 0; a < int(sCatalogue.size()); a++)
                for (int m = 0; m < int(sCatalogue[a].models.size()); m++) {
                    const std::string id = sCatalogue[a].path + ":" + sCatalogue[a].models[m].member;
                    if (sSweep.filter.empty() || id.find(sSweep.filter) != std::string::npos) sSweep.list.push_back({a, m});
                }
            log("[viewer-sweep] %zu models to check, %d frames each", sSweep.list.size(), sSweep.frames);
            sCam.spin = false;
            sCam.yaw = 0.6f;
        }
        if (sSweep.phase == Sweep::Waiting && sSweepShotDone) {
            const bool drawn = sSweep.result.empty() && sSweep.coverage > 0.0;
            if (!drawn && sSweep.result.empty() && ++sSweep.shots < 4) {
                // Aurora compiles new pipelines in the background and skips their draws until
                // they are ready: look again a little later.
                sSweep.phase = Sweep::Drawing;
                sSweep.loadFrame = sGuestFrames + u32(15 - sSweep.frames);
            } else if (!drawn && sSweep.result.empty() && !sSweep.bindPose && sMdlSel >= 0 && sMdlKinds[sMdlSel] == Kind::Mdl &&
                sKeySel >= 0) {
                // Some animations start with parts scaled to nothing: check the bind pose too.
                sSweep.bindPose = true;
                sSweep.shots = 0;
                openArchive(sArchiveSel, "-", nullptr, sMdlSel);
                sSweep.phase = Sweep::Loading;
            } else if (!drawn && sSweep.result.empty() && !sSweep.twoSided) {
                sSweep.twoSided = sTwoSided = true;
                sSweep.shots = 0;
                sSweep.phase = Sweep::Drawing;
                sSweep.loadFrame = sGuestFrames;
            } else {
                log("[viewer-sweep] %4zu %-5s %s  coverage %.2f%%  (%s%s%s)%s%s", sSweep.pos,
                    !sSweep.result.empty() ? "FAIL" : drawn ? "ok" : "EMPTY", sSweep.current.c_str(), sSweep.coverage,
                    sSweep.info.c_str(), sSweep.bindPose ? "; first animation frames empty" : "",
                    sSweep.twoSided ? "; only with back faces" : "",
                    sSweep.result.empty() ? "" : "  ", sSweep.result.c_str());
                if (!sSweep.result.empty()) sSweep.failed++;
                else if (drawn) sSweep.ok++;
                else sSweep.empty++;
                sSweep.pos++;
                sSweep.bindPose = false;
                sSweep.twoSided = sTwoSided = false;
                sSweep.shots = 0;
                sSweep.phase = Sweep::Idle;
            }
        }
        if (sSweep.phase == Sweep::Idle) {
            if (sSweep.pos >= sSweep.list.size()) {
                log("[viewer-sweep] done: %d ok, %d empty, %d failed", sSweep.ok, sSweep.empty, sSweep.failed);
                sSweep.on = false;
                if (sSweep.exitWhenDone) {
                    std::fflush(stdout);
                    std::_Exit(0);
                }
            } else {
                const auto [a, m] = sSweep.list[sSweep.pos];
                const Archive& ar = sCatalogue[a];
                sSweep.current = std::string(kindName(ar.models[m].kind)) + " " + ar.path +
                                 (ar.models[m].member.empty() ? "" : ":" + ar.models[m].member);
                if (sArchiveSel != a) selectArchive(a);
                int sel = -1;
                for (int i = 0; i < int(sMdls.size()); i++)
                    if (sMembers[sMdls[i]].path == ar.models[m].member) sel = i;
                openArchive(a, nullptr, nullptr, sel);
                sSweep.phase = Sweep::Loading;
            }
        }
    }

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + 8, vp->Pos.y + 8), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(400, vp->Size.y * 0.85f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.75f);
    if (!ImGui::Begin("Model viewer (F4)", &sOpen)) {
        ImGui::End();
        return;
    }
    ImGui::TextDisabled("Game input is paused while this window is open.");
    if (!sScanDone)
        ImGui::ProgressBar(sScanQueue.empty() ? 0.0f : float(sScanPos) / float(sScanQueue.size()), ImVec2(-1, 0),
                           ("Scanning disc: " + std::to_string(sScanModels) + " models").c_str());
    else
        ImGui::TextDisabled("%d models in %zu files", sScanModels, sCatalogue.size());

    if (ImGui::BeginTabBar("##viewer")) {
        if (ImGui::BeginTabItem("Models")) {
            sTab = 0;
            ImGui::InputText("Filter", sFilter, sizeof(sFilter));
            if (ImGui::BeginChild("archives", ImVec2(0, ImGui::GetContentRegionAvail().y * 0.35f), ImGuiChildFlags_Borders)) {
                for (int i = 0; i < int(sCatalogue.size()); i++) {
                    const Archive& a = sCatalogue[i];
                    if (sFilter[0] && a.path.find(sFilter) == std::string::npos) continue;
                    char label[300];
                    std::snprintf(label, sizeof(label), "%s  (%zu)%s", a.path.c_str(), a.models.size(), a.ghost ? "  ghost" : "");
                    if (ImGui::Selectable(label, sArchiveSel == i)) openArchive(i, nullptr);
                }
            }
            ImGui::EndChild();
            if (!sMdls.empty()) {
                const std::string cur = sMdlSel >= 0 ? memberLabel(sMdls[sMdlSel]) + " [" + kindName(sMdlKinds[sMdlSel]) + "]" : "";
                if (ImGui::BeginCombo("Model", cur.c_str())) {
                    for (int i = 0; i < int(sMdls.size()); i++) {
                        const std::string l = memberLabel(sMdls[i]) + " [" + kindName(sMdlKinds[i]) + "]";
                        if (ImGui::Selectable(l.c_str(), sMdlSel == i)) openArchive(sArchiveSel, nullptr, nullptr, i);
                    }
                    ImGui::EndCombo();
                }
                if (sMdlSel >= 0 && sMdlKinds[sMdlSel] == Kind::Mdl) {
                    const char* curKey = sKeySel >= 0 ? sMembers[sKeys[sKeySel]].path.c_str() : "(no animation)";
                    if (ImGui::BeginCombo("Animation", curKey)) {
                        if (ImGui::Selectable("(no animation)", sKeySel < 0)) {
                            sKeySel = -1;
                            requestLoad();
                        }
                        for (int i = 0; i < int(sKeys.size()); i++) {
                            if (!keyMatches(sKeys[i], sMdls[sMdlSel])) continue;
                            if (ImGui::Selectable(sMembers[sKeys[i]].path.c_str(), sKeySel == i)) {
                                sKeySel = i;
                                requestLoad();
                            }
                        }
                        ImGui::EndCombo();
                    }
                }
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Maps", nullptr, sSelectMapsTab ? ImGuiTabItemFlags_SetSelected : 0)) {
            sSelectMapsTab = false;
            sTab = 1;
            int n = 0;
            const dev::MapInfo* maps = dev::maps(&n);
            const std::string curMap = sMapSel >= 0 && sMapSel < n ? std::to_string(sMapSel) + "  " + maps[sMapSel].name : "?";
            if (ImGui::BeginCombo("Map", curMap.c_str())) {
                for (int i = 0; i < n; i++)
                    if (ImGui::Selectable((std::to_string(i) + "  " + maps[i].name).c_str(), sMapSel == i)) {
                        sMapSel = i;
                        sRoomSel = -1;
                    }
                ImGui::EndCombo();
            }
            listRooms(sMapSel);
            if (ImGui::BeginChild("rooms", ImVec2(0, ImGui::GetContentRegionAvail().y * 0.35f), ImGuiChildFlags_Borders)) {
                if (ImGui::Selectable("(whole map)", sRoomSel < 0)) sRoomSel = -1;
                for (const RoomName& r : sRooms) {
                    const std::string l = std::to_string(r.index) + "  " + r.path;
                    if (ImGui::Selectable(l.c_str(), sRoomSel == r.index)) sRoomSel = r.index;
                }
            }
            ImGui::EndChild();
            ImGui::Checkbox("Furniture", &sFurniture);
            ImGui::SliderFloat("Ambient", &sRoomAmbient, 0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("Headlight", &sRoomLight, 0.0f, 1.0f, "%.2f");
            ImGui::SameLine();
            if (ImGui::Button("Load")) requestMap(sMapSel, sRoomSel);
            ImGui::TextDisabled("Fly: right-drag to look, WASD/QE to move,\nShift faster, wheel changes speed.");
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    if (!sStatus.empty()) ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "%s", sStatus.c_str());
    if (!sLoadedInfo.empty()) ImGui::Text("%s", sLoadedInfo.c_str());

    ImGui::SeparatorText("Display");
    ImGui::Checkbox("Textures", &sTextures);
    ImGui::SameLine();
    ImGui::Checkbox("Back faces", &sTwoSided);
    ImGui::SameLine();
    ImGui::ColorEdit3("Backdrop", sBackdrop, ImGuiColorEditFlags_NoInputs);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Draw both sides of every triangle (no back-face culling),\ne.g. to see sky domes and rooms from outside.");
    if (sIsGhost) {
        ImGui::SameLine();
        ImGui::Checkbox("Ghost effects", &sGhostFx);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Draw the ghost through the game's ghost layer: ghost lighting, then added\n"
                              "back at half resolution weighted by its alpha (soft, glowing, see-through).\n"
                              "Off: plain opaque model with normal lighting.");
        if (sGhostFx) {
            ImGui::ColorEdit4("Ghost colour", sGhostColor, ImGuiColorEditFlags_NoInputs);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120);
            ImGui::SliderFloat("Alpha", &sGhostAlpha, 0.0f, 1.0f, "%.2f");
        }
    }
    int wire = int(sWire);
    ImGui::SetNextItemWidth(160);
    ImGui::Combo("Wireframe", &wire, "Off\0Over model\0Only\0");
    sWire = Wire(wire);
    if (sWire != Wire::Off) {
        ImGui::SameLine();
        ImGui::ColorEdit4("##wirecol", sWireColor, ImGuiColorEditFlags_NoInputs);
    }

    ImGui::SeparatorText("Camera");
    if (!sCam.fly) {
        ImGui::Checkbox("Spin", &sCam.spin);
        ImGui::SliderFloat("Yaw", &sCam.yaw, -6.2832f, 6.2832f);
        ImGui::SliderFloat("Pitch", &sCam.pitch, -1.5f, 1.5f);
        ImGui::SliderFloat("Distance", &sCam.zoom, 0.05f, 8.0f, "%.2fx", ImGuiSliderFlags_Logarithmic);
        ImGui::TextDisabled("Drag on the picture to orbit, wheel to zoom.");
    } else {
        ImGui::Text("pos %.0f %.0f %.0f", sCam.pos[0], sCam.pos[1], sCam.pos[2]);
        ImGui::SliderFloat("Speed", &sCam.speed, 1.0f, 500.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
    }
    if (!sCam.fly && sNumFrames > 1) {
        ImGui::SeparatorText("Animation");
        ImGui::Checkbox("Play", &sAnim.play);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        ImGui::SliderFloat("Speed##anim", &sAnim.speed, 0.1f, 3.0f, "%.1fx");
        ImGui::SliderFloat("Frame", &sFrame, 0.0f, float(sNumFrames - 1), "%.0f");
    }
    ImGui::End();

    // Mouse/keyboard on the picture (outside ImGui windows).
    ImGuiIO& io = ImGui::GetIO();
    if (sCam.fly) {
        if (!io.WantCaptureMouse) {
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Right) || ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                sCam.yaw += io.MouseDelta.x * 0.005f;
                sCam.pitch = std::clamp(sCam.pitch - io.MouseDelta.y * 0.005f, -1.55f, 1.55f);
            }
            if (io.MouseWheel != 0.0f) sCam.speed = std::clamp(sCam.speed * std::pow(1.2f, io.MouseWheel), 1.0f, 500.0f);
        }
        if (!io.WantCaptureKeyboard) {
            const float sp = sCam.speed * (ImGui::IsKeyDown(ImGuiKey_LeftShift) ? 4.0f : 1.0f);
            const float fw[3] = {std::cos(sCam.pitch) * std::sin(sCam.yaw), std::sin(sCam.pitch),
                                 -std::cos(sCam.pitch) * std::cos(sCam.yaw)};
            const float rt[3] = {std::cos(sCam.yaw), 0.0f, std::sin(sCam.yaw)};
            auto move = [&](const float* dir, float amount) {
                for (int k = 0; k < 3; k++) sCam.pos[k] += dir[k] * amount;
            };
            const float up[3] = {0, 1, 0};
            if (ImGui::IsKeyDown(ImGuiKey_W)) move(fw, sp);
            if (ImGui::IsKeyDown(ImGuiKey_S)) move(fw, -sp);
            if (ImGui::IsKeyDown(ImGuiKey_D)) move(rt, sp);
            if (ImGui::IsKeyDown(ImGuiKey_A)) move(rt, -sp);
            if (ImGui::IsKeyDown(ImGuiKey_E)) move(up, sp);
            if (ImGui::IsKeyDown(ImGuiKey_Q)) move(up, -sp);
        }
    } else if (!io.WantCaptureMouse) {
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            sCam.spin = false;
            sCam.yaw -= io.MouseDelta.x * 0.01f;
            sCam.pitch = std::clamp(sCam.pitch + io.MouseDelta.y * 0.01f, -1.5f, 1.5f);
        }
        if (io.MouseWheel != 0.0f) sCam.zoom = std::clamp(sCam.zoom * std::pow(0.9f, io.MouseWheel), 0.05f, 8.0f);
    }
}

}  // namespace recomp::viewer
