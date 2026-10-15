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

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <unordered_map>

namespace recomp {

u32 copyDispCount();  // hle_gx.cpp

namespace {

using interp::TagCat;
using interp::TagScope;

// Furniture/object scopes open: bin models drawn inside are objects, outside them map geometry.
int sObjectDepth = 0;
// A stream item drawn with a shared S3D model (fn_800F3120 from fn_8011FB40): the model's group is
// per item, not per model (u32 item address; 0: none).
u32 sStreamItem = 0;
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
    // fn_80091A70: a cloth sheet (16x16 grid in world space) drawn as one GXBegin quad per cell from
    // four grid points (column r27, row offset r29); cells whose points are not valid are skipped, so
    // the n-th quad is not always the same cell. (Not the Poltergust hose, which is part of an S3D model.)
    interp::addSiteTag({VA(0x80091EF4), VA(0x80092110), TagCat::Effect, 27, 29});
    // JPA-style emitters (fn_801604FC -> renderer -> fn_8016169C(renderer, view)): one particle per
    // iteration in r19 (list via +0x5C), its renderer in r30; CPU-built quads.
    // Its age (+1 per frame, every particle) is the u16 at +0x32.
    interp::addSiteTag({VA(0x8016169C), VA(0x80161ECC), TagCat::Particle, 19, 30, 0x32, 1});
    // The other JPA-style renderers: fn_801620AC (particle r19, renderer r26, age +0x32) and
    // fn_801611CC (child items r30 via +0x24, owner r29): Bowser's flames, the boss arenas' skies.
    interp::addSiteTag({VA(0x801620AC), VA(0x801628E4), TagCat::Particle, 19, 26, 0x32, 1});
    interp::addSiteTag({VA(0x801611CC), VA(0x80161618), TagCat::Particle, 30, 29});
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
    interp::tagGroup(c.r[3]);  // one instance: its matrices blend alike (interp.cpp, replayPair)
    callOriginal(c, "fn_80059C54");
}
HLE(fn_80059F6C) {
    registerSiteTags();
    TagScope t(TagCat::Object, c.r[3], rd32(c.r[3] + 4));
    // One instance: its matrices blend alike (interp.cpp, replayPair). A shared model drawn once per
    // stream item is one instance per item.
    interp::tagGroup(sStreamItem ? sStreamItem : c.r[3]);
    const u32 item = sStreamItem;
    sStreamItem = 0;  // (not for models drawn further inside)
    callOriginal(c, "fn_80059F6C");
    sStreamItem = item;
}

// The element streams' items (fn_8011FB40; water drops, fire and ice bits): two loops set ONE shared
// S3D model's matrix from each item (fn_80059710) and draw it through fn_800F3120(holder) ->
// fn_80059F6C(holder + 8). Without a scope per item the draws of all items were one model instance
// paired by occurrence: drops swapped partners (random turns, shown at the game frame) and one
// respawning drop switched all of them (~320 per frame in the Pipe Room) to 30 Hz (sweep A; the
// water spray's shimmer). Loop 1 (call at 0x801201F4): items of 0x34 bytes at [r31 + 0x4C] + r27;
// loop 2 (0x801202F8): items of 0x68 bytes at [r31 + 0x48] + r26.
HLE(fn_800F3120) {
    u32 item = 0;
    if (c.lr == VA(0x801201F4)) item = rd32(c.r[31] + 0x4C) + c.r[27];
    else if (c.lr == VA(0x801202F8)) item = rd32(c.r[31] + 0x48) + c.r[26];
    if (!item) {
        callOriginal(c, "fn_800F3120");
        return;
    }
    TagScope t(TagCat::Particle, item, 0x11FB40);
    sStreamItem = item;
    callOriginal(c, "fn_800F3120");
    sStreamItem = 0;
}
HLE(fn_8005A280) {  // shadow casters
    registerSiteTags();
    TagScope t(TagCat::Object, c.r[3], rd32(c.r[3] + 4));
    interp::tagGroup(c.r[3]);  // one instance: its matrices blend alike (interp.cpp, replayPair)
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
// fn_80015BDC(room lighting r3, ambient colour r4), from the room loop fn_80011824: loads the room's
// lights and its ambient colour (GXSetChanAmbColor at 0x80015C88). Untagged, the ambient fades
// (arriving in the foyer, a room's lights coming on, lit/dark changes: ~10/255 per game frame)
// stepped at 30 Hz (sweep C: 94% of the foyer's ramp at the tape change).
HLE(fn_80015BDC) {
    TagScope t(TagCat::World, c.r[3], 0x15BDC);
    callOriginal(c, "fn_80015BDC");
}

HLE(fn_80015844) {  // one node (r6) of the model being drawn, recursive
    const TagCat cat = sObjectDepth > 0 ? TagCat::Object : TagCat::World;
    TagScope t(cat, 0, 0x15844, c.r[6] & 0xFFFF);
    callOriginal(c, "fn_80015844");
}

// --- cloth ---------------------------------------------------------------------------------------
// fn_8008E580(object, pass) -> fn_80091A70(object, mode): a cloth sheet (a 16x16 grid of world-space
// points, CPU-built quads): cell n pairs with cell n of the same object and mode. (It was taken for
// the Poltergust hose at first; the hose is part of an S3D model: its matrices blend as one instance,
// fn_80059C54's tagGroup.)
HLE(fn_80091A70) {
    TagScope t(TagCat::Effect, c.r[3], 0x91A70, c.r[4]);
    callOriginal(c, "fn_80091A70");
}

// The room cloths: the room loop (fn_800118FC -> fn_80028974) walks a table of 0x388-byte items
// (11 slots in map 2) and draws the active ones with fn_80027F08(item): a CPU-simulated cloth mesh
// in an indexed F32 position array (149 vertices) the game rewrites every game frame, loaded with
// the item's own matrix. Only the 2F bathroom's shower curtain was active in the rooms checked
// (slot 10; the ballroom's drapes, the dining room's tablecloth and the others there are
// furniture models, blended by their matrices). It sways all the
// time (median 6, up to 37 units per game frame while the Poltergust pulls at it). Untagged, its
// array had no identity and the curtain stayed at the game frame's shape (user report: "this
// curtain did not get interpolated"). The item is its identity: its position array pairs with the
// same item's array of the game frame before and blends vertex by vertex (interp.cpp, replayPair).
HLE(fn_80027F08) {
    TagScope t(TagCat::Object, c.r[3], 0x27F08);
    callOriginal(c, "fn_80027F08");
}
// The other untagged position-array draw of the room loop: fn_80025FC8 walks 0x24C-byte items and
// draws each with fn_80025E00(item) (its own array and matrix). Tagged alike.
HLE(fn_80025E00) {
    TagScope t(TagCat::Object, c.r[3], 0x25E00);
    callOriginal(c, "fn_80025E00");
}

// --- ghosts' quads ---------------------------------------------------------------------------------
// fn_80061690 loops over the ghost list (0x803D48A0) and draws one CPU-built quad per ghost with
// fn_8005E110(.., ghost + 8 = its position, ..): its identity is the ghost (Bogmire's shadow ghosts,
// Chauncey's arena). Untagged, they were shown at the game frame's place.
HLE(fn_8005E110) {
    TagScope t(TagCat::Object, c.r[5] - 8, 0x5E110);
    callOriginal(c, "fn_8005E110");
}

// fn_80118D3C walks a table of 0xA8-byte items (five layers) and draws each one's CPU-built strip
// with fn_80118E44(item): the item is its identity.
HLE(fn_80118E44) {
    TagScope t(TagCat::Effect, c.r[3], 0x118E44);
    callOriginal(c, "fn_80118E44");
}

// The water spray (and the other element streams): fn_8011FB40 / fn_80120514 walk each stream's
// points (24 bytes each: position first) and draw every one as a GX point with fn_80120C30(point,
// size, colour). Untagged, the points were shown at the game frame's places while everything around
// them blended: the spray flickered.
HLE(fn_80120C30) {
    // LM_POINT_PEEK=<n> (test aid): log the 6 words of the first n points drawn per frame (finding an
    // age / birth field for the identity).
    static const int sPeek = std::getenv("LM_POINT_PEEK") ? std::atoi(std::getenv("LM_POINT_PEEK")) : 0;
    if (sPeek > 0) {
        static u32 frame = ~0u;
        static int n = 0;
        if (copyDispCount() != frame) frame = copyDispCount(), n = 0;
        if (n++ < sPeek)
            log("[point] f%u %08X lr %08X %08X %08X %08X %08X %08X %08X", frame, c.r[3], c.lr, rd32(c.r[3]), rd32(c.r[3] + 4),
                rd32(c.r[3] + 8), rd32(c.r[3] + 12), rd32(c.r[3] + 16), rd32(c.r[3] + 20));
    }
    // A point slot refilled from the nozzle is another particle: each point is position (+0) and
    // velocity (+12), moved by its velocity every game frame. A point that is not where its last
    // frame's position + velocity puts it (or was not drawn in the game frame before) was born anew:
    // new generation, so it never pairs with the slot's previous occupant (the idle ice mist's points
    // paired across rebirths and were shown at the game frame by the turn-back rule; sweep A/B).
    if (interp::capturing()) {
        struct Last {
            u32 frame;
            float p[3], v[3];
        };
        static std::unordered_map<u32, Last> last;
        const u32 frame = copyDispCount();
        float p[3], v[3];
        for (int k = 0; k < 3; k++) p[k] = flt(rd32(c.r[3] + 4 * k)), v[k] = flt(rd32(c.r[3] + 12 + 4 * k));
        auto it = last.find(c.r[3]);
        if (it == last.end() || it->second.frame != frame) {  // (once per game frame: a point drawn in several views)
            bool reborn = it == last.end() || it->second.frame + 1 != frame;
            if (!reborn) {
                float d2 = 0, v2 = 0;
                for (int k = 0; k < 3; k++) {
                    const float e = p[k] - (it->second.p[k] + it->second.v[k]);
                    d2 += e * e, v2 += it->second.v[k] * it->second.v[k];
                }
                reborn = !(d2 <= std::max(25.0f, 0.25f * v2));
            }
            if (reborn && it != last.end()) interp::tagGeneration(c.r[3]);
            if (last.size() > 8192) last.clear();
            last[c.r[3]] = {frame, {p[0], p[1], p[2]}, {v[0], v[1], v[2]}};
        }
    }
    TagScope t(TagCat::Particle, c.r[3], 0x120C30);
    callOriginal(c, "fn_80120C30");
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
    // The packets of one model share its matrix buffer (+0x14): one instance for switching.
    interp::tagGroup(rd32(c.r[3] + 0x14));
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

// --- text boxes: shown at the game frame ----------------------------------------------------------
// Every text box is shown exactly as in its game frame, at 30 Hz, as on the console
// (interp::FrameLockScope; docs/INTERPOLATION.md "Text boxes at the game frame"). The letters appear
// at the game rate; a box that opened smoothly around them, or moved between presents, made the text
// jump against it (user report after 4301b1e: "the text on textboxes is still jumpy").
// fn_80044138, from the frame loop fn_8000BCE8, draws every message window (fn_800433B4): the box
// picture (fn_8003B054, 480x120, grows open over 3-4 game frames), the speaker icon, the text, the
// "more" arrow and choices. All dialogue goes through it: Neville, Toad, E. Gadd in the lab and on
// the Game Boy Horror call, signs. (E. Gadd's call window on the GBH screen: widescreen.cpp
// fn_8003D634 / fn_80114E18; the room name: fn_80045BDC.)
namespace {
u32 sTextBoxFrame = ~0u;  // the game frame (GXCopyDisp count) a message window last drew in
}  // namespace
namespace tags {
// A message window is on screen (drawn in this game frame or the one before; the HUD is drawn
// before the windows). While one is, the Game Boy Horror and the event portrait belong to the text
// box too (widescreen.cpp): E. Gadd's call is a message window next to his portrait on the GBH,
// which shakes as the call comes in (its bezel blended while the screen inside stayed put).
bool textBoxShown() { return sTextBoxFrame != ~0u && copyDispCount() - sTextBoxFrame <= 1; }
}  // namespace tags
HLE(fn_80044138) {
    if (!interp::capturing()) return callOriginal(c, "fn_80044138");
    interp::FrameLockScope lock;
    const u32 draws = interp::drawCount();
    callOriginal(c, "fn_80044138");
    if (interp::drawCount() != draws) sTextBoxFrame = copyDispCount();
}

// --- skies ------------------------------------------------------------------------------------------
// The sky dome (a J3D model with texture SRT scroll / colour animations: map 2's vrball_M, the boss
// arenas' vrball_B on maps 10, 11 and 13, vrKoopa on map 9; docs/INTERPOLATION.md "Boss arena skies").
// fn_8002FA34, from the room loop fn_80011824 (per view, before the rooms), advances the sky's
// animations, places it at the camera (fn_800102B0, J3DSys's view matrix: view-space translation 0)
// and draws it (viewCalc, drawInit, its draw buffers): its matrices, texture matrices, projection,
// material / TEV / fog colours and its own light (fn_800AAF5C: constant J3D channel colours and GX
// light 7) are all set inside, so nothing scene-wide (Bogmire's lightning light 0-2, room ambient)
// reaches it. Its values are marked as the sky's (interp::pushSky): the dome's matrices follow the
// blended camera like any object, and its texture scrolls blend the short way when they wrap
// (blendTexWrap `sky`: the layers repeat 2-10 times, outside the general +-4 bound, and swept back
// through the whole texture in one game frame when a layer wrapped: the "jumpy" boss skies).
// LM_SKY_LOCK=1 (test aid): the boss arenas' sky (maps 9, 10, 11, 13) shown exactly as in its game
// frame (interp::FrameLockScope, like the text boxes; the camera's turns then step it at 30 Hz),
// =2: every map's sky.
namespace {
bool skyLocked() {
    static const int sMode = std::getenv("LM_SKY_LOCK") ? std::atoi(std::getenv("LM_SKY_LOCK")) : 0;
    if (sMode == 0) return false;
    if (sMode == 2) return true;
    const u32 map = rd32(VA(0x804D80C8));  // sCurMapNo
    return map == 9 || map == 10 || map == 11 || map == 13;
}
}  // namespace
HLE(fn_8002FA34) {
    // LM_SKY_HIDE=1 (test aid): the sky is not drawn at all (finding sky pixels: compare with a run
    // that draws it).
    static const bool sHide = std::getenv("LM_SKY_HIDE") != nullptr;
    if (sHide) return;
    if (!interp::capturing()) return callOriginal(c, "fn_8002FA34");
    interp::tagLogMark("sky begin");
    if (skyLocked()) {
        interp::FrameLockScope lock;
        callOriginal(c, "fn_8002FA34");
    } else {
        interp::pushSky();
        callOriginal(c, "fn_8002FA34");
        interp::popSky();
    }
    interp::tagLogMark("sky end");
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
