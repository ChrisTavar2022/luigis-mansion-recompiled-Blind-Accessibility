"""tint.py <run> [steps]: colour/tint check of an LM_INTERP_TEST run (run/dump).

For every game-frame interval (f{n-1}_fresh -> f{n}_s1..s{steps-1} -> f{n}_fresh), an in-between frame
whose mean colour (any channel, whole picture and each of 16 tiles) lies outside the range of its
two neighbouring game frames by more than a tolerance is flagged: interpolation must never produce
a colour or tint that neither game frame shows."""
import os, re, sys
import numpy as np
from PIL import Image

run = sys.argv[1]
steps = int(sys.argv[2]) if len(sys.argv) > 2 else 3
d = os.path.join(run, "dump")
TOL_FULL, TOL_TILE = 0.75, 3.0
L = lambda n: np.asarray(Image.open(f"{d}/{n}.ppm")).astype(np.float32)
ex = lambda n: os.path.exists(f"{d}/{n}.ppm")
frames = sorted({int(m.group(1)) for f in os.listdir(d) for m in [re.match(r"f(\d+)_fresh", f)] if m})


def means(im):
    h, w, _ = im.shape
    full = im.reshape(-1, 3).mean(axis=0)
    tiles = im[: h // 4 * 4, : w // 4 * 4].reshape(4, h // 4, 4, w // 4, 3).mean(axis=(1, 3)).reshape(-1, 3)
    return full, tiles


checked = flagged = 0
worst = []
for n in frames:
    if not ex(f"f{n-1}_fresh") or not all(ex(f"f{n}_s{s}") for s in range(1, steps)):
        continue
    a, b = means(L(f"f{n-1}_fresh")), means(L(f"f{n}_fresh"))
    for s in range(1, steps):
        m = means(L(f"f{n}_s{s}"))
        checked += 1
        outF = np.maximum(m[0] - np.maximum(a[0], b[0]), np.minimum(a[0], b[0]) - m[0]).max()
        outT = np.maximum(m[1] - np.maximum(a[1], b[1]), np.minimum(a[1], b[1]) - m[1]).max()
        bad = outF > TOL_FULL or outT > TOL_TILE
        flagged += bad
        worst.append((max(outF / TOL_FULL, outT / TOL_TILE), n, s, outF, outT))
worst.sort(reverse=True)
print(f"tint: {flagged} of {checked} in-betweens outside their neighbours' colour range "
      f"(whole picture > {TOL_FULL} or a tile > {TOL_TILE} levels)")
for r, n, s, f, t in worst[:5]:
    print(f"  f{n}_s{s}: whole {f:.2f}, tile {t:.2f}")
