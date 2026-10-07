#!/usr/bin/env python3
"""Read typed values from a retail main.dol by GameCube virtual address (for porting work).

    python tools/dolread.py <main.dol> f32 0x804D9968 [count]
    python tools/dolread.py <main.dol> u32|s32|u16|u8|rgba|str|hex 0x80... [count]
"""
import struct
import sys


def load(path):
    d = open(path, "rb").read()
    offs = struct.unpack(">18I", d[0:72])
    addrs = struct.unpack(">18I", d[72:144])
    sizes = struct.unpack(">18I", d[144:216])
    return d, [(a, o, s) for a, o, s in zip(addrs, offs, sizes) if s]


def at(dol, secs, addr, n):
    for a, o, s in secs:
        if a <= addr and addr + n <= a + s:
            return dol[o + addr - a:o + addr - a + n]
    raise SystemExit(f"0x{addr:08X} is not in an initialized section")


def main():
    path, kind, addr = sys.argv[1], sys.argv[2], int(sys.argv[3], 16)
    count = int(sys.argv[4], 0) if len(sys.argv) > 4 else 1
    dol, secs = load(path)
    fmt = {"f32": (">f", 4), "u32": (">I", 4), "s32": (">i", 4), "u16": (">H", 2), "u8": (">B", 1)}
    for i in range(count):
        if kind in fmt:
            f, n = fmt[kind]
            v = struct.unpack(f, at(dol, secs, addr, n))[0]
            print(f"0x{addr:08X}: {v!r}")
            addr += n
        elif kind == "rgba":
            print(f"0x{addr:08X}: {tuple(at(dol, secs, addr, 4))}")
            addr += 4
        elif kind == "str":
            b = bytearray()
            while (c := at(dol, secs, addr + len(b), 1)[0]) != 0:
                b.append(c)
            print(f"0x{addr:08X}: {b.decode('shift_jis', 'replace')!r}")
            addr += len(b) + 1
        elif kind == "hex":
            print(f"0x{addr:08X}: {at(dol, secs, addr, 16).hex(' ')}")
            addr += 16


if __name__ == "__main__":
    main()
