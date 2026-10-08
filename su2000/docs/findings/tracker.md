# InsideTrak tracker (Milestone 3 part 2)

Source: `TRKI_IT_*` and `PROCESS_tracker` in DAC.EXE (debug names in the executable). Emulation: `src/hardware/su2000/tracker.cpp`.

## Protocol [confirmed from code, emulated]

| Step | Host does | Card answers |
|---|---|---|
| open | ASCII commands at base+0: `W`, `S` (status, must contain "InsideTRAK"), `D`, `l1,1` `l2,1`, `l1` | `l1` -> 4 words; bytes 3 and 4 = '1' mark hardware sensors 1 and 2 present. Without this the library uses no sensors at all |
| set-up | `v..`, `x..`, `y0`, `R n`, `A n,...` (alignment), `s n,az,el,roll` (sensor angle offsets), `H n,...` (hemisphere) | nothing needed |
| record | write 0 to base+n (n = hardware sensor) | 8 words: header, x, y, z, azimuth, elevation, roll, spare (int16) |

Scales (`TRKI_IT_GetRawTrackerRecord`): position = word x 0.0916; angle = word x pi/32767 rad; azimuth +pi/2, roll = -word x k + pi.
If fewer than 8 words arrive the last record is reused.

## DAC game frame [measured]

Game head/hand vectors (x, y, z, az, el, roll): x right, y forward, z up. `FIXED_TRACKER` (CONFIG.VPC) uses head (0,0,2100),
hand (300,400,0), angles 0. Head azimuth steers walking.

## Emulation

* Raw pose per sensor from `[su2000] tracker pose`; default puts the head at (0,0,2100) facing forward.
* The hand position mapping (library alignment + game scaling) is solved at run time: the emulator reads DAC's head/hand
  variables (data object found by searching for the IT filter string) and steers the raw words with a Broyden-updated
  Jacobian to `[su2000] tracker hand target` (default 150 450 1750, turned with the head). DAC only.
* Mouse (`[su2000] tracker mouse`): x turns head and hand, y aims the hand; left button = trigger, right = walk.
  Ctrl+arrows do the same.

## Open

* Proper tracker maths (alignment/offset commands) instead of the DAC-specific closed loop.
* Independent head look; VR input.
