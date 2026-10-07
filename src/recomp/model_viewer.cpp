// Model viewer: draws a disc model with the game's own S3DModel code (see model_viewer.h).
//
// Host side (ImGui thread, guest parked): browses archives with Aurora's disc reader,
// decompresses (Yay0/Yaz0) and lists the RARC members (.mdl models, .key animations,
// nested .arc archives such as game_usa.szp's /model/*.arc).
//
// Guest side (end of frame, LMChangeFrameBuffer hook): the model and animation bytes are
// copied into a private JKRExpHeap placed above MEM1 (0x81810000+, host-backed but unused by
// the game), relocated with the game's loaders (fn_80061B40 .mdl, fn_80061A24 .key), wrapped
// in an S3DModel (__ct__8S3DModelFll) and drawn the way the US file-select screen draws its
// Luigi (fn_800494E4): lighting setup fn_80057F90, calcJoints fn_800589B0, calcView
// fn_80059710, opaque fn_80059C54 / translucent fn_80059F6C.
#include "recomp/model_viewer.h"

#include "recomp/guest_util.h"
#include "recomp/hle.h"
#include "recomp/runtime.h"

#include <dolphin/dvd.h>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace recomp::viewer {
namespace {

// ---- archives (host) ------------------------------------------------------------------

u32 be32(const u8* p) { return u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3]; }
u16 be16(const u8* p) { return u16(p[0] << 8 | p[1]); }

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

struct Member {
    std::string path;
    std::vector<u8> data;
};

// Every file in a RARC archive, with nested .arc archives expanded ("inner.arc/dir/file").
void rarcFiles(const std::vector<u8>& d, const std::string& prefix, std::vector<Member>& out, int depth = 0) {
    if (d.size() < 0x40 || std::memcmp(d.data(), "RARC", 4) != 0) return;
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
                if (depth == 0 && en.size() > 4 && en.compare(en.size() - 4, 4, ".arc") == 0) {
                    rarcFiles(decompress(bytes), p + "/", out, depth + 1);
                } else {
                    out.push_back({p, std::move(bytes)});
                }
            }
        }
    };
    walk(walk, 0, "", 0);
}

bool endsWith(const std::string& s, const char* suf) {
    const size_t n = std::strlen(suf);
    return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
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

// Archives that hold .mdl models (map rooms use their own .bin format and are not listed).
std::vector<std::string> sArchives;
void scanDir(const std::string& dir, int depth) {
    DVDDir d{};
    if (!DVDOpenDir(dir.c_str(), &d)) return;
    DVDDirEntry e{};
    std::vector<std::string> subdirs;
    while (DVDReadDir(&d, &e)) {
        if (!e.name) continue;
        const std::string p = (dir == "/" ? "/" : dir + "/") + e.name;
        if (e.isDir) {
            if (std::strcmp(e.name, "CVS") != 0) subdirs.push_back(p);
        } else if (endsWith(p, ".szp") || endsWith(p, ".arc")) {
            sArchives.push_back(p);
        }
    }
    DVDCloseDir(&d);
    if (depth < 3)
        for (const auto& s : subdirs) scanDir(s, depth + 1);
}
void buildCatalogue() {
    if (!sArchives.empty()) return;
    for (const char* d : {"/model", "/Game", "/Kawano", "/Ajioka"}) scanDir(d, 0);
    sArchives.push_back("/gameboy.szp");
    std::sort(sArchives.begin(), sArchives.end());
}

// ---- viewer state ---------------------------------------------------------------------

// Test aid: LM_DEV_VIEWER=<archive path>[#<model name part>][|<key name part>] opens the viewer on it.
const char* sEnvArchive = std::getenv("LM_DEV_VIEWER");
bool sOpen = sEnvArchive != nullptr;
int sArchiveSel = -1;
std::string sArchiveLoaded;
std::vector<Member> sMembers;
std::vector<int> sMdls, sKeys;  // indices into sMembers
int sMdlSel = -1, sKeySel = -1;
char sFilter[64] = "";
std::string sStatus;

struct Camera {
    float yaw = 0.6f, pitch = 0.25f, zoom = 1.0f;
    bool spin = true;
} sCam;
struct Anim {
    bool play = true;
    float speed = 1.0f;
} sAnim;

// Host -> guest request (guest is parked while the host runs, so plain fields suffice).
struct Pending {
    bool load = false;
    std::vector<u8> mdl, key;
    float center[3] = {}, radius = 100.0f;
} sPending;

// Guest-side model.
constexpr u32 kScratch = 0x81810000;   // matrices, colours
constexpr u32 kHeapObj = 0x81811000;   // JKRExpHeap object
constexpr u32 kHeapMem = 0x81812000;
constexpr u32 kHeapEnd = 0x81FF0000;
u32 sHeap = 0;
u32 sModel = 0, sKey = 0;
u32 sNumFrames = 0;
float sFrame = 0.0f;
float sCenter[3] = {}, sRadius = 100.0f;
std::string sLoadedInfo;

// Bounds from the (still unrelocated) .mdl: f32 xyz positions up to the next array.
bool modelBounds(const std::vector<u8>& m, float center[3], float& radius) {
    if (m.size() < 0x80 || be32(m.data()) != 0x04B40000) return false;
    const u32 pos = be32(&m[0x48]);
    u32 end = u32(m.size());
    for (u32 off : {0x4Cu, 0x50u, 0x54u, 0x58u, 0x5Cu, 0x60u})
        if (const u32 o = be32(&m[off]); o > pos && o < end) end = o;
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    u32 n = 0;
    for (u32 p = pos; p + 12 <= end; p += 12, n++)
        for (int k = 0; k < 3; k++) {
            const float v = std::bit_cast<float>(be32(&m[p + 4 * k]));
            if (!std::isfinite(v)) continue;
            lo[k] = std::min(lo[k], v);
            hi[k] = std::max(hi[k], v);
        }
    if (n == 0 || lo[0] > hi[0]) return false;
    for (int k = 0; k < 3; k++) center[k] = (lo[k] + hi[k]) * 0.5f;
    radius = std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]}) * 0.5f + 1.0f;
    return true;
}

