"""Tracks a bright static feature (e.g. the foyer's candle flames) through a screen recording and
checks its motion against the interpolation position of each shown frame (LM_FRAME_TAG).

    track_bright.py <video> --region x,y,w,h [--rgb rmin,gmax,bmax] [--from S] [--to S] [--csv OUT]

Per captured frame: the centroid of the pixels in the region that pass the colour test (default
R >= 170, G <= 150, B <= 120: candle flames). With tags: the feature's position must be a smooth
function of u = tape + alpha; reported per shown present is the deviation of the centroid from the
straight line through its neighbours in u (a shake of static geometry = positions that go back and
forth or step unevenly against u), and back-and-forth reversals within one direction of motion.
"""
import argparse
import os
import subprocess
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from screen_judder import DEFAULT_FFMPEG, tags  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("video")
    ap.add_argument("--region", required=True)
    ap.add_argument("--rgb", default="170,150,120")
    ap.add_argument("--from", dest="t0", type=float, default=0)
    ap.add_argument("--to", dest="t1", type=float, default=0)
    ap.add_argument("--csv", default="")
    ap.add_argument("--ffmpeg", default=DEFAULT_FFMPEG)
    a = ap.parse_args()
    x, y, w, h = (int(v) for v in a.region.split(","))
    rmin, gmax, bmax = (int(v) for v in a.rgb.split(","))
    cmd = [a.ffmpeg, "-hide_banner", "-loglevel", "error"]
    if a.t0:
        cmd += ["-ss", str(a.t0)]
    cmd += ["-i", a.video]
    if a.t1:
        cmd += ["-t", str(a.t1 - a.t0)]
    cmd += ["-an", "-vf", f"crop={w}:{h}:{x}:{y},format=rgb24", "-fps_mode", "passthrough", "-f", "rawvideo", "-"]
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    cents = []
    n = w * h * 3
    while True:
        b = p.stdout.read(n)
        if len(b) < n:
            break
        im = np.frombuffer(b, np.uint8).reshape(h, w, 3).astype(np.int32)
        m = (im[:, :, 0] >= rmin) & (im[:, :, 1] <= gmax) & (im[:, :, 2] <= bmax)
        if m.sum() < 20:
            cents.append(None)
            continue
        ys, xs = np.nonzero(m)
        wgt = im[:, :, 0][m].astype(np.float64)
        cents.append((float((xs * wgt).sum() / wgt.sum()) + x, float((ys * wgt).sum() / wgt.sum()) + y, int(m.sum())))
    p.wait()
    tg = tags(a.ffmpeg, a.video, a.t0, a.t1)
    rows = []
    lastt = None
    bt = 0
    prevp = None
    for i, c in enumerate(cents):
        g = tg[i] if i < len(tg) else None
        if not g or not c:
            continue
        pres, s, al = g
        if lastt is not None and s < lastt - 128:
            bt += 256
        lastt = s
        if pres == prevp:
            continue  # the same present captured again
        prevp = pres
        rows.append((i, pres, s + bt - 1 + al, c[0], c[1], c[2]))
    if a.csv:
        with open(a.csv, "w", encoding="utf-8") as f:
            f.write("capture,present,u,x,y,pixels\n")
            for r in rows:
                f.write("%d,%d,%.4f,%.2f,%.2f,%d\n" % r)
    if len(rows) < 10:
        print("too few tracked frames", len(rows))
        return 1
    u = np.array([r[2] for r in rows])
    px = np.array([r[3] for r in rows])
    dev = []
    rev = 0
    for k in range(2, len(rows) - 2):
        uu = u[k - 2:k + 3]
        xx = px[k - 2:k + 3]
        if np.ptp(uu) <= 0 or np.ptp(uu) > 3:
            continue
        sel = [0, 1, 3, 4]
        A = np.stack([np.ones(4), uu[sel] - uu[2]], axis=1)
        coef, *_ = np.linalg.lstsq(A, xx[sel], rcond=None)
        dev.append(abs(xx[2] - coef[0]))
        d1, d2 = px[k] - px[k - 1], px[k + 1] - px[k]
        if abs(coef[1]) > 6 and d1 * d2 < 0 and min(abs(d1), abs(d2)) > 1.0:
            rev += 1
    dev = np.array(dev)
    print(f"{a.video}: tracked presents {len(rows)}")
    print(f"  position vs u (deviation from the line through 4 neighbours, px): mean {dev.mean():.2f}  p95 "
          f"{np.percentile(dev, 95):.2f}  max {dev.max():.2f}; back-and-forth steps while moving: {rev}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
