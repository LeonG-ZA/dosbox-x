# Milestone 1C/1E — dynamic capture in DOSBox-X

## Set-up (reproducible)

```
# build DOSBox-X once (repo root):  ./autogen.sh && ./configure --enable-sdl2 && make -j4
cd su2000
tools/make_workcopy.sh                  # data/work/c = original HDD + CONFIG.VPC without [NET]
tools/run_game.sh dn2 DN2 "dn2" 90      # headless, SIGKILL after 90 s, trace in logs/dn2.su2k
python3 tools/su2kstats.py logs/dn2.su2k
python3 tools/extract_upload.py logs/dn2.su2k logs/dn2_upload data/original/hdd/DN2/MAINA.OUT data/original/hdd/DN2/MAINB.OUT
python3 tools/fifo_commands.py logs/dn2.su2k data/original/hdd/DN2/DN2.EXE --dump 50
python3 tools/readback_check.py logs/dn2.su2k data/original/hdd/DN2/MAINA.OUT data/original/hdd/DN2/MAINB.OUT
```

Emulator configuration: `dosbox-su2000.conf` (486, 32 MB, `ems=false umb=false` so C8000–E0FFF is free, as with the original
`EMM386 X=C800-E0FF`). Trace format: 24-byte records (kind, width, CS, EIP, address, value, aux, µs). Kinds are listed in
`src/hardware/su2000/su2000.h`; the decoder is `tools/su2klog.py`.

The ETS CPU-A image can be extracted with:
`python3 -c "d=open('data/original/hdd/ETS/ETS.EXE','rb').read(); open('data/extracted/ETS_EMB1.OUT','wb').write(d[0xA1E24:0xA1E24+0x20000])"`.

## Values faked by the stubs (complete list)

| Device | What | Faked value | Why / source |
|---|---|---|---|
| PIX proc card P+0 read | FIFO status | `0xC0 \| (ctrl & 0x1F)`: never full or half-full, always empty | `PIXI_FIFOStatus` semantics; no FIFO model yet |
| PIX board 0x2102 / 0x2100 | CPU-ready words | written as 9 when the host sets run B / run A | firmware does this itself (`ldcr r2,cr0; extu r2,r2,7<1>`; MAINA 0x4A764-0x4A780); host needs ≥ 9 |
| PIX board memory | everything else | plain RAM: reads return what the host wrote (0 otherwise) | so 0x2158/0x214C "busy" reads 0 → render is "done" instantly |
| PIX video card 0x340–0x34F | registers | latch: read returns last value written | no FPGA/video model |
| InsideTrak 0x270/0x278 | status (base+1) | bit1 always set, bit0 when a reply is queued | `TRKI_IT_SendData/GetData` |
| InsideTrak | reply to `S` | 56 bytes: `21S`, version `3.00` at 0x0F, `InsideTRAK` at 0x19 | `TRKI_IT_Open` @873C0 |
| InsideTrak | every other command | no reply | Milestone 3 |
| Format cards 0x210/0x218, network 0x280 | I/O | read 0xFF | not emulated; DN2 prints `CTRL ERROR (-6154) : Format C not found.` and continues |
| Format card window 0xE0000, net window 0xC8000 | memory | plain RAM | not emulated |
| CONFIG.VPC (work copy only) | `[NET]` section removed | — | with `[NET]` present but no card, DN2's NET library clears a buffer sized from card RAM and faults (GPF in `_fmemset`, DN2 image 0x6EE53, called from 0x4E3AB) |

## Results

| Program | Gets past firmware upload? | Main loop? | Notes |
|---|---|---|---|
| **DN2** | yes, both channels ("2 video channels active, 1 and 2 used / PAL Video") | **yes**: 34 345 `RenderSwap` in 69 s; the stub answers instantly, so ~500 fps | 8.57 M FIFO words / 465 104 commands in 90 s |
| ETS | yes (embedded A image + `\MAINB.OUT`) | no: polls InsideTrak status (0x271/0x279) waiting for tracker data | switches the VGA to graphics (attendant/test display) |
| SP | yes (own 1994 firmware) | yes: 1.1 M FIFO words in 45 s; VGA graphics mode | Phar Lap host: the FIFO caller attribution does not work (different stack layout) |
| SFL | **no**: exits before touching hardware | — | "Not enough memory to allocate file structures": Watcom runtime `malloc(8)` fails in start-up (image 0x91214). Heap growth depends on the runtime's extender detection (`[0xC727E]`, path 0x8FA10). Unchanged by memsize 4–64 MB, `xms=false` or `DOS4GVM`. **Open issue.** |

## 1E. Traffic classification (DN2, 90 s)

Every FIFO word is accounted for by a recognised command. Boundaries are found without symbols (`mov eax, OP; call OutWord` at the
caller). Fixed lengths per opcode, in 16-bit words including the opcode:

| Op | Command | Count | Length | Class |
|---|---|---|---|---|
| 0x01 | ViewMAT | 52 092 | 26 (op, view, 12 floats) | scene: camera matrix |
| 0x04 | ViewLIST | 35 804 | 34 | scene: models in view |
| 0x05 | Window | 4 | 15 | set-up |
| 0x07 | LightSource | 4 | 12 | set-up |
| 0x08 | Texture | 97 | 22 (id, type, **board address**, 6 floats) | set-up, references uploaded data |
| 0x09 | WindowSwitch | 3 | 4 | set-up |
| 0x0A | Model | 61 004 | 17–58 (variable) | scene |
| 0x0B | ModelID | 2 | 4 | scene |
| 0x0C | ModelMAT | 149 778 | 26 | scene: object matrix |
| 0x0E | ModelPOS | 7 240 | 8 (op, model, x, y, z) | scene |
| 0x0F | ModelPALETTE | 25 | 18 | scene |
| 0x10 | ModelOTHER | 14 243 | 10 | scene |
| 0x12 | RenderSwap | 32 340 | 1 | frame |
| 0x23 | SetBufferClearMode | 3 | 6 | set-up |
| 0x29 | ModifyModelPriority | 20 846 | 6 | scene |
| 0x2A | ModelDATA | 887 | 17–27 | scene |
| 0x2B | Material | 32 329 | 9 | scene |
| 0x2D | SyncRender | 22 420 | 1 | sync |
| 0x36 | FLIC frame | 371 | 8 | video playback from `.fmvmem` |
| 0x37 | ModifyModelID | 56 | 6 | scene |
| 0x39 | GenerateStatic | 22 401 | 8 | effect |
| 0x3A | SyncRenderFLIC | 9 920 | 1 | sync |
| 0x3E | DownloadColourPalettes | 1 | 109 | set-up |
| 0x3F | SetTwinkleFrame | 3 234 | 4 | effect |

* **Command stream / display list:** all of it. Matrices, positions, IDs, palettes and render/swap/sync. [confirmed]
* **Data uploads mid-game:** yes. FLIC frames go to `.fmvmem` 0x150000, and textures/models are allocated in board memory and
  referenced by board address in commands. [confirmed]
* **Code uploads mid-game:** **none** (0 writes to `.vecs`/`.text` after `RUN_A`). [confirmed for DN2]
* **Read-backs after boot** (complete list from the DN2 trace): board 0x214C/0x214E (channel busy, polled; the newer library uses 0x2158),
  0x213C–0x2146 (load counters), 0x2120–0x2136 (once per frame, `PIXI_LoadManagement`), 0x2100/0x2102 (one boot check),
  processor-card status port P+0 (FIFO flow control). **No geometry, collision or game-state results are read back.** [confirmed for DN2]
