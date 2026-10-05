#!/usr/bin/env python3
"""MC88110 disassembler for PIX firmware (MAINA.OUT / MAINB.OUT).

Decoding uses the MAME mc88110 opcode table (tools/m88110_ops.py, BSD-3-Clause); operand formatting
follows MAME's m88000_disassembler. Branch targets and or.u/or address pairs are annotated with COFF symbols.

Usage: m88kdis.py FILE.OUT [start_hex [end_hex]]   disassemble .text (or a range)
       m88kdis.py FILE.OUT --func NAME              disassemble one symbol up to the next symbol
       m88kdis.py --hist FILE.OUT...                mnemonic-class histogram over .text
       m88kdis.py --mnem FILE.OUT...                per-mnemonic counts (coverage list for the interpreter)
"""
import struct, sys, os, bisect, collections
sys.path.insert(0, os.path.dirname(__file__))
import coff88k
from m88110_ops import OPS, GCR

_BY_MAJOR = collections.defaultdict(list)
for v, m, n, a in OPS:
    for major in range(64):
        if (major << 26) & m == v & m & 0xfc000000:
            _BY_MAJOR[major].append((v, m, n, a))

COND = {1: 'gt0', 2: 'eq0', 3: 'ge0', 0xc: 'lt0', 0xd: 'ne0', 0xe: 'le0'}

def bit(x, n): return (x >> n) & 1

def decode(inst):
    for v, m, n, a in _BY_MAJOR[inst >> 26]:
        if inst & m == v: return n, a
    return None, None

def fmt(inst, pc, syms=None):
    n, a = decode(inst)
    d, s1, s2 = (inst >> 21) & 31, (inst >> 16) & 31, inst & 31
    sym = lambda t: f' <{syms[t]}>' if syms and t in syms else ''
    if n is None:
        return f'{"illop":<12}${inst:08x}'
    if a == 'TRIADIC':
        o = f'{n:<12}'
        if (inst & 0xf800f800) != 0xf000f800: o += f"{'r' if bit(inst, 26) else 'x'}{d},"
        if (inst & 0xf800f000) != 0xf000e000:
            o += f'r{s1}' + (',' if (inst & 0xfc00f800) != 0x88006800 else '')
        if (inst & 0xfc00f800) != 0x88006800: o += f'r{s2}'
        return o
    if a == 'FP':
        if (inst & 0xfc006000) == 0x84004000:
            o = f"{n:<12}{'x' if (inst & 0xfc007e00) == 0x84004200 else 'r'}{d},"
        else:
            o = f"{n:<12}{'x' if bit(inst, 15) and (inst & 0xfc007800) != 0x84003800 else 'r'}{d},"
            if (inst & 0xfc007800) not in (0x84000800, 0x84002000, 0x84007800):
                o += f"{'x' if bit(inst, 15) else 'r'}{s1},"
        return o + f"{'x' if bit(inst, 15) else 'r'}{s2}"
    if a == 'IMM6': return f'{n:<12}r{d},r{s1},<{(inst >> 5) & 63}>'
    if a == 'BITFIELD':
        o = f'{n:<12}r{d},r{s1},'
        w = (inst >> 5) & 31
        if (inst & 0xfc00fc00) != 0xf000a800 and w: o += str(w)
        return o + f'<{inst & 31}>'
    if a == 'SIMM16':
        i = inst & 0xffff
        return f'{n:<12}r{d},r{s1},' + (f'-${0x10000 - i:x}' if i & 0x8000 else f'${i:x}')
    if a == 'IMM16':
        o = f'{n:<12}' + (f'r{d},' if (inst & 0xfc000000) != 0xf8000000 else '') + f'r{s1},'
        return o + (f'${inst & 0xffff:04x}' if inst >= 0x40000000 else f'${inst & 0xffff:x}')
    if a == 'CR':
        o = f'{n:<12}'
        if bit(inst, 14): o += f'r{d},'
        if bit(inst, 15): o += f'r{s2},'
        cr = (inst >> 5) & 63; sfu = (inst >> 11) & 7
        if sfu == 0: o += GCR.get(cr, f'sr{cr - 16}' if 16 <= cr <= 20 else f'cr{cr}')
        elif sfu == 1: o += f'fcr{cr}'
        else: o += f'SFU{sfu}_cr{cr}'
        return o
    if a in ('SI16_GRF', 'SI16_XRF'):
        i = inst & 0xffff
        return f"{n:<12}{'x' if a == 'SI16_XRF' else 'r'}{d},r{s1}," + (f'-${0x10000 - i:x}' if i & 0x8000 else f'${i:x}')
    if a == 'SCALED': return f"{n:<12}{'r' if bit(inst, 26) else 'x'}{d},r{s1}[r{s2}]"
    if a == 'JUMP': return f'{n:<12}r{s2}'
    if a == 'VEC9':
        c = str(d) if bit(inst, 12) else COND.get(d, f'${d:02x}')
        return f'{n:<12}{c},r{s1},${inst & 0x1ff:03x}'
    if a == 'D16':
        t = (pc + ((inst & 0xffff) ^ 0x8000) * 4 - 0x20000) & 0xffffffff
        c = str(d) if bit(inst, 28) else COND.get(d, f'${d:02x}')
        return f'{n:<12}{c},r{s1},${t:08x}' + sym(t)
    if a == 'D26':
        disp = (inst & 0x03ffffff) * 4 - (0x10000000 if bit(inst, 25) else 0)
        t = (pc + disp) & 0xffffffff
        return f'{n:<12}${t:08x}' + sym(t)
    return n

