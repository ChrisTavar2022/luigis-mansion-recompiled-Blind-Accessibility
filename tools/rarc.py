#!/usr/bin/env python3
"""List or extract Nintendo RARC archives (optionally Yay0/Yaz0-compressed) for porting work.

    python tools/rarc.py list    <file.szp|.arc>
    python tools/rarc.py extract <file.szp|.arc> <out_dir>
    python tools/rarc.py dol <main.dol> <addr> <size> <out_file>   # dump an embedded blob

Outputs are derived from your own game files; keep them out of the repository.
"""
import struct
import sys
from pathlib import Path


def yay0(data: bytes) -> bytes:
    size, link_off, chunk_off = struct.unpack_from(">III", data, 4)
    out = bytearray()
    mask_off, bits, mask = 16, 0, 0
    while len(out) < size:
        if bits == 0:
            mask = struct.unpack_from(">I", data, mask_off)[0]
            mask_off += 4
            bits = 32
        if mask & 0x80000000:
            out.append(data[chunk_off])
            chunk_off += 1
        else:
            link = struct.unpack_from(">H", data, link_off)[0]
            link_off += 2
            dist = (link & 0xFFF) + 1
            n = link >> 12
            if n == 0:
                n = data[chunk_off] + 0x12
                chunk_off += 1
            else:
                n += 2
            for _ in range(n):
                out.append(out[-dist])
        mask = (mask << 1) & 0xFFFFFFFF
        bits -= 1
    return bytes(out)


def yaz0(data: bytes) -> bytes:
    size = struct.unpack_from(">I", data, 4)[0]
    out = bytearray()
    src = 16
    while len(out) < size:
        code = data[src]
        src += 1
        for bit in range(8):
            if len(out) >= size:
                break
            if code & (0x80 >> bit):
                out.append(data[src])
                src += 1
            else:
                b1, b2 = data[src], data[src + 1]
                src += 2
                dist = ((b1 & 0xF) << 8 | b2) + 1
                n = b1 >> 4
                if n == 0:
                    n = data[src] + 0x12
                    src += 1
                else:
                    n += 2
                for _ in range(n):
                    out.append(out[-dist])
    return bytes(out)


def decompress(data: bytes) -> bytes:
    if data[:4] == b"Yay0":
        return yay0(data)
    if data[:4] == b"Yaz0":
        return yaz0(data)
    return data


def rarc_files(data: bytes):
    """Yields (path, bytes) for every file in a RARC archive."""
    assert data[:4] == b"RARC", "not a RARC archive"
    data_off = struct.unpack_from(">I", data, 0x0C)[0] + 0x20
    info = 0x20
    num_nodes, node_off, _, entry_off, _, str_off = struct.unpack_from(">IIIIII", data, info)
    node_off += info
    entry_off += info
    str_off += info

    def name(off):
        end = data.index(b"\0", str_off + off)
        return data[str_off + off:end].decode("shift_jis", "replace")

    def walk(node_idx, prefix):
        _, name_off, _, count, first = struct.unpack_from(">IIHHI", data, node_off + node_idx * 16)
        for i in range(first, first + count):
            e = entry_off + i * 20
            _, _, flags_name, a, size = struct.unpack_from(">HHIII", data, e)
            ename = name(flags_name & 0xFFFFFF)
            if ename in (".", ".."):
                continue
            if (flags_name >> 24) & 0x02:
                yield from walk(a, f"{prefix}{ename}/")
            else:
                yield f"{prefix}{ename}", data[data_off + a:data_off + a + size]

    yield from walk(0, "")


def main():
    cmd = sys.argv[1]
    if cmd == "dol":
        dol = Path(sys.argv[2]).read_bytes()
        addr, size = int(sys.argv[3], 16), int(sys.argv[4], 16)
        offs = struct.unpack(">18I", dol[0:72])
        addrs = struct.unpack(">18I", dol[72:144])
        sizes = struct.unpack(">18I", dol[144:216])
        for o, a, s in zip(offs, addrs, sizes):
            if s and a <= addr and addr + size <= a + s:
                Path(sys.argv[5]).write_bytes(dol[o + addr - a:o + addr - a + size])
                return
        sys.exit("address range not in DOL")
    data = decompress(Path(sys.argv[2]).read_bytes())
    if cmd == "list":
        for path, blob in rarc_files(data):
            print(f"{len(blob):8}  {path}  {blob[:8]!r}")
    elif cmd == "extract":
        out = Path(sys.argv[3])
        for path, blob in rarc_files(data):
            dst = out / path
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(decompress(blob) if blob[:4] in (b"Yay0", b"Yaz0") else blob)
            print(dst)


if __name__ == "__main__":
    main()
