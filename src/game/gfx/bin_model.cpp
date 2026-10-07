// Port of the .bin renderer in auto_03_8001461C.
#include "game/gfx/bin_model.h"
#include "game/system/gx_util.h"
#include "game/unported.h"

#include <dolphin/os.h>

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>

namespace lm::gfx {
namespace {

struct BE {
    const u8* base;
    u32 size;
    u8 u8at(u32 o) const { return base[o]; }
    u16 u16at(u32 o) const { return u16((base[o] << 8) | base[o + 1]); }
    s16 s16at(u32 o) const { return s16(u16at(o)); }
    u32 u32at(u32 o) const {
        return (u32(base[o]) << 24) | (u32(base[o + 1]) << 16) | (u32(base[o + 2]) << 8) | base[o + 3];
    }
    f32 f32at(u32 o) const { return std::bit_cast<f32>(u32at(o)); }
};

// lbl_802FF334: color inputs for the first texture stage (index 0) and later
// texture stages (index 2): TEXC * RASC, then TEXC + CPREV.
constexpr GXTevColorArg kTexStageColorIn[3][4] = {
    {GX_CC_ZERO, GX_CC_RASC, GX_CC_TEXC, GX_CC_ZERO},
    {GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC},
    {GX_CC_ZERO, GX_CC_TEXC, GX_CC_ONE, GX_CC_CPREV},
};

// lbl_80300D70: texgen source per texture slot.
constexpr GXTexGenSrc kTexGenSrc[8] = {GX_TG_TEX0, GX_TG_TEX1, GX_TG_TEX2, GX_TG_TEX3,
                                       GX_TG_TEX4, GX_TG_TEX5, GX_TG_TEX6, GX_TG_TEX7};

// Drawing state shared by the draw helpers (retail sdata).
u32 sFaceCounter;     // lbl_804D81B0
bool sSilhouette;     // lbl_804D8230: flat silhouette pass (e.g. the "dark room" outline)
u8 sChanFlags;        // lbl_804D81B5
u32 sLightMask = GX_LIGHT0;   // lbl_804D0044
u32 sAttnFn = GX_AF_SPOT;     // lbl_804D0040

}  // namespace

bool parseBin(const void* file, u32 size, BinData& out) {
    const BE f{static_cast<const u8*>(file), size};
    if (size < 0x4C || f.u8at(0) != 2) {
        OSReport("[bin] unsupported model version %u\n", size ? f.u8at(0) : 0);
        return false;
    }
    out = BinData{};
    out.raw = f.base;
    out.size = size;
    std::memcpy(out.name, f.base + 1, 11);
    out.floatPositions = (f.u32at(0x48) & 2) != 0;

    auto section = [&](u32 headerOffset) -> const u8* {
        const u32 off = f.u32at(headerOffset);
        return off != 0 ? f.base + off : nullptr;
    };
    const u32 offTextures = f.u32at(0x0C), offMaterials = f.u32at(0x10), offShaders = f.u32at(0x34);
    const u32 offBatches = f.u32at(0x38), offNodes = f.u32at(0x3C);

    // Section sizes come from the next section's offset (tables are contiguous).
    // Sections are laid out in header order, but any of them may be absent (offset 0):
    // a table runs to the next section that is present.
    auto nextSection = [&](u32 start) {
        u32 next = size;
        for (u32 h = 0x0C; h <= 0x44; h += 4) {
            const u32 o = f.u32at(h);
            if (o > start && o < next) {
                next = o;
            }
        }
        return next;
    };
    auto countBetween = [&](u32 start, u32 stride) { return start != 0 ? (nextSection(start) - start) / stride : 0u; };
    auto sectionSize = [&](u32 headerOffset) {
        const u32 off = f.u32at(headerOffset);
        return off != 0 ? nextSection(off) - off : 0u;
    };
    out.positions = section(0x14);
    out.positionsSize = sectionSize(0x14);
    out.normals = section(0x18);
    out.normalsSize = sectionSize(0x18);
    out.colors = section(0x1C);
    out.colorsSize = sectionSize(0x1C);
    for (int t = 0; t < 4; t++) {
        out.texCoords[t] = section(0x24 + t * 4);
        out.texCoordsSize[t] = sectionSize(0x24 + t * 4);
    }
    // Textures: the table ends where the first texture's pixel data begins.
    if (offTextures != 0) {
        u32 tableEnd = countBetween(offTextures, 0xC) * 0xC;
        for (u32 o = 0; o < tableEnd; o += 0xC) {
            const u32 dataOff = f.u32at(offTextures + o + 8);
            if (dataOff != 0 && dataOff < tableEnd) {  // 0 = empty slot
                tableEnd = dataOff;
            }
            const u32 e = offTextures + o;
            out.textures.push_back({f.u16at(e), f.u16at(e + 2), f.u8at(e + 4),
                                    dataOff != 0 ? f.base + offTextures + dataOff : nullptr});
        }
        out.textures.resize(tableEnd / 0xC);
    }
    const u32 numMaterials = countBetween(offMaterials, 0x14);
    for (u32 i = 0; i < numMaterials; i++) {
        const u32 o = offMaterials + i * 0x14;
        out.materials.push_back({f.u16at(o), s8(f.u8at(o + 4)), s8(f.u8at(o + 5)), f.u8at(o + 6), f.u8at(o + 7),
                                 f32(f.u8at(o + 8)), f.u8at(o + 0xA)});
    }
    const u32 numShaders = countBetween(offShaders, 0x28);
    for (u32 i = 0; i < numShaders; i++) {
        const u32 o = offShaders + i * 0x28;
        BinShader s{};
        s.lightMode = f.u8at(o);
        s._1 = f.u8at(o + 1);
        s.alphaCategory = f.u8at(o + 2);
        s.tint = {f.u8at(o + 3), f.u8at(o + 4), f.u8at(o + 5), f.u8at(o + 6)};
        for (int m = 0; m < 8; m++) {
            s.materials[m] = f.s16at(o + 8 + m * 2);
        }
        s.lightFlags = f.u16at(o + 0x18);
        s.lightSet = f.u32at(o + 0x1C);
        out.shaders.push_back(s);
    }
    // Batches: entries until the first display list begins.
    u32 firstDl = offNodes;
    for (u32 o = offBatches; o + 0x18 <= firstDl; o += 0x18) {
        const u32 dlOff = f.u32at(o + 0xC);
        firstDl = std::min(firstDl, offBatches + dlOff);
        BinBatch b{};
        b.faceCount = f.u16at(o);
        b.displayListSize = u32(f.u16at(o + 2)) * 32;
        b.attributes = f.u32at(o + 4);
        b.tevMode = f.u8at(o + 8);
        b.cullMode = f.u8at(o + 9);
        b.numTextures = f.u8at(o + 0xA);
        b.nbt = f.u8at(o + 0xB);
        b.displayList = f.base + offBatches + dlOff;
        out.batches.push_back(b);
    }
    // Nodes: the graph section runs to the end of the file; parts lists follow the nodes.
    u32 nodeEnd = size;
    for (u32 o = offNodes; o + 0x8C <= nodeEnd; o += 0x8C) {
        BinNode n{};
        n.parent = f.s16at(o);
        n.child = f.s16at(o + 2);
        n.next = f.s16at(o + 4);
        n.flags = f.u16at(o + 8);
        n.scale = {f.f32at(o + 0xC), f.f32at(o + 0x10), f.f32at(o + 0x14)};
        n.rotation = {f.f32at(o + 0x18), f.f32at(o + 0x1C), f.f32at(o + 0x20)};
        n.translation = {f.f32at(o + 0x24), f.f32at(o + 0x28), f.f32at(o + 0x2C)};
        const u16 numParts = f.u16at(o + 0x4C);
        const u32 partsOff = offNodes + f.u32at(o + 0x50);
        if (numParts != 0) {
            nodeEnd = std::min(nodeEnd, partsOff);
        }
        for (u32 p = 0; p < numParts; p++) {
            n.parts.push_back({f.u16at(partsOff + p * 4), f.u16at(partsOff + p * 4 + 2)});
        }
        out.nodes.push_back(std::move(n));
    }
    return true;
}

// ---------------------------------------------------------------------------

void BinModel::init(const BinData* data) {
    mData = data;
    mNodeMtx.assign(data->nodes.size(), MtxBox{});
    calcMatrices(gIdentityMtx);
}

// fn_8001D96C: local = T * Rz * Ry * Rx * S, rotation in degrees.
static void nodeLocalMatrix(const BinNode& n, Mtx out) {
    auto toAngle = [](f32 deg) { return u16(s32(0.5 + 182.04444444444445 * f64(deg))); };
    makeSRTMtx(n.scale.x, n.scale.y, n.scale.z, n.translation.x, n.translation.y, n.translation.z, out,
               toAngle(n.rotation.x), toAngle(n.rotation.y), toAngle(n.rotation.z));
}

void BinModel::calcNode(s16 node, const Mtx parent) {
    while (node >= 0 && u32(node) < mData->nodes.size()) {
        Mtx local;
        nodeLocalMatrix(mData->nodes[node], local);
        MTXConcat(const_cast<f32(*)[4]>(parent), local, mNodeMtx[node].m);
        calcNode(mData->nodes[node].child, mNodeMtx[node].m);
        node = mData->nodes[node].next;
    }
}

void BinModel::calcMatrices(const Mtx base) {
    if (!mData->nodes.empty()) {
        calcNode(0, base);
    }
}

void BinModel::setupArrays() {
    static constexpr struct {
        GXAttr attr;
        GXCompCnt cnt;
        GXCompType type;
    } kFormats[] = {
        {GX_VA_POS, GX_POS_XYZ, GX_S16}, {GX_VA_NRM, GX_NRM_XYZ, GX_F32}, {GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8},
        {GX_VA_TEX0, GX_TEX_ST, GX_F32}, {GX_VA_TEX1, GX_TEX_ST, GX_F32},  {GX_VA_TEX2, GX_TEX_ST, GX_F32},
        {GX_VA_TEX3, GX_TEX_ST, GX_F32}, {GX_VA_TEX4, GX_TEX_ST, GX_F32},  {GX_VA_TEX5, GX_TEX_ST, GX_F32},
        {GX_VA_TEX6, GX_TEX_ST, GX_F32}, {GX_VA_TEX7, GX_TEX_ST, GX_F32},
    };
    for (const auto& fmt : kFormats) {
        GXSetVtxAttrFmt(GX_VTXFMT0, fmt.attr, fmt.cnt, fmt.type, 0);
    }
    if (mData->floatPositions) {
        GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
        GXSetArray(GX_VA_POS, mData->positions, mData->positionsSize, 12, false);
    } else {
        GXSetArray(GX_VA_POS, mData->positions, mData->positionsSize, 6, false);
    }
    GXSetArray(GX_VA_NRM, mData->normals, mData->normalsSize, 12, false);
    GXSetArray(GX_VA_CLR0, mData->colors, mData->colorsSize, 4, false);
    for (int t = 0; t < 4; t++) {
        GXSetArray(GXAttr(GX_VA_TEX0 + t), mData->texCoords[t], mData->texCoordsSize[t], 8, false);
    }
}

void BinModel::setupVtxDesc(const BinBatch& batch) {
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_INDEX16);
    if (batch.nbt != 0) {
        GXSetVtxDesc(GX_VA_NBT, GX_INDEX16);
        GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_NBT, GX_NRM_NBT3, GX_F32, 0);  // retail: count 2 = NBT3
    } else {
        if (batch.attributes & 0x400) {
            GXSetVtxDesc(GX_VA_NRM, GX_INDEX16);
        }
        GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0);
    }
    if (batch.attributes & 0x800) {
        GXSetVtxDesc(GX_VA_CLR0, GX_INDEX16);
    }
    for (int t = 0; t < 4; t++) {
        if (batch.attributes & (0x2000u << t)) {
            GXSetVtxDesc(GXAttr(GX_VA_TEX0 + t), GX_INDEX16);
        }
    }
}

