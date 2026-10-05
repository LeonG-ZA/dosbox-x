# Milestone 1B — how the game EXEs drive the PIX 1000 (x86 side)

Reference binary: `SFL.EXE` (Watcom C/C++, LE, DOS/4GW) with `SFL.SYM` (Watcom debug info). Symbols are pulled from the `.SYM`
by `tools/watsym.py`. The record layout `off32 seg16 type16 kind8 len8 "W?mangled"` was found empirically. Example: `PIX_Open` is at
`SFL.SYM+0x5B47D`, segment 1 offset 0x4B120. Addresses below are linear in the LE image as `tools/le.py` loads it
(object 1 at 0x10000). The full listing of all `PIX*`/`TRK*`/`CTRL*` functions is produced by:

```
cd data/original/hdd && python3 ../../../tools/disas_le.py SFL.EXE SFL.SYM '^(PIX|TRK|CTRL)' > ../../../re/sfl/pix_trk_ctrl.asm
```

(`re/` listings are git-ignored, since they are disassembly of proprietary code.)

## I/O instructions

`tools/disas_le.py SFL.EXE SFL.SYM --io` lists every `in`/`out` per function. The PIX ones are confined to `PIXI_*` primitives.
Ports are never immediates; they come from the parsed CONFIG.VPC (`PIXI_GetConfigAddress()+0xDD`, `+0xB0` per card, `+0xC5D1`
broadcast FIFO). This is why the stub takes its addresses from the `[su2000]` config, which mirrors CONFIG.VPC.

| Function | Address | Port use | Confidence |
|---|---|---|---|
| `PIXI_OutWordBCFIFO` | 0x5CDE0 | `out FIFO, ax` | [confirmed] |
| `PIXI_OutLongBCFIFO` | 0x5CE00 | `out FIFO, ax` ×2, high word first | [confirmed] |
| `PIXI_OutFloatBCFIFO` | 0x5CE40 | same, float bits | [confirmed] |
| `PIXI_FIFOStatus` | 0x5CFD0 | `in al, P+0`, tests 0x80/0x40/0x20 | [confirmed] |
| `PIXI_SetChannelPage` | 0x5C8D0 | `out P+2, A[23:16]`; `out P+1, A[31:24]` | [confirmed] |
| `PIXI_Enable/DisableChannelMemory` | 0x5C980 / 0x5CA40 | `out P+0, ctrl \| 0x10` / `& ~0x10` | [confirmed] |
| `PIXI_ChannelRunA/B` | 0x5CB00 / 0x5CBC0 | `out P+0, ctrl \| 1` / `\| 2` | [confirmed] |
| `PIXI_ResetChannel`, `PIXI_ResetProcCards` | 0x5C490, 0x80130 | `out P+0,0; out P+3,1; out P+4,1` | [confirmed] |
| `PIXI_DisableProcCards` | 0x801F0 | `out P+0, ctrl & ~0x20` | [confirmed] |
| `PIXI_TestProcessorMasks` | 0x80290 | window reads of 0x2100/0x2102 | [confirmed] |
| `PIXI_ConfigFPGA`, `PIXI_InitVideo*`, `PIXI_WriteRam` | 0x7FC40, 0x59920… | video card ports 0x340+ | [confirmed] |

## Memory window

The window is not reached through a DPMI selector. DOS/4GW maps the first megabyte 1:1, so the library uses the flat linear
address `procMem` (`[PIXI_GetConfigAddress()+0xC5D5]`, set to 0xD0000 from CONFIG.VPC). In the DN2 run all window accesses came
from a few `rep movs`/word loops inside `PIXI_TransferBlock` (DN2 runtime 0x1EA6BB = image 0x636BB) and the status readers.
No DPMI 0x0800/0x0002 mapping is needed. [confirmed by trace: CS=0160 flat, window physical addresses 0xD0000–0xDFFFF]

## Upload routine

`PIX_Open` (0x5B120) → `PIXI_ReadConfig` → `PIXI_ResetProcCards` → … → `PIX_InitChannel` (0x5B240):

1. `PIXI_GetFileVersion("<PIXPATH>\maina.out")`, then `mainb.out`. The PIXPATH default is the current directory, so each
   game loads the copy in its own directory. ETS instead uses an image embedded in ETS.EXE.
2. `PIXI_ResetChannel`, `PIXI_InitVideo`.
3. `PIXI_WriteChannelLong(all, 0x2100, 0)`, `(0x2102, 0)`.
4. `PIXI_LoadCoffFile` (0x805C0) → `PIX_ReadCoff` (0x80680): reads COFF headers (`PIX_Rev2`/`PIX_Rev4` byte-swap), then
   `PIXI_TransferBlock(card mask, section vaddr, buffer, size)` per loadable section, in 64 KB pages. B first, then `PIXI_ChannelRunB`.
5. Same for A, then `PIXI_ChannelRunA`.
6. `PIXI_TestProcessorMasks(mask, 9)` waits until the firmware has written its PID revision to 0x2100/0x2102.

The upload source is a plain file per game (`MAINA.OUT`/`MAINB.OUT` next to the EXE). It is broadcast to all processor cards
at once: the trace shows a single write sequence with every card's bit 4 set.

## Per-frame traffic

`PIX_Render`/`PIX_RenderSwap` (0x5D920/0x5D9E0) walk the host-side scene objects (`PIXI_Update{Textures,Models,Views,Windows,Lights,Palette,Materials}`).
For each dirty object they wait for FIFO space and emit the matching `PIX_Send*` command, then send opcode 0x11/0x12.
`PIX_SyncRender` (0x5DD80) sends 0x2D, waits for the FIFO to drain and for 0x2158/0x215A to clear, then calls
`PIXI_LoadManagement`. That function reads 6 longs of per-CPU load counters (0x212C–0x2142) and regenerates the video line
table (`PIXI_GenerateVideoTable`, `PIXI_WriteChannelVideo`), i.e. it rebalances scan lines between the processors.

## Game logic location

Game-play code is in the x86 EXE (`MoveEntity`, `HandleGameOver`, `AddSection`, `INDUCT_SelectPlayer`, `LoadLevelData`, … in SFL.SYM).
The firmware symbol tables contain only rendering, display-list, FLIC, text and debug-print routines (`tranVertList64`,
`boxClip`, `doSort`, `drwList1-4`, `mixClip`, `swapBuffers`, `CmdProcessFLIC`, `prtmatrix`, …). [confirmed by symbol names]
