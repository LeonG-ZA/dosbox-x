# Virtuality SU2000 Emulation in DOSBox-X — Project Brief

> Handover document for Claude Code. Read this whole file before starting work.
> The end goal is to run original Virtuality SU2000 games inside DOSBox-X. The work is
> split into milestones with explicit decision gates. **Do not skip a gate.** Record evidence
> for every conclusion.

---

## 1. Goal

Emulate the Virtuality SU2000 (1994, W Industries / Virtuality Group) well enough that the original
game executables run unmodified inside a patched DOSBox-X build:

1. 3D graphics produced by the Expality PIX 1000 board are rendered on screen. Later this can go to a modern VR headset.
2. Head tracking, hand/controller tracking and buttons work.
3. Sound (Ensoniq Soundscape) is **out of scope for now** and becomes a later milestone.

---

## 2. What we already know

Confidence tags: **[confirmed]** comes from the supplied files or multiple sources. **[reported]** comes from a
single secondary source. **[inferred]** is our reasoning and must be verified.

### Hardware
| Component | Detail | Confidence |
|---|---|---|
| Host | 486DX-33 single-board PC, MS-DOS | [reported] |
| Graphics | Expality PIX 1000 ISA card, **2× Motorola 88110** RISC CPUs, 8 MB DRAM, 4 MB VRAM | [reported] |
| HMD | Visette 2, two LCDs (one per eye) | [reported] |
| Tracking | Polhemus InsideTrak magnetic 6DOF (head + hand). May be an ISA card, not serial. Verify | [reported] / [inferred] |
| Sound | Ensoniq Soundscape S-2000 | [reported] |
| Storage | IDE HDD + IDE CD-ROM | [confirmed] (CONFIG.SYS) |

What actually ran on the 88110s is **unknown**. Finding that out is Milestone 1.

### Software environment (from the supplied AUTOEXEC.BAT / CONFIG.SYS, "1 player version", 21.7.95)
- `set dos4g=quiet` → games are **32-bit DOS/4GW protected-mode** executables (LE format, very likely
  Watcom C). [confirmed env var / inferred compiler]
- `EMM386.EXE /NOEMS X=C800-E0FF` → a ~100 KB upper-memory window (`C8000h–E0FFFh` physical) is
  reserved from the memory manager. **Most likely the PIX 1000 shared memory / ROM / register window.**
  It may also cover other cards. [inferred]
- `mute` runs at boot. It's a custom utility, probably silencing the sound mixer. [inferred]
- `vpcgo` is the system launcher/supervisor. `c:\vpcexec.bat` is called if it exists, probably generated
  by the supervisor to chain games. `VPCTEMP=c:\vpctemp` is wiped each boot. [inferred]
- No `SSINIT`, no `BLASTER=` → the sound card is initialised by Virtuality's own software. [confirmed absence]
- MSCDEX + IDECD.SYS → CD-ROM present. Game resources live on CD. The main game executable shipped
  separately, on floppy/HDD. Example: the archived Dactyl Nightmare SP CD has `DACC.EXE`, `SP.EXE`
  (supervisory program) and `ETS.EXE` (engineering test system), but not `DAC.EXE`. [reported]

### Known SU2000 game titles
Dactyl Nightmare SP, Dactyl Nightmare 2, Buggy Ball, Zone Hunter, Pac-Man VR, Shoot for Loot,
Missile Command VR, Ghost Train, Virtuality Boxing, Sphere, X-Treme Strike.

---

## 3. Inputs

- An archive containing **multiple SU2000 games** plus system files (supplied by the user).
  Expected to be similar to the Internet Archive "Virtuality SU2000 HDD Dump".
- `AUTOEXEC.BAT` and `CONFIG.SYS` (contents summarised above).

**Rules for the input data**
- Never modify the originals. Extract to `data/original/` (read-only) and work on copies.
- Record a SHA-256 manifest of every original file in `data/MANIFEST.sha256`.
- Do **not** commit game binaries, firmware or CD data to any public repository. ViRtuosityTech
  currently owns the SU2000/SU3000 game IP. Keep proprietary data in a git-ignored folder.

---

## 4. Repository layout (suggested)

