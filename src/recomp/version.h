#pragma once
// Which game release is running, and USA -> European address mapping for host code that
// reads or patches game data (docs/SETUP.md, "Version-aware code").
//
// Host code is written against the USA game (GLME01): its addresses come from the USA
// symbol map. Wrap each such address in VA(0x...): on the USA game it is the address itself,
// on the European game (GLMP01) the matching address from src/recomp/version_addrs.inc, which
// tools/recomp/gen_version_map.py generates by scanning the sources for VA(...) and mapping
// each one with tools/pal_diff.py's function/data matching (code addresses: the same
// instruction in the matched function). Re-run the script after adding a VA(...).
//
// Struct field offsets are not generated: write VOFF(usa, european) after checking the field
// with `tools/pal_diff.py field` (several classes gained members in the European build, e.g.
// Luigi from +0x134C on is shifted by 4).
//
// Game functions are called/hooked by their USA names on both versions (gen_version_map.py
// names their European counterparts the same); hooks need a `pal` tag in hooks.txt.
#include "recomp/ctx.h"

namespace recomp::ver {

bool pal();  // the European game is running

// USA address -> running version's address. 0 (and a log line) when the European game has no
// mapping for it: version_addrs.inc is stale (re-run gen_version_map.py).
u32 addr(u32 usAddr);

inline u32 off(u32 usOff, u32 palOff) { return pal() ? palOff : usOff; }

}  // namespace recomp::ver

// Looked up once per use site (the version never changes while the game runs).
#define VA(usAddr)                                                                  \
    ([]() -> ::recomp::u32 {                                                        \
        static const ::recomp::u32 a = ::recomp::ver::addr(usAddr);                 \
        return a;                                                                   \
    }())
#define VOFF(usOff, palOff) (::recomp::ver::off((usOff), (palOff)))
