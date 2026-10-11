# Rendition Vérité V1000 emulation

Sources: `src/hardware/verite_core.cpp`/`.h` (RISC, pixel engine, host interface; no
DOSBox-X dependencies) and `src/hardware/vga_verite.cpp` (board: PCI, display, RAMDAC,
PLL, VESA hooks, scheduling). Config: `machine=svga_verite` plus the `[verite]` section.
Host-side test: `tests/verite/build.bat` (MSVC) or `tests/verite/run.sh` (gcc).

## Approach: low-level emulation of the RISC

The original brief planned high-level emulation: accept the uploaded microcode without
running it, and reimplement each recognised command. That premise assumed there was no
usable chip documentation. That turned out to be wrong:

- The **Rendition V2200 Technical Specification** (1159-001 RevB 1/98, 69 pages,
  "including RISC programming") documents the whole RISC instruction set, the register
  file, the pixel engine registers and the host I/O registers. It also describes a
  **V1000 compatibility mode**: the `step` / `draw` / `rdraw` instructions that V1000
  microcode uses.
- The V1000 2D microcode shipped with the X.org rendition driver (`v10002d.uc`) uses 79
  distinct opcodes, all documented. It uses none of the V2x00-only triangle or fill
  engine instructions.
- Host drivers control the RISC by forcing single instructions into it through the
  Debug, StateIndex and StateData registers (X.org `v1krisc.c`), so even HLE would need
  a RISC interpreter.
- `.UC` microcode files are big-endian ELF executables with full symbol tables, which
  makes the microcode easy to read when debugging.

The emulation therefore runs whatever microcode the guest uploads, instruction by
instruction. The project owner chose "LLE now, HLE fast paths later only if profiling
needs them".

Nothing closed-source is used at runtime: the microcode comes from the guest's own driver
or game, and DOSBox-X ships none. Rendering is CPU-side software, as for the S3D and
PowerVR work.

## What is emulated

- **PCI device** `1163:0001` (V1000), VGA class. BAR0 is the 16MB local memory window and
  BAR1 is 256 bytes of I/O. Both can be relocated by the guest. VGA ports `3DEh`/`3DFh`
  return the memory and I/O bases, as on the real chip.
- **Host interface**:
  - the input and output FIFOs with their four byte-swap ports;
  - FIFOInFree and FIFOOutValid;
  - Comm, MemEndian, Intr/IntrEn, Debug (soft reset, hold, single step) and Status;
  - StateIndex/StateData (decode IR/PC/S1 read back, forced instruction write);
  - PCI bus master DMA command lists;
  - the Mode register (VGA/native, VESA window, DMA enable) and BankSelect.
- **RISC**:
  - every instruction in [SPEC] tables 2.3–2.6, 2.8 and 2.9: scalar, vector and
    vector-scalar ALU ops, multiplies, f2i, clip, seed/seedsr, sort, loads and stores
    (including the sy/scr/scb byte scatters), branches and jumps with delay slots;
  - the special registers: FIFO, swfifo, npfifo, seed, scale, seg (indirect register
    access), flag, clip, timer and riscintr;
  - the control registers and pixel engine state mapped at `FFE00400h`+.
- **Pixel engine** (V1000 compatibility mode):
  - all spr registers and bit-field sprs;
  - step/stepv for X, Y, Z, U, V, Q, R, G, B, A, I, F and Cnt;
  - parallel draws (drawp/rdrawp/drawpxy/rdrawpxy) with ROPs, plane mask, patterns,
    chroma key, scissor, and source blits in both directions;
  - parameterized draws (draw1–5/rdraw1–5) with DrawCtl parameter pairs;
  - textures in every source format (including 4-bit indexed through Tex4Pal and YUV),
    point and bilinear sampling, wrap and clamp, and perspective correction through Q;
  - decal and modulate, fog, alpha test, Z buffer (all compare modes), blending,
    YUV→RGB, dithering, and pick.
- **Display**:
  - the native CRTC: timing, frame base latched at vertical sync, StrideOffset, line
    doubling, CRTCStatus/CRTCTest scan position, and the FrameDisplayed/FrameSwitched
    flags;
  - the V1000's external Bt485-compatible RAMDAC: palette through DOSBox-X's DAC, 6/8-bit
    width, and the 64×64 or 32×32 hardware cursor. In native 8bpp the pixel indexes the
    palette directly (through the pixel read mask only), not through the VGA attribute
    controller;
  - the external pixel PLL, programmed serially through I/O `C0h`.
  - Native mode is drawn through a small generic `vga.native` override in `vga_draw.cpp`
    that mirrors the existing DOSBox-IG hooks.
