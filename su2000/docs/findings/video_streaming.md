# Headset video: still images vs. a video codec

What the VR link sends today, measured against H.264 / HEVC / VP9 / AV1, and how the two eyes could share work.

## Today

`xrserver.cpp` encodes **each eye separately as a still image** every frame (`Encoded::jpg`, one entry per eye):
PNG (stb_image_write), JPEG (stb, quality 90 by default) or, with `vr image format = auto` (the default), whichever of
the two is smaller for that frame. Nothing is shared between the eyes or between frames. The images go over the
WebRTC data channel (or the WebSocket) in 60 KB pieces; the page decodes them with `createImageBitmap`.

## Measurement

150 consecutive stereo frames (3 s at 50 frames/s) captured from the emulator with `vr image format = png`:
DN2 attract mode (textured, camera moving slowly; 336x250 per eye) and Boxing attract mode (flat / Gouraud shaded,
camera cuts; 352x250 per eye). Encoded with FFmpeg 8.1 in low-latency settings (no B-frames; x264/x265
`-tune zerolatency -preset veryfast`, libvpx `-deadline realtime -cpu-used 8 -lag-in-frames 0`, SVT-AV1 preset 10,
NVENC `-tune ull`) on a Ryzen + RTX 5070 Ti. PSNR is against the source frames in 4:4:4.

Three ways of carrying the two eyes:

* **2 streams**: one stream per eye.
* **side by side**: one stream of 2W x H pictures.
* **alternating L/R**: one stream at 100 frames/s, left and right eye frames interleaved. The encoder's normal
  motion search then predicts the right eye from the left eye frame just before it (the eyes differ only by a small
  horizontal shift), and every standard decoder plays it.

| Stereo, 50 frames/s | DN2 (textured) | | Boxing (flat shaded) | |
|---|---|---|---|---|
| **today: server PNG per eye** (stb) | 143.7 KB/frame | **59 Mbit/s** | 33.5 KB | **14 Mbit/s** |
| PNG per eye, zlib (PIL) | 107.5 KB | 44 Mbit/s | 23.3 KB | 9.6 Mbit/s |
| JPEG q90 per eye | 41.0 KB | 16.8 Mbit/s | 39.9 KB | 16.3 Mbit/s |
| H.264 x264 crf 18, 2 streams | 4.3 KB | 1.76 Mbit/s, 36.8 dB | 5.1 KB | 2.08 Mbit/s, 35.1 dB |
| H.264 x264 crf 18, side by side | 4.1 KB | 1.68 Mbit/s, 36.8 dB | 4.9 KB | 2.00 Mbit/s, 35.0 dB |
| H.264 x264 crf 18, alternating | **2.6 KB** | **1.06 Mbit/s, 36.8 dB** | 3.4 KB | 1.38 Mbit/s, 34.1 dB |
| H.264 x264 crf 18 **4:4:4**, side by side | 4.4 KB | 1.79 Mbit/s, 42.8 dB | 5.6 KB | 2.30 Mbit/s, 40.6 dB |
| H.264 NVENC cq 20, side by side | 5.5 KB | 2.24 Mbit/s, 37.1 dB | 6.5 KB | 2.67 Mbit/s, 35.5 dB |
| H.264 NVENC cq 20, alternating | 3.8 KB | 1.57 Mbit/s, 37.3 dB | 6.5 KB | 2.67 Mbit/s, 35.6 dB |
| HEVC x265 crf 20, alternating | 2.5 KB | 1.02 Mbit/s, 36.8 dB | 3.7 KB | 1.51 Mbit/s, 34.4 dB |
| VP9 libvpx crf 30 realtime, alternating | 2.3 KB | 0.96 Mbit/s, 36.6 dB | 8.6 KB | 3.51 Mbit/s, 34.9 dB |
| AV1 SVT crf 30, alternating | 2.8 KB | 1.15 Mbit/s, 36.8 dB | 10.4 KB | 4.28 Mbit/s, 34.7 dB |
| AV1 NVENC cq 28, side by side | 3.7 KB | 1.52 Mbit/s, 37.0 dB | 4.0 KB | 1.65 Mbit/s, 35.4 dB |

