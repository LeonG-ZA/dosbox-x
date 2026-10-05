#!/usr/bin/env python3
"""List the immediate word values each host function pushes into the PIX broadcast FIFO.
Heuristic: an immediate 'mov eax, IMM' directly followed by 'call PIXI_OutWordBCFIFO'.
Usage: fifo_opcodes.py ASMFILE   (output of disas_le.py)"""
import re, sys
func = None; prev = None; res = {}
for line in open(sys.argv[1]):
    m = re.match(r'===== (\S+) @', line)
    if m: func = m.group(1); prev = None; continue
    parts = line.split(None, 2)
    if len(parts) < 2: continue
    txt = ' '.join(parts[1:])
    if 'PIXI_OutWordBCFIFO' in txt and prev is not None:
        res.setdefault(func, []).append(prev)
    mm = re.match(r'mov\s+eax, (0x[0-9a-f]+|\d+)$', txt.strip())
    prev = int(mm.group(1), 0) if mm else None
by_op = {}
for f, ops in res.items():
    by_op.setdefault(ops[0], []).append(f)
    print(f'{f:<34} ' + ' '.join(f'{o:#06x}' for o in ops))
print('\n# first opcode -> functions')
for op in sorted(by_op): print(f'{op:#06x} {", ".join(by_op[op])}')
