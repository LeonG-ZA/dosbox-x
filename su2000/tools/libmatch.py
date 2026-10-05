#!/usr/bin/env python3
"""Match functions named in a reference EXE+SYM against other LE EXEs (no symbols) by masked byte signature.
Masks rel32 of call/jmp (E8/E9) and absolute 32-bit operands that were fixed up. Reports per-prefix match rate
and writes recovered symbol maps (name -> linear address) for each target.
Usage: libmatch.py REF.EXE REF.SYM REGEX TARGET.EXE..."""
import sys, re, os
sys.path.insert(0, os.path.dirname(__file__))
import le, watsym

def sig(L, lin, n, fix):
    b = bytearray(L.read(lin, n)); mask = [True]*n
    i = 0
    while i < n:
        if b[i] in (0xE8, 0xE9) and i+5 <= n:
            for k in range(1, 5): mask[i+k] = False
            i += 5; continue
        i += 1
    for k in range(n):
        if (lin + k) in fix: mask[k] = False
    return bytes(b), mask

def fixset(L):
    s = set()
    for obj, at, st, tobj, toff in L.fixups:
        base = L.objs[obj-1]['base'] + at
        for k in range(4): s.add(base + k)
    return s

def find(code, base, pat, mask, fixT):
    # anchor on first unmasked 6-byte run
    for a in range(len(pat)-6):
        if all(mask[a:a+6]): break
    anchor = pat[a:a+6]; out = []
    j = code.find(anchor)
    while j >= 0:
        s = j - a
        if s >= 0 and all((not mask[k]) or code[s+k] == pat[k] for k in range(len(pat))):
            out.append(base + s)
        j = code.find(anchor, j+1)
    return out

def main():
    ref, sym, rx = sys.argv[1:4]; targets = sys.argv[4:]
    R = le.LE(ref); fr = fixset(R)
    funcs = sorted((R.linear(s, o), n) for (s, o, n), _ in watsym.symbols(sym) if s == 1)
    rxc = re.compile(rx)
    sel = [(a, n, (funcs[i+1][0] if i+1 < len(funcs) else a+64) - a) for i, (a, n) in enumerate(funcs) if rxc.search(n)]
    for t in targets:
        T = le.LE(t); code = bytes(T.mem[1][:T.objs[0]['vsize']]); base = T.objs[0]['base']
        hit = 0; uniq = 0; out = []
        for a, n, size in sel:
            n_ = min(size, 48)
            if n_ < 12: continue
            p, m = sig(R, a, n_, fr)
            r = find(code, base, p, m, None)
            if r: hit += 1
            if len(r) == 1: uniq += 1; out.append((n, r[0]))
        print(f'{t}: {hit}/{len(sel)} functions found, {uniq} unique')
        mp = os.path.join(os.path.dirname(__file__), '..', 're', os.path.basename(t).lower() + '.symmap')
        with open(mp, 'w') as f:
            for n, a in out: f.write(f'{a:08x} {n}\n')

if __name__ == '__main__': main()
