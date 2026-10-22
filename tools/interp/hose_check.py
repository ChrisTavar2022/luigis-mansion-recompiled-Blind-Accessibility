"""Path check summary for Luigi, the Poltergust hose and its nozzle (docs/INTERPOLATION.md "The hose nozzle").

    hose_check.py <track log> [--top N] [--skip-tapes N] [--radius R]

Reads an LM_TRACK_LOG file (written with LM_JUDDER_LOG and LM_JUDDER_EVERY=1):
  HT seq f<frame> ok  luigi xyz  poltergust xyz  nozzle xyz     per game frame (actor places)
  HR present seq alpha judged J flagged F noz NJ NF max PX      per judged in-between replay
  H  present seq f<frame> alpha px .. w .. chord .. frac .. pol .. mm .. noz .. key .. grp .. site .. src .. ...
                                                               a vertex off the straight line between its
                                                               game-frame places (a jump)
  HC present seq f<frame> alpha reason                         a present shown without blending (cut, gap)

Prints: judged / flagged vertices (all near Luigi, near the nozzle), jump events (game frames with
at least one flagged vertex near the nozzle / anywhere), the classes (snapped to b: frac > 0.8, held
at a: frac < 0.2, other), the policy of the flagged ones, the call sites and model instances most
often involved, the worst events, the whole-picture presents (HC) and game-side jumps of the nozzle
actor (its place stepping far more than Luigi's in one game frame: the game's own, at 30 Hz too).
"""
import argparse
import collections
import math
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--top", type=int, default=12)
    ap.add_argument("--skip-tapes", type=int, default=0, help="ignore the first N tapes after the first judged one")
    a = ap.parse_args()

    tapes = {}
    judged = flagged = nozJ = nozF = replays = 0
    flags = []
    hc = []
    first_seq = None
    for line in open(a.log, encoding="utf-8", errors="replace"):
        p = line.split()
        if not p:
            continue
        try:
            if p[0] == "HT" and len(p) >= 13:
                tapes[int(p[1])] = (int(p[2][1:]), int(p[3]), [float(x) for x in p[4:13]])
            elif p[0] == "HR" and len(p) >= 12:
                seq = int(p[2])
                if first_seq is None:
                    first_seq = seq
                if seq < first_seq + a.skip_tapes:
                    continue
                replays += 1
                judged += int(p[5])
                flagged += int(p[7])
                nozJ += int(p[9])
                nozF += int(p[10])
            elif p[0] == "H" and len(p) > 20:
                d = {"present": int(p[1]), "seq": int(p[2]), "frame": int(p[3][1:]), "alpha": float(p[4])}
                i = 5
                while i + 1 < len(p):
                    k = p[i]
                    if k in ("at", "a", "b"):
                        d[k] = [float(x) for x in p[i + 1:i + 4]]
                        i += 4
                        continue
                    d[k] = p[i + 1]
                    i += 2
                if first_seq is not None and d["seq"] < first_seq + a.skip_tapes:
                    continue
                flags.append(d)
            elif p[0] == "HC" and len(p) >= 6:
                hc.append((int(p[1]), int(p[2]), p[3], float(p[4]), p[5]))
        except (ValueError, IndexError):
            continue  # a line cut off when the game was stopped

    print(f"replays judged {replays}, vertices near Luigi judged {judged}, off their path {flagged} "
          f"({100.0 * flagged / max(judged, 1):.4f}%); near the nozzle {nozJ} judged, {nozF} off")
    if not flags and not hc:
        print("no jumps")
    noz = [f for f in flags if f.get("noz") == "1"]
    for title, fl in (("near the nozzle", noz), ("all near Luigi", flags)):
        if not fl:
            continue
        ev = collections.defaultdict(list)
        for f in fl:
            ev[f["seq"]].append(f)
        cls = collections.Counter()
        pol = collections.Counter()
        for f in fl:
            fr = float(f.get("frac", "0"))
            cls["snapped to b" if fr > 0.8 else "held at a" if fr < 0.2 else "off the line"] += 1
            pol["policy " + f.get("pol", "?") + (" member" if f.get("mm") == "1" else "")] += 1
        print(f"\n== {title}: {len(fl)} flagged vertices in {len(ev)} game frames (jump events)")
        print("   classes: " + ", ".join(f"{k} {v}" for k, v in cls.most_common()))
        print("   policies: " + ", ".join(f"{k} {v}" for k, v in pol.most_common()))
        sites = collections.Counter((f.get("site"), f.get("grp")) for f in fl)
        print("   sites / instances: " + ", ".join(f"{s}/{g[-6:]} {n}" for (s, g), n in sites.most_common(8)))
        worst = sorted(ev.items(), key=lambda kv: -max(float(x["px"]) for x in kv[1]))[:a.top]
        for seq, l in worst:
            m = max(l, key=lambda x: float(x["px"]))
            print(f"   tape {seq} f{m['frame']}: {len(l)} vertices, worst {float(m['px']):.1f} px {float(m['w']):.1f} units "
                  f"(step {float(m['chord']):.1f}, frac {float(m['frac']):.2f}, alpha {m['alpha']:.2f}, pol {m.get('pol')}, "
                  f"site {m.get('site')}, src {m.get('src')})")
    if hc:
        reasons = collections.Counter(r for *_, r in hc)
        print(f"\nwhole-picture presents without blending: {len(hc)} (" + ", ".join(f"{k} {v}" for k, v in reasons.items()) + ")")
    # Game side: the nozzle actor's place per game frame against Luigi's.
    seqs = sorted(tapes)
    big = 0
    steps = []
    for s0, s1 in zip(seqs, seqs[1:]):
        if s1 != s0 + 1:
            continue
        f0, ok0, t0 = tapes[s0]
        f1, ok1, t1 = tapes[s1]
        if not (ok0 and ok1) or any(math.isnan(x) for x in t0[3:6] + t1[3:6]):
            continue
        ls = math.dist(t0[0:3], t1[0:3])
        ns = math.dist(t0[3:6], t1[3:6])
        steps.append(ns)
        if ns > 60 and ns > 3 * ls + 30:
            big += 1
            if big <= 5:
                print(f"game-side nozzle step: tape {s1} f{f1}: nozzle moved {ns:.0f}, Luigi {ls:.0f}")
    if steps:
        steps.sort()
        print(f"nozzle actor steps per game frame: median {steps[len(steps) // 2]:.1f}, p99 {steps[int(len(steps) * 0.99)]:.1f}, "
              f"max {steps[-1]:.1f}; game-side jumps {big}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
