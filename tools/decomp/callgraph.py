# Usage: callgraph.py root_fn [max_depth] -> transitive callees (unported view) with sizes
import re, sys, os, glob
ASM = os.path.join(os.environ.get("LM_WORK", "."), "lm-decomp/build/GLME01/asm")
SYM = os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../decomp/config/GLME01/symbols.txt")
sizes = {}
for l in open(SYM):
    m = re.match(r"(\S+) = \.text:0x(\w+); // type:function size:0x(\w+)", l)
    if m: sizes[m[1]] = int(m[3], 16)
body = {}
for f in glob.glob(ASM + "/**/*.s", recursive=True):
    cur = None
    for l in open(f, errors="replace"):
        m = re.match(r"\.fn (\S+),", l)
        if m: cur = m[1]; body[cur] = set(); continue
        if l.startswith(".endfn"): cur = None; continue
        if cur:
            m = re.search(r"\bbl (\S+)", l)
            if m: body[cur].add(m[1])
            m = re.search(r"\b(?:lis|addi) r\d+, r?\d*,? ?(fn_\w+)@", l)
            if m: body[cur].add(m[1])
skip = re.compile(r"^(GX|OS|VI|DVD|PAD|PS|MTX|C_MTX|AR|CARD|DC|IC|__|memcpy|memset|str|sin|cos|sqrt|atan|ac|J[A-Z])")
root = sys.argv[1]; maxd = int(sys.argv[2]) if len(sys.argv) > 2 else 99
seen = {}; stack = [(root, 0)]
while stack:
    fn, d = stack.pop()
    if fn in seen or d > maxd: continue
    seen[fn] = d
    for c in body.get(fn, ()):
        if not skip.match(c): stack.append((c, d + 1))
tot = sum(sizes.get(f, 0) for f in seen)
print(f"{len(seen)} functions, {tot} bytes ({tot//1024} KiB) reachable from {root}")
if len(sys.argv) > 3:
    for f in sorted(seen, key=lambda f: seen[f]): print(seen[f], f, sizes.get(f, 0))