Findings:

* **Any video codec is 10-40x smaller** than today's images at good quality: about 1-2.5 Mbit/s for both eyes at
  50 frames/s, against 14-59 Mbit/s now. That is the difference between needing a strong Wi-Fi 6 link and working on
  almost anything (the black right eye in the headset coincided with the data rate climbing past ~2-5 MB/s).
* **H.264 is the best fit.** At this tiny resolution HEVC, VP9 and AV1 gain little or nothing over x264 (their tools
  pay off at high resolution), cost more CPU, and have patchier decoder support in headset browsers. H.264 decodes in
  hardware everywhere WebCodecs runs (Quest Browser, Chrome, Edge, Safari).
* **Sharing between the eyes works with standard decoders**: interleaving the eyes in one stream (alternating L/R)
  cuts DN2 by ~38% at the same quality. Boxing gains less in size and loses ~1 dB: its attract camera cuts every few
  seconds, and right after a cut the right eye is the only good reference. Side by side gains almost nothing (motion
  search does not look half a picture away). Proper multi-view coding (MV-HEVC) has no browser support.
* **Chroma subsampling** costs the most quality: the PIX pictures have sharp saturated colour edges (HUD text, flat
  polygons), and 4:2:0 holds PSNR near 35-37 dB whatever the bit rate. 4:4:4 H.264 reaches 41-43 dB for +7-15% bits,
  but browsers / headset hardware decoders generally only do 4:2:0 H.264 (4:4:4 is a High 4:4:4 Predictive profile
  feature). VP9 profile 1 and AV1 do 4:4:4 in software on some browsers only. Encoding the picture at 2x size in
  4:2:0 is a portable alternative if the colour edges look soft.
* **Encoding cost** is negligible at 352x250: x264 veryfast takes well under 1 ms per frame on one core; NVENC
  needs no CPU at all but is ~30% larger at equal quality and adds driver dependence.
* **PNG deflate**: stb's own deflate was weak; the server now hands it to zlib (level 6): 143.7 -> 100.1 KB per
  DN2 stereo frame, done while the images stay.

## What implementing it would take

* **Encoder (server)**: libx264 (GPL-2.0, compatible with DOSBox-X) or OpenH264 (BSD; Cisco's binary module covers
  the H.264 patent licence). One encoder per player, fed the alternating L/R frames (or side by side), settings as
  above: no B-frames, zero lookahead, 2 reference frames, constant quality with a bit-rate cap, a key frame every
  ~2 s and on request. Raw RGB -> I420 conversion first.
* **Transport**: the existing data channel / WebSocket, one message per encoded frame with the pose header as today.
  Video frames depend on earlier frames, so the transport must not drop single pieces the way it does now
  (`client_send` drops pieces when more than 1 MB is queued): skip whole frames at the encoder instead, and send a
  key frame when the client asks (after a gap or an error). The alternative, a WebRTC media track (libdatachannel can
  packetise H.264), gives jitter handling for free but makes it harder to keep each frame's head pose attached.
* **Decoder (page)**: WebCodecs `VideoDecoder` (`avc1.42E01F`-style codec string, `optimizeForLatency: true`), output
  `VideoFrame`s uploaded with `texImage2D` like the bitmaps today; the PNG/JPEG path stays as the fallback for browsers
  without WebCodecs. With alternating L/R the page pairs frame 2n (left) and 2n+1 (right).
* **Latency**: one frame of encode + decode at most with these settings, about what JPEG decoding costs now.

Estimated effort: about two days (encoder integration in the Visual Studio and autotools builds, transport changes,
page decoder with fallback, tests). The measurement script is not in the repository; it used FFmpeg and the frames
captured over the link.
