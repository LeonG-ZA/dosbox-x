# CPU A → CPU B draw-list records (rasteriser input)

Source: DN2 `MAINB.OUT` (1995-02-21) `draw32B` 0xb3f8 / `draw16B` 0xe328, plus SFL `draw32B` 0xb578. The dispatch head is
`draw + 0x2C` (`ld.d r4,r28,$0`): r28 = record pointer, type = signed top byte of the header. Statistics come from the harness
(`run_pix --lle-b`, report at the end), which counts records, bytes and interpreted instructions per type.

Registers set by the `draw*` prologue and used by every handler: r2 = line stride, r3 = draw buffer, x31 = [g+0x240],
x30 = (float)[g+0x48], x29 = (float)[g+0x4C] (g = globals at 0x7000).

| Type | Size | Meaning | Status |
|---|---|---|---|
| 0x00 | 8 | end of list | interpreter (returns) |
| 0x01 / 0x02 / 0x03 | 16 / 8 / 8 | flat-colour trapezoid: header = y (12 bits), lines (12 bits), colour; then left/right edge x in 22.10 fixed point with 21-bit signed slopes; 2/3 replace the right/left edge and continue | documented (SFL draw32B 0xb6f8–0xb78c) |
| 0x0A | 0x38 | scaled bitmap blit: source address/stride, 16.16 u/v steps, destination x/y, width/height, key or constant colour, alpha, flags (bit 16 colour key, bit 17 alpha blend, bit 23 blend with constant colour, bits 24–27 alternate source formats), mode word | **HLE (32-bit, main path)**; 16-bit and alternate paths interpreted |
| 0x0B / 0x0C / 0x0D | 0x50 / 16 / 16 | perspective-correct textured trapezoid: y and texture base; left/right edges + slopes; U/W, V/W, 1/W at the centre with per-line and per-pixel deltas (floats); texture wrap masks. Exact division every 16 pixels, linear in between, exact per pixel at span ends | documented; interpreted |
| 0x33 | 8 | set drawing origin (two 12-bit signed values into x30/x29) | interpreted (cheap) |
| others | | Gouraud, translucent, depth-cued and wire-frame variants (`wfBoth`, `wfLeft`), `bm_fourcolour` | not yet analysed |

Cost in DN2's attract mode with B interpreted: type 0x0A ≈ 90 k instructions per record (full-screen title bitmap), types
0x0B–0x0D ≈ 30 k. These are the HLE priorities.
