#!/usr/bin/env python3
"""List HLE (SDK) functions referenced from recompiled code: the HLE surface to implement."""
import re, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
import recomp as R

def main():
    dol = R.Dol(Path(sys.argv[1]).read_bytes())
    funcs, objs = R.load_symbols(Path(sys.argv[2]))
    funcs = {k: v for k, v in funcs.items() if v[1] > 0 and dol.has(k)}
    rc = R.Recompiler(dol, funcs, objs, R.load_hle_ranges(Path(sys.argv[3])))
    refs = {}
    for a, (name, size) in funcs.items():
        if a in rc.hle:
            continue
        his = {}
        for pc in range(a, a + size, 4):
            w = dol.word(pc)
            op = w >> 26
            t = None
            if op == 18:
                t = (pc + R.sext(w & 0x03FFFFFC, 26)) & 0xFFFFFFFF
            elif op == 15:
                his[R.bits(w, 6, 10)] = (w & 0xFFFF) << 16
            elif op in (14, 24) and R.bits(w, 11, 15) in his:
                lo = R.sext(w & 0xFFFF, 16) if op == 14 else (w & 0xFFFF)
                t = (his[R.bits(w, 11, 15)] + lo) & 0xFFFFFFFF
            if t in rc.hle:
                refs.setdefault(t, set()).add(name)
    # also references from data (vtables / callback tables)
    for a, o, s in dol.sections:
        for i in range(0, s, 4):
            v = int.from_bytes(dol.data[o + i:o + i + 4], "big")
            if v in rc.hle and not (any(fa <= a + i < fa + fs[1] for fa, fs in [])):
                pass
    for t in sorted(refs, key=lambda t: rc.hle[t][1]):
        print(f"{t:08X} {rc.hle[t][1]:40} {rc.hle[t][0]:36} {len(refs[t])}")
    print(len(refs), file=sys.stderr)

main()