void selectArchive(int idx) {
    sArchiveSel = idx;
    sMembers.clear();
    sMdls.clear();
    sKeys.clear();
    sMdlSel = sKeySel = -1;
    if (idx < 0 || idx >= int(sArchives.size())) return;
    sArchiveLoaded = sArchives[idx];
    rarcFiles(decompress(readDisc(sArchiveLoaded)), "", sMembers);
    for (int i = 0; i < int(sMembers.size()); i++) {
        if (endsWith(sMembers[i].path, ".mdl")) sMdls.push_back(i);
        else if (endsWith(sMembers[i].path, ".key")) sKeys.push_back(i);
    }
    sStatus = sMdls.empty() ? "no .mdl models in this archive" : "";
}

// Keys that belong with a model: same nested archive (prefix up to the last ".arc/").
bool keyMatches(int key, int mdl) {
    auto scope = [](const std::string& p) {
        const size_t i = p.rfind(".arc/");
        return i == std::string::npos ? std::string() : p.substr(0, i + 5);
    };
    return scope(sMembers[key].path) == scope(sMembers[mdl].path);
}

void requestLoad();

// Selects an archive, its first model and an animation: the one containing `keyName` if
// given, else a "wait" pose, else the first key of the model's archive.
void openArchive(int idx, const char* keyName, const char* mdlName = nullptr) {
    selectArchive(idx);
    if (sMdls.empty()) return;
    sMdlSel = 0;
    if (mdlName)
        for (int m = 0; m < int(sMdls.size()); m++)
            if (sMembers[sMdls[m]].path.find(mdlName) != std::string::npos) {
                sMdlSel = m;
                break;
            }
    int first = -1, wait = -1, named = -1;
    for (int k = 0; k < int(sKeys.size()); k++) {
        if (!keyMatches(sKeys[k], sMdls[sMdlSel])) continue;
        const std::string& p = sMembers[sKeys[k]].path;
        if (first < 0) first = k;
        if (wait < 0 && p.find("wait") != std::string::npos) wait = k;
        if (named < 0 && keyName && p.find(keyName) != std::string::npos) named = k;
    }
    sKeySel = named >= 0 ? named : wait >= 0 ? wait : first;
    requestLoad();
}

void requestLoad() {
    if (sMdlSel < 0) return;
    sPending = {};
    sPending.load = true;
    sPending.mdl = sMembers[sMdls[sMdlSel]].data;
    if (sKeySel >= 0) sPending.key = sMembers[sKeys[sKeySel]].data;
    if (!modelBounds(sPending.mdl, sPending.center, sPending.radius)) {
        sPending.center[0] = sPending.center[1] = sPending.center[2] = 0;
        sPending.radius = 100.0f;
    }
}

// ---- guest side -----------------------------------------------------------------------

