#include "game/gfx/mdl.h"

#include <dolphin/os.h>

#include <bit>
#include <cstring>

namespace lm::gfx {
namespace {

struct BE {
    const u8* base;
    u32 size;

    bool in(u32 off, u32 len) const { return off <= size && len <= size - off; }
    u8 u8at(u32 off) const { return base[off]; }
    u16 u16at(u32 off) const { return u16((base[off] << 8) | base[off + 1]); }
    u32 u32at(u32 off) const {
        return (u32(base[off]) << 24) | (u32(base[off + 1]) << 16) | (u32(base[off + 2]) << 8) | base[off + 3];
    }
    f32 f32at(u32 off) const { return std::bit_cast<f32>(u32at(off)); }
    const u8* ptr(u32 off) const { return base + off; }
};

constexpr u32 kMdlMagic = 0x04B40000;

}  // namespace

bool parseMdl(const void* file, u32 size, MdlData& out) {
    const BE f{static_cast<const u8*>(file), size};
    if (size < 0x80 || f.u32at(0) != kMdlMagic) {
        OSReport("[mdl] bad model header\n");
        return false;
    }
    out = MdlData{};
    out.raw = f.base;
    out.size = size;
    out.numJoints = f.u16at(0x08);
    out.numEnvelopes = f.u16at(0x0C);
    out.numNormals = f.u16at(0x12);
    out.numColors = f.u16at(0x14);
    out.numTexCoords = f.u16at(0x16);
    // Counts verified against every model in game_usa (table sizes from offset gaps).
    const u16 numTextures = f.u16at(0x20);
    const u16 numSamplers = f.u16at(0x24);
    const u16 numShapes = f.u16at(0x26);
    const u16 numMaterials = f.u16at(0x28);
    const u16 numDraws = f.u16at(0x2A);
    const u16 numPackets = 0;  // 0x0A is not a packet count; derived from the shapes below

    const u32 offNodes = f.u32at(0x30), offPackets = f.u32at(0x34), offInvBind = f.u32at(0x38);
    const u32 offWeights = f.u32at(0x3C), offIndices = f.u32at(0x40), offCounts = f.u32at(0x44);
    const u32 offTexTable = f.u32at(0x60), offMaterials = f.u32at(0x68), offSamplers = f.u32at(0x6C);
    const u32 offShapes = f.u32at(0x70), offDraws = f.u32at(0x74);

    out.positions = f.ptr(f.u32at(0x48));
    out.normals = f.ptr(f.u32at(0x4C));
    out.colors = f.ptr(f.u32at(0x50));
    out.texCoords = f.ptr(f.u32at(0x54));

    // Nodes: the graph is walked by relative links, so count until the table ends.
    // Every joint has exactly one node.
    out.nodes.resize(out.numJoints);
    for (u32 i = 0; i < out.numJoints; i++) {
        const u32 o = offNodes + i * 0x10;
        MdlNode& n = out.nodes[i];
        n.index = f.u16at(o + 0x0);
        n.siblingRel = s16(f.u16at(o + 0x2));
        n.childRel = s16(f.u16at(o + 0x4));
        n.renderFlags = f.u16at(o + 0x6);
        n.drawCount = f.u16at(o + 0x8);
        n.firstDraw = f.u16at(o + 0xA);
    }

    // Packets: the header count is not reliable across all models, so derive it
    // from the shapes that reference packets.
    u32 packetCount = numPackets;
    for (u32 i = 0; i < numShapes; i++) {
        const u32 o = offShapes + i * 8;
        packetCount = std::max<u32>(packetCount, u32(f.u16at(o + 6)) + f.u16at(o + 4));
    }
    out.packets.resize(packetCount);
    for (u32 i = 0; i < packetCount; i++) {
        const u32 o = offPackets + i * 0x20;
        MdlPacket& p = out.packets[i];
        p.displayList = f.ptr(f.u32at(o + 0x0));
        p.displayListSize = f.u32at(o + 0x4);
        p.cullMode = f.u16at(o + 0x8);
        p.numMatrices = f.u16at(o + 0xA);
        for (int m = 0; m < 10; m++) {
            p.matrixIndices[m] = f.u16at(o + 0xC + m * 2);
        }
    }

    out.textures.resize(numTextures);
    for (u32 i = 0; i < numTextures; i++) {
        const u32 t = f.u32at(offTexTable + i * 4);
        MdlTexture& tex = out.textures[i];
        tex.format = f.u8at(t + 0x0);
        tex.width = f.u16at(t + 0x2);
        tex.height = f.u16at(t + 0x4);
        tex.pixels = f.ptr(t + 0x20);
    }

    out.samplers.resize(numSamplers);
    for (u32 i = 0; i < numSamplers; i++) {
        const u32 o = offSamplers + i * 8;
        MdlSampler& s = out.samplers[i];
        s.texture = f.u16at(o + 0x0);
        s.wrapS = f.u8at(o + 0x4);
        s.wrapT = f.u8at(o + 0x5);
        s.flags = f.u8at(o + 0x6);
    }

    out.materials.resize(numMaterials);
    for (u32 i = 0; i < numMaterials; i++) {
        const u32 o = offMaterials + i * 0x120;
        MdlMaterial& m = out.materials[i];
        m.color = {f.u8at(o + 0), f.u8at(o + 1), f.u8at(o + 2), f.u8at(o + 3)};
        m.lightChan0 = f.u8at(o + 4);
        m.lightChan1 = f.u8at(o + 5);
        m.alphaMode = f.u8at(o + 6);
        m.numStages = f.u8at(o + 7);
        m.flags = f.u8at(o + 8);
        for (int s = 0; s < 8; s++) {
            const u32 so = o + 0x20 + s * 0x20;
            m.stages[s].mode = f.u16at(so + 0x0);
            m.stages[s].sampler = f.u16at(so + 0x2);
            for (int k = 0; k < 7; k++) {
                m.stages[s].params[k] = f.f32at(so + 0x4 + k * 4);
            }
        }
    }

    out.shapes.resize(numShapes);
    for (u32 i = 0; i < numShapes; i++) {
        const u32 o = offShapes + i * 8;
        out.shapes[i] = {f.u32at(o + 0), f.u16at(o + 4), f.u16at(o + 6)};
    }

    out.draws.resize(numDraws);
    for (u32 i = 0; i < numDraws; i++) {
        const u32 o = offDraws + i * 4;
        out.draws[i] = {f.u16at(o + 0), f.u16at(o + 2)};
    }

    // Skinning: per envelope, `count` (joint, weight) pairs. Inverse bind
    // matrices are indexed by joint.
    out.inverseBind.resize(out.numJoints);
    for (u32 j = 0; j < out.numJoints; j++) {
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 4; c++) {
                out.inverseBind[j].m[r][c] = f.f32at(offInvBind + j * 0x30 + (r * 4 + c) * 4);
            }
        }
    }
    out.envelopes.resize(out.numEnvelopes);
    u32 pair = 0;
    for (u32 e = 0; e < out.numEnvelopes; e++) {
        MdlEnvelope& env = out.envelopes[e];
        env.count = f.u8at(offCounts + e);
        env.joints = reinterpret_cast<const u16*>(f.ptr(offIndices + pair * 2));
        env.weights = reinterpret_cast<const f32*>(f.ptr(offWeights + pair * 4));
        pair += env.count;
    }
    return true;
}

bool parseKey(const void* file, u32 size, KeyAnim& out) {
    const BE f{static_cast<const u8*>(file), size};
    if (size < 0x20) {
        return false;
    }
    out = KeyAnim{};
    out.raw = f.base;
    const u32 numJoints = f.u32at(0x0);
    out.numFrames = f.u16at(0x6);
    out.flags = f.u32at(0x8);
    out.scales = reinterpret_cast<const f32*>(f.ptr(f.u32at(0xC)));
    out.rotations = reinterpret_cast<const s16*>(f.ptr(f.u32at(0x10)));
    out.translations = reinterpret_cast<const f32*>(f.ptr(f.u32at(0x14)));
    const u32 offIndex = f.u32at(0x18), offCount = f.u32at(0x1C);
    out.joints.resize(numJoints);
    for (u32 j = 0; j < numJoints; j++) {
        for (int c = 0; c < 9; c++) {
            out.joints[j].index[c] = f.u32at(offIndex + j * 0x24 + c * 4);
            out.joints[j].count[c] = f.u16at(offCount + j * 0x12 + c * 2);
        }
    }
    return true;
}

}  // namespace lm::gfx
