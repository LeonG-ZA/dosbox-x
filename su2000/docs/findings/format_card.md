# Format / control card ("CTRL", Milestone 3 part 1)

Source: `CTRLI_FCD_*` in SFL.EXE (symbols from SFL.SYM); listings via
`python -I tools/disas_le.py data/original/hdd/SFL.EXE data/original/hdd/SFL.SYM '^CTRL' > re/sfl/ctrl.asm`.
Emulation: `src/hardware/su2000/fcard.cpp` (`[su2000] ctrl ports`, `ctrl mem`).

## Interface

| Where | What | Evidence | Confidence |
|---|---|---|---|
| mem +0x00 | firmware version, BCD (version = hi·100 + lo·10) | `CTRLI_FCD_Open` 0x7d2c0 | [confirmed] |
| mem +0x02..+0x05 | free-running counter (little-endian). Open reads it, waits 300 ms and requires it to have increased | 0x7d1f4–0x7d29d | [confirmed] |
| mem +0x06 | card type; must be 0x81 or 0x82, otherwise error -6154 "Format C not found" | 0x7d1a1–0x7d1c7 | [confirmed] |
| mem +0x0A..+0x0D | joystick axes (two x/y pairs) | `CTRLI_FCD_ReadJoystick` | [confirmed offsets], centre value [inferred 0x80] |
| mem +0x12, +0x13, +0x14 | buttons: value = (+0x14 & 0x0F) \| +0x12 << 4 \| +0x13 << 12 | `CTRLI_FCD_ReadButton` 0x7d585–0x7d5d2 | [confirmed]; polarity [inferred: 1 = pressed] |
| mem +0x14 bit 4 | credit (coin) present | `CTRLI_FCD_GetCredit` | [confirmed] |
| register map | register n → offset: 1–0x13 → n+1; 0x14–0x16 → 0x17–0x19; 0x17–0x1F → 0x400–0x408 | `registerMap` table at SFL 0xC6EA0 | [confirmed] |
| mem +0x400 | command mailbox: the host waits (2 s timeout) for 0, then writes the command; command 1 also clears +0x402 | `CTRLI_FCD_SendCommand` | [confirmed] |
| mem +0x401..+0x408 | command parameters / output registers (lights, Visette brightness, volume, fade, audio switch) | register map | [inferred] |
| io +0..+3 | 8254 timer. Counters 1 and 2 are programmed mode 2 / 0xFFFF (0x74, 0xB4) and cascade into a 32-bit **1 MHz** down-counter. 0xDC at +3 is the read-back latch; then +1 lo,hi and +2 lo,hi. Open requires it to be within 5 s of its start | `CTRLI_FCD_InitTimers`, `GetSystemTime`, constants 1e6 / 256 at SFL 0xB9730 | [confirmed] |
| io +4, +5 | written during open (+4: 0x18 or 0x10; +5: 0x83, 6, 0x84, 6) | `CTRLI_FCD_Open` 0x7d2f8, 0x7d401 | [confirmed writes], meaning unknown |

## Emulation (first version)

* Version 0x13, type 0x81, millisecond counter, centred joysticks. The command mailbox is acknowledged immediately (no
  microcontroller); commands are logged (`SU2000: format card command ..`). Outputs are only stored.
* Timer: 1 MHz down-counter from emulated time, restarted when counters are programmed, latched by read-back.
* Inputs (DOSBox-X mapper, rebindable): **Ctrl+F5 coin, Ctrl+F6..F9 buttons 0..3** (bits 0..3 of +0x14). Which physical
  trigger is which bit is not known yet. One set of inputs is shared by all cards.

## Results

| Program | Before | Now |
|---|---|---|
| Dactyl Nightmare (Solo) | waited in the set-up menu, "Format C not found" | card opens ("CTRL library", "Backlight disabled"), card commands sent; still in the operator menu, which waits for trigger presses (try Ctrl+F6..F9 interactively) |
| Pac-Man VR | "Initialise_Hardware: (CTRL_Open) Format C not found" | CTRL opens; now stops at "CD #1 failed security check" (needs the Pac-Man VR CD) |
| Missile Command | "Format C not found" | CTRL opens; with an empty CD drive mounted: "Not a Missile Command VR CD in drive 1" (needs its CD) |

Both configs now mount `data/work/emptycd` as CD drive F: (MSCDEX).

## Open

* DAC (CTRL v01.03.51) reads the player triggers from **+0x13** (`CTRLI_FCD_ReadJoystick`, DAC.EXE 0x9d680): bits 0..3 player 1, 4..7 player 2, **active-high**. The start button is bit 0/1 of the button word (+0x14 bits 0/1). The stick bytes +0x0A..+0x0D are `0x7F - raw` (centre 0x7F) but DAC only forwards the stick buttons, never the axes. The operator set-up menu is always shown at boot; Enter (DAC's own joystick-button key) leaves it. The Solo cabinet's player is player 2 (Ctrl+5 fire, Ctrl+6 walk). Credits: plain `c` (DAC keyboard credit); the card's coin bit is only read in credit-device mode.
* Which +0x13 bit is the top / front trigger; whether any game needs command responses from the microcontroller.
* Game CDs for Pac-Man VR and Missile Command are not in either dump.
