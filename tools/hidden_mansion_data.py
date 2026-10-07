#!/usr/bin/env python3
"""Build the PAL Hidden Mansion data overlay from the user's own US + PAL dumps.

    python tools/hidden_mansion_data.py [--us DIR] [--pal DIR] [--out orig/hidden_mansion]

The overlay is what the port serves instead of the US disc files while the PAL
Hidden Mansion mode is selected (see docs/HIDDEN_MANSION.md):

  Map/map2u.szp        copied unchanged from the PAL disc; replaces /Map/map2.szp while the
                       hidden-mansion flag is set (PAL loads it the same way).
  Event/eventNN.szp    the US archive with only text/eventNN.txt (the language-neutral event
                       script) swapped for the PAL script (stored as literal-only Yay0). The US English message table stays,
                       so no PAL text (and no PAL language layout) is involved.
  manifest.txt         one "guest path -> overlay file [condition]" line per redirect.

Everything is derived from your own discs at setup time and written under orig/ (git-ignored).
"""
import argparse
import shutil
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import rarc  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
DUMPS = next((p for p in [ROOT, *ROOT.parents] if (p / "Luigis Mansion US").exists()), ROOT)

# Event scripts the PAL release changed for the Hidden Mansion (CHECKFLAG(8) "ura" branches
# with new Boo-count thresholds, and the URALUIGI/OMOTELUIGI ordering fix in event08).
DEFAULT_EVENTS = ["08", "16", "73"]


def yay0_store(data: bytes) -> bytes:
    """Wrap data in a valid Yay0 stream made only of literal bytes (no back-references), so
    the game's loader sees the same compressed container format as the original file."""
    nwords = (len(data) + 31) // 32
    link_off = 16 + 4 * nwords
    hdr = b"Yay0" + struct.pack(">III", len(data), link_off, link_off)
    return hdr + struct.pack(">I", 0xFFFFFFFF) * nwords + data


def rarc_replace(arc: bytes, member: str, new: bytes) -> bytes:
    """Return a copy of an (uncompressed) RARC archive with one file's contents replaced.

    The new data is appended at the end of the data section and the file entry is pointed
    at it; directory/entry/string tables are left untouched, so ids and names are stable."""
    assert arc[:4] == b"RARC"
    data_off = struct.unpack_from(">I", arc, 0x0C)[0] + 0x20
    data_len, mram, aram = struct.unpack_from(">III", arc, 0x10)
    info = 0x20
    num_nodes, node_off, _, entry_off, _, str_off = struct.unpack_from(">IIIIII", arc, info)
    node_off += info
    entry_off += info
    str_off += info

    def name(off):
        end = arc.index(b"\0", str_off + off)
        return arc[str_off + off:end].decode("shift_jis", "replace")

    target = None

    def walk(node_idx, prefix):
        nonlocal target
        _, _, _, count, first = struct.unpack_from(">IIHHI", arc, node_off + node_idx * 16)
        for i in range(first, first + count):
            e = entry_off + i * 20
            _, _, flags_name, a, size = struct.unpack_from(">HHIII", arc, e)
            ename = name(flags_name & 0xFFFFFF)
            if ename in (".", ".."):
                continue
            if (flags_name >> 24) & 0x02:
                walk(a, f"{prefix}{ename}/")
            elif f"{prefix}{ename}" == member:
                target = (e, flags_name >> 24, size)

    walk(0, "")
    if target is None:
        raise KeyError(member)
    e, flags, old_size = target
    out = bytearray(arc[:data_off + data_len])
    pad = (-len(out)) % 32
    out += b"\0" * pad
    new_rel = len(out) - data_off
    out += new
    out += b"\0" * ((-len(out)) % 32)
    grow = len(out) - (data_off + data_len)
    struct.pack_into(">I", out, e + 8, new_rel)
    struct.pack_into(">I", out, e + 12, len(new))
    struct.pack_into(">I", out, 0x04, len(out))
    struct.pack_into(">I", out, 0x10, data_len + grow)
    if flags & 0x10:  # preloaded to main RAM
        struct.pack_into(">I", out, 0x14, mram + grow)
    elif flags & 0x20:  # preloaded to ARAM
        struct.pack_into(">I", out, 0x18, aram + grow)
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--us", default=str(DUMPS / "Luigis Mansion US"))
    ap.add_argument("--pal", default=str(DUMPS / "Lugis Mansion PAL"))
    ap.add_argument("--out", default=str(ROOT / "orig" / "hidden_mansion"))
    ap.add_argument("--events", default=",".join(DEFAULT_EVENTS))
    a = ap.parse_args()
    us, pal, out = Path(a.us) / "files", Path(a.pal) / "files", Path(a.out)
    (out / "Map").mkdir(parents=True, exist_ok=True)
    (out / "Event").mkdir(parents=True, exist_ok=True)
    manifest = []

    src = pal / "Map" / "map2u.szp"
    shutil.copyfile(src, out / "Map" / "map2u.szp")
    manifest.append("/Map/map2.szp -> Map/map2u.szp [hidden]")

    for ev in [e.strip() for e in a.events.split(",") if e.strip()]:
        name = f"event{ev}.szp"
        member = f"text/event{ev}.txt"
        u = rarc.decompress((us / "Event" / name).read_bytes())
        p = dict(rarc.rarc_files(rarc.decompress((pal / "Event" / name).read_bytes())))
        cur = dict(rarc.rarc_files(u))
        if cur.get(member) == p[member]:
            print(f"{name}: script identical, skipped")
            continue
        patched = rarc_replace(u, member, p[member])
        check = dict(rarc.rarc_files(patched))
        assert check[member] == p[member] and all(check[k] == v for k, v in cur.items() if k != member)
        packed = yay0_store(patched)
        assert rarc.decompress(packed) == patched
        (out / "Event" / name).write_bytes(packed)
        manifest.append(f"/Event/{name} -> Event/{name}")
        print(f"{name}: {len(cur[member])} -> {len(p[member])} byte script")

    (out / "manifest.txt").write_text("\n".join(manifest) + "\n", encoding="utf-8")
    print(f"overlay written to {out}")


if __name__ == "__main__":
    main()
