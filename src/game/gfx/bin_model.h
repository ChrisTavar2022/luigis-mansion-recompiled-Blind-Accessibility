#pragma once
// Luigi's Mansion ".bin" models: rooms, furniture and doors (version byte 2).
// Retail: auto_03_8001461C (relocation fn_8001D150, setup fn_8001D3F4,
// drawing fn_80015CF4 / fn_80015844 / fn_8001566C).
//
// As with .mdl, the file is not relocated in place on PC: vertex arrays and
// display lists are used big-endian from the file, small tables are decoded.
#include "game/gfx/mdl.h"

#include <dolphin/gx.h>
#include <dolphin/mtx.h>

#include <vector>

namespace lm::gfx {

struct BinTexture {
    u16 width;
    u16 height;
    u8 format;  // GX format
    const u8* pixels;
};

struct BinMaterial {  // 0x14 bytes
    u16 texture;
    s8 wrapS;
    s8 wrapT;
    u8 alphaCategory;  // 0x06
    u8 mipmaps;        // 0x07: nonzero = mipmapped
    f32 maxLod;        // 0x08 (low byte also used as a flag at 0x0A by bump materials)
    u8 bump;           // 0x0A
};

struct BinShader {  // 0x28 bytes
    u8 lightMode;      // 0x00: 1 = vertex-color material source
    u8 _1;
    u8 alphaCategory;  // 0x02: nonzero overrides the material's
    GXColor tint;      // 0x03
    s16 materials[8];  // 0x08
    u16 lightFlags;    // 0x18
    u32 lightSet;      // 0x1C
};

struct BinBatch {  // 0x18 bytes
    u16 faceCount;
    u32 attributes;  // 0x04: 0x400 nrm, 0x800 clr0, 0x2000..0x10000 tex0..3
    u8 tevMode;      // 0x08
    u8 cullMode;     // 0x09
    u8 numTextures;  // 0x0A
    u8 nbt;          // 0x0B
    const u8* displayList;
    u32 displayListSize;
};

struct BinPart {
    u16 shader;
    u16 batch;
};

struct BinNode {  // 0x8C bytes
    s16 parent;     // 0x00
    s16 child;      // 0x02 first child, -1 = none
    s16 next;       // 0x04 next sibling, -1 = none
    u16 flags;      // 0x08 visibility categories (see drawNode)
    Vec scale;      // 0x0C
    Vec rotation;   // 0x18 degrees
    Vec translation;  // 0x24
    std::vector<BinPart> parts;  // 0x4C count, 0x50 offset
};

struct BinData {
    const u8* raw = nullptr;
    u32 size = 0;
    char name[12] = {};
    bool floatPositions = false;  // header 0x48 bit 1
    const void* positions = nullptr;
    const void* normals = nullptr;
    const void* colors = nullptr;
    const void* texCoords[4] = {};
    u32 positionsSize = 0, normalsSize = 0, colorsSize = 0, texCoordsSize[4] = {};  // bytes
    std::vector<BinTexture> textures;
    std::vector<BinMaterial> materials;
    std::vector<BinShader> shaders;
    std::vector<BinBatch> batches;
    std::vector<BinNode> nodes;
};

bool parseBin(const void* file, u32 size, BinData& out);

// A drawable instance with per-node world matrices (retail: 0x7C-byte room slot).
class BinModel {
public:
    void init(const BinData* data);          // fn_8001D3F4
    void calcMatrices(const Mtx base);        // fn_8001D96C / fn_8001CFB4

    // fn_80015CF4: draw every part in `pass` (alpha category); alpha 0xFF = opaque.
    void draw(const Mtx view, u8 pass, u8 alpha = 0xFF);

    const BinData* data() const { return mData; }

private:
    void calcNode(s16 node, const Mtx parent);
    void drawNode(bool siblingsDone, u8 pass, u8 alpha, s16 node);  // fn_80015844
    void drawPart(const BinPart& part, u8 alpha, const Mtx& nodeMtx);  // fn_8001566C
    void setupArrays();         // fn_8001D4C0
    void setupVtxDesc(const BinBatch& batch);  // fn_8001D5B8
    void initTexObj(GXTexObj* obj, const BinShader& shader, u32 slot) const;  // fn_8001D6E0
    u8 partCategory(const BinShader& shader) const;  // fn_8001D7FC

    const BinData* mData = nullptr;
    std::vector<MtxBox> mNodeMtx;  // node+0x54 in retail
    Mtx mView;
};

}  // namespace lm::gfx
