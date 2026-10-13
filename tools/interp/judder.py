"""Real-time interpolation pacing metrics from an LM_INTERP_JUDDER log (docs/INTERPOLATION.md, "Pacing").

    judder.py <log> [--frames FIRST,LAST] [--from SECONDS] [--to SECONDS] [--dump N]

Log lines (times in microseconds since start):
  T seq stamp done guestUs cut frame   a tape was closed (stamp in NTSC fields, frame = LM_FRAME_DUMP numbering)
  R time bSeq aSeq alpha game delay    a present replays tape b blended from a
  H phase time                         host loop: 0 before begin_frame, 1 after, 2 after replay, 3 after
                                       end_frame, 4 after pace

The game advances by a fixed step per frame, so motion on screen is smooth exactly when the frame
shown, u = a + alpha (a fractional tape index), advances in proportion to the time between
presents: du x P = dt (P = the nominal frame period, the median stamp step). Reported, over the
presents that show tapes of the given frames:
  present interval   mean / p50 / p99 / max of the time between presents (after begin_frame)
  step error         |du x P - dt| per present (ms): how far one present's motion step is from
                     the ideal; x an object's screen speed (px/ms) = its judder in pixels
  shown-time error   |u x P - t| after removing a 1 s moving median (ms): how far the shown frame
                     is from an even timeline (late/early frames, uneven alpha)
  stalls             presents that showed the same frame as the previous one (dt > 1 ms)
  reversals          presents that went back in time
  alpha-1 presents   presents that showed the newest tape as is (waiting for the game)
  stamp steps        tape stamp steps in fields (2 = a steady 30 fps frame)
"""
import sys
import statistics as st

K = 1001.0 / 60000.0 * 1e3  # ms per NTSC field


def load(path):
    tapes, pres = {}, []
    done = {}  # Aurora frame -> (submit, present, gpu done) ms (P lines, present timing)
    cur = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            p = line.split()
            if not p:
                continue
            if p[0] == "T" and len(p) >= 6:
                tapes[int(p[1])] = (int(p[2]), float(p[3]) / 1e3, int(p[4]) / 1e3, int(p[5]),
                                    int(p[6]) if len(p) > 6 else -1)
            elif p[0] == "R" and len(p) >= 5:
                cur.update({"r": float(p[1]) / 1e3, "b": int(p[2]), "a": int(p[3]), "alpha": float(p[4])})
                if len(p) >= 7:
                    cur["d"] = float(p[6])
                if len(p) >= 9:
                    cur["frame"] = int(p[8])
            elif p[0] == "P" and len(p) >= 5:
                done[int(p[1])] = (float(p[2]) / 1e3, float(p[3]) / 1e3, float(p[4]) / 1e3)
            elif p[0] == "H" and len(p) >= 3:
                ph, t = int(p[1]), float(p[2]) / 1e3
                if ph == 1:
                    cur = {"t1": t}
                elif ph == 4 and cur.get("t1") is not None:
                    cur["t4"] = t
                    pres.append(cur)
                    cur = {}
                elif ph in (2, 3):
                    cur[f"t{ph}"] = t
    return tapes, pres, done


def pct(v, q):
    if not v:
        return 0.0
    v = sorted(v)
    return v[min(len(v) - 1, int(q * len(v)))]


