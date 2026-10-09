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

## Emulation (`[su2000] network`)

`smc8013.cpp` emulates the card and links emulators over UDP (no IPX, no game changes):

* ASIC: MSR (reset, memory enable), ICR bit 0 = 16-bit card (read only), general registers incl. GP2 (passes the
  83C583 write test), station address 00:00:C0:xx:xx:xx at +8..13, board id 0x04 (revision 2), checksum.
* 8390 in shared-memory mode: pages 0..2, transmit from card RAM (TPSR / TBCR, CR 0x26), receive ring PSTART..PSTOP
  with the 4-byte header (count includes 4 CRC bytes), BNRY / CURR, overflow, ISR / IMR and IRQ 5; loopback transmit
  config (TCR 4, used while the library initialises) sends nothing. 16 KB of card RAM at 0xC8000. Broadcast and every
  multicast are accepted when RCR allows them (the library filters by group).
* Link: `network = relay:<port>` on one emulator (it also forwards every datagram to all other pods),
  `network = <host>:<port>` on the others; datagrams "SU2N" + frame, "SU2K" keep-alive, "SU2V" headset microphone
  packets (the pods' analogue MICNET line).
* The game's CONFIG.VPC needs the `[NET]` section of the original files (IRQ 5, IO_ADDRESS 0x280, MEM_ADDRESS 0xC8000);
  the working copies had it removed.

Measured with two DOSBox-X instances on one PC (relay + 127.0.0.1): DAC's library initialises the card (ring
06h..20h, receive config 0Eh), sends to the multicast group 01:44:41:43:30:31 ("\x01DAC01"), both pods receive each
other's frames, and they play one match against each other (pod A's scoreboard STEALTH vs VEGA, pod B's VEGA vs
STEALTH, instead of the computer opponent STING). DAC then sets the MICNET fade (format card register 0x94), and a
headset microphone on pod A is heard on pod B's headset with pod B's MICNET levels.
