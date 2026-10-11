// Frame interpolation: vertex-array snapshots (interp.h, docs/INTERPOLATION.md).
//
// A tape is replayed while the guest is already running the next game frame, and
// Aurora reads indexed vertex attributes from guest memory when it processes a draw.
// Data the CPU rewrites every frame (skinned or animated meshes, particles, anything
// built into a reused buffer) would then be drawn with the next frame's contents,
// half-written, or garbage. So while capturing, the draws are parsed (display lists,
// and the raw vertex stream after GXBegin) to find the range of each array they
// index, that range is copied at the time of the draw (when the console's GPU would
// read it), and the tape's array loads are pointed at the copies.
//
// Each GXSetArray in a tape becomes its own array-load command; copies are shared per
// guest address until GXInvalidateVtxCache (the game's signal that it rewrote vertex
// data), after which the current arrays are loaded again so later draws get new copies.
// Every tape starts by loading the current arrays, so it never depends on another
// tape's copies.
#include "recomp/gx_bridge.h"
#include "recomp/hle.h"
#include "recomp/interp.h"

#include <dolphin/gx.h>

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace recomp::interp {
namespace {

constexpr int kArrays = 16;  // CP array slots: GX_VA_POS..GX_VA_TEX7, then the 4 matrix/light arrays

struct Binding {
    u32 guest = 0, stride = 0;
    bool valid = false;
    int cmd = -1;  // index into sCmds: the array load in the current tape
};
Binding sBind[kArrays];

struct Cmd {
    u32 at;      // tape offset before the load was written
    int snap;    // index into sSnaps, -1: not used by any draw (left as is)
};
std::vector<Cmd> sCmds;

struct Snap {
    u32 guest;
    std::vector<u8> data;
};
std::vector<Snap> sSnaps;
std::unordered_map<u32, int> sSnapOf;  // guest address -> snapshot (this vertex-cache epoch)

// Streaming parser for raw FIFO writes (vertex data after GXBegin, inlined commands).
std::vector<u8> sPending;
u32 sPendingPos = 0;

// Stats (LM_INTERP_TRACE).
u32 sParseTapeBase = ~0u;  // tape offset of the display list being parsed (~0: raw stream)
u32 sBadOp = 0;  // last unparseable opcode | 0x100 if in the raw stream | offset << 12
u32 sUnparsed = 0, sDlCpWrites = 0, sAuroraInDl = 0, sStreamMtxLoads = 0, sStreamIndexedLoads = 0;

bool inMem1(u32 a) { return (a >> 28) == 0x8 || (a >> 28) == 0xC; }

void emitLoad(int slot) {
    Binding& b = sBind[slot];
    const u32 at = mark();
    GXSetArray(GXAttr(GX_VA_POS + slot), hostp(b.guest), 0, u8(b.stride), false);
    b.cmd = int(sCmds.size());
    sCmds.push_back({at, -1});
}

// A draw reads bytes [0, extent) of the array in `slot`.
void use(int slot, u32 extent) {
    Binding& b = sBind[slot];
    if (!b.valid || b.cmd < 0 || !inMem1(b.guest)) return;
    const u32 phys = b.guest & 0x01FFFFFF;
    if (phys >= kMem1Size) return;
    extent = std::min(extent, kMem1Size - phys);
    Cmd& cmd = sCmds[b.cmd];
    if (cmd.snap < 0) {
        auto [it, fresh] = sSnapOf.try_emplace(b.guest, int(sSnaps.size()));
        if (fresh) sSnaps.push_back({b.guest, {}});
        cmd.snap = it->second;
    }
    std::vector<u8>& d = sSnaps[cmd.snap].data;
    if (extent > d.size()) {  // copy what this draw reads beyond the copy so far, as it is now
        const u32 old = u32(d.size());
        d.resize(extent);
        std::memcpy(d.data() + old, hostp(b.guest) + old, extent - old);
    }
}

// Bytes per vertex of format `fmt`, or 0 if unknown.
u32 vertexSize(int fmt) {
    u32 size = 0;
    for (int a = GX_VA_PNMTXIDX; a <= GX_VA_TEX7; a++) {
        const int s = gx::vtxAttrSize(fmt, a);
        if (s < 0) return 0;
        size += u32(s);
    }
    return size;
}

// The indexed attributes of `count` vertices at `v`: extents of the arrays they read.
void scanVertices(int fmt, const u8* v, u32 count, u32 vtxSize) {
    u32 offset = 0;
    for (int a = GX_VA_PNMTXIDX; a <= GX_VA_TEX7; a++) {
        const int s = gx::vtxAttrSize(fmt, a);
        const u8 type = gx::vtxDesc(a);
        if ((type == GX_INDEX8 || type == GX_INDEX16) && a >= GX_VA_POS) {
            const u32 idxSize = type == GX_INDEX16 ? 2 : 1;
            const u32 skip = type == GX_INDEX16 ? 0xFFFF : 0xFF;
            u32 maxIdx = 0;
            bool any = false;
            for (u32 i = 0; i < count; i++) {
                const u8* p = v + i * vtxSize + offset;
                for (u32 k = 0; k < u32(s); k += idxSize) {
                    const u32 idx = idxSize == 2 ? u32(p[k] << 8 | p[k + 1]) : p[k];
                    if (idx == skip) continue;
                    maxIdx = std::max(maxIdx, idx);
                    any = true;
                }
            }
            if (any) {
                const int slot = a - GX_VA_POS;
                // Up to one element past the last index start: 36 bytes covers any element
                // (an NBT normal of 9 floats) when the stride is smaller than the element.
                use(slot, (maxIdx + 1) * sBind[slot].stride + 36);
            }
        }
        offset += u32(s);
    }
}

// Parses GX commands in [p, p + size). Returns bytes consumed; stops early (returning
// how far it got) when a command is incomplete and `partial` is allowed, or on a
// command it can't size (counted in sUnparsed, returns size).
u32 parse(const u8* p, u32 size, bool partial) {
    u32 i = 0;
    auto be16 = [&](u32 o) { return u32(p[o] << 8 | p[o + 1]); };
    auto be32 = [&](u32 o) { return u32(p[o]) << 24 | u32(p[o + 1]) << 16 | u32(p[o + 2]) << 8 | p[o + 3]; };
    while (i < size) {
        const u8 op = p[i];
        u32 len = 0;
        if (op == 0x00 || op == 0x48) {  // NOP, INVAL_VTX
            len = 1;
        } else if (op == 0x08) {  // LOAD_CP_REG
            len = 6;
            if (i + len <= size) {
                const u8 reg = p[i + 1];
                if ((reg & 0xF0) >= 0x50 && (reg & 0xF0) <= 0xB0) sDlCpWrites++;  // VCD/VAT/arrays
            }
        } else if (op == 0x10) {  // LOAD_XF_REG
            if (i + 5 > size) {
                if (partial) return i;
                break;
            }
            len = 5 + 4 * (((be32(i + 1) >> 16) & 0xFFFF) + 1);
            if ((be32(i + 1) & 0xFFFF) < 0x600) sStreamMtxLoads++;  // matrix memory, not noted for blending
        } else if (op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38) {  // LOAD_INDX_A..D
            len = 5;
            if (i + len <= size) {
                const int slot = 12 + (op - 0x20) / 8;
                const u32 idx = be16(i + 1), n = (be16(i + 3) >> 12) + 1;
                use(slot, idx * sBind[slot].stride + n * 4);
                sStreamIndexedLoads++;
            }
        } else if (op == 0x40) {  // CALL_DL (Aurora ignores nested calls)
            len = 9;
        } else if (op == 0x61) {  // LOAD_BP_REG
            len = 5;
            // TEV color/konst and fog color registers set by a display list: blended with its draw.
            if (sParseTapeBase != ~0u && i + len <= size) {
                const u32 reg = p[i + 1];
                if (reg >= 0xE0 && reg <= 0xE7) detail::noteDrawColor(sParseTapeBase + i + 1, reg, 1);
                else if (reg == 0xF2) detail::noteDrawColor(sParseTapeBase + i + 1, reg, 2);
            }
        } else if (op >= 0x80 && op <= 0xBF) {  // draw
            if (i + 3 > size) {
                if (partial) return i;
                break;
            }
            const int fmt = op & 7;
            const u32 vs = vertexSize(fmt);
            const u32 count = be16(i + 1);
            if (vs == 0 && count != 0) {
                sUnparsed++;
                return size;
            }
            len = 3 + count * vs;
            if (i + len <= size) scanVertices(fmt, p + i + 3, count, vs);
        } else if (op == GX_AURORA) {  // only in display lists the game recorded through Aurora
            sAuroraInDl++;
            sUnparsed++;
            return size;
        } else {
            sUnparsed++;
            sBadOp = op | (partial ? 0x100 : 0) | (i << 12);
            static int dumped = 0;
            if (std::getenv("LM_INTERP_TRACE") && dumped++ < 3) {
                char buf[400];
                int n = 0;
                for (u32 k = 0; k < size && k < 96 && n < 380; k++) n += std::snprintf(buf + n, sizeof(buf) - n, "%02X", p[k]);
                log("[interp] unparsed %s stream at +%u: %s", partial ? "raw" : "list", i, buf);
            }
            return size;
        }
        if (i + len > size) {
            if (partial) return i;
            sUnparsed++;
            return size;
        }
        i += len;
    }
    return i;
}

// The open immediate-mode draw with direct float positions (vtxBegin).
struct {
    bool active = false;
    u32 at = ~0u, bytes = 0, count = 0, stride = 0, posOffset = 0;
    u64 id = 0;
} sDirect;

// The immediate-mode draw opened by GXBegin: its vertices are scanned once complete, with the
// vertex layout as it was at GXBegin (the game changes it right after GXEnd, so parsing the
// stream later would size the vertices wrongly).
struct {
    bool active = false;
    int fmt = 0;
    u32 count = 0, size = 0;
    std::vector<u8> bytes;
} sImm;

void finishDirect() {
    sImm.active = false;
    if (!sDirect.active) return;
    sDirect.active = false;
    if (sDirect.at != ~0u && sDirect.stride && sDirect.bytes == sDirect.count * sDirect.stride)
        detail::noteVertices(sDirect.at, sDirect.count, sDirect.stride, sDirect.posOffset, sDirect.id);
}

void flushPending() {
    if (sPendingPos >= sPending.size()) {
        sPending.clear();
        sPendingPos = 0;
        return;
    }
    sPendingPos += parse(sPending.data() + sPendingPos, u32(sPending.size() - sPendingPos), true);
    if (sPendingPos == sPending.size() || sPendingPos > (1u << 20)) {
        sPending.erase(sPending.begin(), sPending.begin() + sPendingPos);
        sPendingPos = 0;
    }
}

}  // namespace

void arrayBase(u32 at, u32 attr, u32 guestAddr, u32 stride) {
    // GX_VA_NBT shares the normal array slot (as on the hardware and in Aurora's GXSetArray).
    const int slot = attr == GX_VA_NBT ? GX_VA_NRM - GX_VA_POS : int(attr) - GX_VA_POS;
    if (slot < 0 || slot >= kArrays) return;
    Binding& b = sBind[slot];
    b.guest = guestAddr;
    b.stride = stride;
    b.valid = guestAddr != 0;
    b.cmd = -1;
    if (capturing() && at != ~0u && b.valid) {
        b.cmd = int(sCmds.size());
        sCmds.push_back({at, -1});
    }
}

void invalidateVtxCache() {
    if (!capturing()) return;
    finishDirect();
    flushPending();
    sSnapOf.clear();  // the game rewrote vertex data: later draws get new copies
    for (int s = 0; s < kArrays; s++)
        if (sBind[s].valid && sBind[s].cmd >= 0 && sCmds[sBind[s].cmd].snap >= 0) emitLoad(s);
}

void vtxBegin(u32 prim, u32 fmt, u32 nVerts, bool perspective) {
    finishDirect();
    flushPending();
    fmt &= 7;
    // What is drawn: primitive, vertex layout and the position array (if positions are indexed:
    // with direct positions the bound array is whatever the last indexed draw left, which changes
    // from frame to frame, and then no draw of this kind ever found its partner; from the SMS port).
    const bool posIndexed = gx::vtxDesc(GX_VA_POS) == GX_INDEX8 || gx::vtxDesc(GX_VA_POS) == GX_INDEX16;
    u64 id = u64(prim & 0xF8 | fmt) << 56 ^ u64(vertexSize(int(fmt))) << 40 ^ (posIndexed ? sBind[0].guest : 0);
    for (int a = GX_VA_PNMTXIDX; a <= GX_VA_TEX7; a++) id = detail::mix(id ^ gx::vtxDesc(a));
    detail::noteDraw(id);
    detail::drawStart(mark());
    detail::drawLog("begin prim %02X fmt %u n %u vsize %u %s pos desc %u size %d tex0 desc %u size %d clr0 desc %u",
                    prim & 0xF8, fmt, nVerts, vertexSize(int(fmt)), perspective ? "persp" : "ortho", gx::vtxDesc(GX_VA_POS),
                    gx::vtxAttrSize(int(fmt), GX_VA_POS), gx::vtxDesc(GX_VA_TEX0), gx::vtxAttrSize(int(fmt), GX_VA_TEX0),
                    gx::vtxDesc(GX_VA_CLR0));
    // CPU-built 3D geometry with direct float positions: blend the positions too.
    if (perspective && gx::vtxDesc(GX_VA_POS) == GX_DIRECT && gx::vtxAttrSize(int(fmt), GX_VA_POS) == 12) {
        u32 posOffset = 0;
        for (int a = GX_VA_PNMTXIDX; a < GX_VA_POS; a++) posOffset += u32(std::max(0, gx::vtxAttrSize(int(fmt), a)));
        sDirect = {true, ~0u, 0, nVerts, vertexSize(int(fmt)), posOffset, detail::mix(id ^ nVerts)};
    }
    const u32 vs = vertexSize(int(fmt));
    sImm.active = vs != 0 && nVerts != 0;
    sImm.fmt = int(fmt);
    sImm.count = nVerts;
    sImm.size = vs;
    sImm.bytes.clear();
    if (!sImm.active && nVerts != 0) sUnparsed++;
}

void vtxBytes(u32 v, int size) {
    if (sDirect.active) {
        if (sDirect.at == ~0u) sDirect.at = mark();
        sDirect.bytes += u32(size);
    }
    if (sImm.active) {
        for (int b = size - 1; b >= 0; b--) sImm.bytes.push_back(u8(v >> (8 * b)));
        if (sImm.bytes.size() >= size_t(sImm.count) * sImm.size) {
            scanVertices(sImm.fmt, sImm.bytes.data(), sImm.count, sImm.size);
            sImm.active = false;
        }
        return;
    }
    for (int b = size - 1; b >= 0; b--) sPending.push_back(u8(v >> (8 * b)));
    if (sPending.size() - sPendingPos >= 4096) flushPending();
}

void vtxDisplayList(u32 guestAddr, const u8* data, u32 size) {
    finishDirect();
    flushPending();
    detail::noteDraw(detail::mix(u64(guestAddr) << 24 ^ size ^ 0xD15Bull << 48));
    detail::drawLog("dlist %08X size %u", guestAddr, size);
    const u32 end = mark();
    sParseTapeBase = end != ~0u && end >= size ? end - size : ~0u;
    if (sParseTapeBase != ~0u) detail::drawRange(sParseTapeBase, end);
    parse(data, size, false);
    sParseTapeBase = ~0u;
}

namespace detail {

void vtxBeginTape() {
    sCmds.clear();
    sSnaps.clear();
    sSnapOf.clear();
    sPending.clear();
    sPendingPos = 0;
    sDirect.active = false;
    sImm.active = false;
    // Self-contained tapes: load the arrays the guest has bound now.
    for (int s = 0; s < kArrays; s++) {
        sBind[s].cmd = -1;
        if (sBind[s].valid) emitLoad(s);
    }
}

void vtxEndTape(u8* tape, u32 size, std::vector<std::vector<u8>>& keep, u64& snapshotBytes) {
    finishDirect();
    flushPending();
    keep.clear();
    keep.reserve(sSnaps.size());
    snapshotBytes = 0;
    for (Snap& s : sSnaps) {
        snapshotBytes += s.data.size();
        keep.push_back(std::move(s.data));
    }
    // LM_INTERP_ARRAYS=0 (testing): leave the array loads pointing at live guest memory.
    static const bool sOff = [] { const char* v = std::getenv("LM_INTERP_ARRAYS"); return v && *v == '0'; }();
    for (const Cmd& c : sCmds) {
        if (c.snap < 0 || sOff) continue;
        const std::vector<u8>& d = keep[c.snap];
        if (d.empty()) continue;
        // The load is GX_AURORA, u16 subcommand, u64 host pointer (big-endian), u32 size, u8.
        const u64 want = u64(reinterpret_cast<uintptr_t>(hostp(sSnaps[c.snap].guest)));
        for (u32 i = c.at; i + 12 <= size && i < c.at + 16; i++) {
            u64 v = 0;
            for (int k = 0; k < 8; k++) v = v << 8 | tape[i + k];
            if (v != want) continue;
            const u64 ptr = u64(reinterpret_cast<uintptr_t>(d.data()));
            const u32 len = u32(d.size());
            for (int k = 0; k < 8; k++) tape[i + k] = u8(ptr >> (56 - 8 * k));
            for (int k = 0; k < 4; k++) tape[i + 8 + k] = u8(len >> (24 - 8 * k));
            break;
        }
    }
    sCmds.clear();
    sSnaps.clear();
    sSnapOf.clear();
    if (sUnparsed || sDlCpWrites || sStreamMtxLoads || sStreamIndexedLoads) {
        static u32 logged = 0;
        if (logged++ < 20)
            log("[interp] vertex scan: %u unparsed streams (%u with Aurora commands), %u CP layout writes, %u matrix "
                "loads and %u indexed loads in lists/raw writes (last bad op %X)",
                sUnparsed, sAuroraInDl, sDlCpWrites, sStreamMtxLoads, sStreamIndexedLoads, sBadOp);
        sUnparsed = sDlCpWrites = sAuroraInDl = sStreamMtxLoads = sStreamIndexedLoads = 0;
    }
}

}  // namespace detail
}  // namespace recomp::interp
