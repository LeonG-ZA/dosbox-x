#!/usr/bin/env python3
"""Disassemble named functions of a Watcom LE EXE using symbols from its .SYM file.
Usage: disas_le.py EXE SYM REGEX     (regex matched against short symbol names)
       disas_le.py EXE SYM --io      (list every in/out instruction, by containing function)"""
import sys, re, bisect, capstone
sys.path.insert(0, __import__('os').path.dirname(__file__))
import le, watsym

def load(exe, sym):
    L = le.LE(exe)
    syms = []
    for (seg, off, short), full in watsym.symbols(sym):
        syms.append((L.linear(seg, off), short, seg))
    syms.sort()
    return L, syms

def main():
    exe, sym, what = sys.argv[1:4]
    L, syms = load(exe, sym)
    addrs = [a for a, *_ in syms]
    names = {a: n for a, n, _ in syms}
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    code_obj = L.objs[0]; base = code_obj['base']; code = bytes(L.mem[1][:code_obj['vsize']])
    funcs = [(a, n) for a, n, s in syms if s == 1]
    fa = [a for a, _ in funcs]
    def owner(a):
        i = bisect.bisect_right(fa, a) - 1
        return funcs[i][1] if i >= 0 else '?'
    def note(ins):
        m = re.search(r'0x([0-9a-f]+)', ins.op_str)
        if m and int(m.group(1), 16) in names: return '  ; ' + names[int(m.group(1), 16)]
        return ''
    if what == '--io':
        # linear sweep per function
        for i, (a, n) in enumerate(funcs):
            end = funcs[i+1][0] if i+1 < len(funcs) else base + len(code)
            for ins in md.disasm(code[a-base:end-base], a):
                if ins.mnemonic in ('in', 'out', 'insb', 'insw', 'insd', 'outsb', 'outsw', 'outsd', 'rep insw', 'rep outsw', 'rep insd', 'rep outsd'):
                    print(f'{ins.address:08x} {n:<34} {ins.mnemonic} {ins.op_str}')
        return
    rx = re.compile(what)
    for i, (a, n) in enumerate(funcs):
        if not rx.search(n): continue
        end = funcs[i+1][0] if i+1 < len(funcs) else base + len(code)
        print(f'\n===== {n} @ {a:08x}')
        for ins in md.disasm(code[a-base:end-base], a):
            print(f'{ins.address:08x}  {ins.mnemonic:<6} {ins.op_str}{note(ins)}')

if __name__ == '__main__': main()
