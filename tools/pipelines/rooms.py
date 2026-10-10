"""Room sample points for the pipeline sweep (tools/pipelines/sweep.py).

One point per mansion room, derived from the map's own furniture table (jmp/furnitureinfo):
the centre of the room's furniture positions at the lowest furniture height. Computed from
the user's disc dump at sweep time; nothing derived from game data is committed.
"""
import struct
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import rarc  # noqa: E402
from jmp_dump import name_hash  # noqa: E402


def jmp_rows(table: bytes, wanted):
    n, nf, data_off, entry_size = struct.unpack_from(">iiiI", table, 0)
    fields = {}
    for i in range(nf):
        h, mask, off, shift, typ = struct.unpack_from(">IIHBB", table, 0x10 + i * 12)
        fields[h] = (mask, off, shift, typ)
    want = {name: fields[name_hash(name.encode())] for name in wanted}
    for e in range(n):
        base = data_off + e * entry_size
        row = {}
        for name, (mask, off, shift, typ) in want.items():
            raw = table[base + off:base + off + 4]
            row[name] = struct.unpack(">f", raw)[0] if typ == 2 else (struct.unpack(">I", raw)[0] & mask) >> shift
        yield row


def room_points(map_szp: Path):
    files = dict(rarc.rarc_files(rarc.decompress(map_szp.read_bytes())))
    rooms = defaultdict(list)
    for r in jmp_rows(files["jmp/furnitureinfo"], ["pos_x", "pos_y", "pos_z", "room_no"]):
        rooms[r["room_no"]].append((r["pos_x"], r["pos_y"], r["pos_z"]))
    points = []
    for room, ps in sorted(rooms.items()):
        x = sum(p[0] for p in ps) / len(ps)
        z = sum(p[2] for p in ps) / len(ps)
        y = min(p[1] for p in ps)
        points.append((room, round(x), round(y) + 10, round(z)))
    return points


if __name__ == "__main__":
    for p in room_points(Path(sys.argv[1])):
        print(*p)
