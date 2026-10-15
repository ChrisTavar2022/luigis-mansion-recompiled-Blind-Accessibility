"""Text boxes at 120 (docs/INTERPOLATION.md): checks LM_TEXTBOX_LOG (+ LM_TEXTBOX_LOG_BLOCKS=1) per present.

 - box edges: every box's shown rectangle moves monotonically within a game frame, from where the
   previous game frame left it toward b's rectangle (no step back, no jump at the game frame);
 - block-to-box offsets: each block's layout position minus its box's shown top-left (divided by the
   box's scale), with the scrolling layer's -(1 - alpha) D removed, must be constant over the
   presents of one game frame (glyphs, icon, arrow never move against their box);
 - letter alpha: a letter new in a game frame has its alpha rising with the present alpha, and the
   first present of the next game frame shows it at least as opaque as the last one before.
usage: textbox_check.py tb.log [group id hex, default 80044138] [first gf] [last gf]
"""
import sys, collections
path = sys.argv[1]
gid = sys.argv[2] if len(sys.argv) > 2 else "80044138"
gf0 = int(sys.argv[3]) if len(sys.argv) > 3 else 0
gf1 = int(sys.argv[4]) if len(sys.argv) > 4 else 1 << 30
recs = []
pending = []
for line in open(path, encoding="utf-8", errors="replace"):
    if line.startswith("  B "):
        pending.append(line.split())
        continue
    if line.startswith("  V "):
        continue
    if not line.startswith("P "):
        continue
    t = line.split()
    blocks, pending = pending, []
    if t[7] != gid:
        continue
    seq, alpha, gf = int(t[1]), float(t[2]), int(t[5])
    if gf < gf0 or gf > gf1:
        continue
    panels = [None]
    for ptxt in line.split("|")[1:]:
        q = ptxt.split()
        panels.append(dict(b=list(map(float, q[1:5])), s=list(map(float, q[6:10]))))
    bl = []
    for b in blocks:
        own = b[3] == "own"; o = 1 if own else 0
        tk = int(b[3 + o][1:]); m = int(b[5 + o]); pn = int(b[7 + o])
        li = b.index("layout")
        bl.append(dict(kind=b[2], own=own, tk=tk, m=m, p=pn, lx=float(b[li + 1]), ly=float(b[li + 2]),
                       al=int(b[b.index("al") + 1]), alb=int(b[b.index("alb") + 1]), i=int(b[b.index("i") + 1]),
                       site=b[b.index("site") + 1]))
    recs.append(dict(seq=seq, alpha=alpha, gf=gf, panels=panels, blocks=bl))

byseq = collections.OrderedDict()
for r in recs:
    byseq.setdefault(r["seq"], []).append(r)
for rs in byseq.values():
    rs.sort(key=lambda r: r["alpha"])

