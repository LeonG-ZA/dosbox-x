#!/usr/bin/env python3
"""Rebuild the bytes the host wrote into PIX board memory from an SU2000 trace, snapshot it at every
CPU start (RUN_A / RUN_B event), and compare against the COFF sections of MAINA/MAINB.
The trace records the board address of the first card selected; writes broadcast to several cards
(the normal case for uploads) are therefore recorded once.
Usage: extract_upload.py TRACE OUTDIR [MAINA.OUT MAINB.OUT ...]
Writes OUTDIR/<snapshot>_<lo>.bin for contiguous regions >= 64 bytes, prints a table."""
import sys, os, hashlib
import numpy as np
sys.path.insert(0, os.path.dirname(__file__))
import coff88k
DT = np.dtype([('kind','u1'),('width','u1'),('cs','<u2'),('eip','<u4'),('addr','<u4'),('value','<u4'),('aux','<u4'),('t','<u4')])

def main():
    trace, out = sys.argv[1:3]; refs = sys.argv[3:]
    os.makedirs(out, exist_ok=True)
    r = np.fromfile(trace, dtype=DT)
    ev = np.nonzero((r['kind'] == 6) & np.isin(r['addr'], (1, 2)))[0]
    wr = (r['kind'] == 4) & (r['addr'] >= 0xd0000) & (r['addr'] < 0xe0000) & (r['aux'] != 0xffffffff) & (r['width'] == 2)
    cuts = list(ev) + [len(r)]
    mem = {}
    prev = 0
    rows = []
    secs = [(os.path.basename(os.path.dirname(f)) + '/' + os.path.basename(f), s) for f in refs for s in coff88k.parse(f)['sections'] if s['raw']]
    for n, c in enumerate(cuts):
        idx = np.nonzero(wr[prev:c])[0] + prev
        for a, v in zip((r['aux'][idx] & np.uint32(0xfffffffe)).tolist(), r['value'][idx].tolist()):
            mem[a] = (v >> 8) & 0xff; mem[a+1] = v & 0xff
        prev = c
        if c == len(r): label = 'end'
        else: label = ('runA' if r['addr'][c] == 1 else 'runB') + f'_card{r["aux"][c]}'
        if not mem: continue
        addrs = np.array(sorted(mem), dtype=np.int64)
        brk = np.nonzero(np.diff(addrs) != 1)[0]
        starts = np.concatenate(([0], brk + 1)); ends = np.concatenate((brk + 1, [len(addrs)]))
        for s, e in zip(starts, ends):
            lo, hi = int(addrs[s]), int(addrs[e-1]) + 1
            if hi - lo < 64: continue
            blob = bytes(mem[a] for a in range(lo, hi))
            h = hashlib.sha256(blob).hexdigest()
            fn = f'{n:02d}_{label}_{lo:08x}.bin'
            open(os.path.join(out, fn), 'wb').write(blob)
            m = []
            for name, sec in secs:
                if sec['vaddr'] >= lo and sec['vaddr'] + len(sec['raw']) <= hi:
                    o = sec['vaddr'] - lo
                    m.append(f"{name}:{sec['name']}={'MATCH' if blob[o:o+len(sec['raw'])] == sec['raw'] else 'diff'}")
            rows.append(f'{fn:<34} {lo:#010x}-{hi:#010x} {hi-lo:>9} {h[:16]} ' + ' '.join(x for x in m if 'MATCH' in x))
    print('\n'.join(rows))

if __name__ == '__main__': main()
