"""Summary of an LM_PACING_LOG file (src/recomp/pacing.cpp; docs/INTERPOLATION.md "Pass 5").

    pacing_stats.py <pacing.log> [--skip S] [--until S] [--worst N] [--per-second]

Per mode: presented fps (from the Present times and from the GPU completion times), frame-interval
percentiles, intervals above 1.5x the target period (with where the time went in each: schedule
wait, GPU wait, aurora_begin_frame, host CPU, render worker until Present, Present until GPU done),
the measured frame cost and `need`, and game-frame stalls.
"""
import argparse
import statistics


def pct(v, p):
    if not v:
        return 0.0
    v = sorted(v)
    return v[min(len(v) - 1, int(len(v) * p))]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--skip", type=float, default=0.0, help="ignore frames before this many seconds")
    ap.add_argument("--until", type=float, default=1e9)
    ap.add_argument("--worst", type=int, default=12)
    ap.add_argument("--per-second", action="store_true")
    ap.add_argument("--dump", action="store_true", help="print every frame in --skip..--until with its phases")
    a = ap.parse_args()
    frames = []
    for line in open(a.log, encoding="utf-8", errors="replace"):
        if not line.startswith("F "):
            if a.per_second and line.startswith("S "):
                t = float(line.split()[1])
                if a.skip <= t <= a.until:
                    print(line.rstrip())
            continue
        f = line.split()
        try:
            r = dict(frame=int(f[1]), mode=f[2], target=float(f[3]), need=float(f[4]), period=float(f[5]),
                     enter=float(f[7]), sched=float(f[8]), woke=float(f[9]), started=float(f[10]), opened=float(f[11]),
                     ended=float(f[12]), submit=float(f[13]), present=float(f[14]), done=float(f[15]), cost=float(f[17]),
                     cpu=float(f[18]), gpu=float(f[19]), tape=int(f[21]), newest=int(f[22]), age=float(f[23]),
                     alpha=float(f[24]), stalled=int(f[25]), warm=int(f[26]) if len(f) > 26 else 0)
        except (IndexError, ValueError):
            continue
        if r["ended"] / 1000.0 < a.skip or r["ended"] / 1000.0 > a.until:
            continue
        frames.append(r)
    if not frames:
        print("no frames")
        return
    if a.dump:
        # frame mode | interval(present) sched gpuw begin cpu worker gpu | cost need period | tape newest stalled
        prev = None
        for r in frames:
            iv = r["present"] - prev["present"] if prev and r["present"] and prev["present"] else 0
            print(f"{r['ended'] / 1000:9.3f} f{r['frame']} {r['mode']} | {iv:6.2f} | {r['woke'] - r['enter']:6.2f} "
                  f"{r['started'] - r['woke']:6.2f} {r['opened'] - r['started']:5.2f} {r['ended'] - r['opened']:6.2f} "
                  f"{r['present'] - r['ended'] if r['present'] else 0:6.2f} {r['done'] - r['present'] if r['done'] else 0:6.2f} | "
                  f"{r['cost']:6.2f} {r['need']:6.2f} {r['period']:6.2f} | {r['tape']} {r['newest']} {r['stalled']} w{r['warm']}")
            prev = r
        return
    # consecutive runs by mode
    by_mode = {}
    for r in frames:
        by_mode.setdefault(r["mode"], []).append(r)
    span = (frames[-1]["ended"] - frames[0]["ended"]) / 1000.0
    print(f"{len(frames)} frames over {span:.1f} s (from {frames[0]['ended'] / 1000:.1f} s)")
    for mode, fr in by_mode.items():
        tgt = statistics.median(r["target"] for r in fr)
        print(f"-- mode {mode}: {len(fr)} frames, target {tgt:.3f} ms ({1000 / tgt if tgt else 0:.2f} fps)")
        for key, name in (("present", "Present"), ("done", "GPU done"), ("woke", "start")):
            iv = []
            over = []
            for p, q in zip(fr, fr[1:]):
                if q["frame"] != p["frame"] + 1 or not p[key] or not q[key]:
                    continue
                d = q[key] - p[key]
                iv.append(d)
                if tgt and d > 1.5 * tgt:
                    over.append((d, p, q))
            if not iv:
                continue
            tot = sum(iv)
            print(f"  {name:8s}: {len(iv) / tot * 1000:7.2f} fps, interval p1 {pct(iv, 0.01):.2f} p50 {pct(iv, 0.5):.2f} "
                  f"p99 {pct(iv, 0.99):.2f} p99.9 {pct(iv, 0.999):.2f} max {max(iv):.2f} ms, "
                  f">1.5x target: {len(over)}, sd {statistics.pstdev(iv):.3f} ms")
            if key == "present" and over:
                # Where the extra time went (the frame q's longest phase): sched = the schedule
                # (pacing chose a longer period), gpuw = waiting for the GPU's previous frame,
                # begin = aurora_begin_frame, cpu = the host building the frame, worker = render
                # worker until Present (pipeline compiles show here), gpu = GPU after Present.
                causes = {}
                for d, p, q in over:
                    ph = {"sched": q["woke"] - q["enter"], "gpuw": q["started"] - q["woke"],
                          "begin": q["opened"] - q["started"], "cpu": q["ended"] - q["opened"],
                          "worker": q["present"] - q["ended"]}
                    c = max(ph, key=ph.get)
                    big = "hitch" if d > 50 else "slow"
                    causes[(c, big)] = causes.get((c, big), 0) + 1
                print("    causes (longest phase, hitch = interval > 50 ms): " +
                      ", ".join(f"{c}/{b} {n}" for (c, b), n in sorted(causes.items(), key=lambda x: -x[1])))
                over.sort(key=lambda x: -x[0])
                print(f"    worst {min(a.worst, len(over))} (interval: q's phases: sched-wait gpu-wait begin cpu "
                      f"end->present present->done | cost need period | tape newest age stalled):")
                for d, p, q in over[:a.worst]:
                    print(f"    {q['ended'] / 1000:8.2f}s f{q['frame']} {d:6.2f} ms: {q['woke'] - q['enter']:.2f} "
                          f"{q['started'] - q['woke']:.2f} {q['opened'] - q['started']:.2f} {q['ended'] - q['opened']:.2f} "
                          f"{q['present'] - q['ended']:.2f} {q['done'] - q['present'] if q['done'] else 0:.2f} | "
                          f"{q['cost']:.2f} {q['need']:.2f} {q['period']:.2f} | {q['tape']} {q['newest']} {q['age']:.1f} "
                          f"{q['stalled']}")
        cost = [r["cost"] for r in fr]
        need = [r["need"] for r in fr]
        per = [r["period"] for r in fr]
        print(f"  cost p50 {pct(cost, 0.5):.2f} p90 {pct(cost, 0.9):.2f} max {max(cost):.2f}; cpu p50 "
              f"{pct([r['cpu'] for r in fr], 0.5):.2f}; gpu p50 {pct([r['gpu'] for r in fr], 0.5):.2f}; need p50 "
              f"{pct(need, 0.5):.2f} max {max(need):.2f}; period p50 {pct(per, 0.5):.2f} max {max(per):.2f}; "
              f"frames with need > target {sum(1 for r in fr if tgt and r['need'] > tgt * 1.02)}")
        ph = {k: [] for k in ("sched", "gpuw", "begin", "cpu", "worker", "gpu")}
        for r in fr:
            ph["sched"].append(r["woke"] - r["enter"])
            ph["gpuw"].append(r["started"] - r["woke"])
            ph["begin"].append(r["opened"] - r["started"])
            ph["cpu"].append(r["ended"] - r["opened"])
            if r["present"]:
                ph["worker"].append(r["present"] - r["ended"])
            if r["done"] and r["present"]:
                ph["gpu"].append(r["done"] - r["present"])
        print("  phases p50/p99 ms: " + ", ".join(f"{k} {pct(v, 0.5):.2f}/{pct(v, 0.99):.2f}" for k, v in ph.items() if v))
        st = sum(r["stalled"] for r in fr)
        print(f"  game-frame stalls (no newer game frame to show): {st}")


if __name__ == "__main__":
    main()