u32 heapAlloc(Ctx& c, u32 size) { return gu::call(c, GU_SYM("alloc__7JKRHeapFUliP7JKRHeap"), {size, 32, sHeap}); }

bool ensureHeap(Ctx& c) {
    if (sHeap) return true;
    constexpr u32 kSystem = 0x804D94F0, kCurrent = 0x804D94F4, kRoot = 0x804D94F8;
    const u32 sys = rd32(kSystem), cur = rd32(kCurrent), root = rd32(kRoot);
    if (!gu::inMem1(root)) return false;
    std::memset(hostp(kHeapObj), 0, kHeapMem - kHeapObj);
    gu::call(c, GU_SYM("__ct__10JKRExpHeapFPvUlP7JKRHeapb"), {kHeapObj, kHeapMem, kHeapEnd - kHeapMem, root, 0});
    wr32(kSystem, sys);  // the JKRHeap constructor may adopt a new heap as system/current heap
    wr32(kCurrent, cur);
    sHeap = kHeapObj;
    log("[viewer] model heap %08X: %u KiB", kHeapMem, (kHeapEnd - kHeapMem) / 1024);
    return true;
}

// A key with every channel at its default: joints stay at their rest transforms.
std::vector<u8> restKey(u32 numJoints) {
    std::vector<u8> k(0x20 + std::max<u32>(numJoints * 0x24, 64) + 64, 0);
    auto put32 = [&](u32 off, u32 v) {
        for (int i = 0; i < 4; i++) k[off + i] = u8(v >> (24 - 8 * i));
    };
    put32(0x00, numJoints);
    k[0x06] = 0;
    k[0x07] = 1;  // one frame
    for (u32 off = 0x0C; off <= 0x1C; off += 4) put32(off, 0x20);
    return k;
}

void loadPending(Ctx& c) {
    sPending.load = false;
    sModel = sKey = 0;
    if (!ensureHeap(c)) {
        sStatus = "game heaps not ready";
        return;
    }
    gu::call(c, GU_SYM("freeAll__10JKRExpHeapFv"), {sHeap});
    const u32 numJoints = be16(&sPending.mdl[0x08]);
    std::vector<u8> key = sPending.key.empty() ? restKey(numJoints) : sPending.key;
    const u32 mdl = heapAlloc(c, u32(sPending.mdl.size()));
    const u32 k = heapAlloc(c, u32(key.size()));
    const u32 obj = heapAlloc(c, 0x114);
    if (!mdl || !k || !obj) {
        sStatus = "model too large for the viewer heap";
        return;
    }
    std::memcpy(hostp(mdl), sPending.mdl.data(), sPending.mdl.size());
    std::memcpy(hostp(k), key.data(), key.size());
    gu::call(c, GU_SYM("fn_80061B40"), {mdl});
    gu::call(c, GU_SYM("fn_80061A24"), {k});
    // S3DModel allocates its matrix arrays with operator new: from the viewer heap.
    constexpr u32 kCurrent = 0x804D94F4;
    const u32 cur = rd32(kCurrent);
    wr32(kCurrent, sHeap);
    gu::call(c, GU_SYM("__ct__8S3DModelFll"), {obj, mdl, 0});
    wr32(kCurrent, cur);
    gu::call(c, GU_SYM("fn_8005AA3C"), {obj, k});
    sModel = obj;
    sKey = k;
    sNumFrames = std::max<u32>(1, be16(&key[6]));
    sFrame = 0.0f;
    std::memcpy(sCenter, sPending.center, sizeof(sCenter));
    sRadius = sPending.radius;
    sLoadedInfo = std::to_string(numJoints) + " joints, " + std::to_string(sNumFrames) + " frames";
    log("[viewer] loaded model (%zu bytes, %s)", sPending.mdl.size(), sLoadedInfo.c_str());
}

// Matrices (row-major, as GX takes them) written to guest scratch memory.
void putMtx(u32 at, const float* m, int n) {
    for (int i = 0; i < n; i++) gu::wrf(at + 4 * i, m[i]);
}

