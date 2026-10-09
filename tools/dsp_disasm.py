#!/usr/bin/env python3
"""Disassembler for the GameCube audio DSP, used to read LM's JAudio microcode
(the `jdsp` image the game uploads; docs/AUDIO.md explains what it does).

    python tools/dsp_disasm.py <main.dol> [address] [size]   # default: jdsp (GLME01 / GLMP01)
    python tools/dsp_disasm.py --raw <image.bin>

Written for this port from the publicly documented DSP instruction set. Operand
names follow the usual convention ($ac0.m, $ax1.h, ...); a few rarely used
encodings may print as ??xxxx. Extended (parallel) ops are shown after " : ".
"""
import struct
import sys

R = ['ar0','ar1','ar2','ar3','ix0','ix1','ix2','ix3','wr0','wr1','wr2','wr3','st0','st1','st2','st3',
     'ac0.h','ac1.h','config','sr','prod.l','prod.m1','prod.h','prod.m2',
     'ax0.l','ax1.l','ax0.h','ax1.h','ac0.l','ac1.l','ac0.m','ac1.m']
CC = ['GE','L','G','LE','NZ','Z','NC','C','x8','x9','xA','xB','LNZ','LZ','O','']

def ext(e, is3):
    if is3: e &= 0x7F
    if e == 0: return ''
    if (e & 0xFC) == 0x04: return "DR $ar%d" % (e & 3)
    if (e & 0xFC) == 0x08: return "IR $ar%d" % (e & 3)
    if (e & 0xFC) == 0x0C: return "NR $ar%d" % (e & 3)
    if (e & 0xF0) == 0x10: return "MV $%s, $%s" % (R[0x18 + ((e >> 2) & 3)], R[0x1c + (e & 3)])
    if (e & 0xE4) == 0x20: return "S @$ar%d, $%s" % (e & 3, R[0x1c + ((e >> 3) & 3)])
    if (e & 0xE4) == 0x24: return "SN @$ar%d, $%s" % (e & 3, R[0x1c + ((e >> 3) & 3)])
    if (e & 0xC4) == 0x40: return "L $%s, @$ar%d" % (R[0x18 + ((e >> 3) & 7)], e & 3)
    if (e & 0xC4) == 0x44: return "LN $%s, @$ar%d" % (R[0x18 + ((e >> 3) & 7)], e & 3)
    if (e & 0xC0) == 0x80:
        d = R[0x18 + ((e >> 4) & 3)]; s = 'ac%d.m' % (e & 1)
        names = ['LS', 'SL', 'LSN', 'SLN', 'LSM', 'SLM', 'LSNM', 'SLNM']
        k = (e >> 1) & 7
        if k & 1: return "%s $%s, $%s" % (names[k], s, d)
        return "%s $%s, $%s" % (names[k], d, s)
    if (e & 0xC0) == 0xC0:
        if (e & 3) == 3:
            names = ['LDAX', 'LDAXN', 'LDAXM', 'LDAXNM']
            return "%s $ax%d, @$ar%d" % (names[(e >> 2) & 3], (e >> 4) & 1, (e >> 5) & 1)
        names = ['LD', 'LDN', 'LDM', 'LDNM']
        return "%s $ax0.%s, $ax1.%s, @$ar%d" % (names[(e >> 2) & 3], 'hl'[(e >> 5) & 1 ^ 1] if False else ('h' if (e >> 5) & 1 else 'l'), 'h' if (e >> 4) & 1 else 'l', e & 3)
    return "ext?%02x" % e

