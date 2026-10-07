#!/usr/bin/env python3
"""Report which translation units (TUs) of the game still need decompiling for the port.

Combines decomp/config/<ver>/splits.txt (every TU and its .text range) with the
Object(...) list in decomp/configure.py (which TUs have source and whether they match).

  python tools/tu_status.py                  # summary per library
  python tools/tu_status.py --list missing   # TUs with no source, largest first
  python tools/tu_status.py --lib Game --list all
"""
import argparse
import re
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DECOMP = ROOT / "decomp"

# How each decomp library is handled by the port.
PORT_ROLE = {
    "dolphin": "replaced by Aurora",
    "msl": "replaced by host libc/C++ runtime",
    "trk": "dropped (debugger stub)",
}

HELPER_LIB = {
    "JSystem": "JSystem",
    "JAudio": "JAudio",
    "Game": "Game",
    "HVQM4": "hvqm4",
    "Trk_Minnow_Dolphin": "trk",
    "MetroworksLib": "msl",
    "DolphinLib": "dolphin",
    "Rel": "Game",
}


def parse_splits(version: str):
    tus = {}
    current = None
    for line in (DECOMP / "config" / version / "splits.txt").read_text().splitlines():
        if line and not line.startswith(("\t", " ")) and line.endswith(":") and line != "Sections:":
            current = line[:-1]
            tus[current] = 0
        elif current and line.strip().startswith(".text"):
            m = re.search(r"start:0x([0-9A-Fa-f]+) end:0x([0-9A-Fa-f]+)", line)
            if m:
                tus[current] += int(m.group(2), 16) - int(m.group(1), 16)
    return tus


def unsplit_text_bytes(version: str) -> int:
    """Bytes of .text in symbols.txt not covered by any TU in splits.txt."""
    ranges = []
    for line in (DECOMP / "config" / version / "splits.txt").read_text().splitlines():
        m = re.search(r"^\s+\.text\s+start:0x(\w+) end:0x(\w+)", line)
        if m:
            ranges.append((int(m.group(1), 16), int(m.group(2), 16)))
    total = 0
    sym_re = re.compile(r"= \.text:0x(\w+); // type:function size:0x(\w+)")
    for line in (DECOMP / "config" / version / "symbols.txt").read_text().splitlines():
        m = sym_re.search(line)
        if m:
            addr = int(m.group(1), 16)
            if not any(a <= addr < b for a, b in ranges):
                total += int(m.group(2), 16)
    return total


def parse_configure():
    """Returns {tu_path: (lib, state)} where state is Matching/NonMatching/Equivalent/..."""
    objs = {}
    lib = "?"
    helper_re = re.compile(r"^\s*(" + "|".join(HELPER_LIB) + r")\(")
    obj_re = re.compile(r'Object\(\s*([A-Za-z_]+(?:\([^)]*\))?)\s*,\s*"([^"]+)"')
    for line in (DECOMP / "configure.py").read_text().splitlines():
        hm = helper_re.match(line)
        if hm:
            lib = HELPER_LIB[hm.group(1)]
        om = obj_re.search(line)
        if om:
            state = om.group(1)
            if state.startswith("MatchingFor"):
                state = "MatchingFor"
            objs[om.group(2)] = (lib, state)
    return objs


def classify_unconfigured(path: str) -> str:
    p = path.lower()
    if p.startswith("dolphin/") or "/dolphin/" in p:
        return "dolphin"
    if p.startswith(("msl", "runtime", "powerpc_eabi_support")) or "msl_c" in p:
        return "msl"
    if "trk" in p or "metrotrk" in p:
        return "trk"
    if p.startswith(("jsystem/", "j3d", "j2d", "jkernel", "jutility", "jsupport", "jgadget", "jmath", "jparticle")):
        return "JSystem"
    if p.startswith("jaudio"):
        return "JAudio"
    if "hvqm" in p:
        return "hvqm4"
    return "Game"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", default="GLME01")
    ap.add_argument("--list", choices=["missing", "partial", "done", "all"])
    ap.add_argument("--lib")
    args = ap.parse_args()

    tus = parse_splits(args.version)
    objs = parse_configure()

    rows = []
    for tu, size in tus.items():
        if tu in objs:
            lib, state = objs[tu]
            status = "done" if state in ("Matching", "MatchingFor") else "partial"
        else:
            lib, status = classify_unconfigured(tu), "missing"
        rows.append((tu, lib, status, size))

    totals = defaultdict(lambda: defaultdict(int))
    counts = defaultdict(lambda: defaultdict(int))
    for _, lib, status, size in rows:
        totals[lib][status] += size
        counts[lib][status] += 1

    print(f"{'library':10} {'role':36} {'done':>14} {'partial':>14} {'missing':>14}")
    port_needed = 0
    for lib in sorted(totals):
        t, c = totals[lib], counts[lib]
        role = PORT_ROLE.get(lib, "must be ported")
        if lib not in PORT_ROLE:
            port_needed += t["partial"] + t["missing"]
        cells = [f"{c[s]:4}/{t[s] // 1024:5}K" for s in ("done", "partial", "missing")]
        print(f"{lib:10} {role:36} {cells[0]:>14} {cells[1]:>14} {cells[2]:>14}")
    unsplit = unsplit_text_bytes(args.version)
    print(f"\nNot yet assigned to any TU (unsplit .text): {unsplit // 1024} KiB")
    print(f"Code still to write for the port: ~{(port_needed + unsplit) // 1024} KiB of PowerPC .text")

    if args.list:
        sel = [r for r in rows if (args.list == "all" or r[2] == args.list)
               and (not args.lib or r[1] == args.lib) and r[1] not in PORT_ROLE]
        for tu, lib, status, size in sorted(sel, key=lambda r: -r[3]):
            print(f"{status:8} {lib:8} {size:8} {tu}")


if __name__ == "__main__":
    main()
