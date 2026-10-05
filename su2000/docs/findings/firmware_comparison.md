# Milestone 1A/1D — PIX firmware: what is uploaded, and is it the same for every game?

## Answer

**Q1. Same firmware for every game?** No, but it is one product. Each title ships the PIX firmware version it was built against.
All four versions on this disk are builds of the same two programs (`mainA`, `mainB`), share symbol names, and use one
**append-only command protocol** (43 → 66 → 67 → 68 opcodes, one slot redefined in the 1994 build). [confirmed]

## Evidence

### 1A. Static: where the 88110 code is

No scanning heuristics were needed. The 88110 code ships as separate m88k COFF executables (magic `0x016D`, big-endian,
44-byte section headers) **with symbol tables**:

```
python3 tools/coff88k.py data/original/hdd/SFL/MAINA.OUT      # sections + symbols
python3 tools/m88kdis.py data/original/hdd/SFL/MAINA.OUT 4a740 4a790
```

ETS.EXE also embeds two COFF images at file offsets `0xA1E24` (CPU A, 1995-06-07) and `0xB3058` (CPU B, 1995-06-07; its `.text` is
byte-identical to the SFL `MAINB.OUT`). Extract them with `dd`/Python into `data/extracted/` (see the snippet in `docs/findings/dynamic_capture.md`).

### 1C/1D. Dynamic: the bytes actually uploaded

Each game was run under the logging stub, and `tools/extract_upload.py` rebuilt the board memory written before each `RUN_A`/`RUN_B` event:

```
tools/run_game.sh dn2 DN2 "dn2" 90
python3 tools/extract_upload.py logs/dn2.su2k logs/dn2_upload data/original/hdd/DN2/MAINA.OUT data/original/hdd/DN2/MAINB.OUT
```

| Game | CPU A upload (bytes, SHA-256/16 of sections, build date) | CPU B upload | Same as reference (SFL)? | Notes |
|---|---|---|---|---|
| SP (supervisor) | 63360 `5765715532093a40` 1994-02-01 | 25400 `4418e9e8e5c80963` 1994-02-01 | no (oldest) | A .text at 0x8A000, B .text at 0x4A000; A also ships a `.model` section (0x7FF000). **Uploaded bytes = COFF sections: MATCH** (both cards) |
| DN2 | 60088 `821ada366e3621cf` 1995-02-21 | 63800 `befd923e0087f8c9` 1995-02-21 | no | **MATCH** on both processor cards |
| ETS | 66080 `1af7f0c4f2799a61` 1995-06-07 (embedded in ETS.EXE) | 64176 `70b393806755bedd` (= `\MAINB.OUT`) | B yes, A no | A image comes from the EXE, not from a file: **MATCH** with the embedded COFF |
| SFL | 66440 `451267824722a48f` 1995-10-04 | 64176 `70b393806755bedd` 1995-10-04 | reference | not yet captured dynamically: SFL.EXE stops in its C runtime start-up under DOSBox-X ("Not enough memory to allocate file structures") before touching the PIX. Static only |

Hashes cover every loaded section (address + bytes), computed from the COFF files. For SP, DN2 and ETS the dynamic upload was
compared byte-for-byte with those sections (`MATCH`). Logs: `logs/*_upload/`.

**Mid-game uploads.** After `RUN_A`, DN2 wrote **0** bytes into either CPU's `.vecs`/`.text` during 90 s (≈32 000 frames)
(`tools/readback_check.py`). Everything uploaded later is data in the board heap (0x150000 up: FLIC frames into `.fmvmem`,
textures, models, palettes) written through `PIXI_TransferBlock` and referenced by address from FIFO commands. [confirmed for DN2, ETS and SP boot]

### Command protocol across versions

`tools/fw_dispatch.py` finds the opcode jump table in each MAINA `.data` (the longest run of `.text` pointers to `Proc*`/`Cmd*`):

| Firmware | Table address | Opcodes | Conflicts with SFL numbering |
|---|---|---|---|
| SP 1994-02-01 | 0x9441C | 43 | 0x28 = `CmdDrawTestImage` (SFL: `CmdDrawBufferOffset`) |
| DN2 1995-02-21 | 0x546F4 | 66 | none |
| ETS 1995-06-07 | 0x55E54 | 67 | none |
| SFL 1995-10-04 | 0x55FB4 | 68 | — |

The host side agrees: the opcode word each `PIX_Send*`/`PIX_*` function in SFL.EXE puts in the FIFO (`tools/fifo_opcodes.py`)
indexes this table to the handler of the same name (`PIX_SendModelPOS` → 0x0E → `ProcModelPOS`, …). DN2's per-frame opcode 0x39,
which SFL's library never sends, is `CmdGenerateStatic` in every firmware ≥ 1995.

### x86 side: shared library or per-game code?

The board driver is a **shared library** ("PIX library", with TRK, CTRL, SND, NET, CD, EXP, VEL, MISC libraries) linked into each game:

* `UTILS\VERSION.EXE` prints the embedded library versions: SFL = PIX v01.05.66, DN2 = PIX v01.05.46 (DN2's start-up banner).
* `tools/libmatch.py` (masked byte signatures of SFL's named functions) finds 376 of 1101 SFL library functions in DN2.EXE
  (335 unique), including `PIXI_LoadCoffFile` and all of `PIX_Send*`. The rest differ by compiler or library revision.
  For ETS.EXE (an older build with different code generation) only 65 of 438 match.

BSim/BinDiff were not needed for this answer. Function-level matching for ETS/SP is left for Milestone 2 if required.
