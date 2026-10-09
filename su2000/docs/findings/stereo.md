# Stereo rendering

## What the original software supports [confirmed]

`VISETTE_MONO` / `VISETTE_STEREO` (CONFIG.VPC `[CTRL] FORMATn`) are read by `CTRLI_ReadConfigFile` (stored as 1 / 2 in the
format-card record) and used only by `CTRLI_FCD_Open`: it writes **0x18 (mono) or 0x10 (stereo)** to format-card I/O +4,
i.e. the headset video routing. The PIX library has per-channel views (`PIX_SetView`, `PIX_SendViewMAT`, `PIX_SelectChannels`)
but DAC and DN2 contain no eye separation or per-eye camera; the game calls `VIEW_mono` only. Native stereo is not available.

## Emulator stereo (`[su2000] stereo`, `stereo separation`)

* Every PIX card gets a twin board (pix1000.cpp `twins[]`) that receives the same host writes: control port (run/stop/reset),
  memory window and broadcast FIFO. Host reads come from the real card only; the twin runs on its own thread.
* CPU A breakpoints after the view stores of `ProcView` / `ProcViewMAT` (12 floats) and `ProcViewPOS` (3 floats), found by
  instruction pattern (`find_view_hooks`; DAC 0x4bb08, 0x4bbb8, 0x4bc88). At the hook the translation x at view + 0x24 is
  moved by the eye's shift: left eye (real card) -separation/2, right eye (twin) +separation/2.
* Direction measured in DAC (near objects appear further right in the left eye); DAC updates its views through
  `PIXI_UpdateViews` about once per frame per card.
* Display and frame dump: left-eye channels first, then the right-eye channels.

Cost: twice the PIX emulation (two extra threads per card). Screen-space overlays (score, clock) are identical in both eyes.

## Open

* Check DN2 / 1994 firmware (same command numbers; hook patterns to be confirmed).
* Separation in game units: DAC head height is 2100, so 65 is about a 65 mm eye distance [inferred].
