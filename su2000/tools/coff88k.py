#!/usr/bin/env python3
"""Parse m88k COFF (magic 0x016D, big-endian) executables such as MAINA.OUT / MAINB.OUT.

Usage: coff88k.py FILE...            -> header, sections, symbols
       coff88k.py --dump-sec NAME FILE OUT  -> write raw section bytes
"""
import struct, sys, hashlib

def parse(path):
    d = open(path, 'rb').read()
    magic, nscns, ts, symptr, nsyms, opthdr, flags = struct.unpack_from('>HHIIIHH', d, 0)
    assert magic == 0x016D, hex(magic)
    r = dict(path=path, data=d, magic=magic, ts=ts, symptr=symptr, nsyms=nsyms, flags=flags)
    if opthdr >= 28:
        r['aout'] = dict(zip(('magic','vstamp','tsize','dsize','bsize','entry','text_start','data_start'),
                             struct.unpack_from('>HHIIIIII', d, 20)))
    off = 20 + opthdr
    secs = []
    for i in range(nscns):
        name = d[off:off+8].rstrip(b'\0').decode('latin1')
        paddr, vaddr, size, scnptr, relptr, lnnoptr, nreloc, nlnno, sflags = struct.unpack_from('>IIIIIIIII', d, off+8)
        # m88k COFF uses 4-byte nreloc/nlnno (44-byte section headers)
        raw = d[scnptr:scnptr+size] if scnptr and sflags & 0x80 == 0 else b''
        secs.append(dict(name=name, paddr=paddr, vaddr=vaddr, size=size, scnptr=scnptr, flags=sflags,
                         sha=hashlib.sha256(raw).hexdigest() if raw else '-', raw=raw))
        off += 44
    r['sections'] = secs
    syms = []
    if symptr and nsyms:
        strtab_off = symptr + nsyms * 20
        i = 0
        while i < nsyms:
            e = d[symptr+i*20:symptr+(i+1)*20]
            if len(e) < 20: break
            if e[:4] == b'\0\0\0\0':
                so = struct.unpack_from('>I', e, 4)[0]
                end = d.find(b'\0', strtab_off+so)
                name = d[strtab_off+so:end].decode('latin1')
            else:
                name = e[:8].rstrip(b'\0').decode('latin1')
            val, scn, typ, scl, naux = struct.unpack_from('>IhHBB', e, 8)
            syms.append((name, val, scn, scl))
            i += 1 + naux
    r['symbols'] = syms
    return r

if __name__ == '__main__':
    a = sys.argv[1:]
    if a and a[0] == '--dump-sec':
        r = parse(a[2]); s = [s for s in r['sections'] if s['name'] == a[1]][0]
        open(a[3], 'wb').write(s['raw']); sys.exit()
    for p in a:
        r = parse(p)
        print(f"== {p}  ts={r['ts']:#x} nsyms={r['nsyms']} flags={r['flags']:#x}")
        if 'aout' in r: print('   aout', {k: hex(v) for k, v in r['aout'].items()})
        for s in r['sections']:
            print(f"   {s['name']:<8} vaddr={s['vaddr']:#010x} size={s['size']:#08x} file@{s['scnptr']:#07x} flags={s['flags']:#x} sha={s['sha'][:16]}")
        print(f"   {len(r['symbols'])} symbols:", ', '.join(n for n, *_ in r['symbols'][:40]))
