#!/usr/bin/env python3
"""Static recompiler: GameCube main.dol (PowerPC 750CL/Gekko) -> C++.

    python tools/recomp/recomp.py --dol <main.dol> --symbols <symbols.txt> --splits <splits.txt> --out <dir>

Runs at build time on the user's own executable; the output is never committed.
Function boundaries and jump tables come from the decomp's symbol map. Functions in
SDK translation units are emitted as calls into the HLE layer (src/recomp/hle_*.cpp)
instead of being translated.
"""
import argparse
import re
import struct
import sys
from pathlib import Path

# Translation units replaced by host (HLE) implementations.
HLE_TU_PREFIXES = ("os/", "gx/", "vi/", "pad/", "dvd/", "ar/", "card/", "ai/", "dsp/", "db/", "si/",
                   "exi/", "TRK_MINNOW_DOLPHIN/", "OdemuExi2/", "amcstubs/", "odenotstub/")
# SDK units that are plain computation on guest memory: recompiled like game code.
HLE_TU_EXCLUDE = ("os/OSAlloc.c", "os/OSArena.c", "pad/Padclamp.c")

FUNCS_PER_FILE = 400


# ---------------------------------------------------------------------------
# Input
# ---------------------------------------------------------------------------

class Dol:
    def __init__(self, data: bytes):
        self.data = data
        offs = struct.unpack(">18I", data[0:72])
        addrs = struct.unpack(">18I", data[72:144])
        sizes = struct.unpack(">18I", data[144:216])
        self.sections = [(a, o, s) for a, o, s in zip(addrs, offs, sizes) if s]
        self.bss_addr, self.bss_size, self.entry = struct.unpack(">III", data[0xD8:0xE4])

    def word(self, addr: int) -> int:
        for a, o, s in self.sections:
            if a <= addr < a + s:
                return struct.unpack_from(">I", self.data, o + addr - a)[0]
        raise KeyError(hex(addr))

    def has(self, addr: int) -> bool:
        return any(a <= addr < a + s for a, o, s in self.sections)


def load_symbols(path: Path):
    funcs = {}   # addr -> (name, size)
    objs = {}    # name -> (addr, size)
    labels = {}  # addr -> name (code labels, e.g. the _savefpr_N entry points)
    rx = re.compile(r"^(\S+) = \.(\w+):0x([0-9A-Fa-f]+); // type:(\w+)(?: size:0x([0-9A-Fa-f]+))?")
    for line in path.read_text().splitlines():
        m = rx.match(line)
        if not m:
            continue
        name, section, addr, typ, size = m.groups()
        addr = int(addr, 16)
        size = int(size, 16) if size else 0
        if typ == "function" and section in ("text", "init"):
            funcs[addr] = (name, size)
        elif typ == "object":
            objs[name] = (addr, size)
        elif typ == "label" and section in ("text", "init"):
            labels[addr] = name
    # A label inside a function is an alternate entry point: it becomes its own
    # function running to the end of the containing one.
    starts = sorted(funcs)
    import bisect
    for addr, name in labels.items():
        i = bisect.bisect_right(starts, addr) - 1
        if i >= 0 and addr != starts[i]:
            fa = starts[i]
            fend = fa + funcs[fa][1]
            if addr < fend:
                funcs[addr] = (name, fend - addr)
    return funcs, objs


def load_hle_ranges(path: Path):
    ranges = []
    cur = None
    for line in path.read_text().splitlines():
        if line and not line.startswith(("\t", " ")) and line.endswith(":") and line != "Sections:":
            cur = line[:-1]
        elif cur and cur.startswith(HLE_TU_PREFIXES) and cur not in HLE_TU_EXCLUDE:
            m = re.search(r"^\s+\.(?:text|init)\s+start:0x(\w+) end:0x(\w+)", line)
            if m:
                ranges.append((int(m.group(1), 16), int(m.group(2), 16), cur))
    return ranges


# ---------------------------------------------------------------------------
# Instruction translation
# ---------------------------------------------------------------------------

def bits(w, start, end):
    """IBM bit numbering (0 = MSB)."""
    return (w >> (31 - end)) & ((1 << (end - start + 1)) - 1)


def sext(v, n):
    return v - (1 << n) if v & (1 << (n - 1)) else v


def mask(mb, me):
    if mb <= me:
        m = ((0xFFFFFFFF >> mb) & (0xFFFFFFFF << (31 - me))) & 0xFFFFFFFF
    else:
        m = ((0xFFFFFFFF >> mb) | (0xFFFFFFFF << (31 - me))) & 0xFFFFFFFF
    return m


class Unsupported(Exception):
    pass


