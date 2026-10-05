# PIX 1000 host interface — working spec (draft 1)

Status: **draft 2: static analysis (SFL.EXE) confirmed dynamically on DN2/ETS/SP** (`docs/findings/dynamic_capture.md`). Every item has a confidence tag and a source. "SFL.EXE @x" means a
linear address in the LE image as loaded by `tools/le.py` (object 1 base 0x10000). Function names come from
the Watcom debug info in `SFL.SYM` (`tools/watsym.py`). Reproduce any listing with:

```
cd su2000/data/original/hdd
python3 ../../../tools/disas_le.py SFL.EXE SFL.SYM '^PIXI_OutLongBCFIFO$'
```

## 1. Board topology

| Item | Value (SU2000 2-player CONFIG.VPC) | Source | Confidence |
|---|---|---|---|
| Broadcast FIFO port | `0x320` | `C:\CONFIG.VPC [PIX] broadcastFIFO` | [confirmed] |
| Processor-card memory window | `0xD0000`, 64 KB | `[PIX] procMem`; window size from `PIXI_TransferBlock` (pages of 0x10000) | [confirmed] |
| Processor card, channel 1 | I/O `0x300`, video bus A | `[PIX] channel1 proc 0x300` | [confirmed] |
| Processor card, channel 2 | I/O `0x360`, video bus B | `[PIX] channel2 proc 0x360` | [confirmed] |
| Video card | I/O `0x340`, mode `WIDEPAL_DUAL_32` | `[PIX] video1` | [confirmed] |
| CPUs per processor card | 2: "A" runs `MAINA.OUT`, "B" runs `MAINB.OUT` | `PIX_InitChannel` loads both, `PIXI_ChannelRunA/B` | [confirmed] |

In the 2-player SU2000 there are **two processor cards (one per player/channel)** sharing one video card
(two outputs, "DUAL"). The "1 player" CONFIG.VPC (`VPCSYS/CONFIG1.VPC`, `CONFV1.VPC`) has one channel.
The original brief describes "2× 88110" as the pair inside one processor card. [confirmed for the pair; the
number of cards per cabinet follows from CONFIG.VPC]

The `EMM386 X=C800-E0FF` exclusion covers exactly: network card `0xC8000` (`[NET] MEM_ADDRESS`), PIX window
`0xD0000-0xDFFFF`, format/control cards `0xE0000` and `0xE0800` (`[CTRL] MEM_ADDRESS`). [confirmed]

## 2. Processor card I/O registers (base P = 0x300 / 0x360)

| Port | Dir | Meaning | Evidence | Confidence |
|---|---|---|---|---|
| P+0 | W | Control. bit0 = run CPU A, bit1 = run CPU B, bit4 = map board memory into the host window, bit5 = cleared by `PIXI_DisableProcCards` | `PIXI_ChannelRunA` @5cb6f `or dl,1`; `RunB` `or dl,2`; `PIXI_EnableChannelMemory` `or dl,0x10`; `Disable…` `and dl,0xef`; `PIXI_DisableProcCards` `and 0xdf` | [confirmed] bits 0,1,4; [inferred] bit5 |
| P+0 | R | FIFO status, active low: bit7 = full, bit6 = half full, bit5 = empty-flag (0 = empty) | `PIXI_FIFOStatus` @5cfd0: mode 0→0x80, 1→0x40, 2→0x20, card flagged when bit is 0. `PIX_Send*` wait while mode 1 flags any card; `PIX_SyncRender` waits until mode 2 flags | [confirmed usage] / [inferred names] |
| P+1 | W | Window page: board address bits 31..24 | `PIXI_SetChannelPage` @5c8d0 | [confirmed] |
| P+2 | W | Window page: board address bits 23..16 | same | [confirmed] |
| P+3 | W | written 1 during reset | `PIXI_ResetProcCards` @80130, `PIXI_ResetChannel` | [confirmed write] / meaning [inferred: reset A] |
| P+4 | W | written 1 during reset | same | meaning [inferred: reset B] |

Reset sequence (`PIXI_ResetProcCards`): `out P+0,0; out P+3,1; out P+4,1` per card, then
`PIXI_WriteChannelWord(all, 0x30000006, 0)`. Board address `0x30000006` is also written by MAINB
(`MAINB.OUT` .text 0xaa58-0xaa60), so it is a board-side register.

## 3. Memory window

* Host window `procMem..procMem+0xFFFF` shows board address `(P1<<24)|(P2<<16)|offset` while P+0 bit4 is set. [confirmed]
* Board memory is big-endian. The host moves **16-bit halfwords**: a 32-bit board long at `a` is written as
  `word[a] = high half`, `word[a+2] = low half` (`PIXI_WriteChannelLong` @5c670/5c683, `PIXI_ReadLong` @5bf50). [confirmed]