- **VESA BIOS**: as on the real Rendition BIOS, VESA modes put the chip into native mode:
  - 8/15/16/32bpp, with MemEndian set so the frame buffer looks linear to x86 code;
  - LFB at BAR0, the 64KB window through BankSelect;
  - VBE 4F05/4F06/4F07 and the protected-mode window function.
  - Standard VGA modes 00h–13h use DOSBox-X's VGA emulation.
- **Rendition BIOS extensions** (INT 10h AH=15h), the vital product data in the VGA ROM
  and VBE mode 7Fh. Speedy3D games need these to find and start the board (see below).
- **CGL**: a built-in driver for games written for Creative's 3D Blaster PCI, which can't
  run Creative's own driver here (see the CGL section).

## Behaviour [SPEC] leaves open, and what is assumed

Each assumption was checked against the X.org V1000 microcode where it exercises the
feature.

- `getpc`, `jmpl`, `rjmpl` and `jmprl` link to their own address + 8. The X.org
  context-switch monitor's dispatch table only works this way.
- The seed ROM returns `floor(2^62 / m)` for the normalised operand. The 2D microcode's
  line setup refines it with one Newton step in 0.24 fixed point, which only converges
  with this scaling. It is computed exactly rather than from a 13-bit ROM index.
- `draw`/`drawp` consume Cnt for the pixels drawn; `rdraw`/`rdrawp` draw Cnt pixels and
  leave Cnt at 0. Pixels taken from XShadow advance it one pixel in XDir.
- Parallel draws walk the source one pixel per destination pixel in XDir. The dU value
  the microcode loads (0 or `FFF80000h`) is not used.
- SrcMask (UMask/VMask) applies to every source read, parallel draws included, as [SPEC]
  4.2 says for U and V entering the pixel engine. The 2D microcode's colour brush fill
  (`FillRectColorBrush`) depends on it: it draws from an 8×8 brush in memory with
  SrcMask = 7,7. Windows 98 draws its tooltips that way; before this they came out black.
  SrcMask resets to all ones.
- DrawCtl pair 2 values 0–7 are not listed in [SPEC] table 2.9c. They are assumed equal
  to pair 3/4 values 0–7, and their use is logged.
- Local memory is stored as host-endian 32-bit words. The RISC is big-endian, and
  MemEndian swaps host accesses:
  - 0 = none (host pointers are mapped directly);
  - 1 = bytes;
  - 2 = bytes within halfwords;
  - 3 = halfwords.
  X.org's per-depth MemEndian choices (8bpp → 1, 16bpp → 3, 32bpp → 0) confirm that
  pixels sit at big-endian RISC byte addresses.
- The pixel engine is synchronous: PixelBusy reads 0. The icache and line buffer are not
  emulated, so memory is always coherent.
- The RISC runs `verite_risc_mhz` (default 25, as on the V1000) million instructions per
  emulated second. A 1ms tick runs the instructions it is owed. When the host polls
  FIFOInFree or FIFOOutValid while waiting for the RISC, it can run early, but only up
  to the instructions owed for the emulated time that has passed.
  - Before this limit, every such poll ran 20000 instructions while the emulated clock
    stood still. When the microcode itself waited for vertical retrace, as in a
    full-screen DirectDraw page flip, the retrace took seconds to arrive.
  - Draw instructions count as one instruction however many pixels they draw, so the
    pixel engine is faster than real hardware.
- Bt485 cursor modes: 1 = three colour, 2 = XGA, 3 = X Window. Each pixel's value is
  (plane 1, plane 0):
  - XGA: 00 = colour 1, 01 = colour 2, 10 = transparent, 11 = complement. The Windows 98
    drivers use this mode, with the AND mask in plane 1 and the XOR mask in plane 0.
  - A 32×32 cursor uses 256 bytes of cursor RAM: plane 0 at 00h, plane 1 at 80h. The
    address wraps from FFh to 00h, and the drivers rely on that: they load both masks
    with one run starting at 80h.
  - The cursor's screen position is (CX − size, CY − size): (CX−64, CY−64) for 64×64, as
    in X.org `verite_movecursor`, and (CX−32, CY−32) for 32×32. The Windows drivers place
    the pointer at the screen centre with (432, 332).
- The pixel PLL word is `(M−2) << 10 | P << 8 | (N−2)`, with M 8 bits wide. X.org never
  produces M above 129, but Rendition's BIOS does (640×480 is `3463Ah`, M = 211).
  Words with bit 18 or 19 set are not frequencies: the BIOS latches `40000h` after every
  clock change. They are ignored.
- Direct colour modes are always displayed without the palette, even after AX=1587h
  BX=2 clears the Bt485 bypass bit (see below).

## Rendition BIOS interface