```
su2000-emu/
├── data/                 # git-ignored: original + working copies of game files
├── tools/                # Python scripts: extraction, hashing, scanning, log decoding
├── re/                   # Ghidra/IDA projects, exported symbol maps, notes per binary
├── dosbox-x/             # fork / submodule of DOSBox-X with our devices
│   └── src/hardware/su2000/   # pix1000.cpp, tracker.cpp, ...
├── logs/                 # captured bus traces from DOSBox-X runs
└── docs/
    ├── findings/         # one markdown file per finding, with evidence (offsets, hashes, traces)
    ├── DECISIONS.md      # log of every decision-gate outcome and why
    └── PROTOCOL.md       # PIX 1000 host interface spec as it is discovered
```

Every finding in `docs/findings/` must include: the file(s) and offsets it is based on, the tool and
script used to reproduce it, and a confidence tag.

---

## 5. Tooling

| Purpose | Tool |
|---|---|
| Emulator base | DOSBox-X, built from source. Use its I/O port handlers (`IO_RegisterReadHandler` / `IO_RegisterWriteHandler`) and memory page handlers for the `C8000–E0FFF` window |
| LE executable inspection | Open Watcom `wdump`. Unbind the DOS/4GW stub if needed |
| x86 disassembly / decompilation | Ghidra (with an LE/LX loader extension) or IDA (native LE support). Apply Watcom runtime signatures (FLIRT / Ghidra Function ID) first |
| Cross-binary function matching | Ghidra BSim / Version Tracking, or BinDiff |
| 88110 disassembly | Check current options: MAME's `m88000` disassembler, older GNU binutils `m88k` targets, OpenBSD/luna88k toolchain. If Ghidra has no 88k module, writing a SLEIGH spec is acceptable |
| 88110 execution (only if needed) | Evaluate MAME's `m88000` CPU core. It targets the 88100. See Milestone 2B |
| Scripting | Python 3 (hashing, entropy scans, log decoding) |

---

## 6. Milestones

### Milestone 0 — Setup and inventory
1. Extract the supplied archive into `data/original/` and generate the SHA-256 manifest.
2. Produce `docs/findings/inventory.md`:
   - Every executable: format (MZ / LE / bound DOS/4GW), size, compiler fingerprint (Watcom strings,
     runtime signatures).
   - Every non-executable file: size, entropy, magic bytes, a guessed type.
   - Which files belong to which game. Note the supervisor (`VPCGO`, `SP.EXE`), test tools (`ETS.EXE`)
     and utilities (`MUTE`).
3. Run `strings` (ASCII and UTF-16) over everything and save per-file string dumps to `re/strings/`.
   Grep for: `PIX`, `Expality`, `88110`, `88k`, `firmware`, `microcode`, `.cod`, `.bin`, `download`,
   `boot`, `Polhemus`, `InsideTrak`, `Visette`, `track`, `COM1`, `COM2`, error messages.
4. Build DOSBox-X from source and confirm that `ETS.EXE`, `VPCGO` or a game at least starts
   (it is expected to hang or fail on missing hardware).

**Exit criteria:** inventory complete, DOSBox-X builds, a list of candidate firmware files and blobs exists.

---

### Milestone 1 — How did the game EXEs drive the PIX 1000? (DECISION GATE)

**Primary questions**
- Q1. Does every game send the **same firmware** to the PIX 1000?
- Q2. After the firmware is loaded, does the host only send **drawing / scene commands**, or does the board
  also do game work: collision, physics, animation, scene logic, or results the host reads back?

#### 1A. Static search for 88110 code
88k code is big-endian with fixed 32-bit instructions.
- Scan every file, including the inside of each EXE's data segments, for 88k code. Use a disassembler
  sweep plus heuristics. For example, the return instruction `jmp r1` (expected encoding `F4 00 C0 01`,
  verify against the disassembler) appears at the end of most functions. Score blocks by the density of
  valid decodes.
- Report for each candidate blob: file, offset, length, SHA-256, and a disassembly sample.
- Look for blobs that are separate files (`*.COD`, `*.BIN`, `*.PIX`, etc.), embedded in EXE data, or on the CD.

