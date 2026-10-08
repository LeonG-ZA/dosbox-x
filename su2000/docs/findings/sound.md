# Sound survey (all games, both drives)

Method: string/library survey of every game executable (`SND library vNN.NN.NN`, card names, CD calls) and the DAC
sound/CD library code (debug names in DAC.EXE). No sound hardware is emulated yet; games log
`SND Error : (-7308) All specified cards NOT successfully opened`.

## Which cards [confirmed]

| Game | Sound library | Effects | Other cards |
|---|---|---|---|
| DAC, DN2, DN2C, BOX, GHOST, GUN, ETS, SFL, ZONE, PAC (both drives) | SND 00.01.49 – 00.01.56, Ensoniq **Soundscape only** | WAV files (DAC: 143, 8-bit 22 kHz mono), `SNDSCAPE.COD` | none |
| MISS, PEARL | SND 00.01.53, Soundscape only | WAV | none |
| SP | no SND library | – | CD audio only |

No game contains Sound Blaster, Gravis Ultrasound, AdLib/OPL or MPU-401 General MIDI support, and no MIDI files are
shipped. CONFIG.VPC `[SND] sound1: Port 0x330, IRQ 7, Code SNDSCAPE.COD, Mode MIDI, SBEnable TRUE`.

## Music [confirmed for DAC]

CD audio through MSCDEX: `CD_PlayTrack` is called from the game loop (DAC.EXE 0x29447). With the empty CD drive the game
logs `CD Error : (-5027) Attempt to access an invalid track number`. Music therefore needs the game's audio CD
(not in either dump); DOSBox-X already plays CD audio from CUE/BIN images.

## What the Soundscape interface looks like [from DAC SND library]

* Open: download the card firmware `SNDSCAPE.COD` (`SND_LoadCode`), configure (`SND_ConfigureSoundscape`,
  `SND_SetMIDIEmulation`, MIDI channel enables, synth mode, priorities).
* Load: each WAV is sent to card memory by DMA (`SNDI_DownloadSample`, `SNDI_OpenDMA`, "ODIE channel"), then wrapped in
  a patch and a program (`SNDI_DownloadPatch`, `SNDI_DownloadProgram`).
* Play: plain MIDI channel messages — `SND_StartSound` = Program Change + Note On (0x90 | ch, note 0x3C, velocity);
  `SND_StopSound` Note Off; pan / volume / pitch as controllers.
* Host commands are 7-bit packed (`SND_HostWrite2..4`) and go through one transport routine (DAC 0xb0dce / 0xb0e85).

## Options

1. **HLE (recommended first):** emulate the Soundscape's host-side ports well enough for `SND_Open`, accept the firmware
   download, capture DMA'd samples and patch/program records, and play Note On/Off with a simple sample mixer
   (pitch, volume, pan, loop points from the patch). Needs: the port map/handshake used by the transport routine, the
   DMA path, and the patch/program record layout (the library has `SNDI_DisplayPatch/Program/Envelope/LFO`, which
   names the fields).
2. **LLE:** emulate the card itself (68000 running `SNDSCAPE.COD`, the Ensoniq wavetable chip and the ODIE gate array).
   Exact, but much larger; only worth it if HLE timing or envelopes sound wrong.

Note: DOSBox-X's own MPU-401 sits at 0x330, the same port the cabinet's Soundscape uses; an emulation must take the
port over when `[su2000]` is enabled.
