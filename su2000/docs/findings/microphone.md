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
* **Fade** (left/right balance / volume of the mixer inputs): `CTRL_SetFade` / `CTRLI_FCD_SetFade`, applied by
  `CTRLI_FCD_UpdateMixer` through card commands.
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
* Not yet: the format card's fade / MICNET volume (decode `CTRLI_FCD_UpdateMixer` commands, then pan the relayed voice
  as DAC asks), side-tone, and voice between two emulators (needs the network card).
