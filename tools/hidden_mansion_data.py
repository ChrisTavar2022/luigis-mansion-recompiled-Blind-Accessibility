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
  Event/event08.szp    E. Gadd's lab: the US archive with the PAL script, plus a follow-up
                       choice "The Hidden Mansion (USA)" / "(PAL)" (two message lines appended);
                       served always ([always]), whichever version is selected.
  manifest.txt         one "guest path -> overlay file [condition]" line per redirect.

and the reverse, the USA Hidden Mansion for the European (PAL) game (docs/SETUP.md):

  usa/Map/map2.szp     the US /Map/map2.szp; replaces /Map/map2u.szp.
  usa/Event/eventNN.szp the PAL archive (every language) with the US script.
  usa/manifest.txt

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


# E. Gadd's lab menu choice between both Hidden Mansions (docs/HIDDEN_MANSION.md, *E. Gadd's
# lab menu*). Picking "The Hidden Mansion" asks a follow-up <CHOICE> between two new message
# lines; the branch taken sets (PAL) or clears (USA) game flag 8, the PAL release's own
# hidden-mansion save flag, right before <URALUIGI>. The port reads the variant from it.
LAB_FLAG_PAL = 8
LAB_HIDDEN_LINE = 38   # "The Hidden Mansion" in message8.csv / book1.txt (every language)
LAB_SUFFIX = (b" (USA)", b" (PAL)")
LAB_SUFFIX_JP = ("（ＵＳＡ）".encode("shift_jis"), "（ＰＡＬ）".encode("shift_jis"))


def lab_table(table: bytes, n: int, japanese: bool) -> bytes:
    """A message table (CRLF-terminated entries, LF inside an entry) with entries n and n+1
    = "<Hidden Mansion line> (USA)" / "(PAL)" appended; shorter tables are padded with empty
    entries so the indices match in every language."""
    entries = table.split(b"\r\n")
    while entries and entries[-1] == b"":
        entries.pop()
    assert len(entries) <= n and len(entries) > LAB_HIDDEN_LINE
    base = entries[LAB_HIDDEN_LINE].rstrip(b". ")
    entries += [b""] * (n - len(entries))
    entries += [base + s for s in (LAB_SUFFIX_JP if japanese else LAB_SUFFIX)]
    return b"\r\n".join(entries) + b"\r\n\r\n"


def lab_entries(table: bytes) -> int:
    entries = table.split(b"\r\n")
    while entries and entries[-1] == b"":
        entries.pop()
    return len(entries)


def lab_script(script: bytes, n: int) -> bytes:
    """event08 script with the "urayasiki" case turned into a USA/PAL follow-up choice."""
    lines = script.split(b"\r\n")
    start = lines.index(b'<CASE>"urayasiki"')
    end = next(i for i in range(start + 1, len(lines)) if lines[i].startswith(b"<END>"))
    body = [ln for ln in lines[start + 1:end + 1] if not ln.startswith((b"<FLAGON>(8)", b"<FLAGOFF>(8)"))]
    ura = body.index(b"<URALUIGI>")

    def branch(label, pal):
        flags = [(b"<FLAGON>(%d)" if pal else b"<FLAGOFF>(%d)") % LAB_FLAG_PAL]
        return [b'<CASE>"%s"' % label] + body[:ura] + flags + body[ura:]

    new = [b'<CASE>"urayasiki"', b'<CHOICE>"ura_us"(%d)"ura_pal"(%d)<LISTEND>' % (n, n + 1), b"<ANYKEY>"]
    new += branch(b"ura_us", False) + branch(b"ura_pal", True)
    return b"\r\n".join(lines[:start] + new + lines[end + 1:])


def lab_archive(arc: bytes, pal_script: bytes) -> bytes:
    """event08 archive (either release, every language it has) with the lab choice."""
    files = dict(rarc.rarc_files(arc))
    tables = [k for k in files if k.startswith("message/")]
    n = max(lab_entries(files[k]) for k in tables)
    out = rarc_replace(arc, "text/event08.txt", lab_script(pal_script, n))
    for k in tables:
        out = rarc_replace(out, k, lab_table(files[k], n, "japanese" in k))
    check = dict(rarc.rarc_files(out))
    assert set(check) == set(files) and all(check[k] == v for k, v in files.items() if k not in tables and k != "text/event08.txt")
    return out


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

    events = [e.strip() for e in a.events.split(",") if e.strip()]

    def swap_scripts(base_dir, from_dir, dest, prefix, manifest):
        for ev in events:
            name = f"event{ev}.szp"
            member = f"text/event{ev}.txt"
            u = rarc.decompress((base_dir / "Event" / name).read_bytes())
            p = dict(rarc.rarc_files(rarc.decompress((from_dir / "Event" / name).read_bytes())))
            cur = dict(rarc.rarc_files(u))
            if ev == "08":
                # The lab menu: served whichever version is chosen, so it can ask. Its script is
                # the European one (which keeps flag 8 in step) on both games.
                pal_script = (p if prefix == "" else cur)[member]
                packed = yay0_store(lab_archive(u, pal_script))
                (dest / "Event" / name).write_bytes(packed)
                manifest.append(f"/Event/{name} -> {prefix}Event/{name} [always]")
                print(f"{prefix}{name}: lab menu with both Hidden Mansions")
                continue
            if cur.get(member) == p[member]:
                print(f"{prefix}{name}: script identical, skipped")
                continue
            patched = rarc_replace(u, member, p[member])
            check = dict(rarc.rarc_files(patched))
            assert check[member] == p[member] and all(check[k] == v for k, v in cur.items() if k != member)
            packed = yay0_store(patched)
            assert rarc.decompress(packed) == patched
            (dest / "Event" / name).write_bytes(packed)
            manifest.append(f"/Event/{name} -> {prefix}Event/{name}")
            print(f"{prefix}{name}: {len(cur[member])} -> {len(p[member])} byte script")

    swap_scripts(us, pal, out, "", manifest)
    (out / "manifest.txt").write_text("\n".join(manifest) + "\n", encoding="utf-8")

    # Reverse: the USA Hidden Mansion on the European game.
    rev = out / "usa"
    (rev / "Map").mkdir(parents=True, exist_ok=True)
    (rev / "Event").mkdir(parents=True, exist_ok=True)
    reverse = []
    shutil.copyfile(us / "Map" / "map2.szp", rev / "Map" / "map2.szp")
    reverse.append("/Map/map2u.szp -> usa/Map/map2.szp [hidden]")
    swap_scripts(pal, us, rev, "usa/", reverse)
    (rev / "manifest.txt").write_text("\n".join(reverse) + "\n", encoding="utf-8")
    print(f"overlay written to {out}")


if __name__ == "__main__":
    main()
