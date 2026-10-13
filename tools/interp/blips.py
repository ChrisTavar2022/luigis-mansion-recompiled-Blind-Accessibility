"""One-frame artifacts in a screen recording: something drawn in a shown frame that neither its
previous nor its next frame has (a stray copy of a model, a draw with the wrong matrix for one
frame). For every shown frame, pixels that differ from both neighbours by more than a threshold in
the same direction are counted per 32x32 block; the largest such blob per frame is reported, and
crops of the worst frames (prev | frame | next) are written to <out dir>.

    blips.py <video> <out dir> [--from S] [--to S] [--thresh 40] [--top 12] [--tag]
"""
import argparse
import os
import subprocess
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from screen_judder import DEFAULT_FFMPEG, probe, tags  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("video")
    ap.add_argument("out")
    ap.add_argument("--from", dest="t0", type=float, default=0)
    ap.add_argument("--to", dest="t1", type=float, default=0)
    ap.add_argument("--thresh", type=float, default=40)
    ap.add_argument("--top", type=int, default=12)
    ap.add_argument("--width", type=int, default=1280)
    ap.add_argument("--tag", action="store_true")
    ap.add_argument("--ffmpeg", default=DEFAULT_FFMPEG)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    w, h, _ = probe(a.ffmpeg, a.video)
    W = a.width
    H = int(round(h * W / w / 2)) * 2
    cmd = [a.ffmpeg, "-hide_banner", "-loglevel", "error"]
    if a.t0:
        cmd += ["-ss", str(a.t0)]
    cmd += ["-i", a.video]
    if a.t1:
        cmd += ["-t", str(a.t1 - a.t0)]
    cmd += ["-an", "-vf", f"scale={W}:{H}:flags=area,format=rgb24", "-fps_mode", "passthrough", "-f", "rawvideo", "-"]
    tg = tags(a.ffmpeg, a.video, a.t0, a.t1) if a.tag else []
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    win = []  # (capture index, rgb, gray)
    res = []
    k = -1
    lastp = None
    B = 32
    while True:
        b = p.stdout.read(W * H * 3)
        if len(b) < W * H * 3:
            break
        k += 1
        rgb = np.frombuffer(b, np.uint8).reshape(H, W, 3)
        g = rgb.astype(np.float32).mean(axis=2)
        if tg:
            t = tg[k] if k < len(tg) else None
            if t and t[0] == lastp:
                continue
            lastp = t[0] if t else None
        if win and np.mean(np.abs(g - win[-1][2])) < 0.2:
            continue
        win.append((k, rgb, g))
        if len(win) > 3:
            win.pop(0)
        if len(win) < 3:
            continue
        g0, g1, g2 = win[0][2], win[1][2], win[2][2]
        d0, d2 = g1 - g0, g1 - g2
        if np.mean(np.abs(g1 - g0)) > 10 or np.mean(np.abs(g2 - g1)) > 10:
            continue  # flash
        th = a.thresh
        m = ((d0 > th) & (d2 > th)) | ((d0 < -th) & (d2 < -th))
        hb, wb = H // B, W // B
        blk = m[:hb * B, :wb * B].reshape(hb, B, wb, B).sum(axis=(1, 3))
        i = np.unravel_index(np.argmax(blk), blk.shape)
        res.append((int(blk[i]), win[1][0], int(i[1]) * B, int(i[0]) * B, win[0][1], win[1][1], win[2][1]))
        res.sort(key=lambda r: -r[0])
        res = res[:a.top]
    p.wait()
    for n, r in enumerate(res):
        score, kk, x, y = r[:4]
        x0, y0 = max(0, x - 96), max(0, y - 96)
        crops = [Image.fromarray(im[y0:y0 + 224, x0:x0 + 224]) for im in r[4:]]
        strip = Image.new("RGB", (224 * 3 + 8, 224))
        for j, c in enumerate(crops):
            strip.paste(c, (j * 228, 0))
        strip = strip.resize((strip.width * 2, strip.height * 2), Image.NEAREST)
        fn = os.path.join(a.out, f"blip{n:02d}_cap{kk}_x{x}_y{y}.png")
        strip.save(fn)
        tag = tg[kk] if tg and kk < len(tg) else None
        print(f"capture {kk} ({kk / 120.0:.2f} s) blob {score} px at ({x},{y}) of {W}x{H}" + (f" present {tag[0]} tape {tag[1]} alpha {tag[2]:.3f}" if tag else ""))


if __name__ == "__main__":
    sys.exit(main())