class FuncEmitter:
    def __init__(self, rc, addr, size, name):
        self.rc = rc
        self.start = addr
        self.end = addr + size
        self.name = name
        self.labels = set()
        self.lines = []

    # --- helpers -----------------------------------------------------------
    def r(self, n):
        return f"c.r[{n}]"

    def ra0(self, n):
        return "0u" if n == 0 else f"c.r[{n}]"

    def f(self, n):
        return f"c.f[{n}]"

    def emit(self, s):
        self.lines.append("    " + s)

    def rec(self, expr="c.r[{d}]"):
        return f"c.setCr0(s32({expr}));"

    def branch_cond(self, bo, bi):
        parts = []
        if not (bo & 0b00100):
            self.emit("c.ctr--;")
            parts.append("c.ctr == 0" if (bo & 0b00010) else "c.ctr != 0")
        if not (bo & 0b10000):
            test = f"c.crbit({bi})"
            parts.append(test if (bo & 0b01000) else f"!{test}")
        return " && ".join(parts) if parts else "true"

    def target_code(self, target, link, cond="true"):
        """Code for a branch to a static target."""
        rc = self.rc
        if link:
            call = rc.call_expr(target)
            self.emit(f"c.lr = 0x{self.pc + 4:08X}u;")
            return f"if ({cond}) {{ {call}; }}" if cond != "true" else f"{call};"
        if self.start <= target < self.end and target != self.start:
            self.labels.add(target)
            jump = f"goto L_{target:08X};"
            if target <= self.pc:  # loop back-edge: let pending interrupts run
                jump = f"{{ c.poll(); {jump} }}"
            return f"if ({cond}) {jump}" if cond != "true" else jump
        # Tail call (branch to another function, or to our own start).
        call = rc.call_expr(target)
        body = f"{{ {call}; return; }}"
        return f"if ({cond}) {body}" if cond != "true" else f"{call}; return;"

    # --- translation -------------------------------------------------------
    def translate(self):
        for pc in range(self.start, self.end, 4):
            self.pc = pc
            w = self.rc.dol.word(pc)
            self.lines.append(f"L_{pc:08X}: // {w:08X}")
            try:
                self.insn(w, pc)
            except Unsupported as e:
                self.emit(f"c.unimplemented(0x{pc:08X}u, 0x{w:08X}u); // {e}")
                self.rc.unsupported[str(e)] = self.rc.unsupported.get(str(e), 0) + 1
        # Execution that runs off the end continues into the next function.
        last = self.rc.dol.word(self.end - 4)
        op = bits(last, 0, 5)
        ends = (op == 18 and not (last & 1)) or (op == 19 and bits(last, 21, 30) in (16, 528)
                                                 and not (last & 1) and (bits(last, 6, 10) & 0b10100) == 0b10100)
        if not ends:
            self.lines.append(f"    {self.rc.call_expr(self.end)}; return;")
        used = self.labels
        out = []
        for line in self.lines:
            m = re.match(r"L_([0-9A-F]{8}): // ", line)
            if m:
                if int(m.group(1), 16) in used:
                    out.append(f"L_{m.group(1)}:;")
                continue
            out.append(line)
        return out

    def insn(self, w, pc):
        op = bits(w, 0, 5)
        d = bits(w, 6, 10)
        a = bits(w, 11, 15)
        b = bits(w, 16, 20)
        simm = sext(w & 0xFFFF, 16)
        uimm = w & 0xFFFF
        R, F, E = self.r, self.f, self.emit

        if op == 18:  # b / bl / ba / bla
            li = sext(w & 0x03FFFFFC, 26)
            aa = (w >> 1) & 1
            lk = w & 1
            target = (li if aa else pc + li) & 0xFFFFFFFF
            E(self.target_code(target, lk))
            return
        if op == 16:  # bc
            bo, bi = d, a
            bd = sext(w & 0xFFFC, 16)
            aa = (w >> 1) & 1
            lk = w & 1
            target = (bd if aa else pc + bd) & 0xFFFFFFFF
            cond = self.branch_cond(bo, bi)
            E(self.target_code(target, lk, cond))
            return
        if op == 19:
            xo = bits(w, 21, 30)
            lk = w & 1
            if xo == 16:  # bclr
                cond = self.branch_cond(d, a)
                if lk:
                    E("{ const u32 t = c.lr;" + f" c.lr = 0x{pc + 4:08X}u; if ({cond}) c.call(t); }}")
                else:
                    E(f"if ({cond}) return;" if cond != "true" else "return;")
                return
            if xo == 528:  # bcctr
                cond = self.branch_cond(d | 0b00100, a)
                if lk:
                    E(f"c.lr = 0x{pc + 4:08X}u; if ({cond}) c.call(c.ctr);")
                else:
                    table = self.rc.jump_tables.get(pc)
                    if table:
                        cases = []
                        for t in sorted(set(table)):
                            if self.start <= t < self.end:
                                self.labels.add(t)
                                cases.append(f"case 0x{t:08X}u: goto L_{t:08X};")
                        E(f"if ({cond}) {{ switch (c.ctr) {{ {' '.join(cases)} default: c.call(c.ctr); return; }} }}")
                    else:
                        E(f"if ({cond}) {{ c.call(c.ctr); return; }}")
                return
            crops = {257: "&", 449: "|", 193: "^", 225: "nand", 33: "nor", 289: "eqv", 129: "andc", 417: "orc"}
            if xo in crops:
                o = crops[xo]
                ea, eb = f"c.crbit({a})", f"c.crbit({b})"
                if o == "nand":
                    v = f"!({ea} && {eb})"
                elif o == "nor":
                    v = f"!({ea} || {eb})"
                elif o == "eqv":
                    v = f"({ea} == {eb})"
                elif o == "andc":
                    v = f"({ea} && !{eb})"
                elif o == "orc":
                    v = f"({ea} || !{eb})"
                else:
                    v = f"({ea} {o} {eb})"
                E(f"c.setCrbit({d}, {v});")
                return
            if xo == 0:  # mcrf
                E(f"c.cr[{d >> 2}] = c.cr[{a >> 2}];")
                return
            if xo in (150, 50):  # isync, rfi
                E("// isync/rfi")
                return
            raise Unsupported(f"op19 xo{xo}")

        # --- integer arithmetic with immediates ---
        if op == 14:  # addi / li
            E(f"{R(d)} = {self.ra0(a)} + 0x{simm & 0xFFFFFFFF:08X}u;")
            return
        if op == 15:  # addis / lis
            E(f"{R(d)} = {self.ra0(a)} + 0x{(simm << 16) & 0xFFFFFFFF:08X}u;")
            return
        if op in (12, 13):  # addic, addic.
            E(f"{{ const u32 s = {R(a)}; {R(d)} = s + 0x{simm & 0xFFFFFFFF:08X}u; c.ca = {R(d)} < s; }}")
            if op == 13:
                E(self.rec().format(d=d))
            return
        if op == 8:  # subfic
            E(f"{{ const u32 s = {R(a)}; {R(d)} = 0x{simm & 0xFFFFFFFF:08X}u - s; "
              f"c.ca = s <= 0x{simm & 0xFFFFFFFF:08X}u; }}")
            return
        if op == 7:  # mulli
            E(f"{R(d)} = u32(s32({R(a)}) * s32({simm}));")
            return
        if op == 10:  # cmpli
            E(f"c.cmpu({d >> 2}, {R(a)}, 0x{uimm:X}u);")
            return
        if op == 11:  # cmpi
            E(f"c.cmps({d >> 2}, s32({R(a)}), {simm});")
            return
        # logical immediates: d = rS, a = rA
        if op == 24:
            E(f"{R(a)} = {R(d)} | 0x{uimm:X}u;")
            return
        if op == 25:
            E(f"{R(a)} = {R(d)} | 0x{uimm << 16:X}u;")
            return
        if op == 26:
            E(f"{R(a)} = {R(d)} ^ 0x{uimm:X}u;")
            return
        if op == 27:
            E(f"{R(a)} = {R(d)} ^ 0x{uimm << 16:X}u;")
            return
        if op == 28:
            E(f"{R(a)} = {R(d)} & 0x{uimm:X}u;")
            E(self.rec().format(d=a))
            return
        if op == 29:
            E(f"{R(a)} = {R(d)} & 0x{uimm << 16:X}u;")
            E(self.rec().format(d=a))
            return
        # rotates
        if op in (21, 20, 23):
            sh_expr = str(b) if op != 23 else f"({R(b)} & 31)"
            mb, me = bits(w, 21, 25), bits(w, 26, 30)
            m = mask(mb, me)
            rot = f"c.rotl({R(d)}, {sh_expr})"
            if op == 20:  # rlwimi
                E(f"{R(a)} = ({rot} & 0x{m:08X}u) | ({R(a)} & 0x{(~m) & 0xFFFFFFFF:08X}u);")
            else:
                E(f"{R(a)} = {rot} & 0x{m:08X}u;")
            if w & 1:
                E(self.rec().format(d=a))
            return

        # --- loads / stores ---
        ea = f"({self.ra0(a)} + 0x{simm & 0xFFFFFFFF:08X}u)"
        upd_ea = f"({R(a)} + 0x{simm & 0xFFFFFFFF:08X}u)"
        loads = {32: ("rd32", ""), 34: ("rd8", ""), 40: ("rd16", ""), 42: ("rd16", "s16")}
        if op in loads:
            fn, cast = loads[op]
            val = f"u32({cast}({fn}({ea})))" if cast else f"{fn}({ea})"
            E(f"{R(d)} = {val};")
            return
        if op in (33, 35, 41, 43):  # update forms
            fn, cast = loads[op - 1]
            E(f"{{ const u32 e = {upd_ea}; {R(d)} = " + (f"u32({cast}({fn}(e)))" if cast else f"{fn}(e)") +
              f"; {R(a)} = e; }}")
            return
        stores = {36: "wr32", 38: "wr8", 44: "wr16"}
        if op in stores:
            E(f"{stores[op]}({ea}, {R(d)});")
            return
        if op in (37, 39, 45):
            E(f"{{ const u32 e = {upd_ea}; {stores[op - 1]}(e, {R(d)}); {R(a)} = e; }}")
            return
        if op == 46:  # lmw
            E(f"{{ u32 e = {ea}; for (int i = {d}; i < 32; i++, e += 4) c.r[i] = rd32(e); }}")
            return
        if op == 47:  # stmw
            E(f"{{ u32 e = {ea}; for (int i = {d}; i < 32; i++, e += 4) wr32(e, c.r[i]); }}")
            return
        if op == 48:  # lfs
            E(f"c.setSingle({d}, rdf32({ea}));")
            return
        if op == 49:
            E(f"{{ const u32 e = {upd_ea}; c.setSingle({d}, rdf32(e)); {R(a)} = e; }}")
            return
        if op == 50:  # lfd
            E(f"{F(d)} = rdf64({ea});")
            return
        if op == 51:
            E(f"{{ const u32 e = {upd_ea}; {F(d)} = rdf64(e); {R(a)} = e; }}")
            return
        if op == 52:  # stfs
            E(f"wrf32({ea}, float({F(d)}));")
            return
        if op == 53:
            E(f"{{ const u32 e = {upd_ea}; wrf32(e, float({F(d)})); {R(a)} = e; }}")
            return
        if op == 54:
            E(f"wrf64({ea}, {F(d)});")
            return
        if op == 55:
            E(f"{{ const u32 e = {upd_ea}; wrf64(e, {F(d)}); {R(a)} = e; }}")
            return
        if op in (56, 57, 60, 61):  # psq_l / psq_lu / psq_st / psq_stu
            wbit = bits(w, 16, 16)
            i = bits(w, 17, 19)
            off = sext(w & 0xFFF, 12)
            base = self.ra0(a) if op in (56, 60) else R(a)
            e = f"({base} + 0x{off & 0xFFFFFFFF:08X}u)"
            if op in (56, 57):
                E(f"{{ const u32 e = {e}; c.psqLoad({d}, e, {wbit}, {i});" + (f" {R(a)} = e;" if op == 57 else "") + " }")
            else:
                E(f"{{ const u32 e = {e}; c.psqStore({d}, e, {wbit}, {i});" + (f" {R(a)} = e;" if op == 61 else "") + " }")
            return

        if op == 31:
            return self.op31(w, pc, d, a, b)
        if op in (59, 63):
            return self.fp(w, op, d, a, b)
        if op == 4:
            return self.ps(w, d, a, b)
        if op == 3:  # twi
            E("// twi")
            return
        if op == 17:  # sc
            E("// sc")
            return
        raise Unsupported(f"op{op}")

    def op31(self, w, pc, d, a, b):
        R, E = self.r, self.emit
        xo10 = bits(w, 21, 30)
        xo9 = bits(w, 22, 30)
        rc = w & 1
        oe = bits(w, 21, 21)
        recd = lambda reg: E(self.rec().format(d=reg)) if rc else None

        if xo10 == 0:  # cmp
            E(f"c.cmps({d >> 2}, s32({R(a)}), s32({R(b)}));")
            return
        if xo10 == 32:  # cmpl
            E(f"c.cmpu({d >> 2}, {R(a)}, {R(b)});")
            return
        arith = {
            266: f"{R(a)} + {R(b)}",            # add
            40: f"{R(b)} - {R(a)}",             # subf
            235: f"u32(s32({R(a)}) * s32({R(b)}))",  # mullw
            104: f"u32(-s32({R(a)}))",          # neg
            75: f"u32((s64(s32({R(a)})) * s64(s32({R(b)}))) >> 32)",  # mulhw
            11: f"u32((u64({R(a)}) * u64({R(b)})) >> 32)",            # mulhwu
            491: f"c.divw({R(a)}, {R(b)})",     # divw
            459: f"c.divwu({R(a)}, {R(b)})",    # divwu
        }
        if xo9 in arith:
            E(f"{R(d)} = {arith[xo9]};")
            recd(d)
            return
        if xo9 == 10:  # addc
            E(f"{{ const u32 s = {R(a)}; {R(d)} = s + {R(b)}; c.ca = {R(d)} < s; }}")
            recd(d)
            return
        if xo9 == 138:  # adde
            E(f"{{ const u64 t = u64({R(a)}) + {R(b)} + c.ca; {R(d)} = u32(t); c.ca = t >> 32; }}")
            recd(d)
            return
        if xo9 == 202:  # addze
            E(f"{{ const u64 t = u64({R(a)}) + c.ca; {R(d)} = u32(t); c.ca = t >> 32; }}")
            recd(d)
            return
        if xo9 == 234:  # addme
            E(f"{{ const u64 t = u64({R(a)}) + c.ca + 0xFFFFFFFFull; {R(d)} = u32(t); c.ca = t >> 32; }}")
            recd(d)
            return
        if xo9 == 8:  # subfc
            E(f"{{ const u32 x = {R(a)}, y = {R(b)}; {R(d)} = y - x; c.ca = y >= x; }}")
            recd(d)
            return
        if xo9 == 136:  # subfe
            E(f"{{ const u64 t = u64(~{R(a)}) + {R(b)} + c.ca; {R(d)} = u32(t); c.ca = t >> 32; }}")
            recd(d)
            return
        if xo9 == 200:  # subfze
            E(f"{{ const u64 t = u64(~{R(a)}) + c.ca; {R(d)} = u32(t); c.ca = t >> 32; }}")
            recd(d)
            return
        if xo9 == 232:  # subfme
            E(f"{{ const u64 t = u64(~{R(a)}) + c.ca + 0xFFFFFFFFull; {R(d)} = u32(t); c.ca = t >> 32; }}")
            recd(d)
            return
        # logical: d = rS, a = rA
        logic = {28: "&", 444: "|", 316: "^", 60: "& ~", 412: "| ~"}
        if xo10 in logic:
            E(f"{R(a)} = {R(d)} {logic[xo10]}{R(b)};")
            recd(a)
            return
        if xo10 == 124:  # nor
            E(f"{R(a)} = ~({R(d)} | {R(b)});")
            recd(a)
            return
        if xo10 == 476:  # nand
            E(f"{R(a)} = ~({R(d)} & {R(b)});")
            recd(a)
            return
        if xo10 == 284:  # eqv
            E(f"{R(a)} = ~({R(d)} ^ {R(b)});")
            recd(a)
            return
        if xo10 == 24:  # slw
            E(f"{R(a)} = ({R(b)} & 0x20) ? 0u : ({R(d)} << ({R(b)} & 31));")
            recd(a)
            return
        if xo10 == 536:  # srw
            E(f"{R(a)} = ({R(b)} & 0x20) ? 0u : ({R(d)} >> ({R(b)} & 31));")
            recd(a)
            return
        if xo10 == 792:  # sraw
            E(f"c.sraw({a}, {R(d)}, {R(b)} & 0x3F);")
            recd(a)
            return
        if xo10 == 824:  # srawi
            E(f"c.sraw({a}, {R(d)}, {b});")
            recd(a)
            return
        if xo10 == 26:  # cntlzw
            E(f"{R(a)} = c.cntlzw({R(d)});")
            recd(a)
            return
        if xo10 == 922:  # extsh
            E(f"{R(a)} = u32(s32(s16({R(d)})));")
            recd(a)
            return
        if xo10 == 954:  # extsb
            E(f"{R(a)} = u32(s32(s8({R(d)})));")
            recd(a)
            return
        # indexed loads/stores
        ea = f"({self.ra0(a)} + {R(b)})"
        uea = f"({R(a)} + {R(b)})"
        lx = {23: ("rd32", ""), 87: ("rd8", ""), 279: ("rd16", ""), 343: ("rd16", "s16"),
              534: ("rd32r", ""), 790: ("rd16r", "")}
        if xo10 in lx:
            fn, cast = lx[xo10]
            E(f"{R(d)} = " + (f"u32({cast}({fn}({ea})))" if cast else f"{fn}({ea})") + ";")
            return
        lux = {55: ("rd32", ""), 119: ("rd8", ""), 311: ("rd16", ""), 375: ("rd16", "s16")}
        if xo10 in lux:
            fn, cast = lux[xo10]
            E(f"{{ const u32 e = {uea}; {R(d)} = " + (f"u32({cast}({fn}(e)))" if cast else f"{fn}(e)") +
              f"; {R(a)} = e; }}")
            return
        sx = {151: "wr32", 215: "wr8", 407: "wr16", 662: "wr32r", 918: "wr16r"}
        if xo10 in sx:
            E(f"{sx[xo10]}({ea}, {R(d)});")
            return
        sux = {183: "wr32", 247: "wr8", 439: "wr16"}
        if xo10 in sux:
            E(f"{{ const u32 e = {uea}; {sux[xo10]}(e, {R(d)}); {R(a)} = e; }}")
            return
        if xo10 == 535:  # lfsx
            E(f"c.setSingle({d}, rdf32({ea}));")
            return
        if xo10 == 567:
            E(f"{{ const u32 e = {uea}; c.setSingle({d}, rdf32(e)); {R(a)} = e; }}")
            return
        if xo10 == 599:
            E(f"c.f[{d}] = rdf64({ea});")
            return
        if xo10 == 631:
            E(f"{{ const u32 e = {uea}; c.f[{d}] = rdf64(e); {R(a)} = e; }}")
            return
        if xo10 == 663:
            E(f"wrf32({ea}, float(c.f[{d}]));")
            return
        if xo10 == 695:
            E(f"{{ const u32 e = {uea}; wrf32(e, float(c.f[{d}])); {R(a)} = e; }}")
            return
        if xo10 == 727:
            E(f"wrf64({ea}, c.f[{d}]);")
            return
        if xo10 == 759:
            E(f"{{ const u32 e = {uea}; wrf64(e, c.f[{d}]); {R(a)} = e; }}")
            return
        if xo10 == 983:  # stfiwx
            E(f"wr32({ea}, u32(std::bit_cast<u64>(c.f[{d}])));")
            return
        # psq_lx / psq_stx
        if bits(w, 25, 30) in (6, 7, 38, 39):
            sub = bits(w, 25, 30)
            wbit = bits(w, 21, 21)
            i = bits(w, 22, 24)
            e = ea if sub in (6, 7) else uea
            if sub in (6, 38):
                E(f"{{ const u32 e = {e}; c.psqLoad({d}, e, {wbit}, {i});" + (f" {R(a)} = e;" if sub == 38 else "") + " }")
            else:
                E(f"{{ const u32 e = {e}; c.psqStore({d}, e, {wbit}, {i});" + (f" {R(a)} = e;" if sub == 39 else "") + " }")
            return
        # special registers
        spr = bits(w, 16, 20) << 5 | bits(w, 11, 15)
        if xo10 == 339:  # mfspr
            if spr == 8:
                E(f"{R(d)} = c.lr;")
            elif spr == 9:
                E(f"{R(d)} = c.ctr;")
            elif spr == 1:
                E(f"{R(d)} = c.getXer();")
            elif 912 <= spr <= 919:
                E(f"{R(d)} = c.gqr[{spr - 912}];")
            else:
                E(f"{R(d)} = c.mfspr({spr});")
            return
        if xo10 == 467:  # mtspr
            if spr == 8:
                E(f"c.lr = {R(d)};")
            elif spr == 9:
                E(f"c.ctr = {R(d)};")
            elif spr == 1:
                E(f"c.setXer({R(d)});")
            elif 912 <= spr <= 919:
                E(f"c.gqr[{spr - 912}] = {R(d)};")
            else:
                E(f"c.mtspr({spr}, {R(d)});")
            return
        if xo10 == 19:  # mfcr
            E(f"{R(d)} = c.getCr();")
            return
        if xo10 == 144:  # mtcrf
            crm = bits(w, 12, 19)
            E(f"c.mtcrf(0x{crm:02X}, {R(d)});")
            return
        if xo10 == 371:  # mftb
            E(f"{R(d)} = c.mftb({spr});")
            return
        if xo10 in (83, 146, 595, 210, 659, 242):  # mfmsr, mtmsr, mfsr, mtsr...
            if xo10 == 83:
                E(f"{R(d)} = c.msr;")
            elif xo10 == 146:
                E(f"c.msr = {R(d)};")
            else:
                E("// sr access")
            return
        if xo10 in (86, 54, 470, 278, 246, 982, 598, 854, 1014, 758):  # cache ops, sync, eieio
            if xo10 == 1014:  # dcbz
                E(f"c.dcbz({ea});")
            else:
                E("// cache/sync")
            return
        if xo10 == 4:  # tw
            E("// tw")
            return
        if xo10 == 512:  # mcrxr
            E(f"c.cr[{d >> 2}] = u8((c.so ? 8 : 0) | (c.ov ? 4 : 0) | (c.ca ? 2 : 0)); c.ca = 0;")
            return
        raise Unsupported(f"op31 xo{xo10}")

    def fp(self, w, op, d, a, b):
        E = self.emit
        cc = bits(w, 21, 25)
        xo5 = bits(w, 26, 30)
        xo10 = bits(w, 21, 30)
        rc = w & 1
        single = op == 59
        A, B, C, D = f"c.f[{a}]", f"c.f[{b}]", f"c.f[{cc}]", d

        def res(expr):
            if single:
                E(f"c.setSingle({D}, float({expr}));")
            else:
                E(f"c.f[{D}] = {expr};")
            if rc:
                E("c.setCr1();")

        ops5 = {21: f"{A} + {B}", 20: f"{A} - {B}", 25: f"{A} * {C}", 18: f"{A} / {B}",
                29: f"std::fma({A}, {C}, {B})", 28: f"std::fma({A}, {C}, -{B})",
                31: f"-std::fma({A}, {C}, {B})", 30: f"-std::fma({A}, {C}, -{B})",
                24: f"1.0 / {B}", 26: f"1.0 / std::sqrt({B})", 22: f"std::sqrt({B})"}
        if xo5 in ops5 and (single or xo5 != 24):
            res(ops5[xo5])
            return
        if xo5 == 23:  # fsel
            res(f"({A} >= 0.0) ? {C} : {B}")
            return
        if op == 63:
            if xo10 == 0:
                E(f"c.fcmp({d >> 2}, {A}, {B}, false);")
                return
            if xo10 == 32:
                E(f"c.fcmp({d >> 2}, {A}, {B}, true);")
                return
            simple = {72: B, 40: f"-{B}", 264: f"std::fabs({B})", 136: f"-std::fabs({B})"}
            if xo10 in simple:
                E(f"c.f[{D}] = {simple[xo10]};")
                if rc:
                    E("c.setCr1();")
                return
            if xo10 == 12:  # frsp
                E(f"c.setSingle({D}, float({B}));")
                return
            if xo10 in (14, 15):  # fctiw / fctiwz
                E(f"c.f[{D}] = c.fctiw({B}, {1 if xo10 == 15 else 0});")
                return
            if xo10 == 583:  # mffs
                E(f"c.f[{D}] = std::bit_cast<double>(u64(c.fpscr));")
                return
            if xo10 == 711:  # mtfsf
                E(f"c.fpscr = u32(std::bit_cast<u64>({B}));")
                return
            if xo10 in (70, 38, 134, 64):  # mtfsb0/1, mtfsfi, mcrfs
                E("// fpscr bit op")
                return
        raise Unsupported(f"fp op{op} xo{xo10}")

    def ps(self, w, d, a, b):
        E = self.emit
        c_ = bits(w, 21, 25)
        xo5 = bits(w, 26, 30)
        xo10 = bits(w, 21, 30)
        A0, A1 = f"c.f[{a}]", f"c.ps1[{a}]"
        B0, B1 = f"c.f[{b}]", f"c.ps1[{b}]"
        C0, C1 = f"c.f[{c_}]", f"c.ps1[{c_}]"

        def pair(e0, e1):
            E(f"{{ const double v0 = double(float({e0})), v1 = double(float({e1})); c.f[{d}] = v0; c.ps1[{d}] = v1; }}")

        ops = {
            21: (f"{A0} + {B0}", f"{A1} + {B1}"),
            20: (f"{A0} - {B0}", f"{A1} - {B1}"),
            25: (f"{A0} * {C0}", f"{A1} * {C1}"),
            18: (f"{A0} / {B0}", f"{A1} / {B1}"),
            29: (f"std::fma({A0}, {C0}, {B0})", f"std::fma({A1}, {C1}, {B1})"),
            28: (f"std::fma({A0}, {C0}, -{B0})", f"std::fma({A1}, {C1}, -{B1})"),
            31: (f"-std::fma({A0}, {C0}, {B0})", f"-std::fma({A1}, {C1}, {B1})"),
            30: (f"-std::fma({A0}, {C0}, -{B0})", f"-std::fma({A1}, {C1}, -{B1})"),
            12: (f"{A0} * {C0}", f"{A1} * {C0}"),  # ps_muls0
            13: (f"{A0} * {C1}", f"{A1} * {C1}"),  # ps_muls1
            14: (f"std::fma({A0}, {C0}, {B0})", f"std::fma({A1}, {C0}, {B1})"),  # ps_madds0
            15: (f"std::fma({A0}, {C1}, {B0})", f"std::fma({A1}, {C1}, {B1})"),  # ps_madds1
            10: (f"{A0} + {B1}", f"{C1}"),  # ps_sum0
            11: (f"{C0}", f"{A0} + {B1}"),  # ps_sum1
            24: (f"1.0 / {B0}", f"1.0 / {B1}"),  # ps_res
            26: (f"1.0 / std::sqrt({B0})", f"1.0 / std::sqrt({B1})"),  # ps_rsqrte
            23: (f"({A0} >= 0.0) ? {C0} : {B0}", f"({A1} >= 0.0) ? {C1} : {B1}"),  # ps_sel
        }
        if xo5 in ops:
            pair(*ops[xo5])
            if w & 1:
                E("c.setCr1();")
            return
        moves = {
            40: (f"-{B0}", f"-{B1}"),        # ps_neg
            72: (B0, B1),                    # ps_mr
            136: (f"-std::fabs({B0})", f"-std::fabs({B1})"),  # ps_nabs
            264: (f"std::fabs({B0})", f"std::fabs({B1})"),    # ps_abs
            528: (A0, B0),                   # ps_merge00
            560: (A0, B1),                   # ps_merge01
            592: (A1, B0),                   # ps_merge10
            624: (A1, B1),                   # ps_merge11
        }
        if xo10 in moves:
            e0, e1 = moves[xo10]
            E(f"{{ const double v0 = {e0}, v1 = {e1}; c.f[{d}] = v0; c.ps1[{d}] = v1; }}")
            return
        if xo10 in (0, 32, 64, 96):  # ps_cmpu0, ps_cmpo0, ps_cmpu1, ps_cmpo1
            lhs, rhs = (A0, B0) if xo10 in (0, 32) else (A1, B1)
            E(f"c.fcmp({d >> 2}, {lhs}, {rhs}, {str(xo10 in (32, 96)).lower()});")
            return
        if xo10 == 1014:  # dcbz_l
            E(f"c.dcbz(c.r[{a}] + c.r[{b}]);")
            return
        sub6 = bits(w, 25, 30)
        if sub6 in (6, 7, 38, 39):  # psq_lx etc. encoded under op4
            wbit = bits(w, 21, 21)
            i = bits(w, 22, 24)
            e = f"({self.ra0(a)} + c.r[{b}])" if sub6 in (6, 7) else f"(c.r[{a}] + c.r[{b}])"
            if sub6 in (6, 38):
                E(f"{{ const u32 e = {e}; c.psqLoad({d}, e, {wbit}, {i});" + (f" c.r[{a}] = e;" if sub6 == 38 else "") + " }")
            else:
                E(f"{{ const u32 e = {e}; c.psqStore({d}, e, {wbit}, {i});" + (f" c.r[{a}] = e;" if sub6 == 39 else "") + " }")
            return
        raise Unsupported(f"ps xo{xo10}/{xo5}")