#### 1B. Find the upload path in the x86 code
- In one game EXE (start with Dactyl Nightmare SP if a complete EXE is present), locate:
  - all `in`/`out` instructions and their port numbers,
  - all accesses to linear addresses `0xC8000–0xE0FFF` (also via DPMI selectors or `0x0000_C8000`
    mappings created with DPMI functions 0x0800 / 0x0002),
  - DPMI interrupt hooks (int 31h, functions 0x0200–0x0205) to identify IRQ handlers.
- Identify the routine that copies a code blob to the board, typically a large block copy to the window
  followed by a "start" or "reset release" register write. Record the source of that blob (file name, embedded
  offset) in `docs/findings/`.
- Identify the routines that send ongoing per-frame traffic, and what they read back.

#### 1C. Dynamic capture in DOSBox-X
- Implement a **logging stub** device `pix1000.cpp`:
  - claim the `C8000–E0FFF` window and any I/O ports found in 1B,
  - log every read and write (address, width, value, x86 CS:EIP, timestamp) to `logs/`,
  - return values that keep the game progressing (status/handshake bits discovered from the
    disassembly). Document every faked value.
- Run each game far enough to get past firmware upload and into the main loop.
- Write `tools/extract_upload.py` to reconstruct, from each log, the exact bytes uploaded to the board.
  Hash them.

#### 1D. Compare across games
Produce `docs/findings/firmware_comparison.md` with a table:

| Game | Upload size | SHA-256 | Same as reference? | Notes (overlays, runtime patches, extra uploads mid-game) |
|---|---|---|---|---|

Also compare the x86 side: use BSim / BinDiff to determine whether the board driver code in each EXE
is a **shared library** (same functions) or per-game code.

#### 1E. Characterise host→board traffic after boot
From the logs and disassembly, classify the per-frame traffic:
- **Command stream / display list:** opcodes plus parameters such as matrices, vertex/object IDs, colours,
  "end frame", "swap eye". A good sign for HLE.
- **Data uploads mid-game:** geometry or textures, which is also fine for HLE.
- **Code uploads mid-game**, or game-specific code: a bad sign for HLE.
- **Read-backs:** list every value the host reads back and what it is used for (sync/status only vs.
  collision results, picking, positions).

Write the first draft of `docs/PROTOCOL.md`.

#### DECISION GATE 1 (record the outcome in `docs/DECISIONS.md`)

**Go to 2A (High-Level Emulation) if ALL of these are true:**
- every game uploads the same firmware, or a small set of versions that share one command protocol,
- post-boot traffic is drawing/scene commands plus data,
- read-backs are limited to status/sync, or are few enough to reimplement on the host side.

**Go to 2B (88110 assessment) if ANY of these are true:**
- games upload different firmware or game-specific code,
- the board does substantial non-rendering work (game logic, collision, physics) whose results the
  host depends on,
- the command protocol cannot be decoded with reasonable effort.

Mixed results are possible. For example, there may be a shared renderer plus small game-specific code overlays.
In that case document the split and propose a hybrid (2A for the common protocol, 2B for overlays).

---

### Milestone 2A — High-Level Emulation of the PIX 1000

1. Turn the logging stub into an HLE device:
   - decode the command stream per `docs/PROTOCOL.md`,
   - maintain board-side state such as uploaded geometry, matrices, materials and per-eye viewports,
   - emulate status/sync registers and any interrupts the host waits on, with correct timing,
   - answer all read-backs.
2. Renderer: start with a **wireframe** debug view in a separate window (OpenGL via DOSBox-X's SDL
   context, or an off-screen buffer composited side-by-side: left eye | right eye).
3. Then add flat/Gouraud shading, depth, and colour/lighting rules matched to reference footage.
4. Keep a frame-dump option (PNG per eye) for regression testing.

**Exit criteria:** at least one game renders recognisable 3D scenes for both eyes and runs at a
stable rate in its attract/demo mode, with fake tracker data.

### Milestone 2B — Can the Motorola 88110 be emulated?

Produce `docs/findings/m88110_assessment.md` answering:
1. What exactly runs on the board: renderer only, renderer plus game logic, or two different programs on the two CPUs?
2. Instruction coverage: does the code use 88110-specific features that a 88100 core lacks? Examples
   are the extended floating-point register file, graphics/pixel instructions, and the different exception
   and cache model. Produce an opcode histogram from the firmware.
