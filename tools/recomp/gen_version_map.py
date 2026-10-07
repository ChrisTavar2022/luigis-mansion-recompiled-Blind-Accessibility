#!/usr/bin/env python3
"""Generate the PAL (GLMP01) version map used by the recompiler (docs/SETUP.md).

    python tools/recomp/gen_version_map.py [--us DIR] [--pal DIR] [--out tools/recomp/GLMP01_map.txt]

The PAL decomp config has no translation-unit splits and fewer named functions, but the
runtime finds SDK functions (HLE) and hooks by their US names. This matches every PAL
function to its US counterpart and writes

    <PAL address> <canonical name> <translation unit | - | hook>

for each PAL function that belongs to an SDK unit (HLE'd or recompiled by TU), carries a
PAL-tagged hook, or is unnamed in the PAL map but named in the US one.

Names come from, in order: the PAL map itself; the US function tools/pal_diff.py pairs it
with; and call-site agreement (a matched caller pair whose `bl` sequences line up names
the PAL callee after the US one). The last step names the SDK functions the PAL release
rewrote (PADInit, PADRead, ...), which the similarity matcher cannot pair.

The output is address/name metadata only (like decomp/config/*/symbols.txt); it is
committed and embedded in the executable. Re-run it after tagging a hook `pal` in
tools/recomp/hooks.txt. Needs the user's own US + PAL dumps (dev machines only).
"""
import argparse
import sys
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import pal_diff as P  # noqa: E402

SDK_PREFIXES = ("os/", "gx/", "vi/", "pad/", "dvd/", "ar/", "card/", "ai/", "dsp/", "db/", "si/",
                "exi/", "TRK_MINNOW_DOLPHIN/", "OdemuExi2/", "amcstubs/", "odenotstub/")


# SDK functions the PAL release reorganised, identified by hand from their callers and
# bodies (tools/pal_diff.py show - --pal ADDR --side pal). These override the matcher.
OVERRIDES = {
    0x801E1E7C: ("OSGetLanguage", "os/OSRtc.c"),           # returns SRAM byte 0x12
    0x801EBDF8: ("VISetPostRetraceCallback", "vi/vi.c"),   # US body, PostCB slot
    0x801EBE3C: ("VISetPreRetraceCallback", "vi/vi.c"),    # second callback slot (new in PAL)
    0x801EE0AC: ("PADReset", "pad/Pad.c"),                 # JUTGamePad::read error path
    0x801EE86C: ("PADSetSamplingRate", "pad/Pad.c"),       # tail-calls SISetSamplingRate
    0x801EE88C: ("PADControlMotor", "pad/Pad.c"),          # JUTGamePad::CRumble callers
}


def unnamed(n):
    return not n or n == "-" or n.startswith(("fn_", "lbl_"))


def calls(v, f):
    out = []
    for a in range(f["addr"], f["addr"] + f["size"], 4):
        w = v.dol.word(a)
        if w >> 26 == 18 and (w & 3) == 1:  # bl
            li = w & 0x03FFFFFC
            if li & 0x02000000:
                li -= 0x04000000
            out.append((a + li) & 0xFFFFFFFF)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--us", default=str(P.DEF_US))
    ap.add_argument("--pal", default=str(P.DEF_PAL))
    ap.add_argument("--out", default=str(ROOT / "tools" / "recomp" / "GLMP01_map.txt"))
    a = ap.parse_args()
    us = P.Version("us", a.us, "GLME01")
    pal = P.Version("pal", a.pal, "GLMP01_00")
    recs, _, _ = P.match_functions(us, pal)

    # PAL address -> [name, TU, kind]
    info = {}
    us_to_pal = {}
    for r in recs:
        u, p = r["us"], r["pal"]
        if not p:
            continue
        name = p["name"] if not unnamed(p["name"]) or not u else u["name"]
        info[p["addr"]] = [name, us.tu(u["addr"]) if u else "", r["kind"]]
        if u:
            us_to_pal.setdefault(u["name"], p["addr"])

    # Call-site agreement: matched callers with equally long bl lists name their callees.
    votes = defaultdict(Counter)
    for r in recs:
        u, p = r["us"], r["pal"]
        if not u or not p or r["kind"] == "pal-only":
            continue
        cu, cp = calls(us, u), calls(pal, p)
        if len(cu) != len(cp):
            continue
        for tu_, tp in zip(cu, cp):
            fu, fp = us.fat.get(tu_), pal.fat.get(tp)
            if fu and fp and unnamed(info.get(tp, ["-"])[0]) and not unnamed(fu["name"]):
                votes[tp][(fu["name"], us.tu(tu_))] += 1
    for tp, c in votes.items():
        (name, tu), n = c.most_common(1)[0]
        if n * 2 > sum(c.values()) and tp in info:
            info[tp][0] = name
            if not info[tp][1]:
                info[tp][1] = tu
            us_to_pal.setdefault(name, tp)

    # The SDK is linked as one contiguous block; a pairing outside it is a false match by
    # the similarity matcher. Unmatched PAL functions inside it take the preceding TU.
    sdk = [addr for addr, (n, tu, kind) in info.items() if kind == "same" and tu.startswith(SDK_PREFIXES)]
    lo, hi = min(sdk), max(sdk)
    last_tu = ""
    for addr in sorted(info):
        e = info[addr]
        if e[1].startswith(SDK_PREFIXES) and not lo <= addr <= hi:
            e[0], e[1] = pal.fat[addr]["name"], ""
        if not e[1] and e[2] == "pal-only" and lo <= addr <= hi:
            e[1] = last_tu
        if e[1]:
            last_tu = e[1]

    for addr, (name, tu) in OVERRIDES.items():
        info[addr][0], info[addr][1] = name, tu
        us_to_pal[name] = addr

    lines = ["# Generated by tools/recomp/gen_version_map.py from tools/pal_diff.py matching. Do not edit.",
             "# <PAL address> <canonical name> <translation unit | - | hook>"]
    named = Counter(n for addr, (n, tu, kind) in info.items() if not tu.startswith(SDK_PREFIXES))
    for addr in sorted(info):
        name, tu, kind = info[addr]
        if tu.startswith(SDK_PREFIXES):
            lines.append(f"{addr:08X} {name} {tu}")
        elif not unnamed(name) and unnamed(pal.fat[addr]["name"]) and named[name] == 1:
            lines.append(f"{addr:08X} {name} -")  # (ambiguous names are left out)

    # Hooks tagged for PAL ("<symbol> ... pal" in hooks.txt) must have a PAL counterpart.
    missing = []
    for line in (ROOT / "tools" / "recomp" / "hooks.txt").read_text(encoding="utf-8").splitlines():
        words = line.split("#")[0].split()
        if len(words) < 2 or "pal" not in words[1:]:
            continue
        addr = us_to_pal.get(words[0])
        if addr is None:
            missing.append(words[0])
        elif not info[addr][1].startswith(SDK_PREFIXES):
            lines.append(f"{addr:08X} {words[0]} hook")
    Path(a.out).write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    left = [l for l in lines[2:] if l.split()[1].startswith("fn_") and l.split()[2] != "-"]
    print(f"{a.out}: {len(lines) - 2} entries; {len(left)} SDK functions still unnamed")
    for l in left:
        print("  " + l)
    if missing:
        sys.exit(f"hooks tagged pal without a PAL match: {', '.join(missing)}")


if __name__ == "__main__":
    main()