* Byte-lane behaviour for 8-bit host accesses is unknown; the stub assumes the bridge swaps lanes. [inferred]
* Block uploads (`PIXI_TransferBlock` @5cc80): set page, enable window, copy, step page every 0x10000 bytes. [confirmed]

### Board memory map (from the COFF headers of MAINA/MAINB, all firmware versions)

| Board address | Use | Source |
|---|---|---|
| 0x00000000 | `.vecsA` exception vectors CPU A (0x1000) | MAINA.OUT section table |
| 0x00001000 | `.vecsB` exception vectors CPU B | MAINB.OUT |
| 0x00002100 / 0x2102 | **ready words** CPU A / CPU B (host polls ≥ 9) | `PIXI_TestProcessorMasks` @80373; MAINA 0x4a764-0x4a780; MAINB 0xaa68 |
| 0x00002158 / 0x215A | channel busy status, host polls until 0 | `PIX_GetChannelStatus` @5cf4b; MAINA 0x4a640, 0x4a8b0 |
| 0x00007000 | `.global` (B) | MAINB.OUT |
| 0x0000A000 | MAINB `.text` (SFL/DN2) | |
| 0x0004A000 | MAINA `.text` (SFL/DN2); SP firmware uses 0x8A000 | |
| 0x00150000 | `.fmvmem` (FLIC/full-motion video buffer, 64 KB) | |
| 0x00161000 | `.pmem` / `palettes` | |
| 0x007FE400-0x007FFFFF | `.font`, `.bitmap`, `.texture`, `.model` tables | |
| 0x10007018 | `hostProfile` (MAINA symbol), separate address space | [inferred] |
| 0x30000006 | board control register | see §2 |

## 4. Boot sequence (`PIX_Open` → `PIX_InitChannel` @5b240)

1. `PIXI_GetFileVersion("\maina.out")`, `("\mainb.out")` from `PIXPATH`; abort with "Bad MAINx.OUT version". [confirmed strings, calls]
2. `PIXI_ResetChannel`, `PIXI_InitVideo` (video card 0x340, `PIXI_ConfigFPGA` loads an FPGA bitstream). [confirmed calls]
3. `WriteChannelLong(0x2100, 0)`, `WriteChannelLong(0x2102, 0)` — clear ready words.
4. `PIXI_LoadCoffFile(mainb.out)` → `PIX_ReadCoff` → `PIXI_TransferBlock` per section; `PIXI_ChannelRunB`.
5. `PIXI_LoadCoffFile(maina.out)`; `PIXI_ChannelRunA`.
6. `PIXI_TestProcessorMasks(mask, 9)`: wait until halfwords 0x2100 and 0x2102 are non-zero, fail if either < 9.
   The firmware stores its CPU's PID revision there (`ldcr r2,cr0; extu r2,r2,7<1>`). [confirmed]
7. `PIXI_SetZoomFactor` sends FIFO opcode 0x3d.

So the board has **no boot ROM dependency for the program**: both CPUs' code is uploaded from the host files. [confirmed]

## 5. Broadcast FIFO command stream

16-bit `OUT` to port 0x320 (`PIXI_OutWordBCFIFO`). Longs and floats are sent as two words, **high half first**
(`PIXI_OutLongBCFIFO` @5ce00, `PIXI_OutFloatBCFIFO` @5ce40; floats are IEEE-754 single). Before each command the host
spins on `PIXI_FIFOStatus(all, 1)` (half-full). Each command starts with an opcode word. Table produced by
`tools/fifo_opcodes.py re/sfl/pix_trk_ctrl.asm`; firmware handler names from the MAINA.OUT symbol table:

