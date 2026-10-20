// Debug "Spawn character" (F1 > Enhancements > Debug): creates one of the character types the
// retail game never places, in front of Luigi, through the game's own factory
// fn_800E977C__Q24Koga9EnManagerFll(type, -1) (what fn_80072554 uses for Luigi's equipment):
// EnManager builds the actor for the type with no JMP row (no ToolData). Nothing happens until
// the player presses a Spawn button. Docs: docs/UNUSED_CONTENT.md ("Second batch").
//
// Guards: only in the game scene with Luigi present; the type's model is loaded first with the
// game's own loader (fn_800620F4, as nozzles.cpp) and the spawn waits for it; types that were
// found to crash or misbehave without their map data are listed but disabled.
#include "port/install.h"
#include "recomp/guest_util.h"
#include "recomp/hle.h"
#include "recomp/menu.h"
#include "recomp/skip.h"
#include "recomp/unused2.h"
#include "recomp/version.h"

#include <imgui.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

namespace recomp::unused2 {
namespace {

struct Spawnable {
    u32 type;
    const char* name;
    const char* what;
    bool safe;  // false: disabled (see note)
    const char* note;
};

// Never-placed types (character table lbl_8033B590; actors2 research list).
const Spawnable kList[] = {
    {69, "telball", "Boo ball", true, nullptr},
    {286, "kopatele", "Bowser with a Boo", true, nullptr},
    {155, "car", "toy car", true, nullptr},
    {156, "plane", "toy plane", true, nullptr},
    {139, "okami1", "okami1 (purpose unknown)", true, nullptr},
    {166, "gun", "toy gun", true, nullptr},
    {165, "heylance", "lance", true, nullptr},
    {163, "heymask", "mask", true, nullptr},
    {164, "heymask2", "mask, second texture set", true, nullptr},
    {162, "hcue", "billiard cue", true, nullptr},
    {167, "zenmai", "wind-up key", true, nullptr},
    {169, "okeito1", "okeito1 (purpose unknown)", true, nullptr},
    {223, "dkikai", "lab machine (dkikai)", true, nullptr},
    {219, "ice", "ice", true, nullptr},
    {216, "turara", "icicle", true, nullptr},
    {285, "obasoul1", "ghost soul (obasoul)", true, nullptr},
    {122, "takara1", "treasure (takara1)", true, nullptr},
    {124, "ibox01", "Item box 1", true, nullptr},
    {125, "ibox02", "Item box 2", true, nullptr},
    {126, "ibox03", "Item box 3", true, nullptr},
    {127, "ibox04", "Item box 4", true, nullptr},
    {128, "ibox05", "Item box 5", true, nullptr},
    {176, "ebone", "bone (ebone)", true, nullptr},
    {72, "ball1", "ball", true, nullptr},
    {73, "ball2", "ball, second texture set", true, nullptr},
    {50, "boy2", "twin boy, second texture set", true, nullptr},
    {75, "telesa1", "Boo (telesa2 model)", true, nullptr},
    {178, "dmario2", "Mario in the painting (dmario model)", true, nullptr},
};

#define kTypeTable VA(0x8033B590)
#define kModelRegistry VA(0x8034E46C)

std::atomic<int> sRequest{-1};  // index into kList
int sPending = -1;               // waiting for the model
u32 sWait = 0;
char sStatus[160] = "";
u32 sEnvRetry = 0, sEnvRetryAt = 0;  // LM_SPAWN of an unlisted type whose model was still loading

bool modelReady(Ctx& c, u32 type) {
    const u32 model = rd32(kTypeTable + 0x1C * type);
    const u32 e = kModelRegistry + 0x34 * model;
    if (rd32(e + 8)) return true;
    if (rd32(e + 0x30) == 0) gu::call(c, GU_SYM("fn_800620F4__Fi"), {model});
    return false;
}

void spawnNow(Ctx& c, const Spawnable& s) {
    const u32 luigi = gu::call(c, GU_SYM("fn_800E9750__Q24Koga9EnManagerFl"), {0});
    if (!gu::inMem1(luigi)) return;
    const u32 idx = gu::call(c, GU_SYM("fn_800E977C__Q24Koga9EnManagerFll"), {s.type, u32(-1)});
    if (s32(idx) < 0) {
        std::snprintf(sStatus, sizeof sStatus, "%s: the game could not create it", s.name);
        log("[spawn] %s (type %u): create failed", s.name, s.type);
        return;
    }
    const u32 act = gu::call(c, GU_SYM("fn_80069130"), {idx});
    if (!gu::inMem1(act)) {
        std::snprintf(sStatus, sizeof sStatus, "%s: created (index %d) but no actor", s.name, s32(idx));
        return;
    }
    const float fx = gu::rdf(luigi + 0xF48), fz = gu::rdf(luigi + 0xF50);
    const float x = gu::rdf(luigi + 0x44) + fx * 200.0f, y = gu::rdf(luigi + 0x48) + 20.0f, z = gu::rdf(luigi + 0x4C) + fz * 200.0f;
    for (u32 o : {0x44u, 0x50u}) {
        gu::wrf(act + o, x);
        gu::wrf(act + o + 4, y);
        gu::wrf(act + o + 8, z);
    }
    wr32(act + 0xB4, rd32(luigi + 0xB4));  // Luigi's room (top byte), so the actor is drawn and updated there
    std::snprintf(sStatus, sizeof sStatus, "%s spawned (actor %08X)", s.name, act);
    log("[spawn] %s (type %u): actor %08X at %.0f,%.0f,%.0f", s.name, s.type, act, x, y, z);
}

// ---- particle test --------------------------------------------------------------------------
// Particle effects are .pcr files in the game archive's kh_effect directory (resource type
// 'KH_E'). fn_80164F5C(id, pos, dir) starts effect `id` of the table lbl_80381B58 + 0xA90 (142
// entries: .pcr, texture, optional second texture) through the LPS manager lbl_803D990C:
// getResource('KH_E', name) for each file, then fn_8015FE9C(manager, pcr, tex, tex2) creates the
// emitter, whose +0x214 is its position. Unused: kome_A/B and ebi_A/B (rice and shrimp, table
// entries 8-11 that no code starts) and pcr files that are in no table at all (elh_fire_a/b,
// elh_ice_a/b, element_test1a-c, candle_yure, smlp). A .pcr does not name its texture, so
// those get the texture their retail siblings use (port's choice, listed).
struct Particle {
    const char* pcr;
    const char* tex;
    int tableId;  // >= 0: start through the game's effect table
    const char* note;
};
const Particle kParticles[] = {
    {"kome_A.pcr", "rice.bti", 8, "rice (effect table 8, never started)"},
    {"kome_B.pcr", "rice.bti", 9, "rice (effect table 9, never started)"},
    {"ebi_A.pcr", "ebi.bti", 10, "shrimp (effect table 10, never started)"},
    {"ebi_B.pcr", "ebi.bti", 11, "shrimp (effect table 11, never started)"},
    {"elh_fire_a.pcr", "honoo1.bti", -1, "fire element, early (texture: honoo1)"},
    {"elh_fire_b.pcr", "hibana2.bti", -1, "fire element, early (texture: hibana2)"},
    {"elh_ice_a.pcr", "ice_smok.bti", -1, "ice element, early (texture: ice_smok)"},
    {"elh_ice_b.pcr", "icerock32.bti", -1, "ice element, early (texture: icerock32)"},
    {"element_test1a.pcr", "glow2_i.bti", -1, "element test (texture: glow2_i)"},
    {"element_test1b.pcr", "glow2_i.bti", -1, "element test (texture: glow2_i)"},
    {"element_test1c.pcr", "glow2_i.bti", -1, "element test (texture: glow2_i)"},
    {"candle_yure.pcr", "honoo1.bti", -1, "swaying candle flame (texture: honoo1)"},
    {"smlp.pcr", "hokori4.bti", -1, "smlp (texture: hokori4)"},
};
std::atomic<int> sParticle{-1};
char sParticleStatus[160] = "";
u32 sVecs = 0;  // two guest vectors (position, direction)

void startParticle(Ctx& c, const Particle& p) {
    const u32 luigi = gu::call(c, GU_SYM("fn_800E9750__Q24Koga9EnManagerFl"), {0});
    if (!gu::inMem1(luigi)) return;
    if (!sVecs) sVecs = extraAlloc(0x20, 8);
    if (!sVecs) return;
    const float fx = gu::rdf(luigi + 0xF48), fz = gu::rdf(luigi + 0xF50);
    gu::wrf(sVecs + 0, gu::rdf(luigi + 0x44) + fx * 150.0f);
    gu::wrf(sVecs + 4, gu::rdf(luigi + 0x48) + 100.0f);
    gu::wrf(sVecs + 8, gu::rdf(luigi + 0x4C) + fz * 150.0f);
    gu::wrf(sVecs + 0x10, 0.0f);
    gu::wrf(sVecs + 0x14, 1.0f);
    gu::wrf(sVecs + 0x18, 0.0f);
    if (p.tableId >= 0) {
        gu::call(c, GU_SYM("fn_80164F5C"), {u32(p.tableId), sVecs, sVecs + 0x10});
        std::snprintf(sParticleStatus, sizeof sParticleStatus, "%s started (effect %d)", p.pcr, p.tableId);
        log("[particles] %s: effect table %d", p.pcr, p.tableId);
        return;
    }
    const u32 arc = gu::call(c, GU_SYM("fn_80066FB4"));
    static std::map<std::string, u32> sNames;
    auto gname = [&](const char* s) {
        u32& g = sNames[s];
        if (!g && (g = extraAlloc(u32(std::strlen(s) + 1), 4))) std::memcpy(hostp(g), s, std::strlen(s) + 1);
        return g;
    };
    const u32 pcr = gu::call(c, GU_SYM("getResource__10JKRArchiveFUlPCc"), {arc, 0x4B485F45, gname(p.pcr)});
    const u32 tex = gu::call(c, GU_SYM("getResource__10JKRArchiveFUlPCc"), {arc, 0x4B485F45, gname(p.tex)});
    if (!gu::inMem1(pcr) || !gu::inMem1(tex)) {
        std::snprintf(sParticleStatus, sizeof sParticleStatus, "%s: resource not found", p.pcr);
        return;
    }
    const u32 em = gu::call(c, GU_SYM("fn_8015FE9C"), {VA(0x803D990C), pcr, tex, 0});
    if (gu::inMem1(em)) {
        for (int i = 0; i < 3; i++) {
            wr32(em + 0x214 + 4 * i, rd32(sVecs + 4 * i));
            wr32(em + 0x220 + 4 * i, rd32(sVecs + 0x10 + 4 * i));
            wr32(em + 0x22C + 4 * i, 0);
        }
        wr32(em + 0xC4, 0);
        wr32(em + 0xC8, 0);
    }
    std::snprintf(sParticleStatus, sizeof sParticleStatus, "%s: emitter %08X", p.pcr, em);
    log("[particles] %s + %s: emitter %08X", p.pcr, p.tex, em);
}

}  // namespace

void particleSection(bool inGame) {
    recomp::menu::openTreesForCheck();
    if (!ImGui::TreeNode("Particle test (unused effects)")) return;
    ImGui::TextDisabled("Starts the effect 150 units in front of Luigi with the game's particle system.");
    ImGui::BeginDisabled(!inGame);
    for (size_t i = 0; i < std::size(kParticles); i++) {
        char label[48];
        std::snprintf(label, sizeof label, "%s##pt%zu", kParticles[i].pcr, i);
        if (recomp::menu::SmallButton(label)) sParticle = int(i);
        ImGui::SameLine();
        ImGui::TextDisabled("%s", kParticles[i].note);
    }
    ImGui::EndDisabled();
    if (sParticleStatus[0]) ImGui::Text("%s", sParticleStatus);
    ImGui::TreePop();
}

void spawnFrame(Ctx& c) {
    {
        // Test aid: LM_PARTICLE=<index>@<guest frame>,...
        static const char* sEnvP = std::getenv("LM_PARTICLE");
        const u32 now = skip::guestFrames();
        for (const char* p = sEnvP; p && *p;) {
            unsigned idx = 0, frame = 0;
            if (std::sscanf(p, "%u@%u", &idx, &frame) == 2 && frame == now && idx < std::size(kParticles)) sParticle = int(idx);
            const char* comma = std::strchr(p, ',');
            p = comma ? comma + 1 : nullptr;
        }
        const int pi = sParticle.exchange(-1);
        if (pi >= 0) startParticle(c, kParticles[pi]);
    }
    // Test aid: LM_SPAWN=<type>@<guest frame>,...
    static const char* sEnv = std::getenv("LM_SPAWN");
    if (sEnv) {
        const u32 now = skip::guestFrames();
        for (const char* p = sEnv; p && *p;) {
            unsigned type = 0, frame = 0;
            if (std::sscanf(p, "%u@%u", &type, &frame) == 2 && frame == now) {
                bool listed = false;
                for (size_t i = 0; i < std::size(kList); i++)
                    if (kList[i].type == type) sRequest = int(i), listed = true;
                if (!listed && type < 400) {  // any type (testing, e.g. the colour variants' types)
                    static Spawnable sAny{0, "type", "test", true, nullptr};
                    sAny.type = type;
                    if (modelReady(c, type)) spawnNow(c, sAny);
                    else sEnvRetry = type, sEnvRetryAt = now + 30;
                }
            }
            if (sEnvRetry && now == sEnvRetryAt) {
                static Spawnable sAny2{0, "type", "test", true, nullptr};
                sAny2.type = sEnvRetry;
                if (modelReady(c, sEnvRetry)) spawnNow(c, sAny2);
                sEnvRetry = 0;
            }
            const char* comma = std::strchr(p, ',');
            p = comma ? comma + 1 : nullptr;
        }
    }
    const int r = sRequest.exchange(-1);
    if (r >= 0) {
        sPending = r;
        sWait = 0;
    }
    if (sPending < 0) return;
    const Spawnable& s = kList[sPending];
    if (!modelReady(c, s.type)) {
        if (++sWait > 300) {
            std::snprintf(sStatus, sizeof sStatus, "%s: model did not load", s.name);
            sPending = -1;
        }
        return;
    }
    sPending = -1;
    spawnNow(c, s);
}

void spawnSection(bool inGame) {
    recomp::menu::openTreesForCheck();
    if (!ImGui::TreeNode("Spawn character (never placed in retail)")) return;
    ImGui::TextDisabled("Creates the character in front of Luigi with the game's own factory, without its map\n"
                        "data. Some only make sense in their scripted scene. Debug feature: may misbehave.");
    if (!inGame) ImGui::TextDisabled("Available while playing.");
    ImGui::BeginDisabled(!inGame);
    if (ImGui::BeginTable("spawn", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        for (size_t i = 0; i < std::size(kList); i++) {
            const Spawnable& s = kList[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            char label[32];
            std::snprintf(label, sizeof label, "Spawn##sp%zu", i);
            ImGui::BeginDisabled(!s.safe);
            if (recomp::menu::SmallButton(label)) sRequest = int(i);
            ImGui::EndDisabled();
            ImGui::TableNextColumn();
            ImGui::Text("%3u  %s", s.type, s.name);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s%s%s", s.what, s.note ? " - " : "", s.note ? s.note : "");
        }
        ImGui::EndTable();
    }
    ImGui::EndDisabled();
    if (sStatus[0]) ImGui::Text("%s", sStatus);
    ImGui::TreePop();
}

}  // namespace recomp::unused2