void drawBackdrop(Ctx& c) {
    // Orthographic full-screen fill at the far plane: clears the picture (and depth) under the model.
    const float ortho[16] = {2.0f / 640, 0, 0, -1, 0, -2.0f / 480, 0, 1, 0, 0, -1, -1, 0, 0, 0, 1};
    const float ident[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    putMtx(kScratch + 0x000, ortho, 16);
    putMtx(kScratch + 0x040, ident, 12);
    wr32(kScratch + 0x070, 0x30343CFF);  // backdrop colour
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
    gu::call(c, GU_SYM("GXSetFog"), {0, kScratch + 0x70}, {0, 0, 0, 0});
    gu::call(c, GU_SYM("GXClearVtxDesc"));
    gu::call(c, GU_SYM("GXSetVtxDesc"), {9, 1});
    gu::call(c, GU_SYM("GXSetVtxAttrFmt"), {0, 9, 1, 4, 0});
    gu::call(c, GU_SYM("GXBegin"), {0x80, 0, 4});
    const float quad[4][3] = {{0, 0, -0.999f}, {640, 0, -0.999f}, {640, 480, -0.999f}, {0, 480, -0.999f}};
    for (const auto& v : quad)
        for (float f : v) mmio_wr(0xCC008000, std::bit_cast<u32>(f), 4);
}

void drawModel(Ctx& c) {
    // Perspective camera orbiting the model's bounds centre.
    const float r = sRadius * 3.2f * sCam.zoom;
    const float cy = std::cos(sCam.yaw), sy = std::sin(sCam.yaw), cp = std::cos(sCam.pitch), sp = std::sin(sCam.pitch);
    const float eye[3] = {sCenter[0] + r * cp * sy, sCenter[1] + r * sp, sCenter[2] + r * cp * cy};
    float f[3] = {sCenter[0] - eye[0], sCenter[1] - eye[1], sCenter[2] - eye[2]};
    const float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (float& v : f) v /= fl;
    // side = normalize(forward x up), up = (0, 1, 0)
    const float sl = std::max(1e-6f, std::sqrt(f[0] * f[0] + f[2] * f[2]));
    const float s[3] = {-f[2] / sl, 0.0f, f[0] / sl};
    const float u[3] = {s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0]};
    const float view[12] = {
        s[0], s[1], s[2], -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]),
        u[0], u[1], u[2], -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]),
        -f[0], -f[1], -f[2], (f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2]),
    };
    const float nearZ = std::max(1.0f, r - sRadius * 4.0f), farZ = r + sRadius * 4.0f;
    const float t = 1.0f / std::tan(0.5f * 40.0f * 3.14159265f / 180.0f);
    const float aspect = 4.0f / 3.0f;
    const float proj[16] = {t / aspect, 0, 0, 0, 0, t, 0, 0, 0, 0, -nearZ / (farZ - nearZ), -(farZ * nearZ) / (farZ - nearZ),
                            0, 0, -1, 0};
    putMtx(kScratch + 0x100, proj, 16);
    putMtx(kScratch + 0x140, view, 12);
    gu::call(c, GU_SYM("GXSetProjection"), {kScratch + 0x100, 0});
    gu::call(c, GU_SYM("GXSetZMode"), {1, 3, 1});

    // Lighting as on the file-select screen (mode 0 of fn_80057F90), then the S3DModel passes.
    constexpr u32 kLightMode = 0x804D07C8;
    const u32 savedMode = rd32(kLightMode);
    wr32(kLightMode, 0);
    gu::call(c, GU_SYM("fn_80057F90"), {0});
    gu::call(c, GU_SYM("fn_8005DB10"));
    gu::call(c, GU_SYM("fn_8005AA34"), {sModel}, {sFrame});
    gu::call(c, GU_SYM("fn_800589B0"), {sModel, 0});
    gu::call(c, GU_SYM("fn_80059710"), {sModel, kScratch + 0x140});
    gu::call(c, GU_SYM("fn_8005832C"), {1});
    gu::call(c, GU_SYM("fn_80059C54"), {sModel});
    gu::call(c, GU_SYM("fn_8005832C"), {0});
    gu::call(c, GU_SYM("fn_80059F6C"), {sModel});
    wr32(kLightMode, savedMode);
}

}  // namespace

bool isOpen() { return sOpen; }
void setOpen(bool open) { sOpen = open; }

void guestFrame(Ctx& c) {
    if (!sOpen) return;
    if (sPending.load) loadPending(c);
    drawBackdrop(c);
    if (!sModel) return;
    if (sAnim.play && sNumFrames > 1) {
        sFrame += sAnim.speed;
        if (sFrame >= float(sNumFrames)) sFrame = std::fmod(sFrame, float(sNumFrames));
    }
    if (sCam.spin) sCam.yaw = std::remainder(sCam.yaw + 0.01f, 6.2831853f);
    drawModel(c);
}

