"""Model-instance decisions per tape (LM_GROUP_LOG), for one instance or all.

    py tools/interp/group_check.py <group log> [--src LO-HI] [--group ID] [--runs]

interp.cpp writes, at the first in-between replay of every tape, one G line per model instance with
4 or more paired members: its members, outliers, erratic members (new / held), voters, members whose
place jumped, fast (turned > 90 degrees, cannot blend rigidly, flat), sheared flips, members without a
partner, the members' matrix source range and the policy (0 per matrix, 1 switched as b, 2 entrywise),
and one E line per erratic member. This prints, per instance (or the ones whose source range meets
--src, e.g. Luigi's S3D matrix pool), how often each policy and each cause occurred, and with --runs
every run of game frames in which an instance was not on its usual policy or had erratic, outlier or
unpaired members, with the reason (a vote that switched it, a fast turn, a sheared flip, members
held erratic, pairing lost).
"""
import argparse
import collections


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--src", default="", help="LO-HI (hex): instances whose member sources overlap")
    ap.add_argument("--group", default="", help="one instance id (hex)")
    ap.add_argument("--runs", action="store_true")
    ap.add_argument("--min-members", type=int, default=4)
    args = ap.parse_args()
    lo = hi = None
    if args.src:
        a, b = args.src.split("-")
        lo, hi = int(a, 16), int(b, 16)

    rows = collections.defaultdict(list)  # group -> [dict]
    erratic = collections.defaultdict(list)
    with open(args.log) as f:
        for line in f:
            t = line.split()
            if not t:
                continue
            if t[0] == "G" and len(t) >= 30:
                d = {"seq": int(t[1]), "frame": t[2], "group": t[3]}
                for k in range(4, len(t) - 1, 2):
                    if t[k] == "src":
                        s0, s1 = t[k + 1].split("-")
                        d["lo"], d["hi"] = int(s0, 16), int(s1, 16)
                    else:
                        try:
                            d[t[k]] = int(t[k + 1])
                        except ValueError:
                            pass
                rows[d["group"]].append(d)
            elif t[0] == "E" and len(t) >= 12:
                erratic[(t[2], int(t[1]))].append(line.strip())

    def wanted(g, rs):
        if args.group and g.lower() != args.group.lower():
            return False
        if lo is not None and not any(r["lo"] <= hi and r["hi"] >= lo for r in rs):
            return False
        return max(r["members"] for r in rs) >= args.min_members

    for g, rs in sorted(rows.items(), key=lambda x: -len(x[1])):
        if not wanted(g, rs):
            continue
        pol = collections.Counter(r["policy"] for r in rs)
        n = len(rs)
        cnt = lambda k: sum(1 for r in rs if r.get(k, 0) > 0)
        usual = pol.most_common(1)[0][0]
        print(f"instance {g}: {n} tapes, members {min(r['members'] for r in rs)}-{max(r['members'] for r in rs)}, "
              f"src {min(r['lo'] for r in rs):08X}-{max(r['hi'] for r in rs):08X}; policy " +
              ", ".join(f"{p}: {c}" for p, c in sorted(pol.items())) +
              f"; tapes with jumped {cnt('jumped')}, fast {cnt('fast')}, turn90 {cnt('turn90')}, skew {cnt('skew')}, "
              f"erratic {cnt('erratic')}, held {cnt('held')}, outliers {cnt('outliers')}, unpaired {cnt('unpaired')}")
        if not args.runs:
            continue
        rs.sort(key=lambda r: r["seq"])
        run = []

        def flush():
            if not run:
                return
            why = collections.Counter()
            for r in run:
                if r["policy"] == 1:
                    why[f"switched ({r['jumped']} of {r['voters']} voters jumped)"] += 1
                if r["policy"] == 2:
                    why["entrywise (" + ("sheared flip" if r["skew"] else "fast turn" if r["turn90"] else "fast") + ")"] += 1
                if r["erratic"] or r["held"]:
                    why[f"erratic members ({r['erratic']} new, {r['held']} held)"] += 1
                if r["outliers"]:
                    why[f"{r['outliers']} outliers"] += 1
                if r["unpaired"]:
                    why[f"{r['unpaired']} unpaired"] += 1
            print(f"  tapes {run[0]['seq']}-{run[-1]['seq']} ({run[0]['frame']}-{run[-1]['frame']}, {len(run)}): " +
                  "; ".join(f"{w} x{c}" for w, c in why.most_common()))
            for r in run:
                for e in erratic.get((g, r["seq"]), [])[:3]:
                    print("     " + e)

        for r in rs:
            odd = r["policy"] != usual or r["erratic"] or r["held"] or r["outliers"] or r["unpaired"]
            if odd and (not run or r["seq"] <= run[-1]["seq"] + 2):
                run.append(r)
            elif odd:
                flush()
                run = [r]
            elif run and r["seq"] > run[-1]["seq"] + 2:
                flush()
                run = []
        flush()


if __name__ == "__main__":
    main()
