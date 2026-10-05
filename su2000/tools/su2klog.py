#!/usr/bin/env python3
"""Decode a DOSBox-X SU2000 bus trace (.su2k, 24-byte records written by src/hardware/su2000/su2000.cpp).
Usage: su2klog.py TRACE [--fifo] [--no-mem] [--summary]
  default    : one line per record
  --fifo     : decode the PIX broadcast FIFO into commands (opcode table from PROTOCOL.md)
  --summary  : per-port / per-kind counts"""
import struct, sys, collections

KIND = {1: 'IN ', 2: 'OUT', 3: 'MRD', 4: 'MWR', 5: 'FIFO', 6: 'EVT'}
EVT = {1: 'RUN_A', 2: 'RUN_B', 3: 'RESET', 4: 'FAKE_WRITE'}
REC = struct.Struct('<BBHIIIII')

def records(path):
    d = open(path, 'rb').read()
    for i in range(0, len(d) - REC.size + 1, REC.size):
        k, w, cs, eip, addr, val, aux, t = REC.unpack_from(d, i)
        yield dict(kind=k, width=w, cs=cs, eip=eip, addr=addr, value=val, aux=aux, t=t)

# opcode -> (name, argument layout). w=word, f=float(2 words), l=long(2 words), *=variable (rest until next opcode unknown)
OPS = {
 0x01: ('ViewMAT', None), 0x02: ('ViewROT', None), 0x03: ('ViewPOS', None), 0x04: ('ViewLIST', None),
 0x05: ('Window', None), 0x06: ('Palette', None), 0x07: ('LightSource', None), 0x08: ('Texture', None),
 0x09: ('WindowSwitch', None), 0x0a: ('Model', None), 0x0b: ('ModelID', None), 0x0c: ('ModelMAT', None),
 0x0d: ('ModelROT', None), 0x0e: ('ModelPOS', 'wfff'), 0x0f: ('ModelPALETTE', None), 0x10: ('ModelOTHER', None),
 0x11: ('Render', ''), 0x12: ('RenderSwap', ''), 0x13: ('SetDrawBuffer', None), 0x14: ('SetDisplayBuffer', None),
 0x15: ('SwapBuffers', ''), 0x16: ('SwapClearBuffers', ''), 0x17: ('DrawPoint', None), 0x18: ('DrawLine', None),
 0x19: ('DrawCircle', None), 0x1a: ('DrawRectangle', None), 0x1b: ('DrawPolyLine', None), 0x1c: ('FillRectangle', None),
 0x1d: ('FillCircle', None), 0x1e: ('FillTrapezoid', None), 0x1f: ('SetPenColor', None), 0x20: ('SetLinePattern', None),
 0x21: ('DrawBitmap', None), 0x23: ('SetBufferClearMode', None), 0x24: ('SetClippingWindow', None), 0x25: ('ReadPixel', None),
 0x26: ('ModelSCALE', None), 0x27: ('SetPixel', None), 0x28: ('SetDrawBufferOffset', None), 0x29: ('ModelPriority', None),
 0x2a: ('ModelDATA', None), 0x2b: ('Material', None), 0x2c: ('SetDepthCue', None), 0x2d: ('SyncRender', ''),
 0x30: ('RenderToTexture', None), 0x36: ('FLIC', None), 0x37: ('ModifyModelID', None), 0x38: ('RenderToBitmap', None),
 0x3a: ('SyncRenderFLIC', ''), 0x3b: ('CopyArea', None), 0x3d: ('SetZoomFactor', None), 0x3e: ('DownloadColourPalettes', None),
 0x3f: ('SetTwinkleFrame', None), 0x40: ('FillTranslucentRect', None), 0x41: ('DisableDepthCue', ''), 0x42: ('SetTextureClipAngle', None),
}

def fifo_words(path):
    for r in records(path):
        if r['kind'] == 5: yield r['value'] & 0xffff, r

def decode_fifo(path):
    """Greedy decode: known layouts are parsed; unknown layouts print raw words up to the next
    plausible opcode boundary (marked '?')."""
    words = list(fifo_words(path)); i = 0
    while i < len(words):
        w, r = words[i]
        name, lay = OPS.get(w, (None, None))
        if name is None:
            print(f'{r["t"]:>10}us  ?? {w:#06x}'); i += 1; continue
        if lay is None:
            j = i + 1; raw = []
            while j < len(words) and words[j][0] not in OPS and j - i < 64: raw.append(words[j][0]); j += 1
            print(f'{r["t"]:>10}us  {name:<20} ' + ' '.join(f'{x:04x}' for x in raw) + ('  (?)' if raw else ''))
            i = j; continue
        args = []; j = i + 1
        for c in lay:
            if c == 'w': args.append(f'{words[j][0]}'); j += 1
            else:
                v = (words[j][0] << 16) | words[j+1][0]; j += 2
                args.append(f'{struct.unpack("<f", struct.pack("<I", v))[0]:.3f}' if c == 'f' else f'{v:#x}')
        print(f'{r["t"]:>10}us  {name:<20} ' + ' '.join(args)); i = j

def main():
    a = sys.argv[1:]; path = a[0]
    if '--fifo' in a: return decode_fifo(path)
    if '--summary' in a:
        c = collections.Counter()
        for r in records(path):
            key = (KIND.get(r['kind']), r['addr'] if r['kind'] in (1, 2, 5) else r['addr'] & ~0xfff)
            c[key] += 1
        for (k, ad), n in sorted(c.items(), key=lambda x: (str(x[0][0]), x[0][1])): print(f'{k} {ad:#07x} {n}')
        return
    nomem = '--no-mem' in a
    for r in records(path):
        k = r['kind']
        if nomem and k in (3, 4): continue
        extra = ''
        if k in (3, 4) and r['aux'] != 0xffffffff and 0xd0000 <= r['addr'] < 0xe0000: extra = f' board={r["aux"]:#010x}'
        if k == 6: print(f'{r["t"]:>10}us {r["cs"]:04x}:{r["eip"]:08x} EVT {EVT.get(r["addr"], r["addr"])} val={r["value"]:#x} aux={r["aux"]:#x}'); continue
        print(f'{r["t"]:>10}us {r["cs"]:04x}:{r["eip"]:08x} {KIND.get(k, k)} {r["width"]} {r["addr"]:#07x} = {r["value"]:#0{2+2*r["width"]}x}{extra}')

if __name__ == '__main__': main()