| Op | Host function | Firmware handler (MAINA) | Known layout |
|---|---|---|---|
| 0x01 | PIX_SendViewMAT | ProcViewMAT | |
| 0x02 | PIX_SendViewROT | ProcViewROT | |
| 0x03 | PIX_SendViewPOS | ProcViewPOS | |
| 0x04 | PIX_SendViewLIST | ProcViewLIST | |
| 0x05 | PIX_SendWindow | ProcScreen | |
| 0x06 | PIX_SendPalette | ProcPalette | |
| 0x07 | PIX_SendLightSource | ProcLight | |
| 0x08 | PIX_SendTexture | ProcTextureRule | |
| 0x09 | PIX_SendWindowSwitch | ProcScrSwitch | |
| 0x0a | PIX_SendModel | ProcModel | |
| 0x0b | PIX_SendModelID | ProcModelID | |
| 0x0c | PIX_SendModelMAT | ProcModelMAT | |
| 0x0d | PIX_SendModelROT | ProcModelROT | |
| 0x0e | PIX_SendModelPOS | ProcModelPOS | `w model, f x, f y, f z` (@6d550) [confirmed] |
| 0x0f | PIX_SendModelPALETTE | ProcModelPALETTE | |
| 0x10 | PIX_SendModelOTHER | ProcModelOTHER | |
| 0x11 | PIX_SendRender | (render) | none |
| 0x12 | PIX_RenderSwap | | |
| 0x13 | PIX_SetDrawBuffer | CmdDrawBuffer | |
| 0x14 | PIX_SetDisplayBuffer | CmdDisplayBuffer | |
| 0x15 | PIX_SwapBuffers | CmdSwapBuffers | |
| 0x16 | PIX_SwapClearBuffers | CmdSwapClrBuffers | |
| 0x17–0x1e | PIX_DrawPoint/Line/Circle/Rectangle/PolyLine, FillRectangle/Circle/Trapezoid | CmdDraw*/CmdFill* | |
| 0x1f | PIX_SetPenColor | CmdSetPenColor | |
| 0x20 | PIX_SetLinePattern | CmdSetLinePattern | |
| 0x21 | PIX_DrawBitmap | CmdDrawBitmap | |
| 0x23 | PIX_SetBufferClearMode | CmdScreenClearMode | |
| 0x24 | PIX_SetClippingWindow | CmdSetClipWindow | |
| 0x25 | PIX_ReadPixel | CmdGetPixel | **read-back** |
| 0x26 | PIX_SendModelSCALE | ProcModelSCALE | |
| 0x27 | PIX_SetPixel | CmdSetPixel | |
| 0x28 | PIX_SetDrawBufferOffset | CmdDrawBufferOffset | |
| 0x29 | PIX_ModifyModelPriority | CmdModelPriority | |
| 0x2a | PIX_SendModelDATA | ProcModelDATA | |
| 0x2b | PIX_SendMaterial | ProcMaterialRule | |
| 0x2c | PIX_SetDepthCue | CmdSetDepthCue | |
| 0x2d | PIX_SyncRender | | then wait FIFO empty and 0x2158 == 0 |
| 0x30 | PIX_RenderToTexture | | |
| 0x36 | PIX_ReadNextFLICFrame / CloseFLIC | CmdProcessFLIC | |
| 0x37 | PIX_ModifyModelID | CmdModelID | |
| 0x38 | PIX_RenderToBitmap | | |
| 0x3a | PIX_SyncRenderFLIC | | |
| 0x3b | PIX_CopyArea | CmdCopyArea | |
| 0x3d | PIXI_SetZoomFactor | | |
| 0x3e | PIX_DownloadColourPalettes | | |
| 0x3f | PIX_SetTwinkleFrame | | |
| 0x40 | PIX_FillTranslucentRectangle | CmdFillTranslucentRectangle | |
| 0x41 | PIX_DisableDepthCue | CmdDisableDepthCue | |
| 0x42 | PIX_SetTextureClipAngle | CmdSetTextureClipAngle | |

Command lengths measured on DN2 are in `docs/findings/dynamic_capture.md` §1E. The firmware dispatch table (`tools/fw_dispatch.py`)
gives the remaining slots: 0x00 ProcView, 0x22 CmdPrintf, 0x2E CmdInitVic, 0x2F CmdPlayVic, 0x32 CmdInitScrFade, 0x33 CmdUpdateScrFade,
0x34 CmdMeltScreen, 0x35 CmdInitMelt, 0x39 CmdGenerateStatic, 0x3C CmdSetStaticPal, 0x43 CmdSXTrapezoid; unnamed handlers at 0x11, 0x12,
0x2D, 0x30, 0x31, 0x38, 0x3A, 0x3D–0x3F (static functions without symbols).
These are not issued by SFL's host library, but DN2 sends 0x39 every frame.

## 6. Bulk data (not via the FIFO)

Models, textures, bitmaps and palettes are allocated in board memory by a **host-side allocator**
(`PIXI_Malloc`, `PIX_GetFreeMemory`, `PIX_PerformGarbageCollection`) and copied with `PIXI_TransferBlock`
(`PIX_LoadModel`, `PIX_LoadTexture`, `PIXI_MBIN_*` for pre-converted `.BIN` model files). FIFO commands then refer to
them by handle or board address. [confirmed functions; addresses-in-commands inferred]

## 7. Read-backs (complete list from the host library)

