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

## HLE status (sscape.cpp) [working in DAC]

| Step | What the library does | Emulation |
|---|---|---|
| detect | ODIE index reg is 4 bits (write 0xFF, read 0x0F); reg 9 bits 7..6 = 01 means "already running" | 4-bit index |
| firmware | DMA A of SNDSCAPE.COD (61440 bytes), then reg 9 = 0xC0 and wait for 0xFE on the host port | bytes taken from the PC DMA channel and dropped; 0xFE queued |
| transport | each byte: wait host status bit 1, `out base+2, 0x81` (commands) or `0x85` (MIDI), `out base+3, byte` | MIDI parsed; commands collected |
| ack / replies | IRQ 7 handler (DAC 0xb13e2) reads ODIE reg 0 (bit 1 host data, bit 7 = more), host status bit 2 = control byte. Control 0x80 = ack; other control bytes go to the reply ring read by 0xb10ae | 0x80 ack 0.2 ms after the last byte of a command; replies as control bytes |
| queries | 0x9F firmware version (10 bytes), 0x85 free memory (6 x 4), 0x89 / 0x99 / 0x9E (1), 0x9B (3) | fixed answers |
| 0x80 download sample | id(2) length(4) format(1: 0x60 = 8-bit) start(4) loopstart(4) loopend(4) loopend(4) pitch(3) loop(2); pitch = (log2(44100/rate)+5)*2048; data by DMA B | stored, 8-bit unsigned -> 16-bit |
| 0x86 patch / 0x87 program | patch id(2) .. sample id at byte 13; program id(2) .. patch id at byte 4 | mapping kept |
| play | MIDI: program change, note on (note 60 = original pitch), CC7, CC10, pitch bend | 32-voice mixer, 44.1 kHz |

Start-up with sound takes about 7 s; a DAC match plays its effects. Two cards: the library opens `sound2` too if CONFIG.VPC
defines it, and game sounds go to card = player + 1 (`SOUND_do_2D_sound`); the Solo machine has one card and the local player is
player 0.

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

## Second card (two-player cabinet)

`[su2000] sound port` takes up to two cards as `port:irq[:dma]`. The first drive's CONFIG.VPC has sound1 at 0x330
IRQ 12 and sound2 at 0x350 IRQ 7 (`DMA 1,3`), so `dosbox-su2000.conf` uses `0x330:12:1 0x350:7:3` and turns off the
PS/2 mouse (`[keyboard] aux=false`, IRQ 12). Each card has its own state, IRQ, DMA channel and mixer channel (SSCAPE,
SSCAPE2). Measured: DN2 now starts both cards, downloads its samples to each without errors (before: "Error installing
sound ... on card 1" for every sample and no picture after the PIX banner) and runs its attract mode. With the VR link
each player's headset gets its own card (44.1 kHz); with one card it gets the whole DOSBox-X mix.
