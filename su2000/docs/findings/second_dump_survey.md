# Second HDD dump: "VR 2000 Series Disc Image (Simon's)" (SU2000 Solo)

Source: `VR 2000 Series Disc Image (Simon's).zip` (user, 6,796 files, file dates 2014-01-22). Extracted unmodified to
`data/original/simon/`. Working copy for runs: `data/work/simon`, config `dosbox-su2000-solo.conf`.

## Machine configuration: SU2000 Solo [confirmed: C:\CONFIG.VPC, "SU2000 Solo version, last updated 12 october 95"]

* **One video channel driven by two processor cards**, each rendering half of the picture:
  `channel1 / video1 BUSA / proc 0x300,1,0.0,0.5 / proc 0x360,2,0.5,1.0`. The 2-player cabinet of the first dump uses one card per
  player instead. This explains the per-frame load counters the host reads back (`PIXI_LoadManagement`): the split between
  the two cards is rebalanced every frame. [inferred from the config syntax and the library functions]
* A commented-out "NEW JUMPER SETTING" moves the processor cards to I/O 0x2A0 / 0x2A8.
* One InsideTrak (0x270, sensors 1 and 2), one format card (0x210, 0xE0000), one Soundscape (0x330, IRQ 7), network card still configured.
* AUTOEXEC/CONFIG.SYS: same as the first dump's 1-player version (`vpcgo`, `EMM386 X=C800-E0FF`), US keyboard.

**How the split reaches the cards [confirmed, Solo DN2 trace]:** at initialisation the host writes each card's scan-line band
into board 0x210C (first line) / 0x2110 (last line): card 1 = 0–122, card 2 = 123–246 (of 247 lines). All other per-card values are
identical. MAINA clamps its 3D view window to this band (DN2 MAINA 0x4a9bc / 0x4d7bc: top = max(top, 0x210C),
bottom = min(bottom, 0x2110)). In the emulation both cards still produce identical full frames in the scenes checked (41 sampled
frames, byte-identical), so the clamp either applies only to some windows or the firmware draws full frames anyway. The band was
written only once in a 60 s run (the load counters did not move it).

**Emulation (implemented):** `PixFrame` carries the card's band. The display builds channels from cards: a card whose band starts at
line 0 begins a channel, and a card whose band starts lower contributes its lines to the previous channel, as the video card
combines them. The 2-player machine stays two channels (both bands start at 0) and the Solo machine becomes one picture. Use
`dosbox-su2000-solo.conf` (mounts `data/work/simon`, where `[NET]` is removed from CONFIG.VPC).

| Game (Solo drive) | Result |
|---|---|
| DN2 v02.05 (`dn2`) | menu, title and 3D attract scenes as one combined picture |
| Dactyl Nightmare (`dacrun`) | runs; renders its operator set-up menu and waits for a trigger press (needs the control card) |
| Missile Command (`missrun`) | exits: "Format C not found". Needs the format/control card (Milestone 3) |
| Pac-Man VR (`pacsrun`) | exits: "Initialise_Hardware: (CTRL_Open) Format C not found". Needs the control card |

## Programs

| Folder | Program (from .INF) | Experience | PIX library | Firmware (CPU A build) |
|---|---|---|---|---|
| DAC | Dactyl Nightmare (+ camera) | v01.04.00 | 1.05.65 | 1995-06-07 `MAINA.OUT` (= ETS's embedded image) |
| DN2 | Dactyl Nightmare 2 | **v02.05.00** (first dump: v02.04.00) | 1.05.65 | 1995-06-07 |
| PEARL | "Pearl" (+ camera); `PEARL.MAP` present | v1.00.4J | 1.05.50 | 1995-02-24 `MAINA.OUT` |
| ZONE | Zone Hunter; `ZONE.MAP` present | **v11.03.02** (first dump: v00.06.00) | 1.05.61 | 1995-05-02 `MAINA.OUT` |
| BOX | Virtuality Boxing | **v04.02.00** (first dump: v04.00.00) | 1.05.66 | **1995-11-29, embedded** in BOX.EXE |
| MISS | Missile Command (pre-release) | v01.00.10 | 1.05.66 | 1995-11-29, embedded |
| GUN | Olin Shotgun Experience | v00.11.00 | 1.05.66 | embedded; identical to SFL's 1995-10-04 build |
| PAC | **Pac-Man VR (pre-release, DUO and SOLO)** | v01.01.00 | **1.05.68** | **1996-12-13, embedded** (newest on either disk; `mainA` at 0x6A640) |
| SFL, GHOST, ETS | identical to the first dump (SFL.EXE differs in size, firmware identical) | | | |
| SP | Supervisory Program | v01.08.00 | 1.03.60 | 1994-07-07 (identical to the first dump's Zone Hunter firmware) |
| SMC | `EZSTART.EXE`, `SMCA70.EXE`, `LA.SMC` - a vendor utility package, not a game | | | |
| BOOTDISK | MS-DOS utilities | | | |

Programs whose PIX library prints `PIXA/PIXB library (Processor A/B Kernel)` carry their firmware inside the EXE instead of
`MAINA.OUT`/`MAINB.OUT` (ETS, GUN, MISS, BOX 4.02, PAC).

## Firmware and command protocol

`tools/fw_dispatch.py` on the new builds: Pearl (1995-02-24) 66 opcodes; Zone (1995-05-02), Boxing/Missile Command
(1995-11-29) and Pac-Man (1996-12-13) 67 opcodes. All agree with SFL's numbering. Only 0x42 (`CmdSetTextureClipAngle`, missing in
Pearl) and 0x43 (`CmdSXTrapezoid`, SFL only) vary. **No new conflicts**; the protocol has been stable since early 1995.

Firmware-layout detection (`PixBoard::find_b_entry_points` / `find_a_layout`, applied offline): every new build is found
(draw32B/draw16B dispatch heads, idle slot, 16-bit flag, buffer rows). All 1995/96 builds use slot +0x100, bit 9, rows +0x50; the
1994 SP build uses +0xB8, bit 8, +0x4C.

## New symbol sources

* `PEARL\PEARL.MAP`, `ZONE\ZONE.MAP` (a `ZONE.MAP` is also on the first dump): **Watcom Linker 10.0 map files** with every
  symbol's segment:offset and C prototype, e.g. `0001:00053d50 long near PIX_Open( char const near * )`,
  `0001:0007ad06 long near TRKI_IT_GetData( long, short near * )`. [confirmed] Together with `SFL.SYM` that gives full symbols for
  three games, which helps Milestone 3 (tracker/control-card drivers).
