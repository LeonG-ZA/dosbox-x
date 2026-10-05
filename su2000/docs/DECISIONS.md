# Decision log

## Gate 1 — how do the games drive the PIX 1000?  (2026-10-05)

**Status: decided 2026-10-05 by the user: hybrid (see Gate 1 outcome below).**

### Findings

| Gate criterion | Result | Evidence |
|---|---|---|
| Every game uploads the same firmware, or a small set sharing one command protocol | **Small set, one protocol.** 4 builds on this disk (SP 1994-02, DN2 1995-02, ETS 1995-06, SFL 1995-10) of the same `mainA`/`mainB` programs. The opcode table is append-only (43→66→67→68), with one slot redefined in the 1994 build. Upload = the COFF files byte-for-byte | `findings/firmware_comparison.md` |
| Post-boot traffic is drawing/scene commands plus data | **Yes.** 100 % of 8.57 M FIFO words in a 90 s DN2 run parse as known commands (view/model matrices, model lists, materials, palettes, render/swap/sync, FLIC frames, effects). Bulk data (textures, models, FLIC frames) goes into a host-managed board heap and is referenced by address. **No code uploads after boot.** | `findings/dynamic_capture.md` §1E |
| Read-backs limited to status/sync or easy to reimplement | **Yes.** FIFO flags, CPU-ready words, channel-busy word, per-frame load counters (feeding the host's scan-line balancing), pixel/bitmap read (`PIX_ReadPixel`, unused in the DN2 run). No collision, picking or game-state results | `PROTOCOL.md` §7, `tools/readback_check.py` |
| Board does game logic | **No.** Firmware symbols are renderer-only; game logic is in the x86 EXEs | `findings/host_driver.md` |

### Recommendation: **go to Milestone 2A (High-Level Emulation)**

The PIX 1000 behaves as a retained-mode 3D renderer behind a FIFO. Implementing the ~45 commands that games actually send
(DN2 uses 24) on top of a host renderer is well bounded.

Caveats that shape the 2A plan:

1. **The real work is the data formats, not the protocol.** Model blocks (`PIXI_MBIN_*`, `*.BIN` files), texture rules,
   materials and palettes live in board memory in firmware-defined layouts. The firmware itself is the specification: it has
   symbols (`processModel`, `tranVertList64`, `drwList1-4`, `ProcTextureRule`, `MixedV2`…), and `tools/m88kdis.py` gives a
   readable start. A proper 88k disassembler (MAME `m88000` disassembler or a Ghidra SLEIGH module with 88110 extensions)
   is the first 2A task.
2. **Version differences** (mailbox offsets 0x214C vs 0x2158, load-counter block, opcode 0x28 in 1994) must be handled per
   firmware version. The stub can tell versions apart from the uploaded COFF header (timestamp/size).
3. **Timing.** The stub completes every render instantly (DN2 runs ~500 fps). HLE must model frame time and the busy/FIFO
   flags so game timing matches (target: PAL 50 Hz video, `PIX_SyncRender` cadence).
4. **Two channels.** The SU2000 2-player cabinet has two processor cards (one per player/HMD). The Visette 2 has one LCD per eye
   (`VISETTE_MONO` in CONFIG.VPC is unexplained so far). Stereo vs. mono per channel must be settled from the view/window
   commands before the renderer is designed.

**Fallback (2B) stays viable** because the actual 88110 binaries are available with symbols. Static opcode classes per
`tools/m88kdis.py --hist`: CPU A ≈ 13 % FP, ≈ 3 % 88110 graphics-unit, ≈ 1.5 % extended-register load/store; CPU B (rasteriser)
≈ 17 % FP, **≈ 9 % 88110 graphics-unit**. A plain 88100 core (MAME) would therefore not be enough; it would need the 88110 graphics and
extended-FP units plus a model of the PIX pixel bus and video. A hybrid (LLE of CPU A for geometry, HLE raster) is a possible
Milestone 2B design if HLE geometry fidelity proves hard.

### Open issues carried forward

* **SFL.EXE does not start under DOSBox-X** (Watcom runtime heap failure before any hardware access), so SFL still needs a
  dynamic capture.
* ETS waits for InsideTrak data; SP's FIFO cannot be attributed to callers (Phar Lap stack layout).
* CTRL/format card (buttons, credits, Visette brightness) and InsideTrak record format are still stubs (Milestone 3).
* Answers to brief §8: the C800–E0FF hole = NET (C8000) + PIX window (D0000–DFFFF) + format cards (E0000/E0800).
  The board has **no program ROM dependency**: both CPUs' code is uploaded. A VGA attendant/operator display exists:
  ETS and SP switch the VGA into graphics mode, and DN2 prints its banner on VGA text.

### Gate 1 outcome (user decision)

The user chose the **hybrid**: emulate CPU A (geometry) at instruction level, and replace the rasteriser (CPU B) with HLE.
The SFL start-up failure was to be attempted without spending too long on it.

Implementation notes (details in `findings/m88110_core.md`):

* CPU A runs the real MAINA in a new MC88110 interpreter. CPU B also runs MAINB for its set-up and idle loop, but each draw-list
  record is offered to the HLE first (`pixraster.cpp`). Unimplemented records fall back to the interpreter, so the output is
  always the firmware's.
* Not done: an automatic translation of B's handlers into C++. That would be decompiled firmware and must not be published
  (brief §7). HLE handlers are written from the record semantics and checked against the interpreter with
  `run_pix --compare N`.

### SFL start-up (time-boxed, not solved)

`SFL.EXE` exits with "Not enough memory to allocate file structures": the Watcom runtime's `malloc(8)` fails in start-up
(image 0x91214). The heap-grow path checks the runtime's extender-type byte `[0xC727E]` and calls `0x8F961`/`0x8FA10`. The
flag `[0xC8554]` is 1 and never cleared. Unchanged with memsize 4–64 MB, `xms=false`, `umb=true` and `DOS4GVM=1`. The next
step would be tracing DOS/4GW's DPMI calls in a DOSBox-X debugger build. Left open by the time box.
