"""Judder of what reaches the screen, from a screen recording (docs/INTERPOLATION.md, "Screen capture").

    screen_judder.py <video> [--from S] [--to S] [--crop x,y,w,h] [--scale W] [--grid C,R]
                     [--ffmpeg PATH] [--csv OUT] [--min-speed PX] [--fixed-rate]

Works on the user's own recordings (ShadowPlay mp4 at a fixed 120 fps: repeated frames are
duplicates) and on tools/interp/screen_run.py captures (ffmpeg ddagrab with dup_frames=0: one
captured frame per desktop update, timestamped when Desktop Duplication delivered it, i.e. close
to when it was shown).

Per pair of consecutive captured frames the displacement of the static scenery is measured by
phase correlation on a C x R grid of blocks (default 8 x 5, outer ring dropped, median of the
blocks). Static scenery under a smoothly moving camera follows a smooth path x(t), so:

  dup            frames identical to the previous one (nothing new reached the screen)
  intervals      time between new frames (ms): mean / p50 / p95 / max, and the share of frames
                 whose interval is off the median by more than 25%
  position error per shown frame, how far the scenery is from a smooth path through the
                 neighbouring frames (local quadratic fit of the cumulative position against
                 time over +-60 ms, the frame itself left out): the judder in source pixels,
                 mean / p95 / max over frames where the scene moves at least --min-speed px per
                 frame interval (scaled to 8.33 ms)
  speed error    |step/dt - local mean speed| / local mean speed (0 = every frame moves the
                 scene by exactly its share of the elapsed time)
  even steps     share of moving frames whose step is within 25% of local speed x dt
"""
import argparse
import re
import subprocess
import sys
import threading

import numpy as np

DEFAULT_FFMPEG = r"C:\Users\rmelv\AppData\Roaming\Python\Python314\site-packages\imageio_ffmpeg\binaries\ffmpeg-win-x86_64-v7.1.exe"


def probe(ff, path):
    r = subprocess.run([ff, "-hide_banner", "-i", path], capture_output=True, text=True)
    w = h = None
    fps = None
    for line in r.stderr.splitlines():
        if "Video:" in line:
            m = re.search(r" (\d{3,5})x(\d{3,5})", line)
            if m:
                w, h = int(m.group(1)), int(m.group(2))
            m = re.search(r"([\d.]+) fps", line)
            if m:
                fps = float(m.group(1))
            break
    return w, h, fps


