# Visette microphone

Source: string and symbol survey of every game executable and CONFIG/KEYWORD.VPC, plus DAC.EXE disassembly
(`re/dac_all.asm`, not in git).

## What the hardware does [inferred from the library]

The microphone is **analogue** and belongs to the format / control card (CTRL library), not to the Soundscape: no game
records it through the sound card (the SND library's "RecordPrep" strings are generic and unused). The card has a
small mixer:

* **MIC**: the player's own microphone into their own headphones (side-tone). ETS warns "The mic can feedback if ..."
  and shows "Mic peak"; SFL's debug keys `h`/`j` change "MIC volume".
* **MICNET**: microphone audio from the other pod(s), carried as an analogue line between pods, into the headphones.
  SFL's `H`/`J` change "MICNET volume".
* **Mic level**: `CTRL_GetMic` -> `CTRLI_FCD_GetMic` sends card command 2 and reads shared-memory byte **+0x17**
  (DAC 0x9e160).
* **Fade** (left/right level of the mixer inputs): `CTRL_SetFade(cards, signals, sides, level 0..1)` stores a level per card,
  signal and side (1 = left, 2 = right); `CTRLI_FCD_UpdateMixer` (DAC 0x9ee80) writes it to **I/O base+5** as register /
  value byte pairs: `reg` = both sides, `reg | 0x20` = left, `reg | 0x40` = right. Registers have bit 7 set; levels are
  mostly 5-bit (0..31). The signal -> register table (DAC.EXE file offset 0xf1210):

  | signal | 0x1 | 0x2 | 0x4 | 0x8 | 0x10 | **0x20** | 0x40 | 0x80 | 0x100 | 0x200 |
  |---|---|---|---|---|---|---|---|---|---|---|
  | register | 0x90 | 0x92 | 0x93 | 0x91 | 0x95 | **0x94** | 0x97 | 0x96 | 0x82 | 0x81 |

  DAC pans signal **0x20** (register 0x94 = MICNET) with level (1 -/+ sin(bearing)) * k, scaled down with distance.
* `[CTRL] MIC_OFFSET` (KEYWORD.VPC) is a configuration keyword; no VPC file on the drives sets it.

## Use by the games

| Game | Evidence |
|---|---|
| DAC | `SOUND_handle` (0x4d502): for each sound view it takes the other player's position (`micnet`, 0x18e174), turns it into the listener's frame and calls `CTRL_SetFade`, so **the opponent's voice comes from the opponent's direction**. `CTRL_GetMic` is linked but not called. |
| ETS | `CTRL_GetMic` with a "Mic peak" display (set-up / test screen). |
| SFL | Operator keys for MIC and MICNET volume. |
| DN2, DN2C, SP, ZONE | `CTRL_MIC`, `CTRL_MICNET` tokens and `MIC_OFFSET`. |
| GHOST, BOX, PAC | `MICNET` / `MIC_OFFSET` tokens only (library). |

So the microphone was an intercom between the players of linked pods (and of the two-player cabinet), positioned in
3D by DAC; games do not process the voice digitally.

## Emulation (VR link)

* The headset page sends its microphone (16 kHz mono) to DOSBox-X; the peak level goes to the player's format card
  byte +0x17 (`FCARD_SetMicLevel`), and the audio is relayed to the clients of the other player (intercom).
* The relayed voice is played with the left / right MICNET levels of the **listener's** format card (register 0x94, fcard.cpp
  `FCARD_GetMicnet`), so it comes from the direction the game gives it. Until a game writes a non-zero MICNET level the
  voice plays centred at full level. Verified by writing the register directly (left 31, right 8 -> gains 1.0 / 0.26).
* With linked pods (`[su2000] network`) DAC drives it: in a two-pod match it writes MICNET levels (for example left 24,
  right 0), and microphone packets travel between the emulators with the frames (`SU2V`), so a headset on one pod hears
  the player of the other pod from the direction the game sets. In a one-pod game `micnet[]` is never activated.
* Not yet: side-tone, MICNET master volume.
