"""Where on screen things go back and forth: a heat map of pixels whose brightness rises and then
falls (or the reverse) by more than a threshold over three consecutive shown frames of a screen
recording. A static object that shakes under a moving camera, or something drawn in the wrong
place for one frame, lights up; smooth motion does not (an edge moving steadily one way changes
each pixel in one direction only).

    osc_map.py <video> <out.png> [--from S] [--to S] [--thresh 24] [--width 960] [--tag]

With --tag (LM_FRAME_TAG), frames that repeat a present are skipped. Also writes <out>_frames.txt:
per captured frame, the number of oscillating pixels (to find the frames to look at).
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
    ap.add_argument("--thresh", type=float, default=24)
    ap.add_argument("--width", type=int, default=960)
    ap.add_argument("--tag", action="store_true")
    ap.add_argument("--ffmpeg", default=DEFAULT_FFMPEG)
    a = ap.parse_args()
    w, h, _ = probe(a.ffmpeg, a.video)
    W = a.width
    H = int(round(h * W / w / 2)) * 2
    cmd = [a.ffmpeg, "-hide_banner", "-loglevel", "error"]
    if a.t0:
        cmd += ["-ss", str(a.t0)]
    cmd += ["-i", a.video]
    if a.t1:
        cmd += ["-t", str(a.t1 - a.t0)]
    cmd += ["-an", "-vf", f"scale={W}:{H}:flags=area,format=gray", "-fps_mode", "passthrough", "-f", "rawvideo", "-"]
    tg = tags(a.ffmpeg, a.video, a.t0, a.t1) if a.tag else []
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    heat = np.zeros((H, W), np.float32)
    acc = None
    hist = []
    frames = []
    k = -1
    last_present = None
    while True:
        b = p.stdout.read(W * H)
        if len(b) < W * H:
            break
        k += 1
        img = np.frombuffer(b, np.uint8).reshape(H, W).astype(np.float32)
        if tg:
            g = tg[k] if k < len(tg) else None
            if g and g[0] == last_present:
                continue
            last_present = g[0] if g else None
        if acc is None:
            acc = img
            continue
        if np.mean(np.abs(img - acc)) < 0.2:  # repeated frame
            continue
        hist.append(img)
        frames.append(k)
        if len(hist) > 3:
            hist.pop(0)
            frames.pop(0)
        acc = img
        if len(hist) == 3:
            d1 = hist[1] - hist[0]
            d2 = hist[2] - hist[1]
            t = a.thresh
            o = ((d1 > t) & (d2 < -t)) | ((d1 < -t) & (d2 > t))
            # a whole-picture flash (lightning) is not a shake
            if np.mean(np.abs(d1)) < 12 and np.mean(np.abs(d2)) < 12:
                heat += o
            with open(a.out + "_frames.txt", "a", encoding="utf-8") as f:
                f.write(f"{frames[1]} {int(o.sum())} {np.mean(np.abs(d1)):.2f}\n")
    p.wait()
    m = heat / max(1.0, float(np.percentile(heat, 99.9)))
    base = (hist[-1] if hist else np.zeros((H, W))) * 0.5
    rgb = np.stack([np.clip(base + 255 * m, 0, 255), base, base], axis=2).astype(np.uint8)
    Image.fromarray(rgb).save(a.out)
    print(f"oscillating pixel-frames total {int(heat.sum())}; written {a.out}")


if __name__ == "__main__":
    sys.exit(main())