def frames(ff, path, t0, t1, crop, outw):
    """Yields (gray image, source px per analysed px, pts seconds)."""
    w, h, fps = probe(ff, path)
    if crop:
        cx, cy, cw, ch = crop
        if max(crop) <= 1.0:  # fractions of the picture
            cx, cy, cw, ch = cx * w, cy * h, cw * w, ch * h
        cx, cy, cw, ch = (int(v) // 2 * 2 for v in (cx, cy, cw, ch))
    else:
        cx, cy, cw, ch = 0, 0, w, h
    outh = int(round(ch * outw / cw / 2)) * 2
    vf = f"crop={cw}:{ch}:{cx}:{cy},scale={outw}:{outh}:flags=area,format=gray,showinfo"
    cmd = [ff, "-hide_banner", "-loglevel", "info"]
    if t0:
        cmd += ["-ss", str(t0)]
    cmd += ["-i", path]
    if t1:
        cmd += ["-t", str(t1 - (t0 or 0))]
    cmd += ["-an", "-vf", vf, "-fps_mode", "passthrough", "-f", "rawvideo", "-"]
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    pts = []
    ev = threading.Condition()

    def reader():
        for raw in p.stderr:
            line = raw.decode("utf-8", "replace")
            m = re.search(r"pts_time:\s*([-\d.e]+)", line)
            if m:
                with ev:
                    pts.append(float(m.group(1)))
                    ev.notify_all()
        with ev:
            pts.append(None)
            ev.notify_all()

    th = threading.Thread(target=reader, daemon=True)
    th.start()
    n = outw * outh
    k = 0
    while True:
        b = p.stdout.read(n)
        if len(b) < n:
            break
        with ev:
            ev.wait_for(lambda: len(pts) > k)
            t = pts[k]
        if t is None:
            t = k / (fps or 120.0)
        yield np.frombuffer(b, np.uint8).reshape(outh, outw).astype(np.float32), cw / outw, t
        k += 1
    p.wait()


def tags(ff, path, t0, t1):
    """LM_FRAME_TAG bar codes (menu.cpp) per captured frame: (present, tape seq & 0xFF, alpha) or None."""
    W, H = 800, 32
    cmd = [ff, "-hide_banner", "-loglevel", "error"]
    if t0:
        cmd += ["-ss", str(t0)]
    cmd += ["-i", path]
    if t1:
        cmd += ["-t", str(t1 - (t0 or 0))]
    cmd += ["-an", "-vf", f"crop={W}:{H}:0:0,format=gray", "-fps_mode", "passthrough", "-f", "rawvideo", "-"]
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    out = []
    while True:
        b = p.stdout.read(W * H)
        if len(b) < W * H:
            break
        img = np.frombuffer(b, np.uint8).reshape(H, W)
        row = img[4].astype(int)
        # cell size: the white reference cell runs from x=0
        if row[2] < 160:
            out.append(None)
            continue
        cell = int(np.argmax(row < 96))
        if cell < 4:
            out.append(None)
            continue
        y = cell // 2
        v = 0
        for i in range(32):
            x = int((2 + i + 0.5) * cell)
            if x >= W:
                break
            v = v << 1 | (1 if img[y, x] > 128 else 0)
        out.append((v >> 16, (v >> 8) & 0xFF, (v & 0xFF) / 255.0))
    p.wait()
    return out


def phase_shift(a, b, win):
    """Shift that moves a onto b (dx, dy) and the correlation peak."""
    fa = np.fft.rfft2((a - a.mean()) * win)
    fb = np.fft.rfft2((b - b.mean()) * win)
    r = fb * np.conj(fa)
    r /= np.abs(r) + 1e-6
    c = np.fft.irfft2(r, s=a.shape)
    h, w = a.shape
    dy, dx = np.unravel_index(np.argmax(c), c.shape)
    peak = c[dy, dx]

    def sub(cm, c0, cp):
        d = cm - 2 * c0 + cp
        return 0.5 * (cm - cp) / d if abs(d) > 1e-9 else 0.0
    sy = sub(c[(dy - 1) % h, dx], c[dy, dx], c[(dy + 1) % h, dx])
    sx = sub(c[dy, (dx - 1) % w], c[dy, dx], c[dy, (dx + 1) % w])
    if dy > h // 2:
        dy -= h
    if dx > w // 2:
        dx -= w
    return dx + sx, dy + sy, peak


def measure(a):
    crop = tuple(float(v) for v in a.crop.split(",")) if a.crop else None
    gc, gr = (int(v) for v in a.grid.split(","))
    prev = None
    rows = []  # (t, dup, mad, dx, dy, blocks)
    win = None
    for img, scale, t in frames(a.ffmpeg, a.video, a.t0, a.t1, crop, a.scale):
        if prev is not None:
            mad = float(np.mean(np.abs(img - prev)))
            dup = mad < a.dup_thresh
            dxs, dys = [], []
            if not dup:
                h, w = img.shape
                bh, bw = h // gr, w // gc
                if win is None:
                    win = np.outer(np.hanning(bh), np.hanning(bw)).astype(np.float32)
                for r in range(1, gr - 1):
                    for c in range(1, gc - 1):
                        pa = prev[r * bh:(r + 1) * bh, c * bw:(c + 1) * bw]
                        pb = img[r * bh:(r + 1) * bh, c * bw:(c + 1) * bw]
                        if pa.std() < 3:  # flat (dark) block: nothing to track
                            continue
                        dx, dy, pk = phase_shift(pa, pb, win)
                        if pk > 0.08:
                            dxs.append(dx)
                            dys.append(dy)
            dx, dy = (float(np.median(dxs)), float(np.median(dys))) if dxs else (0.0, 0.0)
            rows.append((t, dup, mad, dx * scale, dy * scale, len(dxs)))
        prev = img
    return rows


def analyse(rows, min_speed, label=""):
    """Metrics over the shown frames (duplicates merged into the previous frame's interval)."""
    out = {}
    if len(rows) < 10:
        print("  too few frames")
        return out
    t = np.array([r[0] for r in rows])
    dup = np.array([r[1] for r in rows])
    step = np.array([(r[3], r[4]) for r in rows])
    out["frames"] = len(rows) + 1
    out["dups"] = int(dup.sum())
    # shown frames: the new ones; their time and cumulative position
    new = ~dup
    tn = t[new]
    # flashes (lightning, a light switching): the whole picture changes and block matching is
    # unreliable for that frame and the next; they are left out of the error statistics
    madn = np.array([r[2] for r in rows])[new]
    flash = np.zeros(len(madn), bool)
    for i in range(len(madn)):
        nb = np.concatenate([madn[max(0, i - 6):i], madn[i + 1:i + 7]])
        if len(nb) and madn[i] > 2.5 * max(np.median(nb), 0.5):
            flash[i] = True
            if i + 1 < len(flash):
                flash[i + 1] = True
    out["flash"] = int(flash.sum())
    sn = step[new].copy()
    good = np.where(~flash)[0]
    for i in np.where(flash)[0]:  # their steps: from the nearest good frames' speed
        j = good[np.argsort(np.abs(good - i))[:4]] if len(good) else []
        if len(j):
            dts = np.maximum(np.diff(np.concatenate([[tn[0] - 1 / 120], tn]))[j], 1e-4)
            sp = (sn[j] / dts[:, None]).mean(axis=0)
            sn[i] = sp * max(tn[i] - (tn[i - 1] if i else tn[i] - 1 / 120), 1e-4)
    pos = np.cumsum(sn, axis=0)
    dt = np.diff(tn)
    if len(dt) < 5:
        return out
    med = float(np.median(dt))
    out["interval"] = (float(dt.mean()) * 1e3, med * 1e3, float(np.percentile(dt, 95)) * 1e3, float(dt.max()) * 1e3)
    out["uneven"] = float(np.mean(np.abs(dt - med) > 0.25 * med))
    # position error: local quadratic fit of pos(t) over +-60 ms excluding the frame itself
    errs, serr, even = [], [], []
    ref = 1.0 / 120.0
    for i in range(1, len(tn) - 1):
        if flash[i] or flash[i - 1]:
            continue
        sel = np.where(np.abs(tn - tn[i]) <= 0.06)[0]
        sel = sel[(sel != i) & ~flash[sel]]
        if len(sel) < 6:  # low frame rates: the three nearest shown frames on each side
            sel = np.array([j for j in range(max(0, i - 3), min(len(tn), i + 4)) if j != i])
        if len(sel) < 5:
            continue
        tt = tn[sel] - tn[i]
        v = pos[sel]
        A = np.stack([np.ones_like(tt), tt, tt * tt], axis=1)
        coef, *_ = np.linalg.lstsq(A, v, rcond=None)
        speed = np.hypot(*coef[1])  # px/s
        if speed * ref < min_speed:
            continue
        pred = coef[0]
        errs.append(float(np.hypot(*(pos[i] - pred))))
        # step of this frame vs local speed x dt
        d = tn[i] - tn[i - 1]
        s = pos[i] - pos[i - 1]
        exp = coef[1] + 2 * coef[2] * (tn[i] - d / 2 - tn[i])
        e = np.hypot(*exp) * d
        if e > 0:
            r = float(np.dot(s, exp) / (np.hypot(*exp) * e))
            serr.append(abs(r - 1))
            even.append(abs(r - 1) <= 0.25)
    if errs:
        e = np.array(errs)
        out["moving"] = len(errs)
        out["pos_err"] = (float(e.mean()), float(np.percentile(e, 95)), float(e.max()))
        out["speed_err"] = (float(np.mean(serr)), float(np.percentile(serr, 95)))
        out["even"] = float(np.mean(even))
    sp = np.hypot(step[new][:, 0], step[new][:, 1])
    out["speed"] = float(np.median(sp)) if len(sp) else 0.0
    return out


def report(o, title):
    print(title)
    if "frames" not in o:
        return
    print(f"  frames {o['frames']}, duplicates {o['dups']} ({100.0 * o['dups'] / max(1, o['frames'] - 1):.1f}%), "
          f"flash frames left out {o.get('flash', 0)}")
    if "interval" in o:
        m, p50, p95, mx = o["interval"]
        print(f"  shown-frame interval ms: mean {m:.2f}  p50 {p50:.2f}  p95 {p95:.2f}  max {mx:.2f}  "
              f"(off median by >25%: {100.0 * o['uneven']:.1f}%) -> {1000.0 / m:.1f} fps shown")
    if "pos_err" in o:
        print(f"  moving frames {o['moving']}, median step {o['speed']:.1f} px")
        print(f"  position error px: mean {o['pos_err'][0]:.2f}  p95 {o['pos_err'][1]:.2f}  max {o['pos_err'][2]:.2f}")
        print(f"  speed error: mean {o['speed_err'][0]:.3f}  p95 {o['speed_err'][1]:.3f};  even steps {100.0 * o['even']:.1f}%")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("video")
    ap.add_argument("--from", dest="t0", type=float, default=0)
    ap.add_argument("--to", dest="t1", type=float, default=0)
    ap.add_argument("--crop", default="")
    ap.add_argument("--scale", type=int, default=960)
    ap.add_argument("--grid", default="8,5")
    ap.add_argument("--ffmpeg", default=DEFAULT_FFMPEG)
    ap.add_argument("--csv", default="")
    ap.add_argument("--min-speed", type=float, default=2.0, help="source px per 8.33 ms")
    ap.add_argument("--dup-thresh", type=float, default=0.25, help="mean abs diff (0-255) below which a frame is a duplicate")
    ap.add_argument("--tag", action="store_true", help="decode LM_FRAME_TAG bar codes (present, tape, alpha)")
    a = ap.parse_args()
    rows = measure(a)
    tg = tags(a.ffmpeg, a.video, a.t0, a.t1) if a.tag else []
    if a.csv:
        with open(a.csv, "w", encoding="utf-8") as f:
            f.write("t,dup,mad,dx,dy,blocks,present,tape,alpha\n")
            for i, r in enumerate(rows):
                g = tg[i + 1] if i + 1 < len(tg) and tg[i + 1] else (-1, -1, -1.0)
                f.write("%.6f,%d,%.3f,%.3f,%.3f,%d,%d,%d,%.4f\n" % (r[0], r[1], r[2], r[3], r[4], r[5], g[0], g[1], g[2]))
    report(analyse(rows, a.min_speed), a.video)
    if tg:
        tag_report(rows, tg)
    return 0


def piecewise_ratios(steps, min_speed=0.0):
    """Interpolation moves things linearly between game frames: within game frame T (u in
    (T-1, T]) at T's own speed. steps = [(u0, u1, dx)] between consecutive shown presents. Each
    game frame's speed is the median over the steps that lie inside it; a step's expected motion
    is the sum over the game frames it overlaps of overlap x speed. Returns [(dx / expected,
    crosses a game frame)] (1 = exactly what linear interpolation gives; a jump at the game frame
    shows on the crossing steps, an uneven blend on any)."""
    speed = {}
    for u0, u1, dx in steps:
        if u1 - u0 > 0.02 and np.ceil(u0 + 1e-6) == np.ceil(u1 - 1e-6):
            speed.setdefault(int(np.ceil(u1 - 1e-6)), []).append(dx / (u1 - u0))
    med = {k: float(np.median(v)) for k, v in speed.items() if len(v) >= 1}
    out = []
    for u0, u1, dx in steps:
        if u1 - u0 <= 0.02 or u1 - u0 > 2.0:
            continue
        exp = 0.0
        ok = True
        for T in range(int(np.ceil(u0 + 1e-6)), int(np.ceil(u1 - 1e-6)) + 1):
            ov = min(u1, T) - max(u0, T - 1)
            if ov <= 0:
                continue
            if T not in med:
                ok = False
                break
            exp += ov * med[T]
        if not ok or abs(exp) < 1e-9 or abs(exp) / (u1 - u0) < min_speed:
            continue
        out.append((dx / exp, bool(np.ceil(u0 + 1e-6) != np.ceil(u1 - 1e-6))))
    return out


def tag_report(rows, tg):
    """With LM_FRAME_TAG: which presents reached the screen, and whether the scenery moved by what
    their interpolation position u = tape + alpha says (blending) and when (pacing)."""
    tg = tg[1:len(rows) + 1]  # rows[i] is the step into captured frame i + 1
    ok = [i for i, g in enumerate(tg) if g]
    print(f"  tags decoded {len(ok)} of {len(tg)}")
    if len(ok) < 10:
        return
    pres, u = {}, {}
    lastp = lastt = None
    bp = bt = 0
    for i in ok:  # unwrap: present 16 bits, tape 8 bits
        p, s, al = tg[i]
        if lastp is not None and p < lastp - 30000:
            bp += 65536
        if lastt is not None and s < lastt - 128:
            bt += 256
        lastp, lastt = p, s
        pres[i] = p + bp
        u[i] = s + bt - 1 + al  # tape b at alpha 1 = b; alpha 0 = a = b - 1
    seq = [pres[i] for i in ok]
    d = np.diff(seq)
    print(f"  presents captured {len(set(seq))}: repeated (same present captured again) {int(np.sum(d == 0))}, "
          f"skipped (never captured) {int(np.sum(np.maximum(d - 1, 0)))}, backwards {int(np.sum(d < 0))}")
    t = np.array([r[0] for r in rows])
    st = np.array([r[3] for r in rows])  # x (strafing); y is small
    ratios, terr = [], []
    P = 1001.0 / 30000.0
    for k in range(1, len(ok)):
        i, j = ok[k - 1], ok[k]
        if j != i + 1:
            continue
        du = u[j] - u[i]
        if du <= 0.02:
            continue
        nb = []  # local px per game frame: median over the neighbours
        for m in range(max(1, k - 8), min(len(ok), k + 9)):
            a, b = ok[m - 1], ok[m]
            if b == a + 1 and u[b] - u[a] > 0.02 and m != k:
                nb.append(st[b] / (u[b] - u[a]))
        if len(nb) < 4:
            continue
        ref = float(np.median(nb))
        if abs(ref) < 6:  # px per game frame: the scene must move
            continue
        ratios.append(st[j] / du / ref)
        terr.append((t[j] - t[i]) - du * P)
    # Per tape: each present's scenery speed (px per game frame) against the tape's median, by its
    # place in the tape (#0 crosses the tape boundary): a jump at the game frame shows as #0 > 1.
    bytape = {}
    for k in range(1, len(ok)):
        i, j = ok[k - 1], ok[k]
        du = u[j] - u[i]
        if j == i + 1 and du > 0.02:
            bytape.setdefault(int(np.ceil(u[j] - 1e-6)), []).append(st[j] / du)
    pos = {}
    for v in bytape.values():
        med = float(np.median(v))
        if len(v) >= 3 and abs(med) >= 6:
            for n, s in enumerate(v):
                pos.setdefault(n, []).append(s / med)
    if pos:
        print("  speed by place in tape (vs the tape's median): " +
              "  ".join(f"#{n} {np.mean(v):.3f}" for n, v in sorted(pos.items()) if len(v) > 5))
    steps = [(u[ok[k - 1]], u[ok[k]], st[ok[k]]) for k in range(1, len(ok)) if ok[k] == ok[k - 1] + 1]
    pw = piecewise_ratios(steps, min_speed=6.0)
    if pw:
        r = np.abs(np.array([x[0] for x in pw]) - 1)
        cross = [x[0] for x in pw if x[1]]
        inner = [x[0] for x in pw if not x[1]]
        print(f"  blending vs linear interpolation (each game frame's own speed): |r-1| mean {r.mean():.3f}  p95 "
              f"{np.percentile(r, 95):.3f}; off by >25%: {100.0 * np.mean(r > 0.25):.1f}% of {len(r)}; "
              f"crossing a game frame {np.mean(cross) if cross else 0:.3f}, inside {np.mean(inner) if inner else 0:.3f}")
    if ratios:
        r = np.abs(np.array(ratios) - 1)
        print(f"  blending: scenery step / (du x local px per game frame): |r-1| mean {r.mean():.3f}  p95 "
              f"{np.percentile(r, 95):.3f}; off by >25%: {100.0 * np.mean(r > 0.25):.1f}% of {len(r)}")
        te = np.array(terr) * 1e3
        te -= np.median(te)
        print(f"  pacing: display dt - du x 33.37 ms: mean |e| {np.mean(np.abs(te)):.2f} ms  p95 {np.percentile(np.abs(te), 95):.2f} ms")


if __name__ == "__main__":
    sys.exit(main())
