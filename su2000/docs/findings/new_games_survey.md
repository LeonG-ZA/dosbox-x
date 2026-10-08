# Survey: Boxing, Zone Hunter, Ghost Train (local HDD dump)

Source: the user's local copy of `VR2000 HDD Dump (Faulty's).7z` (4,240 files). It is a larger dump than the 1,785-file archive used
in the cloud session and adds `\BOX`, `\ZONE` and `\GHOST`. The SP, DN2 and SFL firmware hashes are identical in both dumps.

Reproduce:

```
python -I tools/coff88k.py data/original/hdd/<GAME>/MAINA.OUT          # build date, sections, symbols
python -I tools/fw_dispatch.py data/original/hdd/<GAME>/MAINA.OUT      # FIFO opcode table
c:\utils\version c:\<game>\<game>.exe   (inside DOSBox-X)               # library versions
```

## Firmware builds (all programs on the disk)

Hashes cover every loaded section (address + bytes), as in `firmware_comparison.md`.

| Program | CPU A build | A bytes / SHA-256 (16) | CPU B build | B bytes / SHA-256 (16) | A opcodes |
|---|---|---|---|---|---|
| SP (supervisor) | 1994-02-01 | 63360 `5765715532093a40` | 1994-02-01 | 25400 `4418e9e8e5c80963` | 43 |
| **Boxing** | 1994-06-25 | 55136 `7afd470d23c78e74` | 1994-06-27 | 49360 `364903abcee83858` | 50 |
| **Zone Hunter** | 1994-07-07 | 56032 `e0ca7b63d3aed72b` | 1994-07-07 | 50856 `167578541520d858` | 50 |
| DN2 | 1995-02-21 | 60088 `821ada366e3621cf` | 1995-02-21 | 63800 `befd923e0087f8c9` | 66 |
| ETS (embedded) | 1995-06-07 | 66080 `1af7f0c4f2799a61` | (uses `\MAINB.OUT`) | | 67 |
| SFL | 1995-10-04 | 66440 `451267824722a48f` | 1995-10-04 | 64176 `70b393806755bedd` | 68 |
| **Ghost Train** | 1995-10-23 | 66080 `166bf6619fe606ce` | 1995-10-23 | 64176 `4e7d0f2d5f7ba4cf` | 67 |

Every game ships its own build, and seven distinct CPU A builds exist on this disk. Ghost Train is the newest. Its symbol set equals
SFL's except that `CmdSXTrapezoid` (opcode 0x43) is missing, and its CPU B has the same symbols as SFL's but different bytes.
[confirmed]

## Command protocol across all versions

`fw_dispatch.py` over the six MAINA files (SP, BOX, ZONE, DN2, SFL, GHOST): opcodes 0x00–0x2A are the same everywhere except:

| Opcode | 1994 (SP, BOX, ZONE) | 1995 (DN2, ETS, SFL, GHOST) | Notes |
|---|---|---|---|
| 0x28 | `CmdDrawTestImage` | `CmdDrawBufferOffset` | redefined (SP had 43 opcodes) |
| 0x30 | `CmdInitFade` (BOX); unnamed (ZONE) | unnamed (host: `PIX_RenderToTexture`) | redefined |
| 0x31 | `CmdDoFade` (BOX); unnamed (ZONE) | unnamed | redefined |
| 0x2B–0x2F | present from BOX on (`ProcMaterialRule`, `CmdSetDepthCue`, `CmdInitVic`, `CmdPlayVic`) | same | added mid-1994 |
| 0x32–0x41 | — | screen fades, melt, FLIC, `CmdModelID`, static, `CmdCopyArea`, translucent rectangle, depth cue off | added by Feb 1995 |
| 0x42, 0x43 | — | `CmdSetTextureClipAngle` (SFL, GHOST), `CmdSXTrapezoid` (SFL only) | added Oct 1995 |

So the protocol is append-only except for three slots (0x28, 0x30, 0x31) that changed meaning between the 1994 and 1995
generations. An HLE of CPU A would need a per-generation table; the hybrid (CPU A interpreted) is unaffected. [confirmed]

## Board-side layout differences (1994 vs 1995 firmware)

Found while bringing the three games up in DOSBox-X. Fixed by detecting them from the uploaded code (`PixBoard::find_b_entry_points`,
`find_a_layout`).

| Item | 1994 (BOX / ZONE) | 1995 (DN2 / SFL / GHOST) | Where found |
|---|---|---|---|
| A→B draw-list slot | global +0xAC (BOX), +0xB8 (ZONE) | +0x100 | MAINB idle loop `ld r28,r30,$X / cmp / bb1 2` (BOX 0xaa74) |
| 16-bit pixel flag in board 0x2104 | bit 8 | bit 9 | MAINB `bb0 N,r26` choosing draw32B/draw16B (BOX 0xaa94) |
| buffer size in 8 KB rows | global +0x4C | +0x50 | MAINA clear loop after `or.u r29,r29,$4f00` |
| stride / draw buffer | +0x40 / +0x38 | same | `draw32B` prologue |
| draw32B dispatch head | +0x2C | +0x2C | |
| SP firmware | single `drawB` routine, no idle loop of this form | | not supported by the B hook (interpreted only) |

## Host library versions (`UTILS\VERSION.EXE`)

| Program | Experience | PIX | TRK | CTRL | NET | SND | CD | EXP | Other |
|---|---|---|---|---|---|---|---|---|---|
| SP | v01.07.00 | v01.03.16 | — | v01.03.25 | — | — | v01.09.01 | v01.02.06 | VPC v00.00.26, BARK tool v00.00.04 |
| **BOX** | v04.00.00 | v01.03.57 | v01.04.00 | v01.03.25 | v02.09.00 | v00.01.26 | v01.09.03 | v01.02.09 | VPC v00.00.28, STAT v00.01.02, VOCALIZER/ZOOM v00.00.03 |
| **ZONE** | v00.06.00 | v01.03.60 | v01.04.00 | v01.03.25 | v02.09.00 | v00.01.26 | v01.09.03 | v01.02.09 | VPC v00.00.28, STAT v00.01.02 |
| DN2 | v02.04.00 | v01.05.46 | v01.04.16 | v01.03.40 | v00.02.21 | v00.01.49 | v01.27.00 | v01.02.23 | STAT v00.01.13, MISC v00.01.05, VEL v01.00.00 |
| ETS | v02.03.08 | v01.05.65 | v01.04.35 | v01.03.47 | v00.02.29 | v00.01.56 | v00.01.35 | v01.02.25 | MISC, VEL; **PIXA/PIXB kernel v01.05.65** (the embedded firmware) |
| SFL | v01.25.02 | v01.05.66 | v01.04.42 | v01.03.51 | v00.02.29 | v00.01.56 | v00.01.34 | v01.02.26 | MISC v00.01.07, VEL v01.00.00 |
| **GHOST** | v01.00.11 | v01.05.67 | v01.04.42 | v01.03.51 | v00.02.31 | v00.01.56 | v00.01.34 | v01.02.26 | STAT v00.01.13, MISC, VEL |

Two library generations: 1994 (PIX 1.03.x, NET 2.09, VPC library) and 1995 (PIX 1.05.x, NET 0.02.x renumbered, MISC/VEL added).
PIX library 1.03.x pairs with the 1994 firmware and 1.05.x with the 1995 firmware.

## First runs in DOSBox-X (hybrid emulation, Windows build, 45 s each)

| Game | Result |
|---|---|
| Ghost Train (`ghost`) | runs: title screen, loading bar, rotating VR logo, "INSERT CREDIT", both channels (966 frames) |
| Boxing (`box`) | title screen and 3D boxing ring render on both channels. **Fixed:** CPU A trapped (vector 0x80, `tb0` in the command loop at 0x4A8CC) on an out-of-range opcode (0x3F80) because the emulated FIFO dropped words when full. The 1994 PIX library (1.03.x) does not throttle on the FIFO flags; the real board holds the ISA write (IOCHRDY) until there is room. `fifo_deliver` now waits for space. Runs 90 s cleanly (4.7 M FIFO words, the FIFO filled ~235 k times) |
| Zone Hunter (`zonerun`) | plain `zone` cannot find its room data; run it through `ZONERUN.BAT` (`zone /s /c /T /D3`). **Fixed:** CPU B jumped into the vector table after a 32-bit bitmap record handled by the experimental HLE: the HLE resumed CPU B at the dispatch head, skipping the reload of the jump-table base (r6). The HLE now resumes at head − 8 like the original handlers, and `pix hle` defaults to false. Attract mode (3D scenes, high scores, Virtuality logo) runs on both channels: 2,194 frames in 90 s. It prints "number of CTRL cards is 0, update CONFIG.VPC" (needs the format/control card, Milestone 3). A yellow strip appears at the right of each channel, possibly frame-buffer area outside the real display window [unverified] |