Rendition boards shipped a VGA BIOS with extra INT 10h functions. DOS games built on
Speedy3D call them to find the board and to start their microcode. Boards without the
functions in ROM were updated with Rendition's `PHX2105.COM` TSR. DOSBox-X implements
the functions itself in `VERITE_BiosExtension()`, and no Rendition code is used at
runtime.

The behaviour below was worked out by black-box observation only. The TSR and vQuake
were run in DOSBox-X with an INT 10h logger (`INTLOG`, which records registers on entry
and exit) and the emulator's own I/O and memory trace (`verite_trace`). Every function
returns AX=0015h on success.

| AX | Function |
|---|---|
| 1580h | BX = 0084h (meaning unknown; Speedy3D only checks for success) |
| 1582h | prepare for a microcode load (see below) |
| 1583h | start the RISC at CX: hold it, force a `jmp`, clear Intr, release it. Returns BX=0064h, DI=0200h |
| 1584h | release the RISC from hold |
| 1587h | RAMDAC direct colour setup: BX=2 clears Bt485 CR1 bit 4, otherwise sets it |
| 158Dh | far pointer to the vital product data in DX:CX (segment DX, offset CX) |

**1582h** sets VESA mode 101h (640×480×8). It then:
- soft-resets the RISC and leaves it held;
- sets DRAM control to `140000h` and clears all interrupts;
- turns the display off (CRTCCtl video and sync enables);
- sets Mode to VESA with DMA enabled, LowWaterMark 3, MemEndian 0 and BankSelect 0;
- stores a palette loader at local address 0.

Speedy3D's microcode calls the loader through a pointer at local `F360h`, which is 0 in
cleared memory. The loader takes:
- a FIFO word holding `(first index << 16) | count`;
- then `count` words of `00RRGGBB`.

It writes them to the Bt485 at `FFE00580h`/`FFE00588h`, returns through `ra`, and uses
registers %160–%165. DOSBox-X's loader is its own 18-word implementation of that
interface. Without it, vQuake's microcode jumps into data at address 0 and draws nothing.

**Vital product data**: `VERITE_SetupRom()` adds a PCI data structure (`PCIR`, pointed
to from ROM offset 18h) to the VGA BIOS ROM. Speedy3D checks its class byte (`PCIR`+0Dh
= 3). The ROM also holds a VPD area in PCI resource-tag format:
- an `82h` identifier string, "Rendition Reference Board";
- a `90h` read-only area with PN "1000A", LL 00, ZC and ZR;
- the `79h` end tag.

The `ZC` keyword is what Speedy3D reads:
- the I/O base >> 8;
- the memory base >> 24;
- the IRQ;
- the local memory size in 64KB units (`0040h` for 4MB);
- then 0020h and zeros.

Speedy3D treats a memory size of 0 as 2MB. It then reserves its top 128KB at `1E0000h`
(BankSelect) instead of `3E0000h`. Games needing more than about 2MB of frame buffers
and textures stop: Tomb Raider with "Could not create texture space on board", and the
Scorched Planet Vérité patch with a misleading "no MOUSE driver installed: unable to
initialise RenderWare". The Rendition TSR left the field at 0 in our emulator, which is
how the wrong value got in.

The VPD is rebuilt whenever the guest moves a BAR.

**VBE mode 7Fh**: Speedy3D sets mode 7Fh around its own mode sets. The Rendition BIOS
only touches scratch registers for it, and DOSBox-X accepts it as a no-op.

**Native mode timing**: VESA modes use the same CRTC values as the Rendition BIOS.
Timings follow the VESA standards. Modes up to 400 pixels wide are line-doubled at half
the pixel clock. Spare lines are split between the porches: 320×200 gives CRTCHorz
`50427h` and CRTCVert `3A3F98Fh`, identical to the TSR.

## Testing done

- `tests/verite` part 1: hand-assembled RISC programs check the following:
  - ALU ops and delay slots;
  - vector ops on constant register pairs;
  - multiplies, getpc/jmpl links and jmprl;
  - big-endian loads and stores and host MemEndian views;
  - FIFO stall and resume, and the output FIFO;
  - the seed reciprocal used by line setup;
  - f2i.
- `tests/verite` part 2 runs the X.org V1000 2D microcode through the host interface
  exactly as the X.org driver does:
  - soft reset, forced steps, the context-switch monitor at `800h` reaching its wait
    loop, ELF upload with MemEndian byte swap, and SETUP;
  - solid fill with copy and XOR ROPs;
  - screen-to-screen blit, plain and overlapping (right-to-left, bottom-up);
  - solid line, and GET_PIXEL sync;
  - a colour brush fill (`LoadColorBrush` and `FillRectColorBrushRop`, as Windows draws
    tooltips) with a non-zero brush origin.
  51 checks pass. The frame buffer is also written to `verite_2d.ppm`.
