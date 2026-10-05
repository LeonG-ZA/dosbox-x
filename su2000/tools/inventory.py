#!/usr/bin/env python3
"""Milestone 0: SHA-256 manifest, per-file classification and string dumps for data/original.
Usage: inventory.py   (run from anywhere; paths are relative to the su2000/ directory)
Writes data/MANIFEST.sha256, re/inventory.tsv, re/strings/<path>.txt (ASCII + UTF-16LE, min length 5)."""
import os, sys, hashlib, math, struct, re, collections
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
ORIG = os.path.join(ROOT, 'data', 'original')

def entropy(b):
    if not b: return 0.0
    c = collections.Counter(b); n = len(b)
    return -sum(v/n * math.log2(v/n) for v in c.values())

def classify(d, name):
    ext = os.path.splitext(name)[1].upper()
    if d[:2] == b'MZ':
        e = struct.unpack_from('<I', d, 0x3c)[0] if len(d) > 0x40 else 0
        sig = d[e:e+2] if e and e + 2 <= len(d) else b''
        comp = []
        if b'WATCOM' in d or b'Watcom' in d: comp.append('Watcom')
        if b'DOS/4G' in d: comp.append('DOS/4G-stub')
        if b'Borland' in d or b'Turbo C' in d: comp.append('Borland')
        if b'Microsoft C' in d or b'MS Run-Time' in d: comp.append('MSC')
        if b'Phar Lap' in d or sig == b'P3': comp.append('PharLap')
        kind = {b'LE': 'MZ+LE', b'LX': 'MZ+LX', b'NE': 'MZ+NE', b'PE': 'MZ+PE', b'P3': 'MZ+P3 (Phar Lap)'}.get(sig, 'MZ (real mode)')
        return kind, ','.join(comp)
    if d[:2] == b'\x01\x6d': return 'm88k COFF executable', 'Motorola 88k toolchain'
    if ext == '.COM': return 'DOS COM', ''
    if d[:3] == b'GIF': return 'GIF image', ''
    if d[:4] == b'RIFF': return 'RIFF (' + d[8:12].decode('latin1') + ')', ''
    if d[:1] == b'\x0a' and ext == '.PCX': return 'PCX image', ''
    if ext == '.TGA': return 'Targa image', ''
    if ext in ('.BAT', '.SYS', '.INF', '.VPC', '.TXT', '.ORG', '.OLD', '.NEW', '.PIF', '.RST', '.INI', '.CFG'):
        return 'text/config' if all(32 <= c < 127 or c in (9, 10, 13, 26) for c in d[:4096]) else 'binary ' + ext, ''
    if len(d) and len(d) == d.count(0): return 'all zero', ''
    return 'data ' + (ext or '(no ext)'), ''

def strings(d, n=5):
    a = [m.group().decode('latin1') for m in re.finditer(rb'[\x20-\x7e]{%d,}' % n, d)]
    u = [m.group().decode('utf-16le') for m in re.finditer(rb'(?:[\x20-\x7e]\x00){%d,}' % n, d)]
    return a, u

def main():
    files = []
    for dp, dn, fn in os.walk(ORIG):
        dn.sort()
        for f in sorted(fn): files.append(os.path.join(dp, f))
    man = open(os.path.join(ROOT, 'data', 'MANIFEST.sha256'), 'w')
    inv = open(os.path.join(ROOT, 're', 'inventory.tsv'), 'w')
    inv.write('path\tsize\tsha256\ttype\tcompiler\tentropy\tmagic\n')
    sdir = os.path.join(ROOT, 're', 'strings')
    for p in files:
        d = open(p, 'rb').read(); rel = os.path.relpath(p, ORIG)
        h = hashlib.sha256(d).hexdigest()
        man.write(f'{h}  {rel}\n')
        kind, comp = classify(d, p)
        inv.write(f'{rel}\t{len(d)}\t{h}\t{kind}\t{comp}\t{entropy(d):.2f}\t{d[:8].hex()}\n')
        if kind.startswith(('MZ', 'm88k', 'DOS COM', 'data', 'binary')) or p.upper().endswith(('.SYM', '.OUT', '.COD', '.BIN')):
            a, u = strings(d)
            out = os.path.join(sdir, rel + '.txt'); os.makedirs(os.path.dirname(out), exist_ok=True)
            with open(out, 'w') as f:
                f.write('\n'.join(a)); f.write('\n\n# UTF-16LE\n'); f.write('\n'.join(u))
    print(f'{len(files)} files')

if __name__ == '__main__': main()
