# CGL board loader for the emulated Rendition Vérité

This document specifies how DOSBox-X starts the RISC for a game's own CGL module when no
dump of a real board ROM is available (`[verite] verite_cgl=loader`). It describes what a
module expects from the board, what the real board provides, and what DOSBox-X provides in
its place. See [Rendition-Verite.md](Rendition-Verite.md) for the emulated board and the
built-in CGL driver.

## 1. Background

A CGL game loads a driver module (`VRIT?170.CGL` for the Vérité) and calls its exported
functions. The module runs on the host. It drives the board through the I/O registers and
runs its own microcode on the board's RISC. The microcode is part of the module; the board
supplies only a small RISC program, the loader, that receives the microcode.

With `verite_cgl=hle` DOSBox-X replaces the module. With `off` or `bios` the module runs
unchanged, and it needs the board to behave as described below.

## 2. What a module does to start the board

Observed with the 1.70 modules (Battle Arena Toshinden upgrade, Flight Unlimited, Screamer 2,
Screamer Rally, UEFA Euro 96), in order:

1. **PCI.** Finds the board by vendor and device ID and reads its I/O and memory bases.
2. **Reset.** Writes `Debug` (48h) with SoftReset and HoldRISC: the RISC is held at
   0xfffffff0.
3. **Stepped instructions.** Executes single instructions on the held RISC: `StateIndex`
   (60h) = 80h, the instruction written to `StateData` (64h), then `Debug` = 6 (HoldRISC and
   StepRISC). Results are read back with `StateIndex` = 82h (the S1 operand of the
   instruction in the decode stage) after an instruction such as `add zero, zero, reg`.
4. **Memory size.** Stores test patterns at 0xffffe0/0xfffff0, 0x7fffe0/0x7ffff0 and
   0x3fffe0/0x3ffff0 and reads them back.
5. **Loader copy.** Clears local memory from 0x3ff800 and copies 2K bytes, one word at a
   time, from RISC address 0xfff10000 (board ROM offset 0x10000) to 0x3ff800: each word is
   loaded on the RISC, read by the host through `StateData`, and stored with `li`/`lui`/
   `addsl8`/`add` and `sw`.
6. **Start.** Sets `flag`, jumps the RISC to 0x3ff800 with a stepped jump, clears the
   interrupt status and releases the RISC (`Debug` = 0).
7. **Microcode.** Writes the microcode through the input FIFO (`FIFOIn`, I/O 00h), waiting
   for `FIFOInFree` (40h): for each section the load address, the length in words and the
   words.
8. **After the load** the module talks to its microcode directly; it needs nothing more from
   the loader (section 6).

## 3. What the real board provides

- **ROM.** A 128K byte EPROM. The RISC sees it in the top 16M bytes of its address space,
  mirrored (DOSBox-X decodes it the same way). Offset 0 holds the VGA BIOS; offset 0x10000
  holds the RISC loader.
- **The loader** at ROM offset 0x10000, at most 2K bytes, run from local memory at 0x3ff800.
  It reads the FIFO, stores each section's words at its load address, and starts the
  microcode.

DOSBox-X has no copy of a board's ROM and the ROM upgrades for the boards contain only the
VGA BIOS, so the loader has to come from DOSBox-X.

## 4. The microcode in the module

- **Container.** A block that starts with `C3DB`, followed by the block size, a version word,
  the number of sections and the sections. Each section starts with `\xe6DAT`, its size in
  bytes (including the eight bytes after the marker) and its load address; the words follow.
- **Words.** The module sends the stored words unchanged.
  - Older modules (1.66, early 1.70) store plain microcode words, in the byte order that the
    RISC reads them through the FIFO.
  - The 1.70 modules scramble the words with a repeating 128-byte pattern. The loader on the
    board undoes it.
- **Recovering the pattern.** The microcode is about half zero bytes, so for each of the 128
  byte positions the most common stored byte is the pattern's byte. This recovers the
  pattern from the data itself, and the result decodes to valid V1000 instructions (checked
  with all four 1.70 modules). DOSBox-X does not store a pattern.

## 5. What DOSBox-X provides (`verite_cgl=loader`)

1. **ROM.** When no `verite_bios` is given, the ROM window holds DOSBox-X's own ROM image:
   0xff everywhere, with the loader stub at offset 0x10000.
2. **Loader stub.** A few RISC instructions written for DOSBox-X: they signal the emulator
   through a reserved control address and wait. The module copies the stub to 0x3ff800 like
   any loader.
3. **Loader in the emulator.** When the RISC reaches the stub, DOSBox-X takes the FIFO
   stream: for each section the address, the length and the words. It recognises a
   scrambled section (the plain words don't decode as V1000 instructions), recovers the
   pattern as in section 4, and stores the words at the load address. Then it starts the
   microcode as the real loader does (section 6).
4. **Setting.** `[verite] verite_cgl` gains `loader`. `bios` keeps using the user's ROM dump.

## 6. Answers found while implementing

- **End of the stream:** a section with address 0 and length 0 (the module sends two zero
  words after its sections).
- **Start:** the microcode starts at the first section's load address (0 for every module
  seen).
- **Handshake:** none. Once the microcode runs, the module talks to it through the FIFOs as
  usual.
- **State:** the microcode needs no registers or memory set up by the loader.

## 7. Implementation

- `Verite::Core::BuildLoaderRom` builds the ROM image: 0xff everywhere and, at offset
  0x10000, a stub that stores to `LOADER_PORT` (0xffe01000, just above the pixel engine
  state registers) and waits.
- The store starts the loader (`LoaderBegin`). While it runs the RISC executes nothing, and
  `LoaderPump` takes the FIFO words on each run of the RISC; `LoaderStore` unscrambles a
  section if needed and writes it to local memory.
- `vga_verite.cpp` installs the image when `verite_cgl=loader` and no `verite_bios` is
  given.

## 8. Testing

With `verite_cgl=loader` the modules' own microcode runs Rebel Moon, Battle Arena Toshinden,
Flight Unlimited, Hi-Octane, Screamer 2, Screamer Rally, UEFA Euro 96 and Actua Soccer. The
older modules (Actua Soccer's) load their unscrambled microcode into local memory themselves
and need no loader; they run the same way.

Found on the way:
- **CRTC stores from the RISC.** The modules' microcode starts by storing to the CRTC
  registers (0xffe00408..0xffe00438), which left the display blanked. On the V1000 these
  stores are taken to have no effect (see `Core::CtlWrite`).
- **Battle Race** reaches its races, but its cockpit stays black; with `hle` it is drawn. Its
  module builds a real-mode interrupt stub when the board's IRQ is 8 or higher and fails
  without free conventional memory (the game takes it all), so it needs `verite_irq` below 8
  (5 works).

Gaps in the RISC and pixel engine emulation that show up with other games are fixed in the
core, not in the loader.