- In DOSBox-X with `machine=svga_verite`:
  - PCI detection and VGA text mode work.
  - VESA mode 111h switches to native 640×480×16 at 60Hz, and the bank window draws.
  - A test program uploads RISC code through the VESA window, starts it with forced
    steps through the I/O BAR, and the RISC draws a rectangle with the pixel engine on
    the native display.
- **vQuake 1.08b1** (Speedy3D, with shareware Quake 1.06 data) runs on DOSBox-X's own
  BIOS with no TSR loaded:
  - E1M1 renders correctly in 320×200×16 (line-doubled native mode) with textures,
    perspective, lighting and the status bar;
  - `timedemo demo1` gives 969 frames in 39.9 s, 24.3 fps at `cycles=max` with the
    default 25 MHz RISC, and 29.0 fps with `verite_risc_mhz=100`;
  - earlier runs scored about 37 fps, but the old catch-up let the RISC run while the
    emulated clock (which the timedemo measures) stood still;
  - DMA command lists, the input FIFO, the parameterized draws and the palette loader
    are all in use.
- **DOS games** (built-in DOS, `core=dynamic`, `cycles=max`, `[dos] ems=false`, no TSR):
  - **Rebel Moon** (Rendition version, `REBMOON`): the demo plays in 640×400×16 with
    bilinear textures, lighting, sprites and overlays. Some wall and ceiling edges show
    faint light seams; not yet compared with real hardware.
  - **IndyCar Racing II** (`ICR2REND`, `INDYCAR.EXE`): runs in 640×480×16, with textured
    cars, track, trees and banners. This copy's `INDYCAR.EXE` is a modded `CART.EXE`: one
    dword at file offset F06F8h raises a cache from `1B1000h` to `C04000h`. The game
    allocates 4× that value plus `19097h` (about 48 MB), so it needs `memsize=64`. With
    less it stops with "RN1 Error: Could not allocate memory via DPMI". DOS/4GW tracks
    free memory in a 16-bit KB counter, so 64 MB is also the most it can use.
  - **Scorched Planet**, CD build (`VERITE\SPVERITE.EXE` with `SPD3D.VOH`): programs the
    chip directly, without any BIOS calls. The intro videos, the menu and its pixel
    engine effects work; gameplay is not checked yet, because the menu needs the mouse.
  - **Scorched Planet**, Vérité patch (`spveis`/`spvamer`, also on the 3D Blaster PCI
    bonus CD): reaches the menu in its default DMA mode, now that the VPD reports 4MB.
    Before that fix it stopped with a misleading "no MOUSE driver" message, and then
    DOS/4GW triple-faulted on exit. The RReady developer reports that its DMA mode locks
    up on real hardware, and runs it with `SPEEDY3D_DMA=0`.
  - **Tomb Raider demo** (3D Blaster PCI bonus CD, Speedy3D 1996-11-05 build): reaches the
    title screen with the 3D passport ring.
  - **CGL titles** (Creative's own API for the 3D Blaster PCI) run through the built-in CGL
    driver described in the CGL section, not on the RISC.
- **Windows 98 SE** (image from the S3 ViRGE tests, `cputype=pentium_mmx`, `core=dynamic`):
  - It finds the board as "Rendition Verite 1000 PCI" and installs Microsoft's own V1000
    driver from `DXREND.INF` (`rend.drv`, `rend2d.uc`, `rend3d.uc`). The desktop runs
    in native 800×600×16, drawn by `rend2d.uc` on the emulated RISC.
  - The miroCRYSTAL VRX 2.2 driver (`MIRODISP.INF`) installs over it. It runs 800×600×16
    at 60 Hz through the Rendition 2D microcode, whose idle loop is at `1800h`.
  - The hardware cursor (32×32, XGA mode) is correct with the 2.2 driver. Microsoft's
    driver showed the same black box before the XGA and address-wrap fixes, but hasn't
    been rechecked since.
  - DxDiag (DirectX 9.0c) with the 2.2 driver:
    - DirectDraw: all tests pass, including the full-screen 640×480 bouncing box at
      normal speed;
    - Direct3D 7: the textured, rotating DirectX cube renders correctly full screen;
    - the Direct3D 8 and 9 tests are skipped, because the driver doesn't support them.
  - Tooltips (yellow, drawn with a colour brush in an off-screen buffer and slid in with
    `DDBlt`) draw correctly since the SrcMask fix.
  - POD 2.0 (Ubisoft) runs through its Direct3D build (`PODD3DX.EXE`/`WPODD3D.EXE`); the
    CD has no Rendition-native build (its other builds are PowerVR, ATI 3D CIF, S3D,
    Glide, Matrox and software).
  - MechWarrior 2 Titanium Edition: the main menu runs in native 640×480×8 with an 8-bit
    DAC. Its colours were scrambled until native 8bpp stopped going through DOSBox-X's
    attribute controller mapping (the 16-entry table, still holding text mode values,
    folded the 256 indices onto 16 palette entries). In game (640×480×16), colours are
    correct with both the Rendition and Direct3D renderers. Both show a black box at the
    bottom left, as the PowerVR build does on DOSBox-X's PowerVR emulation, so it is
    probably the game's own HUD.

## CGL (3D Blaster PCI games)

Games written for Creative's 3D Blaster PCI use Creative's CGL (Creative Graphics Library),
not Speedy3D. Creative's Vérité driver, "Verite CGL 1.70 beta 7" (`VRIT?170.CGL`), doesn't
use the Rendition BIOS. It copies a RISC loader from the board's ROM to local memory and sends
its microcode for that loader to store.

`[verite] verite_cgl` chooses how these games run:

- `native` (the default): the game's own module runs its microcode on the emulated RISC and
  pixel engine. The board's loader comes from `verite_bios` when given, else DOSBox-X provides
  its own (see "The game's own module" below).
- `hle`: a built-in CGL driver (`src/hardware/verite_cgl.cpp`) handles the module's calls and
  draws on the host:

- **Loading.** The games link a loader that finds `*.cgl` in their own directory, reads each
  module's header and loads the variant they were built for:

  | File | Coordinates | Arguments |
  |---|---|---|
  | `VRITB170.CGL` | 16.16 fixed point | registers (Watcom) |
  | `VRITC170.CGL` | 16.16 fixed point | stack, removed by the caller |
  | `VRITE170.CGL` | float | registers |
  | `VRITI170.CGL` | float | stack |

  The variant is the dword at file offset 77h, after "HOT2".
- **The module served.** When a game opens one of these files, DOSBox-X serves a copy of it
  instead:
  - Header, object, page, entry and name tables keep their layout and sizes, and so does the
    file size. The loader therefore makes exactly the allocations it would for Creative's
    module. Hi-Octane is sensitive to this: with smaller tables, its own heap bookkeeping
    records a block of size 0 and later loops forever on a corrupted list.
  - Every code and data page is cleared and every fixup range emptied, so none of the
    module's code runs.
  - Each export is an 8-byte stub at the start of the code object: a DOSBox-X callback, the
    function number and the variant.
  - Nothing has to be added to the game's directory.
- **Calls.** Arguments come from the registers or the stack, by variant; coordinates are
  16.16 fixed point or floats. The handler returns to the caller itself.
- **Screen.**
  - `cglInitScreen` sets a native mode on the emulated Vérité (mode 14 is 640×400) in 5551,
    8888 or 8-bit colour, with one or two buffers.
  - `cglQueryScreen` and `cglInitScreen` report the texture memory left after the frame
    buffers. Hi-Octane sizes its texture cache from it; with 0 it reused one staging area
    for every tile of its menu background.
  - `cglSwapBuffer` flips at the next vertical sync and returns only then: it re-runs its
    stub until the sync, so games draw no faster than the display refreshes.
    `cglCopyDisplayBuffer` (back to front) is paced the same way: at 150 frames a second,
    Hi-Octane's menus didn't react to the keyboard.
  - `cglSetupInterrupt` for the end of a buffer swap (100h): when `cglSwapBuffer` is done,
    the game's routine is called with the mask (in EAX and on the stack) before the call
    returns. Flight Unlimited waits for its routine to clear a flag before it draws the next
    frame.
  - The vertical retrace interrupt (1): after each vertical sync, when the game runs its own
    code with interrupts enabled, its routine is called as an interrupt would call it, and
    the game continues where it was with all registers and flags restored. Battle Race
    installs one and waits for it. Scanline interrupts aren't supported.
  - Depth (16-bit) is kept on the host.
- **Drawing.** `cglSendStream` takes a list of pointers to render headers:
  - **Header layout:** operation, vertex count, primitive, property mask, one 16-bit field,
    then pointers to depth, stencil, blend, texture environment, vertices, texture
    coordinates, colours and depth cue.
  - **Arrays:** they hold only what the property mask enables: x,y (plus z with depth),
    s,t (plus q with perspective), BGRA colours.
  - **Primitives:** points, lines, triangles, strips, fans, quads and rectangles (two
    corners) are drawn by a software rasteriser into the card's frame buffer.
  - **Texture modes:** modulate, decal and copy.
  - **Transparency:** "overlay" makes texels with a clear 5551 alpha bit transparent, and
    chroma keying is supported.
  - **Filtering:** bilinear when the texture environment asks for filtering.
    Transparent texels (overlay or chroma key) are left out of the blend, so their colour
    doesn't bleed into the edges, and a pixel is a hole when they carry half the weight or
    more. Flight Unlimited sets overlay on all its textures, and asks for filtering when the
    "Blending" option in its Renderer menu is on (it is off by default).
  - **Also supported:** depth test, depth cue and constant alpha blending.
  - Hi-Octane sets property bits above 200h on some quads (F800h, FC00h); they are ignored.
  - **Colour index screens** (format 14): a colour index texture writes its texel's index
    unchanged, without filtering, and an untextured primitive writes its red component as
    the index. `cglSetPalette` also loads the RAMDAC palette, which the display reads.
- **Textures.**
  - Addresses count from the start of texture memory, which begins 64KB after the frame
    buffers: Hi-Octane addresses its staging rows from about 32KB below address 0.
  - `cglLoadTextureMap` loads at the free address the game passes and advances it past the
    texture.
  - The texture's card-side row stride can be wider than the texture, so a game can keep
    textures in one wide surface.
  - Addressing is linear, as on the V1000 (which has wrap masks but no clamping): Hi-Octane
    addresses its sources with coordinates past the texture's width and negative rows.
- **Host access.**
  - `cglDownLoad` and `cglUpLoad` copy rectangles between host memory and the frame or depth
    buffer, with no blend, constant alpha or per-pixel alpha.
  - `cglLockBuffer` returns a far pointer into the card's memory window (valid while DOS/4GW
    maps linear addresses to physical ones).

