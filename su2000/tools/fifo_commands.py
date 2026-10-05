#!/usr/bin/env python3
"""Split the PIX broadcast-FIFO stream of an SU2000 trace into commands, without symbols.

A FIFO word is a command opcode when the host instruction 10 bytes before its return address is
'mov eax, imm32' (B8 imm32) with imm == the word, i.e. `mov eax, OP; call PIXI_OutWordBCFIFO`
(pattern from PIX_Send* in SFL.EXE, see docs/PROTOCOL.md §5). The return address is one of the
two stack dwords the stub stores in each FIFO record (addr = [esp+12], aux = [esp+8]).

Usage: fifo_commands.py TRACE EXE [--symmap FILE] [--dump N] [--frames]
  prints per-opcode statistics: count, length histogram (words incl. opcode), host call sites."""
import sys, os, struct, collections
import numpy as np
sys.path.insert(0, os.path.dirname(__file__))
import le
DT = np.dtype([('kind','u1'),('width','u1'),('cs','<u2'),('eip','<u4'),('addr','<u4'),('value','<u4'),('aux','<u4'),('t','<u4')])
NAMES = {}
for line in open(os.path.join(os.path.dirname(__file__), 'opcodes.tsv')):
    if line.strip() and not line.startswith('#'):
        op, name = line.split('\t')[:2]; NAMES[int(op, 16)] = name.strip()

def main():
    a = sys.argv[1:]; trace, exe = a[0], a[1]
    L = le.LE(exe); cbase = L.objs[0]['base']; cend = cbase + L.objs[0]['vsize']
    code = bytes(L.mem[1][:L.objs[0]['vsize']])
    n = os.path.getsize(trace) // DT.itemsize      # a SIGKILLed run can leave a partial record
    r = np.memmap(trace, dtype=DT, mode='r', shape=(n,))
    f = np.asarray(r[r['kind'] == 5])
    # DOS/4GW may load object 1 elsewhere than its LE base: find the delta that maps every FIFO
    # 'out' site onto an 'out dx,ax' (66 EF) in the image.
    eips = set(np.unique(f['eip']).tolist())
    delta = None
    for i in range(len(code) - 1):
        if code[i] == 0x66 and code[i+1] == 0xEF:
            d = min(eips) - (cbase + i)
            if all(0 <= e - d - cbase < len(code) - 1 and code[e - d - cbase] == 0x66 and code[e - d - cbase + 1] == 0xEF for e in eips):
                delta = d; break
    if delta is None: sys.exit('cannot find load delta')
    print(f'object 1 loaded at {cbase + delta:#x} (delta {delta:#x})')
    f['addr'] -= np.uint32(delta & 0xffffffff); f['aux'] -= np.uint32(delta & 0xffffffff)
    words = (f['value'] & 0xffff).astype(np.int64)
    rets = []
    is_op = np.zeros(len(f), bool)
    for i, (x, y, w) in enumerate(zip(f['addr'].tolist(), f['aux'].tolist(), words.tolist())):
        ret = 0
        for cand in (x, y):
            if cbase + 10 <= cand < cend and code[cand - cbase - 5] == 0xE8:
                ret = cand; break
        rets.append(ret)
        if ret and code[ret - cbase - 10] == 0xB8 and struct.unpack_from('<I', code, ret - cbase - 9)[0] == w:
            is_op[i] = True
    starts = np.nonzero(is_op)[0]
    lens = np.diff(np.concatenate((starts, [len(f)])))
    stats = collections.defaultdict(collections.Counter); sites = collections.defaultdict(set)
    for s, n in zip(starts.tolist(), lens.tolist()):
        stats[words[s]][n] += 1; sites[words[s]].add(rets[s] - 15)
    print(f'{len(f)} FIFO words, {len(starts)} commands, {int(starts[0]) if len(starts) else 0} words before first opcode')
    print(f'{"op":>6} {"name":<22} {"count":>8}  lengths (words incl. opcode: count)   call sites')
    for op in sorted(stats):
        ln = ', '.join(f'{k}:{v}' for k, v in sorted(stats[op].items())[:8])
        print(f'{op:#06x} {NAMES.get(op, "?"):<22} {sum(stats[op].values()):>8}  {ln:<38} ' + ' '.join(f'{s:#x}' for s in sorted(sites[op])[:3]))
    if '--dump' in a:
        n = int(a[a.index('--dump') + 1])
        for s, ln in list(zip(starts.tolist(), lens.tolist()))[:n]:
            print(f'{f["t"][s]:>10}us {NAMES.get(words[s], "?"):<20} ' + ' '.join(f'{w:04x}' for w in words[s:s+min(ln, 24)].tolist()) + (' …' if ln > 24 else ''))

if __name__ == '__main__': main()