void window() {
    if (!sOpen) return;
    buildCatalogue();
    if (sEnvArchive) {
        const std::string spec = sEnvArchive;
        sEnvArchive = nullptr;
        // <archive>[#<model name part>][|<animation name part>]
        const size_t bar = spec.find('|'), hash = spec.find('#');
        const std::string key = bar == std::string::npos ? "" : spec.substr(bar + 1);
        const std::string mdl = hash == std::string::npos ? "" : spec.substr(hash + 1, bar == std::string::npos ? std::string::npos : bar - hash - 1);
        for (int i = 0; i < int(sArchives.size()); i++)
            if (sArchives[i] == spec.substr(0, std::min(bar, hash)))
                openArchive(i, key.empty() ? nullptr : key.c_str(), mdl.empty() ? nullptr : mdl.c_str());
    }
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + 8, vp->Pos.y + 8), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(360, vp->Size.y * 0.8f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.75f);
    if (!ImGui::Begin("Model viewer (F4)", &sOpen)) {
        ImGui::End();
        return;
    }
    ImGui::TextDisabled("Game input is paused while this window is open.");
    ImGui::InputText("Filter", sFilter, sizeof(sFilter));
    if (ImGui::BeginChild("archives", ImVec2(0, ImGui::GetContentRegionAvail().y * 0.4f), ImGuiChildFlags_Borders)) {
        for (int i = 0; i < int(sArchives.size()); i++) {
            if (sFilter[0] && sArchives[i].find(sFilter) == std::string::npos) continue;
            if (ImGui::Selectable(sArchives[i].c_str(), sArchiveSel == i)) openArchive(i, nullptr);
        }
    }
    ImGui::EndChild();
    if (!sStatus.empty()) ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "%s", sStatus.c_str());
    if (sMdlSel >= 0) ImGui::Text("%s", sLoadedInfo.c_str());
    if (!sMdls.empty()) {
        const char* cur = sMdlSel >= 0 ? sMembers[sMdls[sMdlSel]].path.c_str() : "";
        if (ImGui::BeginCombo("Model", cur)) {
            for (int i = 0; i < int(sMdls.size()); i++)
                if (ImGui::Selectable(sMembers[sMdls[i]].path.c_str(), sMdlSel == i)) {
                    sMdlSel = i;
                    sKeySel = -1;
                    requestLoad();
                }
            ImGui::EndCombo();
        }
        const char* curKey = sKeySel >= 0 ? sMembers[sKeys[sKeySel]].path.c_str() : "(no animation)";
        if (ImGui::BeginCombo("Animation", curKey)) {
            if (ImGui::Selectable("(no animation)", sKeySel < 0)) {
                sKeySel = -1;
                requestLoad();
            }
            for (int i = 0; i < int(sKeys.size()); i++) {
                if (sMdlSel >= 0 && !keyMatches(sKeys[i], sMdls[sMdlSel])) continue;
                if (ImGui::Selectable(sMembers[sKeys[i]].path.c_str(), sKeySel == i)) {
                    sKeySel = i;
                    requestLoad();
                }
            }
            ImGui::EndCombo();
        }
    }
    ImGui::SeparatorText("Camera");
    ImGui::Checkbox("Spin", &sCam.spin);
    ImGui::SliderFloat("Yaw", &sCam.yaw, -6.2832f, 6.2832f);
    ImGui::SliderFloat("Pitch", &sCam.pitch, -1.5f, 1.5f);
    ImGui::SliderFloat("Distance", &sCam.zoom, 0.2f, 4.0f, "%.2fx", ImGuiSliderFlags_Logarithmic);
    ImGui::TextDisabled("Drag on the picture to orbit, wheel to zoom.");
    ImGui::SeparatorText("Animation");
    ImGui::Checkbox("Play", &sAnim.play);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::SliderFloat("Speed", &sAnim.speed, 0.1f, 3.0f, "%.1fx");
    if (sNumFrames > 1) ImGui::SliderFloat("Frame", &sFrame, 0.0f, float(sNumFrames - 1), "%.0f");
    ImGui::End();

    // Orbit with the mouse anywhere outside ImGui windows.
    ImGuiIO& io = ImGui::GetIO();
    if (!io.WantCaptureMouse) {
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            sCam.spin = false;
            sCam.yaw -= io.MouseDelta.x * 0.01f;
            sCam.pitch = std::clamp(sCam.pitch + io.MouseDelta.y * 0.01f, -1.5f, 1.5f);
        }
        if (io.MouseWheel != 0.0f) sCam.zoom = std::clamp(sCam.zoom * std::pow(0.9f, io.MouseWheel), 0.2f, 4.0f);
    }
}

}  // namespace recomp::viewer
