# describe.py fn [fn...]: size, closure size, and strings referenced anywhere in its closure (depth-limited)
import re, sys, glob, os
S = os.environ.get("LM_WORK", os.path.dirname(os.path.abspath(__file__)))
ASM = os.path.join(S, "lm-decomp/build/GLME01/asm")
SYM = os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../decomp/config/GLME01/symbols.txt")
sizes = {}
for l in open(SYM):
    m = re.match(r"(\S+) = \.text:0x(\w+); // type:function size:0x(\w+)", l)
    if m: sizes[m[1]] = int(m[3], 16)
strings, calls, refs = {}, {}, {}
for f in glob.glob(ASM + "/**/*.s", recursive=True):
    cur = None; obj = None
    for l in open(f, errors="replace"):
        m = re.match(r"\.obj (\S+),", l)
        if m: obj = m[1]; continue
        if obj:
            m = re.match(r'\s*\.string "(.*)"', l)
            if m: strings[obj] = m[1]
            if l.startswith(".endobj"): obj = None
        m = re.match(r"\.fn (\S+),", l)
        if m: cur = m[1]; calls[cur] = set(); refs[cur] = set(); continue
        if l.startswith(".endfn"): cur = None; continue
        if cur:
            for c in re.findall(r"\bbl (\S+)", l): calls[cur].add(c)
            for r in re.findall(r"(lbl_[0-9A-F]+|\w+)@(?:ha|l|sda21)", l): refs[cur].add(r)
skip = re.compile(r"^(GX|OS|VI|DVD|PAD|PS|MTX|C_MTX|AR|CARD|DC|IC|__|mem|str|sin|cos|sqrt|atan|ac)")
def closure(root, maxd):
    seen = {}; st = [(root, 0)]
    while st:
        fn, d = st.pop()
        if fn in seen or d > maxd: continue
        seen[fn] = d
        for c in calls.get(fn, ()):
            if not skip.match(c): st.append((c, d + 1))
    return seen
depth = 2
for fn in sys.argv[1:]:
    full = closure(fn, 99); near = closure(fn, depth)
    tot = sum(sizes.get(f, 0) for f in full)
    strs = []
    for f in sorted(near, key=lambda f: near[f]):
        for r in sorted(refs.get(f, ())):
            if r in strings and strings[r] not in strs: strs.append(strings[r])
    print(f"{fn}: size {sizes.get(fn,0)}, closure {len(full)} fns / {tot//1024} KiB")
    for s in strs[:12]: print(f"    \"{s}\"")