Status:

- **Rebel Moon** (`REB3D.BAT`, i.e. `-3DBLASTER`) runs: title screen, menus, mission
  briefing and textured 3D gameplay with its status bar.
- **Hi-Octane** (3D Blaster PCI patch `HIOPCI2`, `EXEC\HIOCTANE.EXE`) runs: menus,
  championship, track and car selection with their 3D models, and races with the HUD,
  opponents, smoke and fog. It keeps texture memory as one 1024-wide surface and writes
  most of its 2D graphics (menu backgrounds, text) straight into it and into the back buffer
  through `cglLockBuffer`. Its menus read the keyboard state once a frame and miss presses
  held much longer than a normal keystroke, as the software build (`HIOSW.EXE`) does too.
- **Battle Arena Toshinden** (v2.0 with the 3D Blaster PCI upgrade `TSDUPGRD`: `TSD.EXE`
  and `VRITB170.CGL`) runs: menus and fights with the 3D stages, shaded fighters, shadows
  and HUD. It needs a CD image of the original disc, audio tracks included; without them it
  reports "NO CD DETECTED" and allows only network play.
- **Actua Soccer** (3D Blaster CD version, `SOC3DB.EXE` from the `SOC3DB` patch) and
  **UEFA Euro 96 England** (`EUROBLST.EXE` with the patch's `VRITB170.CGL`) run matches in
  8-bit colour index mode: Actua at 640×400, Euro 96 at 800×600. Their menus are VESA
  graphics drawn by the game. Actua's installer stops with "Divide error" at
  `cycles=max`; it installs with fixed cycles.
- **Screamer 2** (Italian 3D patch, `S2_3DBL.EXE` at 320×200) and **Screamer Rally**
  (`STRT3DBL.EXE`) run menus and races, with `HOTCGL` pointing at their `HOTCGL`
  directory. Screamer 2's high resolution build (`S2_3DBH.EXE`, 640×400) was tested as far
  as its menus.
- **Battle Race** (Ikarion, with the 1.01 patch) plays its intro movie and reaches the main
  menu at 640×400; the races are not tested yet. It ships no Vérité module, only the VLB 3D
  Blaster's `GIGTB163.CGL`, so `VRITB170.CGL` from another game has to be added. Its SETUP
  doesn't react to input in DOSBox-X (it does in MS-DOS mode of a real Windows 98
  installation); a `SYSTEM.CFG` saved there selects the CGL drivers. One OEM edition is
  tied to the ELSA Victory 3D ("This is an ELSA Victory 3D version only").
