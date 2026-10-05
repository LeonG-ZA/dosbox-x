#!/usr/bin/env python3
"""Minimal Motorola 88000 (88100 + common 88110) disassembler, for firmware triage.
Covers integer ALU, loads/stores, branches, bit-field, control-register and marks FP / 88110-only
groups by class. Unknown encodings print as '.word'.
Usage: m88kdis.py FILE.OUT [start_hex end_hex]    disassemble .text (annotated with COFF symbols)
       m88kdis.py --hist FILE.OUT...               opcode-class histogram over .text"""
import struct, sys, os, collections
sys.path.insert(0, os.path.dirname(__file__))
import coff88k

LDST = {0:'xmem.bu',1:'xmem',2:'ld.hu',3:'ld.bu',4:'ld.d',5:'ld',6:'ld.h',7:'ld.b',
        8:'st.d',9:'st',10:'st.h',11:'st.b'}
ALUI = {0x10:'and',0x11:'and.u',0x12:'mask',0x13:'mask.u',0x14:'xor',0x15:'xor.u',0x16:'or',0x17:'or.u',
        0x18:'addu',0x19:'subu',0x1a:'divu',0x1b:'mulu',0x1c:'add',0x1d:'sub',0x1e:'div',0x1f:'cmp'}
BR = {0x30:'br',0x31:'br.n',0x32:'bsr',0x33:'bsr.n'}
BB = {0x34:'bb0',0x35:'bb0.n',0x36:'bb1',0x37:'bb1.n',0x3a:'bcnd',0x3b:'bcnd.n'}
BF = {0x20:'clr',0x22:'set',0x24:'ext',0x26:'extu',0x28:'mak',0x2a:'rot'}
RR = {0x10:'and',0x11:'and.c',0x14:'xor',0x15:'xor.c',0x16:'or',0x17:'or.c',0x18:'addu',0x19:'subu',
      0x1a:'divu',0x1b:'mulu',0x1c:'add',0x1d:'sub',0x1e:'div',0x1f:'cmp',
      0x20:'clr',0x22:'set',0x24:'ext',0x26:'extu',0x28:'mak',0x2a:'rot',
      0x30:'jmp',0x31:'jmp.n',0x32:'jsr',0x33:'jsr.n',0x3a:'ff1',0x3b:'ff0',0x3e:'tbnd',0x3f:'rte'}
COND = {2:'eq0',0xd:'ne0',1:'gt0',0xc:'lt0',3:'ge0',0xe:'le0'}

def dis(w, pc, syms={}):
    op = w >> 26; d = (w >> 21) & 31; s1 = (w >> 16) & 31; imm = w & 0xffff; s2 = w & 31
    simm = imm - 0x10000 if imm & 0x8000 else imm
    if op in LDST: return 'ldst', f'{LDST[op]:<8} r{d},r{s1},{imm:#x}'
    if 0x0c <= op <= 0x0f: return 'x-ldst(88110)', f'.x-ldst  {w:08x}'
    if op in ALUI: return 'alu', f'{ALUI[op]:<8} r{d},r{s1},{imm:#x}'
    if op in BR:
        disp = w & 0x3ffffff
        if disp & 0x2000000: disp -= 0x4000000
        t = pc + disp*4
        return 'branch', f'{BR[op]:<8} {t:#x}' + (f' <{syms[t]}>' if t in syms else '')
    if op in BB:
        t = pc + simm*4
        cnd = COND.get(d, str(d)) if op >= 0x3a else str(d)
        return 'branch', f'{BB[op]:<8} {cnd},r{s1},{t:#x}' + (f' <{syms[t]}>' if t in syms else '')
    if op == 0x3c:
        sub = (w >> 10) & 0x3f; w5 = (w >> 5) & 31; o5 = w & 31
        if sub in BF: return 'bitfield', f'{BF[sub]:<8} r{d},r{s1},{w5}<{o5}>'
    if op == 0x3d:
        sub = (w >> 10) & 0x3f
        if (w >> 12) & 0xf in (0x0, 0x1, 0x2, 0x3) and sub < 0x10:  # register-indexed ld/st
            return 'ldst', f'ldst.rr  {w:08x} r{d},r{s1},r{s2}'
        if sub in RR:
            if sub in (0x30, 0x31, 0x32, 0x33): return 'branch', f'{RR[sub]:<8} r{s2}'
            return 'alu', f'{RR[sub]:<8} r{d},r{s1},r{s2}'
        return 'alu?', f'.rr      {w:08x}'
    if op == 0x20: return 'ctrlreg', f'ctlreg   {w:08x}'
    if op == 0x21: return 'fp', f'fp       {w:08x}'
    if op == 0x22: return 'graphics(88110)', f'gfx      {w:08x}'
    if op == 0x3e: return 'trap', f'tbnd     r{s1},{imm:#x}'
    if op == 0x3c: return 'trap', f'tb/bitf  {w:08x}'
    return 'unknown', f'.word    {w:08x}'

def text(path):
    r = coff88k.parse(path)
    t = [s for s in r['sections'] if s['name'] == '.text'][0]
    syms = {v: n for n, v, s, c in r['symbols']}
    return t['vaddr'], t['raw'], syms

if __name__ == '__main__':
    a = sys.argv[1:]
    if a[0] == '--hist':
        for p in a[1:]:
            base, raw, _ = text(p); c = collections.Counter()
            for i in range(0, len(raw) - 3, 4): c[dis(struct.unpack_from('>I', raw, i)[0], base+i)[0]] += 1
            tot = sum(c.values())
            print(p, ', '.join(f'{k}={v} ({100*v/tot:.1f}%)' for k, v in c.most_common()))
        sys.exit()
    base, raw, syms = text(a[0])
    lo = int(a[1], 16) if len(a) > 1 else base; hi = int(a[2], 16) if len(a) > 2 else base + len(raw)
    for pc in range(lo, hi, 4):
        w = struct.unpack_from('>I', raw, pc - base)[0]
        if pc in syms: print(f'\n<{syms[pc]}>:')
        print(f'{pc:08x}: {w:08x}  {dis(w, pc, syms)[1]}')