box_bad = []; box_series = 0
off_worst = 0.0; off_where = None; off_series = 0
al_bad = []; al_checked = 0; cross_bad = []; cross_checked = 0
prev_last = None
jump_checked = [0]; jump_worst = [0.0, None]; text_series = [0, 0.0, None]
for seq, rs in byseq.items():
    npan = max(len(r["panels"]) for r in rs)
    for k in range(1, npan):
        if any(len(r["panels"]) <= k for r in rs):
            continue
        box_series += 1
        for c in range(4):
            vals = [r["panels"][k]["s"][c] for r in rs] + [rs[0]["panels"][k]["b"][c]]
            inc = all(vals[i] <= vals[i + 1] + 1e-3 for i in range(len(vals) - 1))
            dec = all(vals[i] >= vals[i + 1] - 1e-3 for i in range(len(vals) - 1))
            if not (inc or dec):
                box_bad.append((seq, rs[0]["gf"], k, c, vals))
    series = collections.defaultdict(list)
    for r in rs:
        for b in r["blocks"]:
            if b["kind"] == "box" or b["own"]:
                continue
            pnl = r["panels"][b["p"]] if b["p"] < len(r["panels"]) else None
            if pnl:
                sx = (pnl["s"][2] - pnl["s"][0]) / max(1e-6, pnl["b"][2] - pnl["b"][0])
                sy = (pnl["s"][3] - pnl["s"][1]) / max(1e-6, pnl["b"][3] - pnl["b"][1])
                ox, oy = (b["lx"] - pnl["s"][0]) / sx, (b["ly"] - pnl["s"][1]) / sy
            else:
                ox, oy = b["lx"], b["ly"]
            series[b["i"]].append((r["alpha"], ox, oy, b))
    for i, ser in series.items():
        if len(ser) < 2:
            continue
        a0, x0, y0, b0 = ser[0]; a1, x1, y1, _ = ser[-1]
        sxl = (x1 - x0) / (a1 - a0) if a1 > a0 else 0; syl = (y1 - y0) / (a1 - a0) if a1 > a0 else 0
        if b0["tk"] != 2 and b0["m"] >= 0:
            sxl = syl = 0  # blocks at their place: constant
        dev = max(max(abs(x - (x0 + sxl * (a - a0))), abs(y - (y0 + syl * (a - a0)))) for a, x, y, _ in ser)
        off_series += 1
        if b0["site"] == "801D69DC":
            text_series[0] += 1
            if dev > text_series[1]:
                text_series[1], text_series[2] = dev, (seq, rs[0]["gf"], i)
        if dev > off_worst:
            off_worst, off_where = dev, (seq, rs[0]["gf"], i, b0["tk"], b0["m"])
        if b0["m"] < 0 and b0["kind"] == "glyph" and b0["alb"] > 0:
            al_checked += 1
            als = [b["al"] for _, _, _, b in ser]
            if any(als[k] > als[k + 1] for k in range(len(als) - 1)):
                al_bad.append((seq, rs[0]["gf"], i, als))
    if prev_last is not None and prev_last["seq"] == seq - 1:
        # a block at its place in its box: the same offset at the last present of the game frame
        # before and the first of this one (no jump against the box at the game frame)
        def off(r, b):
            pnl = r["panels"][b["p"]] if b["p"] < len(r["panels"]) else None
            if not pnl:
                return b["lx"], b["ly"]
            sx = (pnl["s"][2] - pnl["s"][0]) / max(1e-6, pnl["b"][2] - pnl["b"][0])
            sy = (pnl["s"][3] - pnl["s"][1]) / max(1e-6, pnl["b"][3] - pnl["b"][1])
            return (b["lx"] - pnl["s"][0]) / sx, (b["ly"] - pnl["s"][1]) / sy
        prevb = {b["i"]: b for b in prev_last["blocks"]}
        for b in rs[0]["blocks"]:
            if b["kind"] == "box" or b["own"] or b["m"] not in prevb or prevb[b["m"]]["own"]:
                continue
            pb_ = prevb[b["m"]]
            o1 = off(prev_last, pb_); o2 = off(rs[0], b)
            # expected: unchanged, except blocks in the scrolling layer (both sides move linearly)
            if b["tk"] == 1 and pb_["tk"] != 2:
                jump_checked[0] += 1
                d = max(abs(o1[0] - o2[0]), abs(o1[1] - o2[1]))
                if d > jump_worst[0]:
                    jump_worst[0] = d; jump_worst[1] = (seq, rs[0]["gf"], b["i"])
        newprev = {b["i"]: b["al"] for b in prev_last["blocks"] if b["m"] < 0 and b["kind"] == "glyph" and b["alb"] > 0}
        for b in rs[0]["blocks"]:
            if b["m"] in newprev:
                cross_checked += 1
                if b["al"] < newprev[b["m"]]:
                    cross_bad.append((seq, rs[0]["gf"], b["i"], newprev[b["m"]], b["al"]))
    prev_last = rs[-1]

print(f"{path}: group {gid}, game frames {len(byseq)}, presents {len(recs)}")
print(f"  box edges monotonic within each game frame: {box_series - len(set((x[0], x[2]) for x in box_bad))} of {box_series} box/game-frame series", box_bad[:2])
print(f"  block-to-box offsets constant (scroll layer linear) within each game frame: {off_series} series, max deviation {off_worst:.4f} layout units at {off_where}")
print(f"  new letters: {al_checked} ramps, {len(al_bad)} not monotonic {al_bad[:2]}; next game frame at least as opaque: {cross_checked - len(cross_bad)} of {cross_checked} {cross_bad[:2]}")
print(f"  at the game frame changes, blocks at their place keep their offset to the box: {jump_checked[0]} checked, max change {jump_worst[0]:.4f} layout units at {jump_worst[1]}")
print(f"  of which text glyphs (JUTResFont quads, f32): {text_series[0]} series, max deviation {text_series[1]:.4f} at {text_series[2]}")