- **Flight Unlimited** (bonus CD, `vF1.2BS`) runs: the FBO lobby in 3D, plane selection,
  pilot log, flight with the textured terrain and instrument panel, the in-flight menus,
  and quitting to DOS. It needs an installed setup: the CD's `HD` tree copied to a
  writable directory, run from there (it tests its file handles by creating `test*.bin`
  in the current directory), and a `flite.cfg` with `pile_path`, `lang_path`, `cat_path`
  and `cd_dir`. The game builds the paths of the 3D Blaster art (`ENGPILEB`) from
  `cd_dir`. Without it, the 2D screens come out half drawn.
- **Not implemented:** copy streams, spans, stencil, logic operations, picking, cursors and
  scanline interrupts. These are logged with `verite_debug=true`, which also logs each
  distinct render setup and texture load.

The game's own module (`verite_cgl=native`): the 1.70 modules start the board by
stepping the RISC through its debug registers, copy a 2K byte RISC loader from the board ROM
(RISC address 0xfff10000, ROM offset 0x10000) to local memory at 0x3ff800, start the RISC
there and send their microcode through the FIFO. The V1000 decodes its 128K byte ROM, mirrored,
in the top 16M bytes of the RISC address space (except the memory-mapped registers), and
`[verite] verite_bios` loads a dump of the user's own board ROM there. Without one, the board
gets a ROM holding a stub that hands the RISC to DOSBox-X, which receives the microcode in
place of the board's loader
([Rendition-Verite-CGL-loader.md](Rendition-Verite-CGL-loader.md)). With it, the modules' own
microcode runs Rebel Moon, Battle Arena Toshinden, Flight Unlimited, Hi-Octane, Screamer 2,
Screamer Rally, UEFA Euro 96, Actua Soccer and Battle Race on the emulated RISC and pixel
engine. Battle Race needs `verite_irq` below 8 with the game's own module.

