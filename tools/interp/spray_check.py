"""Checks the element streams' drops and particle points present by present (LM_SPRAY_LOG).

    py tools/interp/spray_check.py <spray log> [--from N] [--until N] [--examples N] [--origin]

The log (interp.cpp replayPair, test aid LM_SPRAY_LOG=<file>) has one `P present seq alpha blend` line
per replay and one `<p|d> <how> <key> x y z [ox oy oz]` line per drawn point / drop: how it was shown
(b paired and blended, n without partner, s born moving along its step), its place in the world as
shown (a drop: the centre of its mesh) and, for drops, its model origin (--origin follows that).

Each key (the interpolator's identity) is followed over the presents. A present of tape `seq` at
`alpha` shows the interval from game frame seq - 1 to seq; an interval seen in two or more presents
gives the thing's motion in it (its velocity, per game frame, from the first and last present) and
where it is at the interval's start (alpha 0) and end (alpha 1), extrapolated along that motion.
Flagged (the game's own motion, kinks included, is never flagged):
  hold      it stands still for a whole interval (moves < 5% of its own median speed, which is at
            least 3 units per game frame): a thing shown at the game frame's place (no partner);
  gap       where an interval ends is not where the next one starts (more than 3 units + 25% of its
            speed): a snap or a wrong pair at the game frame;
  nonlinear inside one interval a present is off the straight line between the interval's ends by
            more than 1.5 units + 10% of its motion there (or goes back): a blend that is not linear.
Prints totals per kind (points, drops) and the worst examples.
"""
import argparse
import collections
import math


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--from", dest="first", type=int, default=0, help="first present")
    ap.add_argument("--until", type=int, default=1 << 60, help="last present")
    ap.add_argument("--examples", type=int, default=12)
    ap.add_argument("--origin", action="store_true", help="drops: follow the model origin, not the drawn centre")
    args = ap.parse_args()

    tracks = collections.defaultdict(lambda: collections.defaultdict(list))  # key -> seq -> [(alpha, pos, how, present)]
    present = None
    seq = 0
    alpha = 1.0
    hows = collections.Counter()
    presents = 0
    with open(args.log) as f:
        for line in f:
            t = line.split()
            if not t:
                continue
            if t[0] == "P" and len(t) >= 5:
                present = int(t[1])
                seq, alpha, blend = int(t[2]), float(t[3]), int(t[4])
                if not blend:
                    alpha = 1.0
                if args.first <= present <= args.until:
                    presents += 1
                continue
            if present is None or not (args.first <= present <= args.until) or len(t) < 6:
                continue
            kind, how, key = t[0], t[1], t[2]
            try:
                o = 6 if args.origin and kind == "d" and len(t) >= 9 else 3
                w = (float(t[o]), float(t[o + 1]), float(t[o + 2]))
            except ValueError:
                continue
            hows[(kind, how)] += 1
            tracks[(kind, key)][seq].append((alpha, w, how, present))

    flags = collections.Counter()
    judged = collections.Counter()
    examples = []
    for (kind, key), bySeq in tracks.items():
        ints = {}
        for sq, smp in bySeq.items():
            smp.sort()
            # (presents at alpha 1 of a tape: the game frame itself; one sample per alpha)
            uniq = []
            for s in smp:
                if not uniq or s[0] - uniq[-1][0] > 1e-3:
                    uniq.append(s)
            if len(uniq) < 2 or uniq[-1][0] - uniq[0][0] < 0.15:
                continue
            a0, p0 = uniq[0][0], uniq[0][1]
            a1, p1 = uniq[-1][0], uniq[-1][1]
            v = [(p1[c] - p0[c]) / (a1 - a0) for c in range(3)]
            start = [p0[c] - a0 * v[c] for c in range(3)]
            end = [p1[c] + (1.0 - a1) * v[c] for c in range(3)]
            ints[sq] = (uniq, v, start, end)
        if not ints:
            judged[(kind, "short")] += 1
            continue
        speeds = sorted(math.hypot(*i[1]) for i in ints.values())
        speed = speeds[len(speeds) // 2]
        for sq, (uniq, v, start, end) in ints.items():
            judged[(kind, "intervals")] += 1
            vl = math.hypot(*v)
            how = uniq[-1][2]
            if speed >= 3.0 and vl < 0.05 * speed:
                flags[(kind, "hold", how)] += 1
                examples.append((speed, kind, "hold", key, sq, uniq[0][3], f"still for the interval ({how}), speed {speed:.1f}", uniq[-1][1]))
            # inside: every sample on the line start + alpha v
            worst = 0.0
            for al, p, _, _ in uniq[1:-1]:
                e = math.dist(p, [start[c] + al * v[c] for c in range(3)])
                worst = max(worst, e)
            if worst > 1.5 + 0.1 * vl:
                flags[(kind, "nonlinear", how)] += 1
                examples.append((worst, kind, "nonlinear", key, sq, uniq[0][3], f"off the line by {worst:.1f}", uniq[-1][1]))
            nxt = ints.get(sq + 1)
            if nxt:
                judged[(kind, "joins")] += 1
                g = math.dist(end, nxt[2])
                if g > 3.0 + 0.25 * speed:
                    flags[(kind, "gap", nxt[0][-1][2])] += 1
                    examples.append((g, kind, "gap", key, sq + 1, nxt[0][0][3], f"starts {g:.1f} from where it ended, speed {speed:.1f}", nxt[0][0][1]))

    print(f"{presents} presents; shown: " + ", ".join(f"{k}{h} {n}" for (k, h), n in sorted(hows.items())))
    for kind in ("p", "d"):
        name = "points" if kind == "p" else "drops"
        n, nj = judged[(kind, "intervals")], judged[(kind, "joins")]
        fl = {w: sum(v for (k, ww, _), v in flags.items() if k == kind and ww == w) for w in ("hold", "nonlinear", "gap")}
        print(f"{name}: {n} intervals, {nj} game-frame joins judged ({judged[(kind, 'short')]} tracks too short); "
              f"hold {fl['hold']} ({100.0 * fl['hold'] / max(n, 1):.3f}%), nonlinear {fl['nonlinear']} ({100.0 * fl['nonlinear'] / max(n, 1):.3f}%), "
              f"gap {fl['gap']} ({100.0 * fl['gap'] / max(nj, 1):.3f}%)")
        by = collections.Counter({(w, h): v for (k, w, h), v in flags.items() if k == kind})
        if by:
            print("   by how it was shown: " + ", ".join(f"{w}/{h} {v}" for (w, h), v in sorted(by.items())))
    examples.sort(key=lambda e: -e[0])
    for size, kind, why, key, sq, pres, what, p in examples[: args.examples]:
        print(f"  {why:9s} {kind} {key} tape {sq} present {pres}: {what} at {p[0]:.0f} {p[1]:.0f} {p[2]:.0f}")


if __name__ == "__main__":
    main()
