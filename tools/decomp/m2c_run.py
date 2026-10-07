# m2c driver: decompile functions by name, fixing dtk jump tables (fn+0xOFF -> .L_ADDR labels).
import re, subprocess, sys, glob, os, tempfile
S = os.environ.get("LM_WORK", os.path.dirname(os.path.abspath(__file__)))
ASM = os.path.join(S, "lm-decomp/build/GLME01/asm")
SYM = os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../decomp/config/GLME01/symbols.txt")
addr = {}
for l in open(SYM):
    m = re.match(r"(\S+) = \.\w+:0x(\w+);", l)
    if m: addr[m[1]] = int(m[2], 16)
files = glob.glob(ASM + "/**/*.s", recursive=True)
def find(pattern):
    rx = re.compile(pattern, re.M)
    for f in files:
        with open(f, errors="replace") as fh:
            if rx.search(fh.read()): return f
for fn in sys.argv[1:]:
    src = find(r"^\.fn %s," % re.escape(fn))
    if not src:
        print(f"// ===== {fn}: not found"); continue
    text = open(src, errors="replace").read()
    body = text[text.index(f".fn {fn},"):]
    body = body[:body.index(f".endfn {fn}")]
    extra = []
    for jt in sorted(set(re.findall(r"jumptable_[0-9A-F]+", body))):
        df = find(r"^\.obj %s," % jt)
        d = open(df, errors="replace").read()
        start = d.index(f".obj {jt},"); end = d.index(f".endobj {jt}", start)
        chunk = d[start:end + len(f".endobj {jt}")]
        chunk = re.sub(r"\.4byte (\w+)\+0x([0-9A-Fa-f]+)",
                       lambda m: ".4byte .L_%08X" % (addr[m[1]] + int(m[2], 16)), chunk)
        chunk = re.sub(r"\.4byte (fn_\w+)$", lambda m: ".4byte .L_%08X" % addr[m[1]], chunk, flags=re.M)
        extra.append('.section .data\n' + chunk + "\n")
    tmp = tempfile.NamedTemporaryFile("w", suffix=".s", delete=False)
    tmp.write("\n".join(extra)); tmp.close()
    # Make sure every jump-table target has a label in (a copy of) the text file.
    targets = set(re.findall(r"\.4byte \.L_([0-9A-F]{8})", "".join(extra)))
    def add_label(m):
        a = m.group(1)
        return (f".L_{a}:\n" if a in targets and f".L_{a}:" not in text else "") + m.group(0)
    patched = re.sub(r"^/\* ([0-9A-F]{8}) ", add_label, text, flags=re.M)
    srctmp = tempfile.NamedTemporaryFile("w", suffix=".s", delete=False)
    srctmp.write(patched); srctmp.close()
    out = subprocess.run([sys.executable, os.path.join(S, "m2c/m2c.py"), "-t", "ppc-mwcc-c", "--no-cache",
                          "-f", fn, srctmp.name] + ([tmp.name] if extra else []), capture_output=True, text=True)
    os.unlink(tmp.name)
    os.unlink(srctmp.name)
    print(f"// ===== {fn} [{os.path.relpath(src, ASM)}]")
    for line in (out.stdout + out.stderr).splitlines():
        if line.endswith("/* extern */") or line.startswith("extern ") or line.endswith("/* static */"):
            continue
        print(line)