## Microcode found in released Rendition software (brief section 6)

Source: the VOGONS Drivers "Rendition - Game Patch Compilation"
(`game_updates_compilation.7z`, fileid 168). The file name, size and first 64 bits of
SHA-256 are listed for each microcode.

| Microcode | Used by | Size | SHA-256 (64 bits) | Notes |
|---|---|---|---|---|
| Speedy3D, 1996-11-05 | Tomb Raider (Rendition), Whiplash, an IndyCar II replacement `spd3d.uc` | 67280 | `e50a13579db9e81a` | stripped, loads at `1000h` |
| Speedy3D (vQuake), 1996-11-07 | vQuake 1.08b1 | 65176 | `a7151eefd5740389` | stripped; vQuake's `spd3d.uc2` "for V2x00" is byte-identical |
| Speedy3D, 1996-09-13 | IndyCar Racing II Rendition (`ICR2REND`), Scorched Planet Vérité patch (all three regions), Descent II Vérité patch | 56680 | `bf7dbd617f5fffbc` | stripped, loads at `1000h`; byte-identical in all of them |
| Speedy3D pre-alpha | Descent II Vérité patch `SPD3D.VOH` (3D Blaster PCI bonus CD) | 192516 | `b21c8136dd1a1a83` | text format; differs from the Scorched Planet CD's `.VOH` |
| Speedy3D | Rebel Moon (Rendition version, 1996-12-24) | 56680 | `0ebf8dd4452c0842` | same layout as the one above; 788 code and 14 data words differ |
| Speedy3D pre-alpha, 1996-03-21 | Scorched Planet CD `VERITE\SPD3D.VOH` (unpatched game) | 21376 words | `$Id: spd3d.voh,v 1.32 1996/04/26` | text format (one hex word per line, `address_` markers), not ELF |
| DirectX 2 Direct3D | Scorched Planet CD `DIRECTX\DRIVERS\DISPLAY\BIN\D3D.UC` | 94744 | `66bcfffe291ccfe9` | 730 symbols, loads at `4000h` |
| DirectX 2 Windows 2D | same directory, `WIN31.UC` | 21216 | `a945208c3761df11` | 244 symbols; the "undefined opcodes" in `.text` are pen and brush tables |
| Speedy3D V2x00 beta, 1997-08 | IndyCar II V2000 patch | 72280 | `c52e32e352ac725f` | symbols present; uses V2x00-only `tri`, `fill`, `cbltf`, `sort` |
| RRedline 3D, 1997-10 | V1000 reference driver beta 5 `special\v10003d.uc` (also in VHexen II) | 93132 | `602cdb202ebffb3d` | stripped, loads at `8000h` |
| RRedline 3D | V1000 reference driver 2.1b2 `v10003d.uc` | 93132 | `7f031521373e86a3` | same layout as beta 5; 164 code words differ |
| OpenGL | V1000 reference driver 2.1b2 `v1000gl.uc` | 93132 | `1ecff4afb97fb3e8` | variant of the 2.1b2 3D microcode: 190 code and 49 data words differ |
| OpenGL, 1998-05-23 | miroCRYSTAL VRX driver 2.2 (1998-11-19) `V1000GL.UC` | 93132 | `48a60dc6d23ad554` | rebuild: same section layout, but 5274 code words differ from the 2.1b2 file |
| 2D | V1000 reference driver 2.1b2 `v10002d.uc` | 26253 | `0768ffc017197dc9` | symbols present |
| 2D (X.org) | XFree86/X.org rendition driver | 26253 | `f0a3514ae9c9a3d8` | same code and data as the 2.1b2 file; only the unloaded `.reginfo`/`.bss` headers differ |

