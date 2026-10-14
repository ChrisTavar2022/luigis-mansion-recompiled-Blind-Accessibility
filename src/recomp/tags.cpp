// Identity tags for frame interpolation (interp.h, docs/INTERPOLATION.md "Tagged interpolation").
//
// Hooks on the game's draw entry points open a tag scope around each thing they draw, so every
// matrix, light and CPU-built vertex block the GX HLE notes inside carries a stable identity: the
// scope path (view > object > part > mesh), the GX call site and its occurrence there. The
// interpolator pairs values between game frames only by identical identities.
//
// Each hook wraps the original (callOriginal) and changes nothing else. Addresses are USA; hooks are
// tagged `us pal` in tools/recomp/hooks.txt where the European function was checked to take the same
// arguments, and call-site ranges go through VA().
#include "recomp/hle.h"
#include "recomp/interp.h"
#include "recomp/version.h"

namespace recomp {

namespace {

using interp::TagCat;
using interp::TagScope;

// Furniture/object scopes open: bin models drawn inside are objects, outside them map geometry.
int sObjectDepth = 0;
struct ObjectDepth {
    ObjectDepth() { sObjectDepth++; }
    ~ObjectDepth() { sObjectDepth--; }
};

// Call-site tags: draw loops that handle one item per iteration inside one function.
void registerSiteTags() {
    static bool done = false;
    if (done) return;
    done = true;
    // (VA() needs the version map, which is ready once the game runs.)
    // Dust and other particle groups (fn_801304B0 -> fn_8012FD14(manager, view, group)): one
    // particle per iteration in r30, its group in r29.
    interp::addSiteTag({VA(0x8012FD14), VA(0x8012FE84), TagCat::Particle, 30, 29});
    // Second particle draw (fn_8012CB74 -> fn_8012B6C4(manager, view, group)): particle r29, group r28.
    // Its remaining life counts down in the u16 at +0x24 (98% of frames in the vacuum scenario).
    interp::addSiteTag({VA(0x8012B6C4), VA(0x8012B918), TagCat::Particle, 29, 28, 0x24, -1});
    // fn_80162AE8 -> fn_801796EC: 45 fixed slots of 0x18 bytes (array at [r25]), slot offset in r28.
    interp::addSiteTag({VA(0x801796EC), VA(0x8017996C), TagCat::Particle, 28, 25});
    // The Poltergust hose (fn_80091A70): a tube drawn as a grid of quads, one GXBegin per cell from
    // four grid points (column r27, row offset r29); cells whose points are not valid are skipped, so
    // the n-th quad is not always the same cell (quads swapped places: the hose "freaked out").
    interp::addSiteTag({VA(0x80091EF4), VA(0x80092110), TagCat::Effect, 27, 29});
    // JPA-style emitters (fn_801604FC -> renderer -> fn_8016169C(renderer, view)): one particle per
    // iteration in r19 (list via +0x5C), its renderer in r30; CPU-built quads.
    // Its age (+1 per frame, every particle) is the u16 at +0x32.
    interp::addSiteTag({VA(0x8016169C), VA(0x80161ECC), TagCat::Particle, 19, 30, 0x32, 1});
}

}  // namespace

namespace tags {
void init() { registerSiteTags(); }
}  // namespace tags

// --- views -------------------------------------------------------------------------------------
// (fn_8000BB98, the per-view scene draw, is hooked in enhanced_lighting.cpp: it opens the View scope.)

// Events 5/6 (Mario in the painting, the 2F bathroom): a model drawn with its own camera into a
// 128x128 copy (fn_8006033C, docs/RENDERING.md "Indirect effects").
HLE(fn_8006033C) {
    registerSiteTags();
    TagScope v(TagCat::View, VA(0x8006033C), 0, c.r[3]);
    callOriginal(c, "fn_8006033C");
}

// --- S3DModel (characters, Luigi, ghosts, Poltergust, props drawn as actors) ---------------------
// Model draws take the model instance in r3 (its data at +4); node draws (model, node index);
// fn_80058A60 loads a mesh's matrices (model, mesh entry), fn_800595B8 draws it (model, mesh).
HLE(fn_80059C54) {
    registerSiteTags();
    TagScope t(TagCat::Object, c.r[3], rd32(c.r[3] + 4));
    callOriginal(c, "fn_80059C54");
}
HLE(fn_80059F6C) {
    registerSiteTags();
    TagScope t(TagCat::Object, c.r[3], rd32(c.r[3] + 4));
    callOriginal(c, "fn_80059F6C");
}
HLE(fn_8005A280) {  // shadow casters
    registerSiteTags();
    TagScope t(TagCat::Object, c.r[3], rd32(c.r[3] + 4));
    callOriginal(c, "fn_8005A280");
}
HLE(fn_800597C0) {
    TagScope t(TagCat::Object, c.r[3], 0x597C0, c.r[4]);
    callOriginal(c, "fn_800597C0");
}
HLE(fn_800598E0) {
    TagScope t(TagCat::Object, c.r[3], 0x598E0, c.r[4]);
    callOriginal(c, "fn_800598E0");
}
HLE(fn_800599E8) {
    TagScope t(TagCat::Object, c.r[3], 0x599E8, c.r[4]);
    callOriginal(c, "fn_800599E8");
}
HLE(fn_80058A60) {
    TagScope t(TagCat::Object, c.r[3], 0x58A60, c.r[4]);
    callOriginal(c, "fn_80058A60");
}
HLE(fn_800595B8) {
    TagScope t(TagCat::Object, c.r[3], 0x595B8, c.r[4]);
    callOriginal(c, "fn_800595B8");
}

// Actor lighting (fn_80060E70's actor loop: the actor in the loop's r21): the GX lights loaded for
// one actor belong to it.
HLE(fn_80057F90) {
    const bool inLoop = c.lr >= VA(0x80060E70) && c.lr < VA(0x80061278);
    TagScope t(inLoop ? TagCat::Object : TagCat::View, inLoop ? c.r[21] : 0, inLoop ? 0x57F90 : 0);
    callOriginal(c, "fn_80057F90");
}
HLE(fn_800581E8) {
    const bool inLoop = c.lr >= VA(0x80060E70) && c.lr < VA(0x80061278);
    TagScope t(inLoop ? TagCat::Object : TagCat::View, inLoop ? c.r[21] : 0, inLoop ? 0x581E8 : 0);
    callOriginal(c, "fn_800581E8");
}

// --- map rooms and furniture (bin models) ---------------------------------------------------------
// fn_80015CF4(model, node matrices, pass, ...): rooms (fn_8001798C: no matrices) are map
// geometry; inside a furniture scope (fn_8001F098 / fn_8001F138(object)) the model is an object.
HLE(fn_8001F098) {
    registerSiteTags();
    ObjectDepth d;
    TagScope t(TagCat::Object, c.r[3], 0x1F098);
    callOriginal(c, "fn_8001F098");
}
HLE(fn_8001F138) {
    registerSiteTags();
    ObjectDepth d;
    TagScope t(TagCat::Object, c.r[3], 0x1F138);
    callOriginal(c, "fn_8001F138");
}
HLE(fn_80015CF4) {
    registerSiteTags();
    TagScope t(sObjectDepth > 0 || c.r[4] != 0 ? TagCat::Object : TagCat::World, c.r[3], c.r[4], c.r[5] & 0xFF);
    callOriginal(c, "fn_80015CF4");
}
HLE(fn_80015844) {  // one node (r6) of the model being drawn, recursive
    const TagCat cat = sObjectDepth > 0 ? TagCat::Object : TagCat::World;
    TagScope t(cat, 0, 0x15844, c.r[6] & 0xFFFF);
    callOriginal(c, "fn_80015844");
}

// --- Luigi's effects -----------------------------------------------------------------------------
// fn_8008E580(object, pass) -> fn_80091A70(object, mode): CPU-built segments drawn for the
// Poltergust (hundreds of vertex blocks per frame): segment n pairs with segment n of the same
// object and mode (and only with the same vertex count).
HLE(fn_80091A70) {
    TagScope t(TagCat::Effect, c.r[3], 0x91A70, c.r[4]);
    callOriginal(c, "fn_80091A70");
}

// --- particles ---------------------------------------------------------------------------------
// A particle slot linked into or out of a group: its next use is another particle.
HLE(fn_80127A74) {
    if (c.r[5]) interp::tagGeneration(c.r[5]);
    callOriginal(c, "fn_80127A74");
}
HLE(fn_80127AA8) {
    if (c.r[3]) interp::tagGeneration(c.r[3]);
    if (c.r[4]) interp::tagGeneration(c.r[4]);
    if (c.r[5]) interp::tagGeneration(c.r[5]);
    callOriginal(c, "fn_80127AA8");
}

// --- J3D ---------------------------------------------------------------------------------------
// Shape packets belong to one model instance and one shape: the packet is the identity.
HLE(draw__14J3DShapePacketFv) {
    TagScope t(TagCat::Object, c.r[3], 0x13D);
    callOriginal(c, "draw__14J3DShapePacketFv");
}

// Skinned shapes load the joint matrices of each matrix group (J3DShapeMtx::loadMtxIndx_*(this,
// slot, joint index)), skipping ones already in place, so the n-th load of a packet is not always
// the same joint: each load is tagged with its joint index.
#define J3D_MTX_LOAD(fn)                                       \
    HLE(fn) {                                                  \
        TagScope t(TagCat::Object, 0, 0x4D7, c.r[5] & 0xFFFF); \
        callOriginal(c, #fn);                                  \
    }
J3D_MTX_LOAD(loadMtxIndx_PNGP__11J3DShapeMtxCFiUs)
J3D_MTX_LOAD(loadMtxIndx_PCPU__11J3DShapeMtxCFiUs)
J3D_MTX_LOAD(loadMtxIndx_NCPU__11J3DShapeMtxCFiUs)
J3D_MTX_LOAD(loadMtxIndx_PNCPU__11J3DShapeMtxCFiUs)
#undef J3D_MTX_LOAD

// J3DSys::drawInit loads default matrices into every slot (10 position, normal and texture
// matrices) before each J3D draw phase: tagged so they pair (identical values; they used to count
// as ~480 untagged values per game frame).
HLE(drawInit__6J3DSysFv) {
    TagScope t(TagCat::Object, c.r[3], 0xD1);
    callOriginal(c, "drawInit__6J3DSysFv");
}

// A material packet loads its material (texture matrices among it) and then draws its shapes.
HLE(draw__12J3DMatPacketFv) {
    TagScope t(TagCat::Object, c.r[3], 0x13E);
    callOriginal(c, "draw__12J3DMatPacketFv");
}

// --- 2D ----------------------------------------------------------------------------------------
HLE(draw__7J2DPaneFiiPC14J2DGrafContextb) {
    TagScope t(TagCat::Hud, c.r[3], 0x2D);
    callOriginal(c, "draw__7J2DPaneFiiPC14J2DGrafContextb");
}

// --- memory ------------------------------------------------------------------------------------
// A freed heap block: objects that lived in it get new identities (a new actor allocated at the
// same address is not the old one).
HLE(free__10JKRExpHeapFPv) {
    const u32 p = c.r[4];
    if (p >= 0x80000010u && p < 0x84000000u) {
        u32 size = rd32(p - 0x10 + 4);
        if (size == 0 || size > (16u << 20)) size = 64;
        interp::tagFreed(p, size);
    }
    callOriginal(c, "free__10JKRExpHeapFPv");
}

}  // namespace recomp
