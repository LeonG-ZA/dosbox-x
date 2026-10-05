#!/usr/bin/env python3
"""Extract global symbols (segment:offset -> Watcom-mangled name) from a Watcom debug/.SYM file.
Record layout found empirically in SFL.SYM: off32 seg16 type16 kind8 len8 'W?name$...'.
Usage: watsym.py FILE.SYM [filter-regex]"""
import struct, sys, re

def symbols(path):
    d = open(path, 'rb').read()
    out = {}
    for m in re.finditer(rb'W\?', d):
        i = m.start()
        if i < 10: continue
        n = d[i-1]
        name = d[i:i+n]
        if len(name) != n or not re.fullmatch(rb'W\?[\x21-\x7e]+', name): continue
        off, seg, typ, kind = struct.unpack_from('<IHHB', d, i-10)
        if seg not in (1, 2, 3) or off > 0x400000: continue
        dm = name[2:].decode('latin1')
        short = re.split(r'\$', dm)[0]
        out[(seg, off, short)] = dm
    return sorted(out.items())

if __name__ == '__main__':
    flt = re.compile(sys.argv[2]) if len(sys.argv) > 2 else None
    for (seg, off, short), full in symbols(sys.argv[1]):
        if flt is None or flt.search(short):
            print(f'{seg}:{off:08x} {short:<40} {full}')
