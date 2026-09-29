# PowerVR Series 1 (PCX1/PCX2) emulation

Source: `src/hardware/pvr_pcx.cpp`. Config: the `[powervr]` section (`powervr_card = pcx1|pcx2`).
Host-side test: `tests/powervr/run.sh`.

## What is emulated

The PCX1 and PCX2 are 3D-only PCI cards with no display output. The guest's SGL library:

1. builds ISP plane parameters in host memory. The chip reaches them through a 256-entry
   page table (the TLB registers, `0x100`-`0x1FF`).
2. builds a list of object pointers grouped by screen tile. This is the region data,
   either at the physical address in `OBJECT_OFFSET` (PCX1) or inside the TLB space
   (PCX2, `OBJECT_OFFSET` bit 0 set).
3. stores the TSP (texture and shading) parameters and the textures in the card's 4MB
   texture memory, which is PCI BAR1.
4. programs `SOF_ADDR`, `LSTRIDE` and `PACKMODE` with the VGA card's back buffer. It then
   writes `START_RENDER`.

The chip renders tile by tile and writes the finished pixels straight into the VGA
linear frame buffer. It then sets `INT_STATUS` bit 1 (end of render) and raises its
interrupt.

The emulation renders the whole frame synchronously when `START_RENDER` is written, then
reports end-of-render immediately. The device consists of:

- **PCI device** `1033:002A` (PCX1) or `1033:0046` (PCX2). BAR0 is the 4KB register
  file and BAR1 is the 4MB texture memory. Both BARs can be relocated by the guest.
  (The vendor's DOS ISR moves BAR0 to `B8000h`, and that works.)
- **ISP** ("SABRE"): the 32-cell hidden-surface engine. It covers:
  - the instruction set (forward/reverse/perpendicular planes, shadow and light volumes,
    translucent pass start)
  - the "first plane of the second object" pipelining
  - the 40-bit depth adder
  - PCX1 20-bit float and PCX2 IEEE float plane formats
  - PCX2 linked object lists
- **TSP** ("TEXAS"):
  - flat and smooth shading, shadow light, flat highlights, fog (table from the
    `FOG_TABLE` registers)
  - translucency (4444 textures plus global translucency)
  - perspective-correct texturing: 8-bit 332, 16-bit 555 and 4444 texels, twiddled
    layout, mipmaps, U/V flip
  - PCX1: point sampling with linear blending between two mipmap levels.
    PCX2: bilinear filtering when the `BILINEAR` register asks for it.
- **Output** in 555, 565, 24-bit and 32-bit, with `X_CLIP`.
- **Robustness**: display lists are bounded:
  - regions per render, objects per region, planes per region and per render
  - link hops
  - a per-render work budget measured in plane x 32-pixel span evaluations

  A garbage list stops the render with a log message instead of hanging the emulator.

## Where the behaviour comes from

The primary source is Imagination Technologies' MIT-licensed release of the original
driver source, [PowerVR-Series1](https://github.com/powervr-graphics/PowerVR-Series1).

**The ISP and TSP are ports of the vendor's own software simulators**:

- `Source/simulat3/hwsabsim.c`: `SurfProcess` (cell state machine), `ExpandInstruction`
  and `conv20_to_30`
- `Source/simulat3/hwsabren.c`: `HWISPRenderer`. Tile/span traversal, translucent pass
  handling, the flush at the end of each tile, and fog.
- `Source/simulat3/texas.c`: `Texas`, `TexturePixel`, `AddressCalc` and `FetchPixel`.
  Shading, the perspective divide, mipmap selection and bilinear filtering.
- `Source/simulat/texas.c`, the PCX1-era simulator: the non-bilinear sampling path
  (two point samples blended between mip levels).

The structure of the emulation follows the simulators: decode a tile's objects once,
then feed every plane to 32 cells per span, then shade what the cells hold. The main
change is that the tile's display list is decoded once per tile, not re-read for every
span.

The other sources:

- `Source/pcx/hwregs.h`, `hwregs.c`, `hwtexas.h`: the register map, the TSP control
  word bits, and how the driver initialises the chip.
- `Source/pvrlims.h`: the tile header and object pointer formats. `Source/pto20b.h`: the
  20-bit float format.
- `Source/win32/system.c`, `brdsetup.c`: object pointer and TLB usage for PCX1 versus
  PCX2. `texif.c`: texture memory is BAR1.
- `Source/dos32/irq.c`, `isr.asm`: BAR0 is the register file, reading `INT_STATUS`
  acknowledges the interrupt, and end-of-render is bit 1.
- `Source/texapi.c`: the texture memory layout. Two 16-bit texels per 32-bit word,
  with the even texel in the upper half, in twiddled (Morton) order.

## Not documented by the release, and what is assumed

The low-level board code, meaning the VxD's `brdio.c` and the DOS `brdsetup.c`, is not
part of the release. The following are assumptions, and each is marked in the source:

- **TLB entry and `PAGE_CTRL` formats.** The driver keeps one physical address per slot
  and a page size of 4, 8 or 16KB (`DMASBToPCXTLB`). An entry is taken to be the slot's
  physical address, and `PAGE_CTRL` bits 1:0 the page size code. An entry that is not
  page aligned, or is below 1MB, is treated as a page frame number.
- **PCX1 `OBJECT_OFFSET`.** The Win32 path writes a physical address. The shared
  non-Win32 `HWSetSabPtrRegister` writes a word offset, relative to a base that the
  missing DOS board code would set. The physical-address reading is used, because the
  Win32 path is the one known to have driven real PCX1 boards. **If a DOS game renders
  nothing, check this first** (`powervr_debug = true` logs these registers).
- **TSP parameter location.** They are read from texture memory at `PREC_BASE`, taken
  as a 32-bit word offset. The driver's `ISP_BASE = 0x80000` is commented as "the
  bottom of the second bank", which is 2MB. The copy from the driver's software buffer
  into texture memory is done by the missing VxD.
- **`PCX_ID` and `PCX_REVISION` values** are not known. The PCI IDs are returned.
- **`X_CLIP`**: pixels left of the left value, or right of the right value, are not
  written.

Deliberate deviations from the simulators:

- `PACKMODE` dithering is ignored.
- Depth values that overflow the 32-bit comparator saturate instead of wrapping.
- 8-bit texels are always expanded to 8 bits per channel. The PCX1 simulator skips that
  on its point-sampled path, which would make 332 textures too dark.
- `ToPfloat` is bounded (the simulator loops forever on `INT_MIN`).

## Which card the games need (brief section 2)

This is **not conclusively resolved**. Most primary sources (VOGONS, vintage3d.org,
MobyGames) could not be reached from the environment this was developed in. The
evidence found:

- A VOGONS list of proprietary-API games describes the PowerVR release of Flight
  Unlimited as "locked to 640x480" and working "on both Midas3 and PCX2 cards". Midas3
  is the pre-PCX1 development board: an ISPTSP design with 4-word planes and a separate
  PCI bridge. If that is right, the game's SGL is chip-abstracted: it detects the board
  and packs the display list for it, as the released source does. Such a game will also
  drive a PCX1, and `pcx2` should be tried if `pcx1` fails. Midas3 itself is not
  emulated.
- Nothing was found tying Actua Soccer Club Edition to a particular board. Its PowerVR
  build dates from 1996-97, when PCX1 boards (Apocalypse 3D) were on sale.
- As the brief suspected, "3D Blaster VLB / GiGi" has nothing to do with PowerVR. Nothing
  here depends on that association.

The default is therefore **PCX1**, as the brief proposes, with **PCX2** selectable.

## Testing done

- `tests/powervr/run.sh` builds `pvr_pcx.cpp` on the host against shim headers, for both
  chips. It renders a display list packed the way the vendor packer does: background
  plane, flat triangle, perspective-textured quad occluding the triangle, and a 4444
  translucent pass blended over both. It then checks pixel values and writes PPM images.
  It also renders 300 random display lists per chip, to check that rendering always
  terminates. A 640x480 frame of that scene takes about 36 ms.
- In DOSBox-X with `machine=svga_s3` and `powervr_card=pcx1`, a small DOS program:
  - found the card through the PCI BIOS (`1033:002A`)
  - relocated BAR0 to `D000:0000`
  - read and wrote registers
  - started a render, then saw `INT_STATUS` = 2 and saw the read acknowledge it

## Findings from real software

- **Tomb Raider (PowerVR DOS port, `tombpcx1.exe`, SGL4DOS 1.27)**: the game identifies the
  board by the handle `(bus << 8) | (device << 3) | function` from its own PCI scan, and treats
  a handle of 0 as "PCX1 not found", exiting without an on-screen message. The card is therefore
  never placed in bus 0 slot 0 (on real PCs that is the host bridge). `tombpcx1.exe` rejects a
  PCX2 ("run tombpcx2.exe instead") and needs `sglhw.ini` in `%WINDIR%` only for optional
  settings.

## Next steps

- Run Actua Soccer Club Edition and Flight Unlimited (PowerVR builds) with
  `powervr_debug = true`, then check the TLB, `PAGE_CTRL` and `OBJECT_OFFSET` values
  against the assumptions above.
- Resolve the platform of 3D Shooting Maker and A Train No. 5. For PC-98, the card
  would have to be offered on the PC-98 PCI bus as well.
- Windows 9x PowerSGL drivers (secondary): these use the same hardware interface through
  the VxD, and should work once the DOS path is confirmed.
- Save states are not supported yet.
