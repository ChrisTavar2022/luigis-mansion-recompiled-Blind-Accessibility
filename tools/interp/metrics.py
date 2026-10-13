import sys, os, re, numpy as np
from PIL import Image
# metrics.py <rundir> [steps]: one summary of a LM_INTERP_TEST run (dump/ + log.txt)
run = sys.argv[1]; steps = int(sys.argv[2]) if len(sys.argv) > 2 else 3
d = os.path.join(run, "dump"); T = 40
L = lambda n: np.asarray(Image.open(f"{d}/{n}.ppm")).astype(np.int16)
ex = lambda n: os.path.exists(f"{d}/{n}.ppm")
def erode(x):
    for _ in range(2):
        y = x.copy(); y[1:, :] &= x[:-1, :]; y[:-1, :] &= x[1:, :]; y[:, 1:] &= x[:, :-1]; y[:, :-1] &= x[:, 1:]; x = y
    return x
frames = sorted({int(m.group(1)) for f in os.listdir(d) for m in [re.match(r"f(\d+)_fresh", f)] if m})
stale = []; snapS = []; snapE = []; worst = []; osc = []
for n in frames:
    if not ex(f"f{n}_s{steps}"): continue
    fresh = L(f"f{n}_fresh")
    st = int((np.abs(L(f"f{n}_s{steps}") - fresh).max(axis=2) > T).sum()); stale.append(st)
    if not ex(f"f{n-1}_fresh"): continue
    seq = [L(f"f{n-1}_fresh")] + [L(f"f{n}_s{s}") for s in range(1, steps)] + [fresh]
    diffs = [np.abs(seq[i + 1] - seq[i]).max(axis=2) > T for i in range(len(seq) - 1)]
    rest = np.zeros_like(diffs[0]); [rest.__ior__(x) for x in diffs[1:]]
    first = np.zeros_like(diffs[0]); [first.__ior__(x) for x in diffs[:-1]]
    a = int(erode(diffs[0] & ~rest).sum()); b = int(erode(diffs[-1] & ~first).sum())
    snapS.append(a); snapE.append(b)
    # back-and-forth within the interval (luminance goes up then down or vice versa by > 24)
    lum = [x.astype(np.float32).mean(axis=2) for x in seq]
    dl = [lum[i + 1] - lum[i] for i in range(len(lum) - 1)]
    o = np.zeros(lum[0].shape, bool)
    for i in range(len(dl)):
        for j in range(i + 1, len(dl)):
            o |= ((dl[i] > 24) & (dl[j] < -24)) | ((dl[i] < -24) & (dl[j] > 24))
    oc = int(erode(o).sum()); osc.append(oc)
    worst.append((a + b + st + oc, n, a, b, st, oc))
log = open(os.path.join(run, "log.txt"), encoding="utf-8", errors="replace").read()
kinds = ["pos", "nrm", "tex", "proj", "light", "verts"]
sn = {k: [] for k in kinds}; up = {k: [] for k in kinds + ["fadein", "fadeout", "snapin", "snapout", "colorswitch"]}
for m in re.finditer(r"_s1: snapped (.*?); unpaired (.*?)(\n| CUT)", log):
    for part, dst in ((m.group(1), sn), (m.group(2), up)):
        for k, v in re.findall(r"(\w+) (\d+)", part):
            if k in dst: dst[k].append(int(v))
avg = lambda v: sum(v) / len(v) if v else 0
print(f"frames {len(stale)}  stale px total {sum(stale)} (frames>0: {sum(1 for x in stale if x)})  snap blobs start {sum(snapS)} end {sum(snapE)}  back-and-forth {sum(osc)}")
print("per frame snapped: " + " ".join(f"{k} {avg(sn[k]):.1f}" for k in kinds) + "   unpaired: " + " ".join(f"{k} {avg(up[k]):.1f}" for k in kinds))
print(f"per frame fades (dithered): in {avg(up['fadein']):.1f} out {avg(up['fadeout']):.1f}; snapped (game-frame timing): in {avg(up['snapin']):.1f} out {avg(up['snapout']):.1f}; color switches {avg(up['colorswitch']):.1f}")
cl = re.findall(r"tape close ([0-9.]+) ms", log); pr = re.findall(r"present ([0-9.]+) ms", log)
if cl: print("tape close ms (last):", cl[-1])
worst.sort(reverse=True)
print("worst frames (sum, f, snapStart, snapEnd, stale, osc):", worst[:6])
