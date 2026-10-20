"""One-present artifacts in LM_PRESENT_DUMP dumps (f<frame>_p<present>_a<alpha>.ppm, 640x480 EFB).

    py tools/interp/present_blips.py <dump dir> [--thresh 40] [--region x0,y0,x1,y1] [--top 10] [--crops DIR]

Presents are taken in present order. A pixel is a blip in present k when it differs from both
present k-1 and present k+1 by more than --thresh (0-255, max over RGB) in the same direction:
something shown for one present only (a snap there and back, a thing drawn at another place for one
present). Per present: blip pixels in the region and the largest 16x16 block count. Reports the
totals, the share of presents with a blip block of 24 or more pixels, how blips split between
presents that start a game frame (#0) and the others, and the worst presents (with crops of
prev | present | next when --crops is given; --heat writes where the blips are). Compare runs of the
same scenario (A/B).
"""
import argparse
import glob
import os
import re

import numpy as np
from PIL import Image


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--thresh", type=float, default=40)
    ap.add_argument("--region", default="")
    ap.add_argument("--top", type=int, default=10)
    ap.add_argument("--crops", default="")
    ap.add_argument("--block", type=int, default=24, help="block count that makes a present 'blipped'")
    ap.add_argument("--heat", default="", help="PNG: how often each pixel blipped (white = often)")
    a = ap.parse_args()
    rx = re.compile(r"f(\d+)_p(\d+)_a(\d+)\.ppm$")
    items = []
    for f in glob.glob(os.path.join(a.dir, "*.ppm")):
        m = rx.search(os.path.basename(f))
        if m:
            items.append((int(m.group(2)), int(m.group(1)), int(m.group(3)), f))
    items.sort()
    if len(items) < 3:
        print("not enough presents")
        return
    reg = [int(v) for v in a.region.split(",")] if a.region else None

    def load(f):
        im = np.asarray(Image.open(f).convert("RGB"), dtype=np.int16)
        if reg:
            im = im[reg[1]:reg[3], reg[0]:reg[2]]
        return im

    prev, cur = load(items[0][3]), load(items[1][3])
    rows = []
    heat = None
    for k in range(1, len(items) - 1):
        nxt = load(items[k + 1][3])
        if items[k + 1][0] != items[k][0] + 1 or items[k - 1][0] != items[k][0] - 1:
            prev, cur = cur, nxt
            continue  # a present missing from the dump: not judged
        d1 = cur - prev
        d2 = cur - nxt
        up = (d1 > a.thresh) & (d2 > a.thresh)
        dn = (d1 < -a.thresh) & (d2 < -a.thresh)
        blip = (up | dn).any(axis=2)
        h, w = blip.shape
        hb, wb = h // 16, w // 16
        blocks = blip[: hb * 16, : wb * 16].reshape(hb, 16, wb, 16).sum(axis=(1, 3))
        first = items[k][1] != items[k - 1][1]  # the present that crosses a game frame
        rows.append((int(blip.sum()), int(blocks.max()), items[k], first))
        heat = blip.astype(np.int32) if heat is None else heat + blip
        prev, cur = cur, nxt
    n = len(rows)
    tot = sum(r[0] for r in rows)
    bad = [r for r in rows if r[1] >= a.block]
    print(f"{n} presents judged ({len(items)} dumped, game frames {items[0][1]}-{items[-1][1]}): blip pixels {tot} "
          f"({tot / max(n, 1):.1f} per present); presents with a block of >= {a.block}: {len(bad)} ({100.0 * len(bad) / max(n, 1):.1f}%), "
          f"of them crossing a game frame {sum(1 for r in bad if r[3])}")
    rows.sort(key=lambda r: -r[1])
    for px, blk, it, first in rows[: a.top]:
        print(f"  present {it[0]} frame {it[1]} alpha {it[2] / 1000:.3f}{' (#0)' if first else ''}: {px} px, block {blk}")
    if a.heat and heat is not None:
        Image.fromarray(np.clip(heat * (255.0 / max(1, heat.max())), 0, 255).astype(np.uint8)).save(a.heat)
    if a.crops:
        os.makedirs(a.crops, exist_ok=True)
        idx = {it[0]: i for i, it in enumerate(items)}
        for px, blk, it, first in rows[: a.top]:
            i = idx[it[0]]
            ims = [Image.open(items[j][3]).convert("RGB") for j in (i - 1, i, i + 1)]
            w, h = ims[0].size
            s = Image.new("RGB", (w * 3, h))
            for j, im in enumerate(ims):
                s.paste(im, (j * w, 0))
            s.save(os.path.join(a.crops, f"p{it[0]}_f{it[1]}.png"))


if __name__ == "__main__":
    main()