def dis(w, nxt):
    """returns (text, size, has_ext, ext_is3)"""
    t = w >> 12
    if t == 0:
        if w == 0: return 'NOP', 1
        if (w & 0xFFFC) == 0x0004: return 'DAR $ar%d' % (w & 3), 1
        if (w & 0xFFFC) == 0x0008: return 'IAR $ar%d' % (w & 3), 1
        if (w & 0xFFFC) == 0x000C: return 'SUBARN $ar%d' % (w & 3), 1
        if (w & 0xFFF0) == 0x0010: return 'ADDARN $ar%d, $ix%d' % (w & 3, (w >> 2) & 3), 1
        if w == 0x0021: return 'HALT', 1
        if (w & 0xFFE0) == 0x0040: return 'LOOP $%s' % R[w & 31], 1
        if (w & 0xFFE0) == 0x0060: return 'BLOOP $%s, 0x%04x' % (R[w & 31], nxt), 2
        if (w & 0xFFE0) == 0x0080: return 'LRI $%s, #0x%04x' % (R[w & 31], nxt), 2
        if (w & 0xFFE0) == 0x00C0: return 'LR $%s, @0x%04x' % (R[w & 31], nxt), 2
        if (w & 0xFFE0) == 0x00E0: return 'SR @0x%04x, $%s' % (nxt, R[w & 31]), 2
        if (w & 0xFEFC) == 0x0210: return 'ILRR $ac%d.m, @$ar%d' % ((w >> 8) & 1, w & 3), 1
        if (w & 0xFEFC) == 0x0214: return 'ILRRD $ac%d.m, @$ar%d' % ((w >> 8) & 1, w & 3), 1
        if (w & 0xFEFC) == 0x0218: return 'ILRRI $ac%d.m, @$ar%d' % ((w >> 8) & 1, w & 3), 1
        if (w & 0xFEFC) == 0x021C: return 'ILRRN $ac%d.m, @$ar%d' % ((w >> 8) & 1, w & 3), 1
        if (w & 0xFFF0) == 0x0270: return 'IF%s' % CC[w & 15], 1
        if (w & 0xFFF0) == 0x0290: return 'J%s 0x%04x' % (CC[w & 15] or 'MP', nxt), 2
        if (w & 0xFFF0) == 0x02B0: return 'CALL%s 0x%04x' % (CC[w & 15], nxt), 2
        if (w & 0xFFF0) == 0x02D0: return 'RET%s' % CC[w & 15], 1
        if w == 0x02FF: return 'RTI', 1
        ops = {0x00: 'ADDI', 0x20: 'XORI', 0x40: 'ANDI', 0x60: 'ORI', 0x80: 'CMPI', 0xA0: 'ANDF', 0xC0: 'ANDCF'}
        if (w & 0xFE1F) == 0x0200 and (w & 0xE0) in ops:
            return '%s $ac%d.m, #0x%04x' % (ops[w & 0xE0], (w >> 8) & 1, nxt), 2
        if (w & 0xFE00) == 0x0400: return 'ADDIS $ac%d.m, #%d' % ((w >> 8) & 1, struct.unpack('b', bytes([w & 0xFF]))[0]), 1
        if (w & 0xFE00) == 0x0600: return 'CMPIS $ac%d.m, #%d' % ((w >> 8) & 1, struct.unpack('b', bytes([w & 0xFF]))[0]), 1
        if (w & 0xF800) == 0x0800: return 'LRIS $%s, #%d' % (R[0x18 + ((w >> 8) & 7)], struct.unpack('b', bytes([w & 0xFF]))[0]), 1
        return '??%04x' % w, 1
    if t == 1:
        if (w & 0xFF00) == 0x1000: return 'LOOPI #%d' % (w & 0xFF), 1
        if (w & 0xFF00) == 0x1100: return 'BLOOPI #%d, 0x%04x' % (w & 0xFF, nxt), 2
        if (w & 0xFFF8) == 0x1200: return 'SBCLR #%d' % (w & 7), 1
        if (w & 0xFFF8) == 0x1300: return 'SBSET #%d' % (w & 7), 1
        if (w & 0xFE00) == 0x1400:
            r = (w >> 8) & 1; k = (w >> 6) & 3; i = w & 0x3F
            if k == 0: return 'LSL $ac%d, #%d' % (r, i), 1
            if k == 1: return 'LSR $ac%d, #%d' % (r, 64 - i), 1
            if k == 2: return 'ASL $ac%d, #%d' % (r, i), 1
            return 'ASR $ac%d, #%d' % (r, 64 - i), 1
        if (w & 0xFF00) == 0x1600: return 'SI @0xff%02x, #0x%04x' % (w & 0xFF, nxt), 2
        if (w & 0xFF10) == 0x1700:
            if (w & 0xF) == 0xF and (w & 0x10) == 0x10: pass
        if (w & 0xFF1F) == 0x171F: return 'CALLR $%s' % R[(w >> 5) & 7], 1
        if (w & 0xFF10) == 0x1700: return 'JR%s $%s' % (CC[w & 15] or 'MP', R[(w >> 5) & 7]), 1
        if (w & 0xFC00) == 0x1800:
            k = (w >> 7) & 7; s = (w >> 5) & 3; d = w & 31
            n = ['LRR', 'LRRD', 'LRRI', 'LRRN'][k & 3]
            if k < 4: return '%s $%s, @$ar%d' % (n, R[d], s), 1
            n = ['SRR', 'SRRD', 'SRRI', 'SRRN'][k & 3]
            return '%s @$ar%d, $%s' % (n, s, R[d]), 1
        if (w & 0xFC00) == 0x1C00: return 'MRR $%s, $%s' % (R[(w >> 5) & 31], R[w & 31]), 1
    if t == 2:
        if w & 0x0800: return 'SRS @0xff%02x, $%s' % (w & 0xFF, R[0x18 + ((w >> 8) & 7)]), 1
        return 'LRS $%s, @0xff%02x' % (R[0x18 + ((w >> 8) & 7)], w & 0xFF), 1
    # arithmetic with extension
    d = (w >> 8) & 1
    if t == 3:
        hi = (w >> 7) & 0x1F  # bits 11..7
        s = (w >> 9) & 1
        b = (w >> 7) & 1
        top = (w >> 10) & 3
        if b == 0:
            if top == 0: base = 'XORR $ac%d.m, $ax%d.h' % (d, s)
            elif top == 1: base = 'ANDR $ac%d.m, $ax%d.h' % (d, s)
            elif top == 2: base = 'ORR $ac%d.m, $ax%d.h' % (d, s)
            else: base = ('ANDC' if s == 0 else 'ORC') + ' $ac%d.m, $ac%d.m' % (d, 1 - d)
        else:
            if top == 0: base = ('XORC' if s == 0 else 'NOT') + ' $ac%d.m' % d + ('' if s else '') + ('' if s else ', $ac%d.m' % (1 - d))
            elif top == 1: base = 'LSRNRX $ac%d, $ax%d.h' % (d, s)
            elif top == 2: base = 'ASRNRX $ac%d, $ax%d.h' % (d, s)
            else: base = ('LSRNR' if s == 0 else 'ASRNR') + ' $ac%d' % d
        return base, 1, True
    if t in (4, 5, 6):
        nm = {4: ('ADDR', 'ADDAX', 'ADD', 'ADDP'), 5: ('SUBR', 'SUBAX', 'SUB', 'SUBP'), 6: ('MOVR', 'MOVAX', 'MOV', 'MOVP')}[t]
        if not (w & 0x0800):
            ss = (w >> 9) & 3
            return '%s $ac%d, $%s' % (nm[0], d, R[0x18 + ss]), 1, True
        if not (w & 0x0400): return '%s $ac%d, $ax%d' % (nm[1], d, (w >> 9) & 1), 1, True
        if not (w & 0x0200): return '%s $ac%d, $ac%d' % (nm[2], d, 1 - d), 1, True
        return '%s $ac%d' % (nm[3], d), 1, True
    if t == 7:
        k = (w >> 9) & 7
        if k <= 1: return 'ADDAXL $ac%d, $ax%d.l' % (d, (w >> 9) & 1), 1, True
        return ('', '', 'INCM', 'INC', 'DECM', 'DEC', 'NEG', 'MOVNP')[k] + ' $ac%d' % d, 1, True
    if t == 8:
        k = (w >> 8) & 0xF
        tbl = {0: 'NX', 8: 'NX', 1: 'CLR $ac0', 9: 'CLR $ac1', 2: 'CMP', 3: 'MULAXH', 4: 'CLRP', 5: 'TSTPROD', 6: 'TSTAXH $ax0.h', 7: 'TSTAXH $ax1.h',
               0xA: 'M2', 0xB: 'M0', 0xC: 'CLR15', 0xD: 'SET15', 0xE: 'SET16', 0xF: 'SET40'}
        return tbl[k], 1, True
    if t == 9:
        s = (w >> 11) & 1; k = (w >> 8) & 7; r = (w >> 8) & 1
        if k == 0: return 'MUL $ax%d.l, $ax%d.h' % (s, s), 1, True
        if k == 1: return 'ASR16 $ac%d' % ((w >> 11) & 1), 1, True
        n = {2: 'MULMVZ', 3: 'MULMVZ', 4: 'MULAC', 5: 'MULAC', 6: 'MULMV', 7: 'MULMV'}[k]
        return '%s $ax%d.l, $ax%d.h, $ac%d' % (n, s, s, r), 1, True
    if t in (0xA, 0xB):
        s = (w >> 12) & 1; tt = (w >> 11) & 1; k = (w >> 8) & 7; r = (w >> 8) & 1
        sn = 'ax%d.%s' % (0, 'h' if s else 'l'); tn = 'ax1.%s' % ('h' if tt else 'l')
        if k == 1: return ('ABS $ac%d' if t == 0xA else 'TST $ac%d') % ((w >> 11) & 1), 1, True
        if k == 0: return 'MULX $%s, $%s' % (sn, tn), 1, True
        n = {2: 'MULXMVZ', 3: 'MULXMVZ', 4: 'MULXAC', 5: 'MULXAC', 6: 'MULXMV', 7: 'MULXMV'}[k]
        return '%s $%s, $%s, $ac%d' % (n, sn, tn, r), 1, True
    if t in (0xC, 0xD):
        s = (w >> 12) & 1; tt = (w >> 11) & 1; k = (w >> 8) & 7; r = (w >> 8) & 1
        if k == 1: return 'CMPAR $ac%d, $ax%d.h' % (tt, s), 1, True
        if k == 0: return 'MULC $ac%d.m, $ax%d.h' % (s, tt), 1, True
        n = {2: 'MULCMVZ', 3: 'MULCMVZ', 4: 'MULCAC', 5: 'MULCAC', 6: 'MULCMV', 7: 'MULCMV'}[k]
        return '%s $ac%d.m, $ax%d.h, $ac%d' % (n, s, tt, r), 1, True
    if t == 0xE:
        k = (w >> 10) & 3; s = (w >> 9) & 1; tt = (w >> 8) & 1
        if k < 2: return ('MADDX', 'MSUBX')[k] + ' $ax0.%s, $ax1.%s' % ('h' if s else 'l', 'h' if tt else 'l'), 1, True
        return ('MADDC', 'MSUBC')[k - 2] + ' $ac%d.m, $ax%d.h' % (s, tt), 1, True
    if t == 0xF:
        k = (w >> 9) & 7
        if k == 0: return 'LSL16 $ac%d' % d, 1, True
        if k == 1: return 'MADD $ax%d.l, $ax%d.h' % (d, d), 1, True
        if k == 2: return 'LSR16 $ac%d' % d, 1, True
        if k == 3: return 'MSUB $ax%d.l, $ax%d.h' % (d, d), 1, True
        if k == 4 or k == 5: return 'ADDPAXZ $ac%d, $ax%d.h' % (d, (w >> 9) & 1), 1, True
        if k == 6: return 'CLRL $ac%d.l' % d, 1, True
        return 'MOVPZ $ac%d' % d, 1, True
    return '??%04x' % w, 1