3. Rest of the board: memory map, how the two CPUs communicate, VRAM/framebuffer layout, video
   output to the HMD, interrupts to the host. Any of these that are unknown will block low-level emulation.
4. Options, with effort estimates:
   - extend MAME's `m88000` core to 88110 and port it into DOSBox-X,
   - write a minimal interpreter in DOSBox-X covering only the opcodes actually used,
   - hybrid: emulate the CPUs at low level but replace the framebuffer/video output with a high-level path.
5. A recommendation.

Then implement the chosen option, aiming for the same exit criteria as 2A.

---

### Milestone 3 — Head tracking, hand tracking, buttons

Do not start until Milestone 2A or 2B renders images.

1. Locate the tracker driver in the x86 code. Find its I/O ports or ISA card window, serial ports
   (`0x3F8` / `0x2F8` + IRQ), and record parsing. Determine whether InsideTrak is an ISA card or a serial
   device in this system.
2. Document the data format in `docs/findings/tracking.md`: sensor IDs (head vs. hand), position and
   orientation encoding (Euler or quaternion, units, coordinate frame), update rate, and the
   initialisation and command sequence.
3. Locate button input for the hand controller, the trigger, and any operator or coin inputs. Note which port,
   which bits and any debounce logic.
4. Implement `tracker.cpp` in DOSBox-X:
   - Phase 1: synthetic input (keyboard/mouse → head pose, hand pose, buttons) for testing.
   - Phase 2: modern VR headset and controller input through OpenXR, mapped into the original coordinate
     frame. Optionally render each eye straight to the headset from the PIX HLE.
5. Note any calibration or attendant procedures that the supervisor (`VPCGO`/`SP.EXE`) runs.

**Exit criteria:** a game is fully playable with synthetic input. Stretch goal: playable in a modern headset.

---

### Milestone 4 (later) — Sound
Out of scope for now. Notes for later:
- There is no Soundscape emulation in DOSBox-X or 86Box yet. Useful references: the Linux ALSA driver
  `sound/isa/sscape.c` (register layout, firmware upload), the Rise of the Triad `audiolib/SNDSCAPE.C`,
  MAME's ES5506 (OTTO) emulation, the existing AD1848 emulation, and the dumped instrument ROM (OS/2 Museum).
- The card's 68EC000 runs firmware uploaded by the host. Low-level emulation (68000 + OTTO + ODIE +
  AD1848) is probably feasible.
- First check how `MUTE`, `VPCGO` and the games initialise the card.

---

## 7. Working rules for Claude Code

- **Evidence first.** Every claim in `docs/` cites file + offset, a log excerpt, or a script. Mark
  anything unverified as [inferred].
- **Small, reproducible scripts** in `tools/`, never one-off manual steps.
- **Stop and report at each decision gate.** Summarise the findings and the recommended path in
  `docs/DECISIONS.md` and wait for the user's confirmation before starting Milestone 2.
- Prefer the shared library: once board, tracker or input functions are identified in one EXE, match them
  across all games before analysing each game separately.
- Keep DOSBox-X changes isolated in `src/hardware/su2000/` behind a config option
  (e.g. `[su2000] enable=true`) so they can be maintained against upstream.
- Do not publish proprietary binaries, firmware, decompiled game code or captured asset data.

---

## 8. Open questions to resolve along the way

- Exact I/O ports and memory map of the PIX 1000. Is all of `C800–E0FF` the PIX window, or is it shared
  with other cards?
- Does the PIX board have its own boot ROM, or is everything uploaded by the host?
- Do the two 88110s run the same program (split by eye or by screen half) or different roles
  (geometry vs. rasterisation)?
- Is there also a VGA card for an attendant monitor? If so, what does it show?
- Where does each game's main EXE live: HDD, floppy image or CD? Are any EXEs missing from the supplied set?
- What do `VPCGO`, `vpcexec.bat` and `SP.EXE` do in the game launch sequence, and does the
  supervisor itself talk to the PIX board, for example for test patterns?
- Is there a multiplayer/network component (a "1 player version" config implies a multi-player one)?