The miroCRYSTAL VRX 2.2 package (The Retro Web, `w9x-vrx220.zip`) is the 2.1b2 reference
driver with newer RRedline and OpenGL parts:
- **Identical to 2.1b2:** `V10002D.UC`, `V10003D.UC`, `VERITE.DRV`, `V100032.DLL`,
  `V1000.DLL` and `V1000.VXD`.
- **Newer:** `REDLINE.DLL` and `VERITE.DLL` (1998-09), an `OPENGL32.DLL` (instead of
  `veritegl.dll`) and the OpenGL microcode above.
- **Device match:** its INF matches plain `PCI\VEN_1163&DEV_0001` and needs no
  subsystem ID.
- **DOS TSR:** `VROM.COM` is byte-identical to Rendition's `PHX2105.COM` BIOS TSR. The
  readme recommends it for display problems under DOS.

The new OpenGL microcode uses the same 85 opcodes as the 3D microcode, and the core
implements all of them.

The DOS side has five distinct V1000 Speedy3D microcodes:
- **1996-03 pre-alpha** (`.VOH` text format): the Scorched Planet CD as shipped.
- **1996-09-13:** IndyCar Racing II and the Scorched Planet Vérité patch. The two files
  are byte-identical.
- **Rebel Moon's build:** same size and layout as 1996-09-13, but about 6% of the code
  differs.
- **1996-11-05:** shared by Tomb Raider (Core Design), Whiplash (Gremlin) and the IndyCar
  II replacement `spd3d.uc` of unknown origin.
- **vQuake's own build.**

So no single microcode can be assumed per game, and LLE covers all of them.

The two Scorched Planet patches carry the same `SPD3D.UC`; only `SPVERITE.EXE` differs:
- `spvamer` is for the American retail CD;
- `spveis` is for the English/Italian/Spanish one, which is the European CD image used
  here.

Speedy3D loads `SPD3D.UC` from the current directory unless the `SPEEDY3D` environment
variable names another file (IndyCar II V2000 readme).

Every V1000 microcode above uses only instructions the core implements: no
triangle/fill-engine opcodes and no undefined ones. (The only undefined words in `.text`
are data tables, such as `WIN31.UC`'s.) All of them rely heavily on the
parameterized `draw1`–`draw5`/`rdraw1`–`rdraw5` paths and on `step q` (perspective and
reciprocals). vQuake exercises these paths and renders correctly.

**Direct3D and DirectDraw (brief section 4).** The Windows 95 reference driver 2.1b2
installs three separate microcodes:
- `v10002d.uc` (2D);
- `v10003d.uc` (3D);
- `v1000gl.uc` (OpenGL).

The INF points RRedline at the 3D file with the registry value
`HKLM\SOFTWARE\RRedline\v1000\microcode`. So a single shared microcode is not a safe
assumption.

Observed with the 2.2 driver in Windows 98:
- the desktop and DirectDraw run the 2D microcode (idle loop at `1800h`);
- for Direct3D the driver loads a 3D microcode at `8000h` (the RISC ran at `8F1Ch`
  during the DxDiag cube), and reloads the 2D one afterwards.

So **DirectDraw and Direct3D do not share microcode**. `v10003d.uc` and `v1000gl.uc`
both load at `8000h`, so the PC alone does not say which of the two Direct3D uses (RRedline
is configured for `v10003d.uc`). The emulation does not depend on the answer, because it
runs whatever microcode is uploaded.

## Not yet verified (needs real software)

- 3D microcode beyond vQuake: RRedline (`v10003d.uc`), the other Speedy3D builds and the
  V2x00-only instructions. Fog and blending modes that vQuake doesn't use follow [SPEC]
  but are unverified.
- Windows 9x: real DirectDraw, Direct3D and RRedline games (brief phases 1–2). So far
  only the desktop and DxDiag's DirectDraw and Direct3D 7 tests have been checked.
- The DOS games (brief phase 3): Rebel Moon, IndyCar Racing II and the Tomb Raider demo
  run. Both Scorched Planet builds reach their menu. Still open: actual gameplay (the
  menus need the mouse). For the CGL titles see the CGL section.
- Exact pixel engine rounding (bilinear filter, dithering, fog) has no hardware reference
  and aims at "visually correct".
- Bt485 cursor modes 1 and 3 and the 64×64 cursor have not been seen in use.

## Next steps

1. Windows 98: run Direct3D and RRedline games, and compare Microsoft's `rend2d.uc` and
   `rend3d.uc` with the Rendition files.
2. Play the three DOS games (in-game, mouse-driven menus), and check Rebel Moon's edge
   seams against real hardware captures.
3. CGL: the functions still missing, more titles, and Rebel Moon's edge seams.
4. Profile; add HLE fast paths only where needed, with LLE as the fallback. vQuake runs
   at full speed under LLE, so none are needed yet.