def dol_bytes(path, addr, size):
    d = open(path, "rb").read()
    offs = struct.unpack(">18I", d[0:72])
    addrs = struct.unpack(">18I", d[72:144])
    sizes = struct.unpack(">18I", d[144:216])
    for o, a, s in zip(offs, addrs, sizes):
        if s and a <= addr and addr + size <= a + s:
            return d[o + addr - a:o + addr - a + size]
    raise SystemExit("0x%08X is not in an initialized DOL section" % addr)


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == "--raw":
        data = open(sys.argv[2], "rb").read()
    elif len(sys.argv) >= 2:
        addr = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x8038F340  # jdsp (same image on PAL, other address)
        size = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x17E0
        data = dol_bytes(sys.argv[1], addr, size)
    else:
        raise SystemExit(__doc__)
    words = struct.unpack(">%dH" % (len(data) // 2), data)
    pc = 0
    while pc < len(words):
        w = words[pc]
        nxt = words[pc + 1] if pc + 1 < len(words) else 0
        r = dis(w, nxt)
        txt, sz = r[0], r[1]
        if len(r) > 2:
            e = ext(w & 0xFF, (w >> 12) == 3)
            if e:
                txt += " : " + e
        raw = "%04x" % w + (" %04x" % nxt if sz == 2 else "     ")
        print("%04x  %s  %s" % (pc, raw, txt))
        pc += sz


if __name__ == "__main__":
    main()
