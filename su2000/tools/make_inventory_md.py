#!/usr/bin/env python3
"""Render docs/findings/inventory.md from re/inventory.tsv (run tools/inventory.py first)."""
import csv, collections, os
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
rows = list(csv.DictReader(open(os.path.join(ROOT, 're/inventory.tsv')), delimiter='\t'))
exe = [r for r in rows if r['type'].startswith(('MZ', 'm88k', 'DOS COM')) and not r['path'].startswith('hdd/DOS/')]
ext = collections.defaultdict(lambda: [0, 0, 0.0])
for r in rows:
    if r['type'].startswith(('MZ', 'm88k', 'DOS COM', 'text')) or r['path'].startswith('hdd/DOS/'): continue
    name = r['path'].split('/')[-1]
    e = name.rsplit('.', 1)[-1].upper() if '.' in name else '(none)'
    x = ext[e]; x[0] += 1; x[1] += int(r['size']); x[2] += float(r['entropy'])
guess = {'BIN': 'pre-converted PIX model/texture binaries (PIXI_MBIN_*, UTILS/MAKEBIN.EXE) [inferred]',
         'VST': 'bitmap/sprite data [inferred]', '16B': '16-bit bitmaps (PIX_LoadBitmap) [inferred]', 'WAV': 'sound samples',
         'TEX': 'textures', 'ANM': 'animation data', '16T': '16-bit textures [inferred]', 'TGA': 'Targa images', 'HLP': 'DOS help',
         'DBS': 'SFL databases (RUNFILES)', 'COD': 'Ensoniq Soundscape firmware', 'VYW': 'view files [inferred]', 'MAT': 'materials [inferred]',
         'RTE': 'routes (LoadRouteData)', 'FLI': 'FLIC animation (CmdProcessFLIC)', 'FLC': 'FLIC animation', 'DN2': 'DN2 run files',
         'CPI': 'DOS code pages', 'GIF': 'images', 'PCX': 'images', 'SYM': 'Watcom debug symbols', 'CHK': 'CHKDSK fragments'}
o = open(os.path.join(ROOT, 'docs/findings/inventory.md'), 'w')
o.write('''# Milestone 0 — inventory of the supplied SU2000 HDD dump

Source: `VR2000_HDD_Dump_Faultys.7z`, extracted unmodified to `data/original/hdd/` (top folder renamed from
`VR2000 HDD Dump (Faulty's)`, files set read-only). 1785 files.
Reproduce: `python3 tools/inventory.py && python3 tools/make_inventory_md.py`
→ `data/MANIFEST.sha256` (SHA-256 of every original), `re/inventory.tsv`, `re/strings/` (ASCII + UTF-16LE), this file.

## Programs on the disk  [confirmed]

| Program | Directory | Main EXE | PIX firmware it uploads | Notes |
|---|---|---|---|---|
| Shoot For Loot | `\\SFL` (copies in `\\`) | `SFL.EXE` LE/Watcom, 1996-02-06, "SHOOT FOR LOOT experience v01.25.02", PIX lib v01.05.66 | `MAINA/MAINB.OUT` built 1995-10-04 | **`SFL.SYM`: complete Watcom debug info** (2062 global symbols + types) |
| Dactyl Nightmare 2 | `\\DN2` | `DN2.EXE` (+ `DN2C.EXE` camera build), PIX lib v01.05.46 | own `MAINA/MAINB.OUT`, 1995-02-21 | |
| Supervisory Program | `\\SP` | `SP.EXE` — **Phar Lap TNT (P3)**, not DOS/4GW | own `MAINA/MAINB.OUT`, 1994-02-01 | attract/credit supervisor started by `VPCGO.BAT` |
| Engineering Test System | `\\ETS` | `ETS.EXE` LE/Watcom + bound DOS/4G, 1995-07-28 | **embedded** CPU-A image (file offsets 0xA1E24, 0xB3058), built 1995-06-07; CPU B from `\\MAINB.OUT` | diagnostics |
| Utilities | `\\UTILS` | `MUTE`, `RUNC`, `MAKEBIN`, `EXPCOPY`, `VFF`, `TX`, `INFO`, `VERSION`, `DISPSTAT`, `AM2PC`, `ASC2MFF`, `HEX`, `DOS4GW.EXE` (1994-09) | | `VERSION.EXE x.exe` prints the library versions inside an EXE |
| Launcher | `\\UTILS\\VPCGO.BAT` | loops `run sp`; SP writes `%vpctemp%\\spscript.bat` setting `vpcnext`/`vpcexp`; `RUN.BAT` = `RUNC.EXE` + `runtemp7.bat` | | |

Referenced but **not on this disk**: Boxing (`\\box`), Ghost Train (`\\ghost`), Zone Hunter (`\\zone`, `zhsrun`), Pac-Man (`\\pac`, `ETS\\RUNTEMP7.BAT`).
`AUTO.BAT` (2007 "VR Menu") jumps on `%CONFIG%` to each game's `*RUN.BAT`.
Hardware map: `C:\\CONFIG.VPC` (SU 2-player, 21-07-95); 1-player variants `CONFV1.VPC`, `VPCSYS\\CONFIG1.VPC`; keyword list `VPCSYS\\KEYWORD.VPC`.

## Executables (MS-DOS 6.22 files in `hdd/DOS/` omitted)

| Path | Size | Type | Compiler | SHA-256 (16) |
|---|---|---|---|---|
''')
for r in exe:
    o.write(f"| {r['path']} | {r['size']} | {r['type']} | {r['compiler']} | `{r['sha256'][:16]}` |\n")
o.write('\nNote: Microsoft VxDs/`DRVSPACE.BIN` are also MZ+LE but are not DOS/4GW programs.\n\n## Other files by extension\n\n| Ext | Count | Bytes | Mean entropy | Guessed type |\n|---|---|---|---|---|\n')
for e, (n, b, h) in sorted(ext.items(), key=lambda x: -x[1][1]):
    o.write(f'| {e} | {n} | {b} | {h/n:.2f} | {guess.get(e, "")} |\n')
o.write('''
## Candidate firmware and blobs  [confirmed]

* `MAINA.OUT` / `MAINB.OUT`: m88k COFF executables (magic 0x016D) **with symbol tables**, the code for the two 88110s on a PIX
  processor card. Distinct builds: SP 1994-02-01, DN2 1995-02-21, ETS (embedded) 1995-06-07, SFL 1995-10-04. Root copies = SFL.
  See `firmware_comparison.md`.
* `SNDSCAPE.COD` (all four copies identical, `b1cfbb78…`): Ensoniq Soundscape firmware (Milestone 4).
* PIX video card FPGA configuration: `PIXI_ConfigFPGA` (SFL.EXE @7fc40) reads a comma-separated text file (`fopen(name,"rt")`, `%d,`)
  whose name comes from the caller. No such file is on this disk and the DN2 run never called it in mode `WIDEPAL_DUAL_32`, so it is
  probably only for other video modes. [inferred]

## String search highlights (`re/strings/`)

`PIX 88110 Monitor` / `PixMon>` (built-in board debugger), `\\maina.out`, `\\mainb.out`, `** FATAL ** Bad MAINA.OUT version`,
`Trying to open card %ld as an InsideTRAK at address 0x%lx`, `InsideTRAK string not found in correct place`, `Cannot open format card`,
`VISETTE`, `LCD Non-interlaced PAL 32 bits per pixel, 7.389365MHz clock`, `Opening tracker library v%s`.
No serial ports are used by the SU2000 configuration; `SERIAL_PORT`/`FASTRAK` exist only as alternative keywords.
''')
