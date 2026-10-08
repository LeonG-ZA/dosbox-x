# Network (pod linking) survey

Source: NET library in DAC.EXE (debug names: `NETI_CardGetBoardId`, `NETI_CardCheck585`, `NETI_CardCheckFor690`,
`NET_SendPacket`, `NET_AddToPacket`, `NET_GenerateMulticastTable`, ...). CONFIG.VPC (both drives):
`[NET] IRQ 5, IO_ADDRESS 0x280, MEM_ADDRESS 0xC8000`.

## Hardware [confirmed]

SMC / Western Digital 8003/8013 family Ethernet card (the library contains SMC's board-identification routine: board IDs
0x35/0x3A, 83C584/585 and 83C690 checks, EEPROM/RAM size probes). It is a **shared-memory** card:

* base+0x00..0x0F: SMC ASIC registers (memory enable, LAN address PROM, board ID, checksum)
* base+0x10..0x1F: the National 8390 network controller (`NET_SendPacket` writes TPSR at +0x14, TBCR at +0x15/16 and
  command 0x26 = transmit at +0x10)
* frames are built and received directly in the card's RAM at 0xC8000 (through a protected-mode selector)

This is **not** NE2000-compatible: the NE2000 puts the 8390 at base+0x00 and moves data through a remote-DMA port at
base+0x10, with no shared memory. The library has no packet-driver, ODI or NDIS path.

## Protocol [confirmed]

Raw Ethernet, no IPX/SPX, no IP:

| Offset | Content |
|---|---|
| 0..5 | destination: multicast (first byte 0x01, built by `NETI_PhysToMcast`) or broadcast |
| 6..11 | source = card address |
| 12..13 | type/length = **0x0000** |
| 14..15 | payload length (little-endian) |
| 16.. | atoms: type (1), length (2, little-endian), data; packets up to 1500 bytes |

Games use it to exchange player state (`TX_joystick`, `TX_tracker`, `TX_credit_info`, ... in DAC).

## Which games [confirmed, string survey]

NET library 00.02.27 – 00.02.31 in DAC, ETS, GUN, PEARL, SFL, MISS, PAC (Solo drive) and ETS, SFL (first drive).
DN2, BOX, GHOST and ZONE have no NET library. `SMC\EZSTART.EXE` is SMC's own card set-up utility.

## Consequences for emulation

* DOSBox-X's NE2000 and its IPX-over-UDP tunnel cannot be used as they are: the games talk to SMC hardware directly and do
  not use IPX.
* Option: emulate the SMC 8013 (ASIC registers + shared RAM at 0xC8000 + IRQ 5) around an 8390 core (DOSBox-X's NE2000
  module already contains one), and carry the raw frames between emulator instances, for example through a small UDP
  "hub" (frames are multicast/broadcast, so a hub that forwards everything is enough), or through pcap on a real LAN.
  DOSBox-X's slirp backend only carries IP and would not forward these frames.
