#!/usr/bin/env python3
"""Fast statistics over a large .su2k trace (numpy). Usage: su2kstats.py TRACE"""
import sys, numpy as np
dt = np.dtype([('kind','u1'),('width','u1'),('cs','<u2'),('eip','<u4'),('addr','<u4'),('value','<u4'),('aux','<u4'),('t','<u4')])
r = np.fromfile(sys.argv[1], dtype=dt)
print(f'{len(r)} records, {r["t"][-1]/1e6:.2f}s emulated')
names = {1:'IN',2:'OUT',3:'MRD',4:'MWR',5:'FIFO',6:'EVT'}
for k in range(1,7):
    s = r[r['kind']==k]
    if not len(s): continue
    key = s['addr'] if k in (1,2,5) else (s['aux'] if k in (3,4) else s['addr'])
    if k in (3,4): key = np.where(s['aux']==0xffffffff, s['addr'], s['aux'] & 0xffff0000)
    u, c = np.unique(key, return_counts=True)
    top = np.argsort(-c)[:12]
    print(f'{names[k]:<4} {len(s):>10}  ' + ', '.join(f'{u[i]:#x}:{c[i]}' for i in top))
    if k in (3, 4):
        e, ec = np.unique(s['eip'], return_counts=True); t = np.argsort(-ec)[:6]
        print('      by eip: ' + ', '.join(f'{e[i]:#x}:{ec[i]}' for i in t))
