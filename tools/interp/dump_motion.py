"""Scenery motion through the in-between frames of an LM_INTERP_TEST run (docs/INTERPOLATION.md).

    dump_motion.py <run dir> [steps] [--crop x,y,w,h]

For every game frame N with dumps f<N-1>_fresh, f<N>_s1 .. s<steps-1>, f<N>_fresh, measures the
global shift of the picture (phase correlation over a grid of blocks, median) between each pair of
consecutive images. Under a moving camera the in-between steps must be equal: each 1/steps of the
whole step. Reports per frame the steps and flags frames whose steps are uneven (in-betweens that
hold the camera and jump at the game frame show as [x, 0, 0] instead of [x/3, x/3, x/3]).
"""
import os
import re
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from screen_judder import phase_shift  # noqa: E402


def gray(path, crop):
    im = Image.open(path).convert("L")
    if crop:
        x, y, w, h = crop
        im = im.crop((x, y, x + w, y + h))
    return np.asarray(im).astype(np.float32)


def shift(a, b, grid=(6, 4)):
    h, w = a.shape
    gc, gr = grid
    bh, bw = h // gr, w // gc
    win = np.outer(np.hanning(bh), np.hanning(bw)).astype(np.float32)
    xs, ys = [], []
    for r in range(gr):
        for c in range(gc):
            pa = a[r * bh:(r + 1) * bh, c * bw:(c + 1) * bw]
            pb = b[r * bh:(r + 1) * bh, c * bw:(c + 1) * bw]
            if pa.std() < 3:
                continue
            dx, dy, pk = phase_shift(pa, pb, win)
            if pk > 0.08:
                xs.append(dx)
                ys.append(dy)
    if not xs:
        return 0.0, 0.0
    return float(np.median(xs)), float(np.median(ys))


def main():
    run = sys.argv[1]
    steps = int(sys.argv[2]) if len(sys.argv) > 2 and not sys.argv[2].startswith("--") else 3
    crop = None
    if "--crop" in sys.argv:
        crop = tuple(int(v) for v in sys.argv[sys.argv.index("--crop") + 1].split(","))
    d = os.path.join(run, "dump")
    frames = sorted({int(m.group(1)) for f in os.listdir(d) for m in [re.match(r"f(\d+)_fresh", f)] if m})
    bad = 0
    total = 0
    errs = []
    for n in frames:
        names = [f"f{n - 1}_fresh"] + [f"f{n}_s{s}" for s in range(1, steps)] + [f"f{n}_fresh"]
        if not all(os.path.exists(os.path.join(d, x + ".ppm")) for x in names):
            continue
        imgs = [gray(os.path.join(d, x + ".ppm"), crop) for x in names]
        st = [shift(imgs[i], imgs[i + 1])[0] for i in range(len(imgs) - 1)]
        whole = sum(st)
        if abs(whole) < 1.5:
            continue
        total += 1
        ideal = whole / len(st)
        e = max(abs(s - ideal) for s in st) / abs(ideal)
        errs.append(e)
        flag = e > 0.3
        bad += flag
        print(f"f{n}: steps " + " ".join(f"{s:6.2f}" for s in st) + f"  (whole {whole:6.2f}, max err {100 * e:5.1f}%)" + ("  UNEVEN" if flag else ""))
    if errs:
        print(f"moving frames {total}, uneven (>30% off) {bad}; mean max err {100 * np.mean(errs):.1f}%")


if __name__ == "__main__":
    main()
