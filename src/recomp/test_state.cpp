// LM_TEST_STATE=<preset>[+<preset>...] (testing): once the game is in gameplay, keeps
// Luigi in a known equipment state, the way the story's event commands would, so features
// that need the Poltergust or an element can be tested from a new game (opening foyer,
// LM_START_MAP=<n>, the debug warps). Applied every gameplay frame from the GBH HUD update
// hook (unused_content.cpp). Documented in CLAUDE.md ("Debugging").
//
// Presets:
//   poltergust  Poltergust 3000 + Game Boy Horror: Luigi flags +0x792 |= 4|8, the same bits
//               the <WEAPON> event command sets (Luigi message handler at 0x800B0040). The
//               player record mirrors them every frame (+0x55 = flag 8 -> GBH HUD shown).
//   fire|water|ice  poltergust + that element (Luigi +0x1184 = 2/3/4), meter kept full
//               (Luigi +0x1188 = 100.0; the record's +0x52 meter level is derived from it).
#include "recomp/hle.h"
#include "recomp/runtime.h"

#include <bit>
#include <cstdlib>
#include <cstring>
#include <string>

namespace recomp {
namespace {

constexpr u32 kActorSlots = 0x803D48A0;  // lbl_803D48A0[0] = Luigi
constexpr u32 kLuigiFlags = 0x792;       // u16: 4 = Poltergust, 8 = Game Boy Horror
constexpr u32 kLuigiElement = 0x1184;    // 1 none (L blows air), 2 fire, 3 water, 4 ice
constexpr u32 kLuigiElemAmount = 0x1188; // f32 0..100 (meter level = amount / 10)

struct State {
    bool poltergust = false;
    int element = -1;
};

State parse() {
    State s;
    const char* env = std::getenv("LM_TEST_STATE");
    if (!env) return s;
    std::string spec = env;
    size_t p = 0;
    while (p <= spec.size()) {
        const size_t e = spec.find('+', p);
        const std::string name = spec.substr(p, e == std::string::npos ? std::string::npos : e - p);
        if (name == "poltergust") s.poltergust = true;
        else if (name == "fire") s.poltergust = true, s.element = 2;
        else if (name == "water") s.poltergust = true, s.element = 3;
        else if (name == "ice") s.poltergust = true, s.element = 4;
        else if (!name.empty()) log("[test] LM_TEST_STATE: unknown preset '%s'", name.c_str());
        if (e == std::string::npos) break;
        p = e + 1;
    }
    return s;
}

}  // namespace

void testStateFrame(Ctx&) {
    static const State s = parse();
    if (!s.poltergust && s.element < 0) return;
    const u32 l = rd32(kActorSlots);
    if (l < 0x80000000u || l >= 0x81800000u) return;
    static bool sLogged = false;
    if (s.poltergust) wr16(l + kLuigiFlags, rd16(l + kLuigiFlags) | 0xC);
    if (s.element >= 0) {
        wr32(l + kLuigiElement, u32(s.element));
        wr32(l + kLuigiElemAmount, std::bit_cast<u32>(100.0f));
    }
    if (!sLogged) {
        sLogged = true;
        log("[test] LM_TEST_STATE applied (poltergust %d, element %d)", s.poltergust, s.element);
    }
}

}  // namespace recomp
