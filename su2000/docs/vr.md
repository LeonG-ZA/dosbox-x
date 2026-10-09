# Playing in a VR headset (WebXR)

DOSBox-X serves a WebXR page; the headset's browser shows the PIX video, plays the game sound and sends the head and
controller poses, buttons and microphone back. The poses replace the InsideTrak sensors (sensor 1 = head, sensor 2 =
hand). No headset runtime or plug-in is linked into DOSBox-X.

## Setup

```ini
[su2000]
vr port = 8090        # 0 = off
stereo = true         # optional: a real right-eye image (doubles the PIX work)
vr image format = png # png (lossless, default) or jpeg
vr jpeg quality = 95  # with jpeg; above 90 the colour is not subsampled
vr audio = true       # game sound to the headset (it also keeps playing on the PC)
vr webrtc = true      # UDP data channel; false = WebSocket (TCP) only
vr certificate = su2000vr   # https certificate files su2000vr.crt / .key (created if missing)
```

WebXR only runs on `localhost` or https. The server answers both on the same port: plain http, and https with a
self-signed certificate (EC P-256, "CN=SU2000 VR"), created on first start as `su2000vr.crt` / `su2000vr.key` in the
working directory and reused afterwards (they are git-ignored; delete them to make a new one). The log lists the https
addresses at start-up.

* **Quest browser over the network (Wi-Fi / Virtual Desktop):** open `https://<pc-ip>:8090/` (for example
  `https://192.168.88.24:8090/`), choose *Advanced* -> *Proceed* at the certificate warning once, then **Enter VR**.
  Allow DOSBox-X through the firewall on private networks if asked.
* **PC headset (Rift S, Link, Virtual Desktop with SteamVR):** desktop Chrome or Edge on the same PC,
  `http://localhost:8090/`. The browser uses the active OpenXR runtime: start it first (for example SteamVR), or set
  Virtual Desktop's VDXR as the active runtime.
* **Quest by USB:** `adb reverse tcp:8090 tcp:8090`, then `http://localhost:8090/` in the Quest browser.

The page also works without a headset as a flat preview of both eyes. Browsers only start sound after a click: the
**Enter VR** click (or any click on the page) starts it. Tick **Microphone** to send your voice (the browser asks for
permission).

URL parameters: `player=1|2` (video channel; only player 1 drives the tracker so far), `fov=<degrees>` (horizontal
field of view of the picture, default 60), `aspect=<w/h>` (default: width / height of the image), `rtc=0` (no WebRTC).

## Controls

| Headset | Game |
|---|---|
| head turn / tilt | head sensor azimuth / elevation |
| head movement | head position (relative to where you recentred) |
| controller aim and position | hand sensor (gun) |
| trigger | fire (format card button 8, Ctrl+5) |
| grip or A/X | walk (button 9, Ctrl+6) |
| B/Y | recentre |
| thumbstick up/down | field of view |
| microphone | format card mic level; heard by the other player's headset |

Credits are still added on the PC (`c`).

## Transport

* **WebSocket** (`/ws`, TCP, ws or wss): control (player choice, recentre) and WebRTC signalling, and the fallback for
  everything else.
* **WebRTC data channel** (UDP, DTLS + SCTP, unordered, no retransmissions): video, game audio, microphone and poses.
  A lost packet is skipped instead of delaying the ones behind it. The browser offers, DOSBox-X answers
  (libdatachannel); only host candidates are used, which is enough on a LAN.
* **Video** is sent as images, not a video stream: each eye as a lossless PNG (or JPEG; stb_image_write), cropped to the drawn width
  (the game draws into about 352 of the 768 pixels of a line in DAC), split into pieces of at most 60 KB, with the head
  pose the frame was taken with. One picture is sent once every board (and with stereo both eyes) has a new frame.
  Measured in DAC: PNG about 6 KB per eye, 50 stereo frames/s, 0.55 to 1.2 MB/s. The flat-shaded pictures compress
  better losslessly than as JPEG (JPEG at quality 85 was about 14 KB and showed artefacts on the polygon edges).
* **Game audio**: the DOSBox-X mixer output (48 kHz stereo, 16-bit PCM) in 10 ms packets; the page plays it through an
  AudioWorklet with a 60 ms jitter buffer (trimmed back to 80 ms when it grows past 250 ms) and resamples to the
  headset's rate.
* **Microphone**: 16 kHz mono 16-bit PCM in 20 ms packets, with echo cancellation and noise suppression by the browser.
* Each client logs its transport, frame rate and data rate every 10 s.

## How the picture is shown

* The page draws each eye's image on a quad 10 m away, placed with that frame's head yaw and pitch. Turning the head
  between emulator frames is therefore corrected by the headset at its own frame rate (rotational time-warp), and head
  roll keeps the horizon level (the game does not roll the camera).
* `tracker.cpp` `TRACKER_XRPose`: yaw and pitch are written straight into the raw azimuth / elevation words (offset
  from `[su2000] tracker pose`, yaw relative to the recentre direction). Positions become targets of the DAC closed-loop
  placement (head = (0, 0, 2100) + movement, hand = head + controller offset, both in the head's yaw frame).

## Building

* **Windows (Visual Studio, x64):** the project builds mbedTLS 3.6 and libdatachannel 0.24 (submodules `vs/mbedtls`,
  `vs/libdatachannel`) once with `vs/build-webrtc.cmd` (CMake from Visual Studio) into `obj/webrtc`. After a fresh
  clone run `git submodule update --init vs/mbedtls vs/libdatachannel` (the script fetches the nested ones it needs).
* **Linux:** install libdatachannel and mbedTLS 3 (development packages), then `./autogen.sh && ./configure`; configure
  enables https / WebRTC when it finds both (`--disable-su2000-vrlink` turns it off). Without them DOSBox-X still serves
  the page over plain http / ws. Some distributions still ship mbedTLS 2, which is not enough.

## Status and limits

* Tested on Windows: scripted clients over https on the LAN address (frames, audio, microphone relay between player 1
  and player 2) and headless Edge (WebRTC data channel opened, 50 frames/s). Headset use is reported working over https;
  WebRTC, sound and microphone have not been tried in a headset yet.
* Linux: the code builds without the libraries on the CI; the build with libdatachannel and mbedTLS has not been
  compiled yet.
* Position placement only works in DAC (it uses DAC's data addresses); other games get orientation only.
* Only player 1 drives the tracker; DN2's second player and the second tracker card are not wired yet.
* The relayed microphone is played centred: the format card's fade (DAC places the opponent's voice in 3D) is not
  decoded yet. See `findings/microphone.md`.
