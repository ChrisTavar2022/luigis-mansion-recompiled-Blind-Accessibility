"""The screen-capture regression suite (docs/INTERPOLATION.md, "Screen capture"): runs
tools/interp/screen_run.py for each scenario at the user's settings (fullscreen) and prints one
table of what reached the screen.

    screen_suite.py <out dir> [--exe PATH] [--scenarios foyer,turn,vacuum,nursery,ballroom,ghosts]
                    [--secs 20] [--set section.key=value ...] [--env K=V ...]

Columns: shown fps (captured screen updates), interval p95 (ms), static-scenery position error
(mean / p95 px, a 4K frame), even steps (%), blending error (|scenery step / (du x speed) - 1|
mean, off by > 25%: needs LM_FRAME_TAG, i.e. this branch's build), display step error vs GPU
completion (mean / p95 ms, from the game's own log: needs Aurora patch 0197), one-frame blobs
(tools/interp/blips.py: the largest blob of the worst frame, px at 1280 wide).
"""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def grab(pat, text, n=1, cast=float):
    m = re.search(pat, text)
    if not m:
        return None
    return tuple(cast(m.group(i + 1)) for i in range(n)) if n > 1 else cast(m.group(1))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--exe", default="")
    ap.add_argument("--scenarios", default="foyer,turn,vacuum,nursery,ballroom,ghosts")
    ap.add_argument("--secs", type=float, default=20)
    ap.add_argument("--set", action="append", default=[])
    ap.add_argument("--env", action="append", default=[])
    ap.add_argument("--disc", default="")
    ap.add_argument("--keep-video", action="store_true")
    a = ap.parse_args()
    rows = []
    for sc in a.scenarios.split(","):
        out = os.path.join(a.out, sc)
        cmd = [sys.executable, os.path.join(HERE, "screen_run.py"), out, "--scenario", sc, "--secs", str(a.secs),
               "--keep-video", "--set", "video.fullscreen=true"]
        for s in a.set:
            cmd += ["--set", s]
        for e in a.env:
            cmd += ["--env", e]
        if a.exe:
            cmd += ["--exe", a.exe]
        if a.disc:
            cmd += ["--disc", a.disc]
        r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
        text = r.stdout + r.stderr
        open(os.path.join(a.out, f"{sc}.txt"), "w", encoding="utf-8").write(text)
        video = os.path.join(out, "capture.mp4")
        blob = None
        if os.path.exists(video):
            b = subprocess.run([sys.executable, os.path.join(HERE, "blips.py"), video, os.path.join(out, "blips"), "--top", "3"],
                               capture_output=True, text=True, encoding="utf-8", errors="replace")
            blob = grab(r"blob (\d+) px", b.stdout, cast=int)
            if not a.keep_video:
                os.remove(video)
        rows.append((sc,
                     grab(r"-> ([\d.]+) fps shown", text),
                     grab(r"interval ms: mean [\d.]+  p50 [\d.]+  p95 ([\d.]+)", text),
                     grab(r"position error px: mean ([\d.]+)  p95 ([\d.]+)", text, 2),
                     grab(r"even steps ([\d.]+)%", text),
                     grab(r"\|r-1\| mean ([\d.]+) .*?off by >25%: ([\d.]+)%", text, 2),
                     grab(r"display step error ms \(vs gpu done\): mean ([\d.]+) p95 ([\d.]+)", text, 2),
                     blob))
        print(f"{sc} done", flush=True)
    fmt = lambda v, f="{:.2f}": "-" if v is None else (" / ".join(f.format(x) for x in v) if isinstance(v, tuple) else f.format(v))
    print("| scenario | shown fps | interval p95 ms | position error px | even steps % | blending err (mean / >25%) | display step err ms | worst blob px |")
    print("|---|---|---|---|---|---|---|---|")
    for r in rows:
        print(f"| {r[0]} | {fmt(r[1], '{:.1f}')} | {fmt(r[2])} | {fmt(r[3])} | {fmt(r[4], '{:.1f}')} | {fmt(r[5])} | {fmt(r[6])} | {fmt(r[7], '{}')} |")


if __name__ == "__main__":
    main()