def klass(inst):
    n, a = decode(inst)
    if n is None: return 'illegal/data'
    if inst >> 26 == 0x22: return 'graphics(88110)'
    if a == 'FP' or a == 'SI16_XRF' or n.endswith('.x') or (inst >> 27 == 0x1e and not bit(inst, 26)): return 'fp/xrf'
    if a in ('D16', 'D26', 'JUMP'): return 'branch'
    if n.startswith(('ld', 'st', 'xmem', 'lda')): return 'load/store'
    if a == 'CR': return 'ctrlreg'
    return 'integer'

def text(path):
    r = coff88k.parse(path)
    t = [s for s in r['sections'] if s['name'] == '.text'][0]
    syms = {v: n for n, v, s, c in r['symbols']}
    return t['vaddr'], t['raw'], syms

def main():
    a = sys.argv[1:]
    if a[0] in ('--hist', '--mnem'):
        for p in a[1:]:
            base, raw, _ = text(p); c = collections.Counter()
            for i in range(0, len(raw) - 3, 4):
                w = struct.unpack_from('>I', raw, i)[0]
                c[klass(w) if a[0] == '--hist' else (decode(w)[0] or '??')] += 1
            tot = sum(c.values())
            print(p + ': ' + ', '.join(f'{k}={v} ({100*v/tot:.1f}%)' for k, v in c.most_common()))
        return
    base, raw, syms = text(a[0])
    if len(a) > 2 and a[1] == '--func':
        addrs = sorted(syms); lo = [v for v, n in syms.items() if n == a[2]][0]
        i = bisect.bisect_right(addrs, lo); hi = addrs[i] if i < len(addrs) else base + len(raw)
        hi = min(hi, base + len(raw))
    else:
        lo = int(a[1], 16) if len(a) > 1 else base
        hi = int(a[2], 16) if len(a) > 2 else base + len(raw)
    hi_reg = {}
    for pc in range(lo, hi, 4):
        w = struct.unpack_from('>I', raw, pc - base)[0]
        if pc in syms: print(f'\n<{syms[pc]}>:')
        s = fmt(w, pc, syms)
        # annotate or.u rX,r0,HI ; or/ld/st ...,rX,LO pairs
        op = w >> 26; d = (w >> 21) & 31; s1 = (w >> 16) & 31
        note = ''
        if op == 0x17 and s1 == 0: hi_reg[d] = (w & 0xffff) << 16
        elif s1 in hi_reg and op in (0x16, 0x18) or (s1 in hi_reg and op < 0x30):
            t = hi_reg[s1] + ((w & 0xffff) if op >= 0x10 else ((w & 0xffff) ^ 0x8000) - 0x8000)
            note = f'   ; ={t & 0xffffffff:#x}' + (f' <{syms[t]}>' if t in syms else '')
            if op != 0x17: hi_reg.pop(s1, None) if d == s1 else None
        print(f'{pc:08x}: {w:08x}  {s}{note}')

if __name__ == '__main__': main()
