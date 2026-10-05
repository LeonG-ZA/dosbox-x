#!/usr/bin/env python3
"""Convert an SU2000 bus trace into a board replay file for tests/run_pix (card 0 view).
Record = <B type><I addr><I value> little-endian:
  1 = host 16-bit write to board memory (addr = board address)
  2 = FIFO word (value)
  3 = RUN_A, 4 = RUN_B
Usage: make_replay.py TRACE OUT.bin"""
import sys, os, numpy as np
DT = np.dtype([('kind','u1'),('width','u1'),('cs','<u2'),('eip','<u4'),('addr','<u4'),('value','<u4'),('aux','<u4'),('t','<u4')])
trace, out = sys.argv[1:3]
n = os.path.getsize(trace) // DT.itemsize
r = np.memmap(trace, dtype=DT, mode='r', shape=(n,))
k = r['kind']
mw = (k == 4) & (r['addr'] >= 0xd0000) & (r['addr'] < 0xe0000) & (r['aux'] != 0xffffffff) & (r['width'] == 2)
ff = k == 5
ev = (k == 6) & ((r['addr'] == 1) | (r['addr'] == 2)) & (r['aux'] == 0)
sel = np.nonzero(mw | ff | ev)[0]
s = np.asarray(r[sel])
o = np.zeros(len(s), dtype=np.dtype([('t','u1'),('a','<u4'),('v','<u4')]))
o['t'] = np.where(s['kind'] == 4, 1, np.where(s['kind'] == 5, 2, np.where(s['addr'] == 1, 3, 4)))
o['a'] = np.where(s['kind'] == 4, s['aux'] & np.uint32(0xfffffffe), 0)
o['v'] = np.where(s['kind'] == 5, s['value'] & 0xffff, s['value'])
o.tofile(out)
print(f'{len(o)} records: {int((o["t"]==1).sum())} writes, {int((o["t"]==2).sum())} FIFO words, {int((o["t"]>=3).sum())} run events')
