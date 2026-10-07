#pragma once
// Luigi's Mansion ".mdl" models (magic 0x04B40000) and ".key" joint animations.
//
// On console the game relocates the file in place (fn_80061B40 / fn_80061A24),
// turning 32-bit offsets into pointers. On PC the file stays untouched (vertex
// arrays and display lists are consumed big-endian by Aurora) and the small
// tables are decoded into these host-side structures instead.
#include <dolphin/gx.h>
#include <dolphin/mtx.h>

#include <vector>

namespace lm::gfx {

// Mtx is a C array; this makes it storable in containers.
struct MtxBox {
    Mtx m;
};

// Scene-graph node (16 bytes in file). Links are relative node counts.
struct MdlNode {
    u16 index;
    s16 siblingRel;
    s16 childRel;
    u16 renderFlags;  // copied to the render state before drawing this node
    u16 drawCount;
    u16 firstDraw;
};

// Display-list packet (32 bytes in file).
struct MdlPacket {
    const u8* displayList;
    u32 displayListSize;
    u16 cullMode;            // index into the cull-mode table
    u16 numMatrices;
    u16 matrixIndices[10];   // 0xFFFF = keep the currently loaded matrix
};

struct MdlTexture {
    u8 format;  // LM format code; see kTextureFormats in s3d_model.cpp
    u16 width;
    u16 height;
    const u8* pixels;
};

struct MdlSampler {
    u16 texture;
    u8 wrapS;
    u8 wrapT;
    u8 flags;  // bits 0-1: alpha mode, bit 2: environment-mapped
};

struct MdlTexStage {
    u16 mode;     // 0 = normal, 1 = (unused), 2 = projected
    u16 sampler;
    f32 params[7];  // mode 2 texture-projection parameters
};

struct MdlMaterial {
    GXColor color;
    u8 lightChan0;    // material color source for channel 0 lighting
    u8 lightChan1;
    u8 alphaMode;     // 0 opaque, 1 alpha-test, 2 translucent
    u8 numStages;
    u8 flags;         // bit 0: two color channels, bit 1: skip drawing
    MdlTexStage stages[8];
};

struct MdlShape {
    u32 flags;  // 0x01000000 lit, 0x02000000 uses NBT normals
    u16 numPackets;
    u16 firstPacket;
};

struct MdlDraw {
    u16 material;
    u16 shape;
};

struct MdlEnvelope {
    u8 count;
    const u16* joints;   // big-endian
    const f32* weights;  // big-endian
};

struct MdlData {
    const u8* raw = nullptr;
    u32 size = 0;

    u16 numJoints = 0;     // 0x08
    u16 numEnvelopes = 0;  // 0x0C: weighted (skinned) matrices after the joints
    u16 numNormals = 0;    // 0x12
    u16 numColors = 0;     // 0x14
    u16 numTexCoords = 0;  // 0x16

    // Vertex arrays, used directly (big-endian) via GXSetArray.
    const void* positions = nullptr;  // f32 xyz
    const void* normals = nullptr;    // f32 xyz
    const void* colors = nullptr;     // rgba8
    const void* texCoords = nullptr;  // f32 st

    std::vector<MdlNode> nodes;
    std::vector<MdlPacket> packets;
    std::vector<MdlTexture> textures;
    std::vector<MdlSampler> samplers;
    std::vector<MdlMaterial> materials;
    std::vector<MdlShape> shapes;
    std::vector<MdlDraw> draws;
    std::vector<MtxBox> inverseBind;  // one per envelope source joint
    std::vector<MdlEnvelope> envelopes;
};

// Parses a model file. The returned data references `file`, which must outlive it.
bool parseMdl(const void* file, u32 size, MdlData& out);

// --- Joint animation (".key") ---
struct KeyJointChannels {
    u16 count[9];  // sx sy sz rx ry rz tx ty tz: 0 = default, 1 = constant, n = keyframes
    u32 index[9];  // into the scale/rotation/translation tables
};

struct KeyAnim {
    const u8* raw = nullptr;
    u32 numFrames = 0;
    u32 flags = 0;  // bit 1: rotations are absolute (SRT order) rather than TRS
    const f32* scales = nullptr;        // big-endian
    const s16* rotations = nullptr;     // big-endian
    const f32* translations = nullptr;  // big-endian
    std::vector<KeyJointChannels> joints;
};

bool parseKey(const void* file, u32 size, KeyAnim& out);

}  // namespace lm::gfx
