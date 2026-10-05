#!/usr/bin/env python3
"""Find the FIFO opcode dispatch table in a MAINA.OUT (.data array of handler addresses that are
Proc*/Cmd* symbols) and print opcode -> handler. Usage: fw_dispatch.py MAINA.OUT..."""
import sys, os, struct
sys.path.insert(0, os.path.dirname(__file__))
import coff88k
for p in sys.argv[1:]:
    r = coff88k.parse(p)
    syms = {v: n for n, v, s, c in r['symbols']}
    text = [s for s in r['sections'] if s['name'] == '.text'][0]
    lo, hi = text['vaddr'], text['vaddr'] + text['size']
    for sec in r['sections']:
        if sec['name'] != '.data': continue
        raw = sec['raw']; words = [struct.unpack_from('>I', raw, i)[0] for i in range(0, len(raw) - 3, 4)]
        best = (0, 0)
        i = 0
        while i < len(words):
            j = i
            while j < len(words) and lo <= words[j] < hi: j += 1
            named = sum(1 for w in words[i:j] if syms.get(w, '').startswith(('Proc', 'Cmd')))
            if named > best[1]: best = (i, named, j)
            i = j + 1
        i, named, j = best
        print(f'== {p}: table at {sec["vaddr"] + 4*i:#x}, {j - i} entries ({named} named Proc*/Cmd*)')
        for k, w in enumerate(words[i:j]):
            print(f'   {k:#04x} {syms.get(w, hex(w))}')
