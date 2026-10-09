# Playing in a VR headset (WebXR)

DOSBox-X serves a WebXR page; the headset's browser shows the PIX video and sends the head and controller poses back,
which replace the InsideTrak sensors (sensor 1 = head, sensor 2 = hand) and the trigger / walk buttons.
No headset runtime or plug-in is linked into DOSBox-X.

## Setup

```ini
[su2000]
vr port = 8090        # 0 = off
stereo = true         # optional: a real right-eye image (doubles the PIX work)
```

Open `http://localhost:8090/` in a WebXR browser and press **Enter VR**. WebXR only runs on `localhost` or https:

* **PC headset (Rift S, Link, SteamVR):** desktop Chrome or Edge on the same PC, `http://localhost:8090/`.
* **Quest (standalone):** connect by USB, run `adb reverse tcp:8090 tcp:8090`, then open `http://localhost:8090/` in
  the Quest browser. (Plain `http://<pc-ip>:8090/` only shows the flat preview: the browser refuses WebXR there.)

The page also works without a headset as a flat preview of both eyes.

URL parameters: `player=1|2` (video channel; only player 1 drives the tracker so far), `fov=<degrees>` (horizontal
field of view of the picture, default 60), `aspect=<w/h>` (default: drawn width / height).

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

Credits are still added on the PC (`c`).

## How it works

* `xrserver.cpp`: HTTP + WebSocket server on its own threads. Every new PIX frame is sent as RGB565 (both eyes when
  `stereo` is on) with the head pose that was current when it was taken.
* The page draws each eye's image on a quad 10 m away, placed with that frame's head yaw and pitch. Turning the head
  between emulator frames is therefore corrected by the headset at its own frame rate (rotational time-warp), and head
  roll keeps the horizon level (the game does not roll the camera).
* `tracker.cpp` `TRACKER_XRPose`: yaw and pitch are written straight into the raw azimuth / elevation words (offset
  from `[su2000] tracker pose`, yaw relative to the recentre direction). Positions become targets of the DAC closed-loop
  placement (head = (0, 0, 2100) + movement, hand = head + controller offset, both in the head's yaw frame).
* The game draws into only part of each 768-pixel line (about 352 pixels in DAC); the page crops to the widest drawn
  column it has seen.

## Status and limits

* Tested with a scripted WebSocket client in DAC: 40 degrees of headset yaw gave game azimuth 0.698 rad, and the hand
  went to the controller's head-relative position. Not yet tried in a headset; the elevation sign and the default
  field of view (60 degrees) are unconfirmed.
* Position placement only works in DAC (it uses DAC's data addresses); other games get orientation only.
* Only player 1 drives the tracker; DN2's second player and the second tracker card are not wired yet.
* Uncompressed RGB565: about 9 MB/s mono / 18 MB/s stereo at 24 frames/s. Fine over USB or on the same PC; Wi-Fi may
  need compression later.