def main():
    args = sys.argv[1:]
    path = args[0]
    opt = lambda name, default: args[args.index(name) + 1] if name in args else default
    lo, hi = float(opt("--from", 0)), float(opt("--to", 1e18))
    dump = int(opt("--dump", 0))
    frames = tuple(int(x) for x in opt("--frames", "").split(",")) if "--frames" in args else None
    tapes, pres, done = load(path)
    # Nominal game frame period: the median stamp step over the measured tapes (2 fields at 30 fps).
    td = sorted((k, v) for k, v in tapes.items() if not frames or frames[0] <= v[4] <= frames[1])
    ints = [td[i][1][0] - td[i - 1][1][0] for i in range(1, len(td)) if td[i][0] == td[i - 1][0] + 1]
    P = (st.median(ints) if ints else 2) * K
    rows = []
    for p in pres:
        if "b" not in p or p["t1"] / 1e3 < lo or p["t1"] / 1e3 > hi:
            continue
        b = tapes.get(p["b"])
        if not b or (frames and not frames[0] <= b[4] <= frames[1]):
            continue
        u = (p["a"] + p["alpha"]) if p["a"] else p["b"]
        rows.append((p["t1"], u * P, p, b))
    if len(rows) < 10:
        print("not enough presents")
        return
    t = [r[0] for r in rows]
    g = [r[1] for r in rows]
    dt = [t[i] - t[i - 1] for i in range(1, len(t))]
    dg = [g[i] - g[i - 1] for i in range(1, len(g))]
    ok = [i for i in range(len(dt)) if abs(dg[i]) < 500 and dt[i] < 500]
    steperr = [abs(dg[i] - dt[i]) for i in ok]
    stalls = sum(1 for i in ok if dg[i] <= 0.01 and dt[i] > 1)
    back = sum(1 for i in ok if dg[i] < -0.01)
    off = [g[i] - t[i] for i in range(len(t))]
    err = []
    for i in range(len(t)):
        win = [off[k] for k in range(max(0, i - 150), min(len(t), i + 150)) if abs(t[k] - t[i]) <= 500]
        err.append(abs(off[i] - st.median(win)))
    late = sum(1 for r in rows if r[2]["alpha"] >= 0.999)
    span = (t[-1] - t[0]) / 1e3
    print(f"presents {len(rows)} over {span:.1f} s = {len(rows) / span:.1f} fps; tapes "
          f"{len(set(r[2]['b'] for r in rows))}; frame period {P:.2f} ms")
    print(f"present interval ms: mean {st.mean(dt):.2f} p50 {pct(dt, .5):.2f} p99 {pct(dt, .99):.2f} max {max(dt):.2f}")
    print(f"step error ms: mean {st.mean(steperr):.2f} p95 {pct(steperr, .95):.2f} p99 {pct(steperr, .99):.2f} "
          f"max {max(steperr):.2f}")
    print(f"shown-time error ms: mean {st.mean(err):.2f} p95 {pct(err, .95):.2f} p99 {pct(err, .99):.2f} max {max(err):.2f}")
    print(f"stalls {stalls} reversals {back} alpha-1 presents {late}")
    # Present timing (Aurora patch 0197): with a variable refresh rate display a frame is shown
    # about when the GPU finishes it, so the motion step of a present should match the time between
    # GPU completions: display step error = |du x P - d(gpu done)|.
    gd = [(r[2].get("frame"), r[0], r[1]) for r in rows]
    gd = [(done[f][2], tt, gg, done[f][0]) for f, tt, gg in gd if f is not None and f in done and done[f][2] > 0]
    if len(gd) > 10:
        lat = [x[0] - x[1] for x in gd]
        dd = [gd[i][0] - gd[i - 1][0] for i in range(1, len(gd))]
        de = [abs((gd[i][2] - gd[i - 1][2]) - dd[i - 1]) for i in range(1, len(gd)) if dd[i - 1] < 500]
        print(f"gpu-done interval ms: mean {st.mean(dd):.2f} p50 {pct(dd, .5):.2f} p95 {pct(dd, .95):.2f} p99 {pct(dd, .99):.2f} "
              f"max {max(dd):.2f}; latency replay->gpu done ms: p5 {pct(lat, .05):.2f} p50 {pct(lat, .5):.2f} p95 {pct(lat, .95):.2f}")
        print(f"display step error ms (vs gpu done): mean {st.mean(de):.2f} p95 {pct(de, .95):.2f} p99 {pct(de, .99):.2f} max {max(de):.2f}")
    hist = {}
    for v in ints:
        hist[v] = hist.get(v, 0) + 1
    print("tape stamp steps (fields: count):", dict(sorted(hist.items())))
    if dump:  # the N presents with the largest shown-time error, with their neighbours
        for w in sorted(sorted(range(len(rows)), key=lambda i: -err[i])[:dump]):
            for i in range(max(1, w - 3), min(len(rows), w + 3)):
                p, b = rows[i][2], rows[i][3]
                print(f"{'*' if i == w else ' '} t {t[i]:10.2f} dt {t[i] - t[i - 1]:6.2f} step {g[i] - g[i - 1]:6.2f} "
                      f"err {err[i]:6.2f} b {p['b']} alpha {p['alpha']:.3f} stamp {b[0]} done-t {b[1] - t[i]:7.2f} "
                      f"delay {p.get('d', 0):.1f}")
            print()


main()
