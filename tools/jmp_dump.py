#!/usr/bin/env python3
"""Dump a JMP parameter table (e.g. jmp/roominfo from /Map/map2.szp).

    python tools/jmp_dump.py <archive.szp> <path-in-archive> [--dol main.dol] [--rows N]

Field names are stored only as hashes; they are resolved by hashing every
NUL-terminated ASCII string found in the DOL. Output is for local reference.
"""
import argparse
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import rarc  # noqa: E402


def name_hash(s: bytes) -> int:
    h = 0
    for c in s:
        h = ((c + (h << 8)) & 0xFFFFFFFF) % 0x1FFFFD9  # u32 arithmetic, as in the game
    return h


def dol_strings(dol: bytes):
    for m in re.finditer(rb"[\x20-\x7e]{2,40}(?=\x00)", dol):
        yield m.group(0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("archive")
    ap.add_argument("path")
    ap.add_argument("--dol", default="Luigis Mansion US/sys/main.dol")
    ap.add_argument("--rows", type=int, default=5)
    a = ap.parse_args()

    files = dict(rarc.rarc_files(rarc.decompress(Path(a.archive).read_bytes())))
    d = files[a.path]
    names = {name_hash(s): s.decode() for s in dol_strings(Path(a.dol).read_bytes())}

    n, nf, data_off, entry_size = struct.unpack_from(">iiiI", d, 0)
    print(f"{a.path}: {n} entries, {nf} fields, entry size {entry_size}")
    fields = []
    for i in range(nf):
        h, mask, off, shift, typ = struct.unpack_from(">IIHBB", d, 0x10 + i * 12)
        fields.append((names.get(h, f"#{h:07x}"), mask, off, shift, typ))
        print(f"  {i:2} {fields[-1][0]:24} off {off:3} type {typ} mask {mask:08x} shift {shift}")
    for e in range(min(n, a.rows)):
        row = []
        base = data_off + e * entry_size
        for name, mask, off, shift, typ in fields:
            raw = d[base + off:base + off + 4]
            if typ == 1:
                v = d[base + off:base + off + 32].split(b"\0")[0].decode("shift_jis", "replace")
            elif typ == 2:
                v = round(struct.unpack(">f", raw)[0], 2)
            else:
                v = (struct.unpack(">I", raw)[0] & mask) >> shift
            row.append(f"{name}={v}")
        print(f"  [{e}] " + ", ".join(row))


if __name__ == "__main__":
    main()
