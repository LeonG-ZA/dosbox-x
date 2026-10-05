# SU2000 emulation research (Virtuality SU2000 in DOSBox-X)

Work area for the project brief "Virtuality SU2000 Emulation in DOSBox-X". The emulator code lives in the normal DOSBox-X
tree under `src/hardware/su2000/` and is off unless `[su2000] enable=true`.

| Path | Content | In git? |
|---|---|---|
| `data/original/hdd/` | read-only extraction of the user's HDD dump | **no** (proprietary) |
| `data/MANIFEST.sha256` | SHA-256 of every original file | no (lives with the data) |
| `data/work/c/` | working copy mounted as C: (`tools/make_workcopy.sh`) | no |
| `data/extracted/` | blobs carved out of EXEs (ETS embedded firmware) | no |
| `logs/` | bus traces (`*.su2k`), screen dumps, rebuilt uploads | no |
| `re/` | generated disassembly, strings, symbol maps, inventory TSV | no (except notes) |
| `tools/` | all analysis scripts (Python 3; `pip install capstone numpy py7zr`) | yes |
| `docs/` | `PROTOCOL.md`, `DECISIONS.md`, `findings/*.md` | yes |
| `dosbox-su2000.conf` | DOSBox-X config for research runs | yes |

## Quick start

```sh
pip install capstone numpy py7zr
mkdir -p su2000/data/original && python3 -c "import py7zr; py7zr.SevenZipFile('VR2000_HDD_Dump_Faultys.7z').extractall('su2000/data/original')"
mv "su2000/data/original/VR2000 HDD Dump (Faulty's)" su2000/data/original/hdd && chmod -R a-w su2000/data/original/hdd
./autogen.sh && ./configure --enable-sdl2 && make -j4        # repo root
cd su2000
python3 tools/inventory.py && python3 tools/make_inventory_md.py
tools/make_workcopy.sh
tools/run_game.sh dn2 DN2 "dn2" 90
python3 tools/extract_upload.py logs/dn2.su2k logs/dn2_upload data/original/hdd/DN2/MAINA.OUT data/original/hdd/DN2/MAINB.OUT
python3 tools/fifo_commands.py logs/dn2.su2k data/original/hdd/DN2/DN2.EXE
```

Interactive run: `../src/dosbox-x -conf dosbox-su2000.conf -c "cd \DN2" -c dn2`.

## Tools

| Script | Purpose |
|---|---|
| `inventory.py`, `make_inventory_md.py` | manifest, file classification, string dumps, inventory report |
| `coff88k.py` | m88k COFF parser (sections, symbols) for `MAINA.OUT`/`MAINB.OUT` |
| `m88kdis.py` | minimal 88100/88110 disassembler and opcode-class histogram |
| `fw_dispatch.py` | firmware FIFO opcode → handler table |
| `le.py` | LE (DOS/4GW) loader with fixups |
| `watsym.py` | global symbols from Watcom debug info (`SFL.SYM`) |
| `disas_le.py` | disassemble named functions / list all `in`/`out` |
| `fifo_opcodes.py` | opcode each host `PIX_*` function sends (→ `opcodes.tsv`) |
| `libmatch.py` | find SFL's library functions in other EXEs (masked signatures) |
| `su2klog.py`, `su2kstats.py` | decode / summarise bus traces |
| `extract_upload.py` | rebuild the bytes uploaded to the board, compare with COFF sections |
| `fifo_commands.py` | split the FIFO stream into commands without symbols |
| `readback_check.py` | gate checks: code writes after boot, read-back addresses |
| `make_workcopy.sh`, `run_game.sh` | working copy, headless runs |

## Status

Milestone 0 and Milestone 1 are done up to **Decision Gate 1** (see `docs/DECISIONS.md`). Recommendation: 2A (HLE).
Waiting for confirmation.
