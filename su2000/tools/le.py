#!/usr/bin/env python3
"""Minimal Linear Executable (LE, DOS/4GW) loader: objects, pages, fixups applied at a chosen base."""
import struct

class LE:
    def __init__(self, path):
        d = self.d = open(path, 'rb').read()
        self.hoff = h = struct.unpack_from('<I', d, 0x3c)[0]
        assert d[h:h+2] == b'LE', 'not LE'
        g = lambda o, f='<I': struct.unpack_from(f, d, h+o)[0]
        self.eip_obj, self.eip = g(0x18), g(0x1c)
        self.page_size, self.last_page = g(0x28), g(0x2c)
        nobj, objtab, objpm = g(0x44), g(0x40), g(0x48)
        fpt, frt = g(0x68), g(0x6c)
        self.data_pages = g(0x80)
        npages = g(0x14)
        self.objs = []
        for i in range(nobj):
            vsize, base, flags, pidx, pcnt, _ = struct.unpack_from('<IIIIII', d, h+objtab+i*24)
            self.objs.append(dict(vsize=vsize, base=base, flags=flags, pidx=pidx, pcnt=pcnt))
        self.mem = {}
        for i, o in enumerate(self.objs):
            buf = bytearray(o['vsize'] + 0x1000)
            for p in range(o['pcnt']):
                pg = o['pidx'] - 1 + p
                size = self.page_size if pg != npages - 1 else self.last_page
                src = self.data_pages + pg * self.page_size
                buf[p*self.page_size:p*self.page_size+size] = d[src:src+size]
            self.mem[i+1] = buf
        # fixups (internal references, 32-bit offset and 16:32 types)
        self.fixups = []
        for i, o in enumerate(self.objs):
            for p in range(o['pcnt']):
                pg = o['pidx'] - 1 + p
                a, b = struct.unpack_from('<II', d, h+fpt+pg*4)
                q = h + frt + a; end = h + frt + b
                while q < end:
                    src, flg = d[q], d[q+1]; q += 2
                    cnt = None
                    if src & 0x20: cnt = d[q]; q += 1
                    else: soff = struct.unpack_from('<h', d, q)[0]; q += 2
                    if flg & 3 != 0: raise ValueError('import fixup unsupported')
                    if flg & 0x40: tobj = struct.unpack_from('<H', d, q)[0]; q += 2
                    else: tobj = d[q]; q += 1
                    st = src & 0xf
                    if st == 2: toff = 0
                    elif flg & 0x10: toff = struct.unpack_from('<I', d, q)[0]; q += 4
                    else: toff = struct.unpack_from('<H', d, q)[0]; q += 2
                    offs = [soff] if cnt is None else []
                    if cnt is not None:
                        for _ in range(cnt): offs.append(struct.unpack_from('<h', d, q)[0]); q += 2
                    for so in offs:
                        self.fixups.append((i+1, p*self.page_size+so, st, tobj, toff))
        for obj, at, st, tobj, toff in self.fixups:
            if at < 0: continue
            tgt = self.objs[tobj-1]['base'] + toff
            if st == 7:
                self.mem[obj][at:at+4] = struct.pack('<I', tgt & 0xffffffff)
            elif st == 8:  # 32-bit self-relative
                src = self.objs[obj-1]['base'] + at + 4
                self.mem[obj][at:at+4] = struct.pack('<i', tgt - src)
            elif st == 6:
                self.mem[obj][at:at+4] = struct.pack('<I', tgt); 

    def linear(self, obj, off): return self.objs[obj-1]['base'] + off
    def read(self, lin, n):
        for i, o in enumerate(self.objs):
            if o['base'] <= lin < o['base'] + o['vsize']:
                return bytes(self.mem[i+1][lin-o['base']:lin-o['base']+n])
        raise KeyError(hex(lin))
