#!/usr/bin/env python3
"""Rebuild a GameCube disc image (GCM/ISO) from an extracted dump (Dolphin "Extract Entire Disc" layout).

    python tools/make_iso.py "Luigis Mansion US" build/GLME01.iso

The dump must contain sys/{boot.bin,bi2.bin,apploader.img,main.dol,fst.bin} and files/.
Each file is written at the offset recorded in fst.bin, so the result matches the original
disc layout. The image is for local use with your own dump only; never distribute it.
"""
import struct
import sys
from pathlib import Path

GCM_SIZE = 1_459_978_240  # standard GameCube disc size


def parse_fst(fst: bytes):
    """Yields (path, disc_offset, size) for every file entry."""
    count = struct.unpack_from(">I", fst, 8)[0]
    strings = 12 * count

    def name(i):
        off = struct.unpack_from(">I", fst, 12 * i)[0] & 0xFFFFFF
        end = fst.index(b"\0", strings + off)
        return fst[strings + off:end].decode("shift_jis")

    # Walk entries keeping a stack of (dir_path, end_index).
    stack = [("", count)]
    for i in range(1, count):
        while i >= stack[-1][1]:
            stack.pop()
        flags_name, a, b = struct.unpack_from(">III", fst, 12 * i)
        path = f"{stack[-1][0]}/{name(i)}" if stack[-1][0] else name(i)
        if flags_name >> 24:
            stack.append((path, b))  # directory: b = index one past its last child
        else:
            yield path, a, b


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    dump, out = Path(sys.argv[1]), Path(sys.argv[2])
    sysdir, files = dump / "sys", dump / "files"
    boot = (sysdir / "boot.bin").read_bytes()
    dol_off, fst_off, fst_size = struct.unpack_from(">III", boot, 0x420)
    fst = (sysdir / "fst.bin").read_bytes()
    assert len(fst) == fst_size, "fst.bin size does not match boot.bin"

    out.parent.mkdir(parents=True, exist_ok=True)
    with open(out, "wb") as f:
        f.truncate(GCM_SIZE)

        def put(offset, data):
            f.seek(offset)
            f.write(data)

        put(0x0, boot)
        put(0x440, (sysdir / "bi2.bin").read_bytes())
        put(0x2440, (sysdir / "apploader.img").read_bytes())
        put(dol_off, (sysdir / "main.dol").read_bytes())
        put(fst_off, fst)
        n = 0
        for path, offset, size in parse_fst(fst):
            data = (files / path).read_bytes()
            if len(data) != size:
                sys.exit(f"size mismatch for {path}: fst says {size}, file is {len(data)}")
            put(offset, data)
            n += 1
    print(f"wrote {out} ({n} files, game id {boot[:6].decode()})")


if __name__ == "__main__":
    main()