class Recompiler:
    def __init__(self, dol, funcs, objs, hle_ranges):
        self.dol = dol
        self.funcs = funcs
        self.hle = {}
        for addr, (name, size) in funcs.items():
            for lo, hi, tu in hle_ranges:
                if lo <= addr < hi:
                    self.hle[addr] = (name, tu)
        while self.add_interior_entries():
            pass
        self.unsupported = {}
        self.jump_tables = self.find_jump_tables(objs)

    def add_interior_entries(self):
        """Branches from one function into the middle of another make the target an
        extra entry point (a function running to the end of the containing one)."""
        import bisect
        starts = sorted(self.funcs)
        extra = {}
        for faddr, (name, size) in list(self.funcs.items()):
            if faddr in self.hle:
                continue
            for pc in range(faddr, faddr + size, 4):
                w = self.dol.word(pc)
                op = bits(w, 0, 5)
                if op == 18:
                    t = (pc + sext(w & 0x03FFFFFC, 26)) & 0xFFFFFFFF if not (w & 2) else sext(w & 0x03FFFFFC, 26) & 0xFFFFFFFF
                elif op == 16:
                    t = (pc + sext(w & 0xFFFC, 16)) & 0xFFFFFFFF
                else:
                    continue
                if faddr <= t < faddr + size or t in self.funcs:
                    continue
                i = bisect.bisect_right(starts, t) - 1
                if i < 0:
                    continue
                ca = starts[i]
                cend = ca + self.funcs[ca][1]
                if t < cend and ca not in self.hle:
                    extra[t] = (f"{self.funcs[ca][0]}_entry_{t:08X}", cend - t)
        self.funcs.update(extra)
        return bool(extra)

    def find_jump_tables(self, objs):
        """Map each bctr address to its jump-table targets (table found from the lis/addi
        that loads its base earlier in the same function)."""
        tables = {}
        by_addr = {addr: size for name, (addr, size) in objs.items() if name.startswith("jumptable_")}
        for faddr, (name, size) in self.funcs.items():
            if faddr in self.hle:
                continue
            hi = {}
            pending = None
            for pc in range(faddr, faddr + size, 4):
                w = self.dol.word(pc)
                op = bits(w, 0, 5)
                if op == 15 and bits(w, 11, 15) == 0:  # lis rD, hi
                    hi[bits(w, 6, 10)] = (w & 0xFFFF) << 16
                elif op == 14 and bits(w, 11, 15) in hi:  # addi rD, rA, lo
                    base = (hi[bits(w, 11, 15)] + sext(w & 0xFFFF, 16)) & 0xFFFFFFFF
                    if base in by_addr:
                        pending = base
                elif op == 19 and bits(w, 21, 30) == 528 and not (w & 1) and pending is not None:
                    n = by_addr[pending] // 4
                    tables[pc] = [self.dol.word(pending + i * 4) for i in range(n)]
                    pending = None
        return tables

    def fname(self, addr):
        return f"f_{addr:08X}"

    def call_expr(self, target):
        if target in self.funcs:
            return f"{self.fname(target)}(c)"
        return f"c.call(0x{target:08X}u)"

    def run(self, out: Path):
        out.mkdir(parents=True, exist_ok=True)
        addrs = sorted(self.funcs)
        header = ["#pragma once", "#include \"recomp/ctx.h\"", "namespace recomp {"]
        header += [f"void {self.fname(a)}(Ctx& c);" for a in addrs]
        header += ["}"]
        (out / "funcs.h").write_text("\n".join(header) + "\n")

        files = []
        for i in range(0, len(addrs), FUNCS_PER_FILE):
            chunk = addrs[i:i + FUNCS_PER_FILE]
            lines = ["// Generated from the user's main.dol by tools/recomp/recomp.py. Do not commit.",
                     "#include \"funcs.h\"", "#include \"recomp/hle.h\"", "#include <bit>", "#include <cmath>",
                     "namespace recomp {"]
            for a in chunk:
                name, size = self.funcs[a]
                lines.append(f"// {name}")
                lines.append(f"void {self.fname(a)}(Ctx& c) {{")
                if a in self.hle:
                    lines.append(f"    static const HleFn s = hleLookup(\"{name}\");")
                    lines.append(f"    if (s) {{ s(c); }} else {{ hleMissing(\"{name}\", 0x{a:08X}u, c); }}")
                else:
                    lines += FuncEmitter(self, a, size, name).translate()
                lines.append("}")
            name = f"funcs_{i // FUNCS_PER_FILE:03d}.cpp"
            (out / name).write_text("\n".join(lines) + "\n}\n")
            files.append(name)

        table = ["#include \"funcs.h\"", "namespace recomp {",
                 "extern const FuncEntry kFuncTable[] = {"]
        table += [f"    {{0x{a:08X}u, &{self.fname(a)}}}," for a in addrs]
        table += ["};", f"extern const unsigned kFuncTableSize = {len(addrs)};"]
        table += ["extern const SymEntry kSymTable[] = {"]
        table += [f'    {{"{self.funcs[a][0]}", 0x{a:08X}u}},' for a in addrs]
        table += ["};", f"extern const unsigned kSymTableSize = {len(addrs)};", "}"]
        (out / "func_table.cpp").write_text("\n".join(table) + "\n")
        (out / "files.txt").write_text("\n".join(files + ["func_table.cpp"]) + "\n")
        return files


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dol", required=True)
    ap.add_argument("--symbols", required=True)
    ap.add_argument("--splits", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    dol = Dol(Path(a.dol).read_bytes())
    funcs, objs = load_symbols(Path(a.symbols))
    funcs = {k: v for k, v in funcs.items() if v[1] > 0 and dol.has(k)}
    rc = Recompiler(dol, funcs, objs, load_hle_ranges(Path(a.splits)))
    files = rc.run(Path(a.out))
    print(f"recomp: {len(funcs)} functions ({len(rc.hle)} HLE) -> {len(files)} files; "
          f"{len(rc.jump_tables)} jump tables")
    for k, v in sorted(rc.unsupported.items(), key=lambda kv: -kv[1]):
        print(f"  unsupported {k}: {v}")


if __name__ == "__main__":
    main()
