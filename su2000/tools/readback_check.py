#!/usr/bin/env python3
"""Gate-1 checks on an SU2000 trace: (1) writes into firmware code/vector regions after the CPUs
started, (2) every board address the host reads back after boot, (3) port reads.
Usage: readback_check.py TRACE MAINA.OUT MAINB.OUT"""
import sys, os, collections, numpy as np
sys.path.insert(0, os.path.dirname(__file__))
import coff88k
DT = np.dtype([('kind','u1'),('width','u1'),('cs','<u2'),('eip','<u4'),('addr','<u4'),('value','<u4'),('aux','<u4'),('t','<u4')])
trace, fa, fb = sys.argv[1:4]
n = os.path.getsize(trace) // DT.itemsize
r = np.memmap(trace, dtype=DT, mode='r', shape=(n,))
ev = np.nonzero((r['kind'] == 6) & (r['addr'] == 1))[0]
boot = int(ev[-1]) if len(ev) else 0
print(f'{n} records; last RUN_A at record {boot} (t={r["t"][boot]/1e6:.2f}s)')
code = []
for f in (fa, fb):
    for s in coff88k.parse(f)['sections']:
        if s['name'] in ('.vecsA', '.vecsB', '.text'): code.append((s['vaddr'], s['vaddr'] + s['size'], os.path.basename(f) + s['name']))
post = np.asarray(r[boot:])
w = post[(post['kind'] == 4) & (post['aux'] != 0xffffffff)]
for lo, hi, name in code:
    k = np.count_nonzero((w['aux'] >= lo) & (w['aux'] < hi))
    print(f'writes into {name:<16} {lo:#010x}-{hi:#010x} after boot: {k}')
rd = post[(post['kind'] == 3) & (post['aux'] != 0xffffffff)]
c = collections.Counter((rd['aux'] & np.uint32(0xfffffffe)).tolist())
print(f'board reads after boot: {len(rd)} at {len(c)} addresses')
for a, k in sorted(c.items(), key=lambda x: -x[1])[:20]: print(f'   {a:#010x} x{k}')
pr = post[post['kind'] == 1]
c = collections.Counter(pr['addr'].tolist())
print('port reads after boot: ' + ', '.join(f'{p:#x}:{k}' for p, k in sorted(c.items())))