| What | How | Use |
|---|---|---|
| FIFO full/half/empty | `in P+0` | flow control |
| CPU ready words 0x2100/0x2102 | window | boot check |
| Channel busy 0x2158/0x215A (PIX lib v1.05.66; **0x214C/0x214E in DN2's v1.05.46**) | window | `PIX_SyncRender`, `PIX_GetRenderStatus` |
| `PIX_GetProcessorStatus` | `PIXI_ReadLong` | diagnostics |
| `PIX_ReadPixel` (op 0x25), `PIX_GetBitmap`, `PIXI_GetBlock` | window | pixel/bitmap read-back |
| `PIXI_LoadManagement` after SyncRender: 6 longs at 0x212C–0x2142 (DN2: 0x2120–0x2136), then regenerates the video line table | window | per-processor load counters used to rebalance scan lines [inferred from the calls] |
| PixMon debugger (`PIX_RunDebugger`, tokens DB/DW/DL…) | window | developer tool |

No read-back of collision or game-state results exists in the library, and none occurs in a 90 s DN2 run. The game logic runs on
the x86 (e.g. `MoveEntity`, `HandleGameOver` in SFL.SYM). [confirmed]

## 8. Other SU2000 cards (Milestone 3 material)

| Card | Address | Library | Notes |
|---|---|---|---|
| InsideTrak ×2 | I/O 0x270, 0x278 | `TRKI_IT_*` | ISA card; library checks an "InsideTRAK" signature string |
| Format/control card ×2 | I/O 0x210/0x218, mem 0xE0000/0xE0800 | `CTRLI_FCD_*` | buttons, joystick, flexor, credits, lights, Visette brightness, volume, mic, security |
| Network | I/O 0x280, mem 0xC8000, IRQ 5 | `NET_*` | multi-player linking |
| Soundscape ×2 | 0x330 IRQ 12, 0x350 IRQ 7 | `SND_*` | firmware `SNDSCAPE.COD` uploaded by host |

## 9. Board side (what the 88110s see) — from MAINA/MAINB, confirmed by running them

| Physical address | Use | Evidence | Confidence |
|---|---|---|---|
| 0x00000000–0x00FFFFFF | shared DRAM (both CPUs and the host window) | COFF section addresses, upload trace | [confirmed] |
| 0x10000000 + a | uncached alias of DRAM `a` (data BATC entry `0x10000039`) | `mainA` 0x4a708 | [confirmed by running] |
| 0x20000006 | FIFO data, 16-bit read pops one word | `waitWordFIFO`, `readWordFIFO` | [confirmed] |
| 0x30000000 | status: bit 29 = FIFO not empty; bits 10..1 = current video line; bits 28..25 printed as card number | `waitWordFIFO`, `waitVBI`, `mainA` banner | [confirmed] / card id [inferred] |
| 0x30000006 | 16-bit control (written by host reset and by `mainB`) | | [confirmed write] |
| 0x40000000–0x403FFFFF | VRAM, 4 MB; frame buffers at 0x40000000 and 0x4006C000 (DN2), palettes at 0x403FC000 | `mainB` clear loop, display pointer 0x200C | [confirmed] |
| 0x48xxxxxx / 0x49xxxxxx (read) | VRAM read transfer: load the 8 KB row at xxxxxx into the serial access memory | `swapBuffersFLIC` / clear code (DN2 0x4d2d0–0x4d380) | [inferred from usage; confirmed by output] |
| 0x4Fxxxxxx (write) | VRAM write transfer: store the SAM into row xxxxxx under a bit mask (the written value) | same | [inferred; confirmed by output] |

Shared globals (both CPUs set r30 = 0x10007000, i.e. physical 0x7000):

| Offset | Use |
|---|---|
| +0x38 | current draw buffer (VRAM address) |
| +0x40 | line stride in bytes (1536 = 768 × 16-bit pixels in DN2) |
| +0x50 | buffer size in 8 KB VRAM rows |
| +0xC8 / +0xCC | front / back buffer |
| +0x100 | **draw-list hand-off**: A stores the list address, B renders it and writes 0 |
| +0x238, +0x240, +0x48, +0x4C | floating-point constants used by the rasteriser (span step, numerator, screen centre) |

Mailboxes in low DRAM: 0x2004–0x2020 video set-up (0x200C = displayed buffer, 0x2020 = VBI line window),
0x2100/0x2102 CPU ready, 0x2104 channel configuration from the host (bit 9 = 16-bit pixels), 0x2108–0x2150 status and
load counters.

Pixel formats: 32-bit 0x00RRGGBB; 16-bit 4:4:4:4 with R in bits 11..8, G 7..4, B 3..0 (from `CmdScreenClearMode`'s
`punpk.b / prot / ppack.16.h` conversion). [inferred; colours look right in DN2]

### CPU A → CPU B draw lists

A builds a list of records in DRAM and hands it to B through global +0x100. Each record starts with an 8-byte header whose signed
top byte is the record type (0 = end of list). `draw32B`/`draw16B` jump through a 56-entry table. Most primitives come in triples
(full set-up, new right edge, new left edge). See `findings/drawlist.md`.
