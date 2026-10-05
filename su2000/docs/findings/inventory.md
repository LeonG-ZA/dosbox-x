# Milestone 0 — inventory of the supplied SU2000 HDD dump

Source: `VR2000_HDD_Dump_Faultys.7z`, extracted unmodified to `data/original/hdd/` (top folder renamed from
`VR2000 HDD Dump (Faulty's)`, files set read-only). 1785 files.
Reproduce: `python3 tools/inventory.py && python3 tools/make_inventory_md.py`
→ `data/MANIFEST.sha256` (SHA-256 of every original), `re/inventory.tsv`, `re/strings/` (ASCII + UTF-16LE), this file.

## Programs on the disk  [confirmed]

| Program | Directory | Main EXE | PIX firmware it uploads | Notes |
|---|---|---|---|---|
| Shoot For Loot | `\SFL` (copies in `\`) | `SFL.EXE` LE/Watcom, 1996-02-06, "SHOOT FOR LOOT experience v01.25.02", PIX lib v01.05.66 | `MAINA/MAINB.OUT` built 1995-10-04 | **`SFL.SYM`: complete Watcom debug info** (2062 global symbols + types) |
| Dactyl Nightmare 2 | `\DN2` | `DN2.EXE` (+ `DN2C.EXE` camera build), PIX lib v01.05.46 | own `MAINA/MAINB.OUT`, 1995-02-21 | |
| Supervisory Program | `\SP` | `SP.EXE` — **Phar Lap TNT (P3)**, not DOS/4GW | own `MAINA/MAINB.OUT`, 1994-02-01 | attract/credit supervisor started by `VPCGO.BAT` |
| Engineering Test System | `\ETS` | `ETS.EXE` LE/Watcom + bound DOS/4G, 1995-07-28 | **embedded** CPU-A image (file offsets 0xA1E24, 0xB3058), built 1995-06-07; CPU B from `\MAINB.OUT` | diagnostics |
| Utilities | `\UTILS` | `MUTE`, `RUNC`, `MAKEBIN`, `EXPCOPY`, `VFF`, `TX`, `INFO`, `VERSION`, `DISPSTAT`, `AM2PC`, `ASC2MFF`, `HEX`, `DOS4GW.EXE` (1994-09) | | `VERSION.EXE x.exe` prints the library versions inside an EXE |
| Launcher | `\UTILS\VPCGO.BAT` | loops `run sp`; SP writes `%vpctemp%\spscript.bat` setting `vpcnext`/`vpcexp`; `RUN.BAT` = `RUNC.EXE` + `runtemp7.bat` | | |

Referenced but **not on this disk**: Boxing (`\box`), Ghost Train (`\ghost`), Zone Hunter (`\zone`, `zhsrun`), Pac-Man (`\pac`, `ETS\RUNTEMP7.BAT`).
`AUTO.BAT` (2007 "VR Menu") jumps on `%CONFIG%` to each game's `*RUN.BAT`.
Hardware map: `C:\CONFIG.VPC` (SU 2-player, 21-07-95); 1-player variants `CONFV1.VPC`, `VPCSYS\CONFIG1.VPC`; keyword list `VPCSYS\KEYWORD.VPC`.

## Executables (MS-DOS 6.22 files in `hdd/DOS/` omitted)

| Path | Size | Type | Compiler | SHA-256 (16) |
|---|---|---|---|---|
| hdd/COMMAND.COM | 54645 | DOS COM |  | `65fa71b0a34e91fe` |
| hdd/DRVSPACE.BIN | 66294 | MZ+LE |  | `4c1fefb3b9a58cd9` |
| hdd/MAINA.OUT | 70590 | m88k COFF executable | Motorola 88k toolchain | `a182dfc0ea5acf9c` |
| hdd/MAINB.OUT | 65536 | m88k COFF executable | Motorola 88k toolchain | `b17e667f75d72d47` |
| hdd/SFL.EXE | 854388 | MZ+LE | Watcom | `4bc4149c80574141` |
| hdd/WINA20.386 | 9349 | MZ+LE |  | `76bc80950f72e583` |
| hdd/CD/MSCDEX.EXE | 25377 | MZ (real mode) | MSC | `441ee3c40ca7622d` |
| hdd/DN2/DN2.EXE | 851144 | MZ+LE | Watcom | `276c9fa0c7177290` |
| hdd/DN2/DN2C.EXE | 674212 | MZ+LE | Watcom | `4ff312a46d0d3fd4` |
| hdd/DN2/MAINA.OUT | 63884 | m88k COFF executable | Motorola 88k toolchain | `2d3422c645d36b64` |
| hdd/DN2/MAINB.OUT | 65160 | m88k COFF executable | Motorola 88k toolchain | `9e0938691392261f` |
| hdd/ETS/ETS.EXE | 1350529 | MZ+LE | Watcom,DOS/4G-stub | `49ac39ffedc2e264` |
| hdd/SFL/MAINA.OUT | 70590 | m88k COFF executable | Motorola 88k toolchain | `a182dfc0ea5acf9c` |
| hdd/SFL/MAINB.OUT | 65536 | m88k COFF executable | Motorola 88k toolchain | `b17e667f75d72d47` |
| hdd/SFL/SFL.EXE | 854388 | MZ+LE | Watcom | `5c12392dd60c3e41` |
| hdd/SFL/BITMAPS/VR2TGA.EXE | 17107 | MZ (real mode) | Borland | `cca6c56587a550ee` |
| hdd/SP/MAINA.OUT | 66510 | m88k COFF executable | Motorola 88k toolchain | `34fdc12612b1d6f2` |
| hdd/SP/MAINB.OUT | 26458 | m88k COFF executable | Motorola 88k toolchain | `e2e714dce2dce11d` |
| hdd/SP/SP.EXE | 401652 | MZ+P3 (Phar Lap) | Watcom,DOS/4G-stub,Borland,PharLap | `0bf76293a1f6cc26` |
| hdd/UTILS/AM2PC.EXE | 25934 | MZ+LE | Watcom | `0d7dd336ab7a348c` |
| hdd/UTILS/ASC2MFF.EXE | 64858 | MZ+LE | Watcom | `c6e60ea60ed10729` |
| hdd/UTILS/DISPSTAT.EXE | 45162 | MZ+LE | Watcom | `9d61537a1d447477` |
| hdd/UTILS/DOS4GW.EXE | 265420 | MZ (real mode) | Watcom,DOS/4G-stub,MSC,PharLap | `dd9f4f342533f995` |
| hdd/UTILS/EXPCOPY.EXE | 73229 | MZ+LE | Watcom | `b2d2f0cb1de9add6` |
| hdd/UTILS/HEX.EXE | 15232 | MZ (real mode) | Watcom,PharLap | `0924d8d949032027` |
| hdd/UTILS/INFO.EXE | 37628 | MZ+LE | Watcom | `fc77eb8f0a6aac72` |
| hdd/UTILS/MAKEBIN.EXE | 82278 | MZ+LE | Watcom | `f5d7929fc46c323e` |
| hdd/UTILS/MUTE.EXE | 63230 | MZ+LE | Watcom | `876b9c019b9000e7` |
| hdd/UTILS/RUNC.EXE | 67578 | MZ+LE | Watcom | `32159808a31b4b49` |
| hdd/UTILS/TX.EXE | 37130 | MZ+LE | Watcom | `6aa72df8217a8b86` |
| hdd/UTILS/VERSION.EXE | 36144 | MZ+LE | Watcom | `aa5d0d164e6de98d` |
| hdd/UTILS/VFF.EXE | 30934 | MZ+LE | Watcom | `3838cdf0f2f72f7e` |

Note: Microsoft VxDs/`DRVSPACE.BIN` are also MZ+LE but are not DOS/4GW programs.

## Other files by extension

| Ext | Count | Bytes | Mean entropy | Guessed type |
|---|---|---|---|---|
| BIN | 487 | 13352539 | 4.30 | pre-converted PIX model/texture binaries (PIXI_MBIN_*, UTILS/MAKEBIN.EXE) [inferred] |
| WAV | 227 | 9877870 | 5.90 | sound samples |
| FLI | 7 | 9286842 | 6.65 | FLIC animation (CmdProcessFLIC) |
| 16B | 227 | 6994209 | 1.81 | 16-bit bitmaps (PIX_LoadBitmap) [inferred] |
| VST | 287 | 6907478 | 2.56 | bitmap/sprite data [inferred] |
| FLC | 1 | 4759750 | 7.64 | FLIC animation |
| TEX | 85 | 4276208 | 2.96 | textures |
| TGA | 29 | 3106323 | 2.20 | Targa images |
| DAT | 3 | 1943876 | 1.77 |  |
| 16T | 38 | 1572514 | 3.60 | 16-bit textures [inferred] |
| OLD | 18 | 1098626 | 4.24 |  |
| SYM | 2 | 867280 | 6.35 | Watcom debug symbols |
| VYW | 8 | 660000 | 1.36 | view files [inferred] |
| ANM | 61 | 466181 | 3.70 | animation data |
| MAT | 8 | 261968 | 3.52 | materials [inferred] |
| COD | 4 | 245760 | 4.00 | Ensoniq Soundscape firmware |
| GIF | 4 | 137422 | 7.87 | images |
| PCX | 2 | 109576 | 4.17 | images |
| SYS | 5 | 105911 | 6.39 |  |
| RTE | 7 | 105000 | 1.57 | routes (LoadRouteData) |
| SMP | 2 | 73190 | 4.23 |  |
| LBM | 2 | 45080 | 4.90 |  |
| NEW | 2 | 35876 | 3.21 |  |
| DN2 | 6 | 32188 | 3.96 | DN2 run files |
| (none) | 8 | 22263 | 2.36 |  |
| DBS | 10 | 18422 | 4.65 | SFL databases (RUNFILES) |
| VED | 1 | 9375 | 1.51 |  |
| CHK | 2 | 8192 | 5.05 | CHKDSK fragments |
| TSC | 1 | 3105 | 4.15 |  |
| PIF | 2 | 1934 | 1.67 |  |
| STE | 1 | 1764 | 3.95 |  |
| NW1 | 2 | 1254 | 5.34 |  |
| BMC | 2 | 522 | 4.66 |  |

## Candidate firmware and blobs  [confirmed]

* `MAINA.OUT` / `MAINB.OUT`: m88k COFF executables (magic 0x016D) **with symbol tables**, the code for the two 88110s on a PIX
  processor card. Distinct builds: SP 1994-02-01, DN2 1995-02-21, ETS (embedded) 1995-06-07, SFL 1995-10-04. Root copies = SFL.
  See `firmware_comparison.md`.
* `SNDSCAPE.COD` (all four copies identical, `b1cfbb78…`): Ensoniq Soundscape firmware (Milestone 4).
* PIX video card FPGA configuration: `PIXI_ConfigFPGA` (SFL.EXE @7fc40) reads a comma-separated text file (`fopen(name,"rt")`, `%d,`)
  whose name comes from the caller. No such file is on this disk and the DN2 run never called it in mode `WIDEPAL_DUAL_32`, so it is
  probably only for other video modes. [inferred]

## String search highlights (`re/strings/`)

`PIX 88110 Monitor` / `PixMon>` (built-in board debugger), `\maina.out`, `\mainb.out`, `** FATAL ** Bad MAINA.OUT version`,
`Trying to open card %ld as an InsideTRAK at address 0x%lx`, `InsideTRAK string not found in correct place`, `Cannot open format card`,
`VISETTE`, `LCD Non-interlaced PAL 32 bits per pixel, 7.389365MHz clock`, `Opening tracker library v%s`.
No serial ports are used by the SU2000 configuration; `SERIAL_PORT`/`FASTRAK` exist only as alternative keywords.