u8 BinModel::partCategory(const BinShader& shader) const {
    if (shader.alphaCategory != 0) {
        return shader.alphaCategory;
    }
    if (shader.materials[0] < 0 || u32(shader.materials[0]) >= mData->materials.size()) {
        return 0;
    }
    return mData->materials[shader.materials[0]].alphaCategory;
}

void BinModel::initTexObj(GXTexObj* obj, const BinShader& shader, u32 slot) const {
    const BinMaterial& mat = mData->materials[shader.materials[slot]];
    const BinTexture& tex = mData->textures[mat.texture];
    GXInitTexObj(obj, const_cast<u8*>(tex.pixels), tex.width, tex.height, GXTexFmt(tex.format),
                 GXTexWrapMode(mat.wrapS), GXTexWrapMode(mat.wrapT), mat.mipmaps ? GX_TRUE : GX_FALSE);
    if (mat.mipmaps == 0) {
        GXInitTexObjLOD(obj, GX_LINEAR, GX_LINEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
    } else {
        GXInitTexObjLOD(obj, GX_LIN_MIP_LIN, GX_LINEAR, 0.0f, mat.maxLod, -1.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
    }
}

// fn_8001566C: material/TEV setup for one part, then its display list.
void BinModel::drawPart(const BinPart& part, u8 alpha, const Mtx&) {
    if (part.shader >= mData->shaders.size() || part.batch >= mData->batches.size()) {
        return;
    }
    const BinShader& shader = mData->shaders[part.shader];
    const BinBatch& batch = mData->batches[part.batch];
    u8 numTexGens = 0, numStages = 0;
    setupVtxDesc(batch);

    // fn_80014C78: lighting channels and material color.
    const GXColorSrc src = shader.lightMode == 1 ? GX_SRC_REG : GX_SRC_VTX;
    const GXBool lit = (batch.tevMode != 0 && !sChanFlags) ? GX_TRUE : GX_FALSE;
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0, lit, src, src, sLightMask, GX_DF_CLAMP, GXAttnFn(sAttnFn));
    GXSetChanCtrl(GX_ALPHA0, GX_FALSE, src, src, sLightMask, GX_DF_CLAMP, GXAttnFn(sAttnFn));
    GXColor color = shader.tint;
    if (color.a > alpha) {
        color.a = alpha;
    }
    GXSetChanMatColor(GX_COLOR0A0, color);
    GXSetChanMatColor(GX_COLOR1A1, GXColor{0, 0, 0, 0});
    GXSetNumChans(2);
    GXSetChanCtrl(GX_ALPHA1, GX_TRUE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT7, GX_DF_CLAMP, GX_AF_SPOT);

    // fn_80015350 / fn_80014E68: one stage per texture.
    bool anyTexture = false;
    for (u32 t = 0; t < std::min<u32>(batch.numTextures, 8); t++) {
        anyTexture |= shader.materials[t] >= 0 && u32(shader.materials[t]) < mData->materials.size() &&
                      mData->materials[shader.materials[t]].texture < mData->textures.size() &&
                      mData->textures[mData->materials[shader.materials[t]].texture].pixels != nullptr;
    }
    if (!anyTexture) {
        // Untextured (or textures missing from this model): lit vertex/material color only.
        const auto stage = GXTevStageID(numStages++);
        GXSetTevSwapMode(stage, GX_TEV_SWAP0, GX_TEV_SWAP0);
        GXSetTevOrder(stage, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
        GXSetTevColorIn(stage, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_RASC);
        GXSetTevAlphaIn(stage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
        GXSetTevColorOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GXSetTevAlphaOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    } else {
        const u32 numTextures = std::min<u32>(batch.numTextures, 8);
        if (batch.numTextures > 8) {
            static bool sWarned;
            if (!sWarned) {
                sWarned = true;
                OSReport("[bin] %s: batch with %u texture slots (max 8); clamping\n", mData->name,
                         batch.numTextures);
            }
        }
        for (u32 t = 0; t < numTextures; t++) {
            if (shader.materials[t] < 0 || u32(shader.materials[t]) >= mData->materials.size() ||
                mData->materials[shader.materials[t]].texture >= mData->textures.size() ||
                mData->textures[mData->materials[shader.materials[t]].texture].pixels == nullptr) {
                continue;
            }
            const auto stage = GXTevStageID(numStages);
            const auto coord = GXTexCoordID(numTexGens);
            const auto map = GXTexMapID(numTexGens);
            GXTexObj obj;
            initTexObj(&obj, shader, t);
            GXLoadTexObj(&obj, map);
            // TODO(port): animated texture matrices (fn_8001DBF8 / fn_8001DC6C).
            GXSetTexCoordGen2(coord, GX_TG_MTX2x4, kTexGenSrc[t], GX_IDENTITY, GX_FALSE, GX_PTIDENTITY);
            GXSetTevSwapMode(stage, GX_TEV_SWAP0, GX_TEV_SWAP0);
            GXSetTevOrder(stage, coord, map, GX_COLOR0A0);
            const auto& in = kTexStageColorIn[t == 0 ? 0 : 2];
            GXSetTevColorIn(stage, in[0], in[1], in[2], in[3]);
            GXSetTevAlphaIn(stage, GX_CA_ZERO, GX_CA_RASA, GX_CA_TEXA, GX_CA_ZERO);
            GXSetTevColorOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
            GXSetTevAlphaOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
            numStages++;
            numTexGens++;
        }
    }
    if (std::getenv("LM_TRACE_BIN") != nullptr) {
        OSReport("[trace] %s shader %u batch %u: texSlots %u stages %u texgens %u attr %08x\n", mData->name,
                 part.shader, part.batch, batch.numTextures, numStages, numTexGens, batch.attributes);
    }
    GXSetNumTexGens(numTexGens);
    GXSetNumTevStages(numStages);

    // fn_8001D894: cull mode (mirrored for the silhouette pass).
    GXCullMode cull = GXCullMode(batch.cullMode);
    if (sSilhouette) {
        cull = batch.cullMode == 2 ? GX_CULL_FRONT : batch.cullMode == 1 ? GX_CULL_BACK : GX_CULL_NONE;
    }
    GXSetCullMode(cull);
    GXCallDisplayList(const_cast<u8*>(batch.displayList), batch.displayListSize);
    sFaceCounter += batch.faceCount;
}

// fn_80015844: siblings first (only from the head of a sibling list), then this
// node's parts, then its children.
void BinModel::drawNode(bool siblingsDone, u8 pass, u8 alpha, s16 node) {
    if (node < 0 || u32(node) >= mData->nodes.size()) {
        return;
    }
    const BinNode& n = mData->nodes[node];
    if (!siblingsDone) {
        drawNode(false, pass, alpha, n.next);
    }
    // TODO(port): visibility categories in n.flags (lights-off variants, silhouette pass).
    if (!n.parts.empty()) {
        Mtx viewNode, nrm;
        MTXConcat(mView, mNodeMtx[node].m, viewNode);
        GXLoadPosMtxImm(viewNode, GX_PNMTX0);
        Mtx inv;
        if (MTXInverse(viewNode, inv)) {
            MTXTranspose(inv, nrm);
            GXLoadNrmMtxImm(nrm, GX_PNMTX0);
        }
        for (const BinPart& part : n.parts) {
            if (alpha == 0xFF) {
                if (part.shader < mData->shaders.size() && pass == partCategory(mData->shaders[part.shader])) {
                    drawPart(part, alpha, mNodeMtx[node].m);
                }
            } else if (pass == 2) {
                drawPart(part, alpha, mNodeMtx[node].m);
            }
        }
    }
    drawNode(false, pass, alpha, n.child);  // retail recurses with siblings for children too
}

void BinModel::draw(const Mtx view, u8 pass, u8 alpha) {
    if (mData == nullptr) {
        return;
    }
    MTXCopy(const_cast<f32(*)[4]>(view), mView);
    setupArrays();
    GXSetCurrentMtx(GX_PNMTX0);
    drawNode(false, pass, alpha, 0);
}

}  // namespace lm::gfx
