/*
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

/* Rendition Verite V1000 core: RISC processor, pixel engine and host interface.
 *
 * This file and verite_core.cpp have no DOSBox-X dependencies, so the core can be built
 * and tested on its own (tests/verite). The board glue (PCI, VGA, CRTC, RAMDAC, PLL,
 * scheduling) is in vga_verite.cpp and talks to the core through the Board interface.
 *
 * Local memory is stored as host-endian 32-bit words: the 32-bit word at RISC byte address
 * A (A % 4 == 0) is the uint32_t at mem + A. The RISC is big-endian, so the RISC byte at
 * address A is mem[A ^ 3] on a little-endian host. A PCI access with MemEndian = 0 ("no
 * swap") therefore maps x86 byte address A straight onto mem[A].
 */

#ifndef DOSBOX_VERITE_CORE_H
#define DOSBOX_VERITE_CORE_H

#include <stdint.h>
#include <stddef.h>

namespace Verite {

enum Chip {
	CHIP_V1000 = 0,
	CHIP_V2x00 = 1
};

/* Native I/O register offsets (V2200 spec table 5.2; X.org commonregs.h) */
enum {
	IO_FIFO0        = 0x00, /* 0x00..0x0f: FIFO, four byte swap variants */
	IO_FIFOINFREE   = 0x40,
	IO_FIFOOUTVALID = 0x41,
	IO_COMM         = 0x42,
	IO_MEMENDIAN    = 0x43,
	IO_INTR         = 0x44,
	IO_INTREN       = 0x46,
	IO_DEBUG        = 0x48,
	IO_LOWWATERMARK = 0x49,
	IO_STATUS       = 0x4a,
	IO_XBUSCTL      = 0x4b,
	IO_PCITEST      = 0x4c,
	IO_DMACMDPTR    = 0x50,
	IO_DMAADDRESS   = 0x54,
	IO_DMALENGTH    = 0x58,
	IO_VGAEXTEND    = 0x5c,
	IO_MEMBASE      = 0x5e,
	IO_STATEINDEX   = 0x60,
	IO_STATEDATA    = 0x64,
	IO_SCLKPLL      = 0x68,
	IO_SCRATCH      = 0x70,
	IO_MODE         = 0x72,
	IO_SCRATCH1     = 0x73,
	IO_BANKSELECT   = 0x74,
	IO_EXT_FIRST    = 0x80  /* 0x80..0xff: CRTC, memory control, RAMDAC, PLL: board side */
};

/* Debug register bits */
enum {
	DEBUG_SOFTRESET    = 0x01,
	DEBUG_HOLDRISC     = 0x02,
	DEBUG_STEPRISC     = 0x04,
	DEBUG_SCRATCH      = 0x08,
	DEBUG_SOFTVGARESET = 0x10,
	DEBUG_SOFTXRESET   = 0x20
};

/* Interrupt register bits (host side) */
enum {
	INTR_VERT      = 0x0001,
	INTR_FIFOLOW   = 0x0002,
	INTR_RISC      = 0x0004,
	INTR_HALT      = 0x0008,
	INTR_FIFOERROR = 0x0010,
	INTR_DMAERROR  = 0x0020,
	INTR_DMA       = 0x0040,
	INTR_X         = 0x0080,
	INTR_IDLE      = 0x0400
};

/* Mode register bits */
enum {
	MODE_VESA   = 0x01,
	MODE_VGA    = 0x02,
	MODE_VGA32  = 0x04,
	MODE_DMA_EN = 0x08
};

/* RISC special register file indices */
enum {
	R_SWFIFO   = 32,
	R_FIFO     = 33,
	R_SEED     = 34,
	R_SEG      = 35,
	R_SCALE    = 36,
	R_FLAG     = 37,
	R_CLIP     = 38,
	R_TIMER    = 39,
	R_NPSWFIFO = 40, /* read */
	R_DRAWCTL  = 40, /* write */
	R_NPFIFO   = 41,
	R_WINCLIP  = 46,
	R_RISCINTR = 47,
	R_SP       = 252,
	R_IRA      = 253,
	R_RA       = 254,
	R_FP       = 255
};

/* Flag register bits */
enum {
	FLAG_SIGNALSYS    = 0x00000001u,
	FLAG_PIXELBUSY    = 0x00000002u,
	FLAG_FIFOOUTRDY   = 0x00000004u,
	FLAG_ICACHEOFF    = 0x00000008u,
	FLAG_STATUS_MASK  = 0x000000f0u,
	FLAG_SYSSTAT_MASK = 0x00000f00u,
	FLAG_PICKVALID    = 0x00010000u,
	FLAG_PICKHIT      = 0x00020000u,
	FLAG_FIFOOUTFORCE = 0x40000000u,
	FLAG_FIFOINRDY    = 0x80000000u
};

enum {
	IN_FIFO_SIZE  = 32,
	OUT_FIFO_SIZE = 8,
	ROM_SIZE      = 128 * 1024
};

/* Services the core needs from the emulator. */
class Board {
public:
	virtual ~Board() {}
	/* Native I/O registers 0x80..0xff, from the host or from the RISC (0xffe00400..) */
	virtual uint32_t ExtRegRead(uint32_t off, unsigned len) = 0;
	virtual void ExtRegWrite(uint32_t off, uint32_t val, unsigned len) = 0;
	/* Interrupt line to the host */
	virtual void IrqUpdate(bool asserted) = 0;
	/* PCI bus master reads of host physical memory */
	virtual uint32_t DmaRead32(uint32_t phys) = 0;
	/* Mode or BankSelect register changed (VGA/native switch, VESA window) */
	virtual void ModeChanged() = 0;
	/* MemEndian changed: host mappings of local memory need refreshing */
	virtual void MemEndianChanged() {}
	/* RISC instructions that may run now because the host polls a register while waiting
	 * for the RISC. A board with a clock should limit this to the time that has passed:
	 * if the RISC itself waits for vertical retrace, unbounded catch-up runs stall the
	 * emulated clock and the retrace never seems to come. */
	virtual uint32_t CatchUpBudget() { return 20000; }
	/* Diagnostics */
	virtual void Message(const char *msg) = 0;
};

/* Pixel engine state (V2200 spec section 4, V1000 compatibility mode) */
struct PixelEngine {
	/* spr registers */
	uint32_t src_base, src_mode, src_mask, stride;
	uint32_t dst_base, dst_mode, dst_fmt, pmask;
	uint32_t pattern, pat_mode, pat_offset;
	uint32_t scissor_x, scissor_y, z_base, dither_offset;
	uint32_t fg_color, bg_color, fog_color, dst_color;
	uint32_t chroma_color, yuv_contrast, chroma_mask, alpha_thres, spec_color;
	uint32_t tex4pal[8];
	bool pick_valid, pick_hit;

	/* iterated parameters, as last sent by step/draw (raw fixed point) */
	int32_t x_shadow;  /* 16.16, X for draws that take X from XShadow */
	bool x_dir;        /* 1 = right to left */
	int32_t x_param;   /* 16.16, X given explicitly by a draw */
	int32_t y;         /* 16.16 */
	int32_t z;         /* 21.11 */
	bool z_ovf;
	int32_t u, v;      /* 16.16 (U*Q, V*Q when perspective correct) */
	bool u_dir, v_dir;
	uint32_t q;        /* 0.24, the Q the next pixel uses (Q runs one pixel ahead) */
	int32_t r, g, b, a, f; /* 16.16 */
	uint32_t cnt;      /* 16-bit down counter (b31:b16 of the step value) */
	uint32_t drawctl;
};

class Core {
public:
	Core(Board *board, Chip chip, uint32_t mem_size);
	~Core();

	/* hard = power on reset; otherwise a SoftReset (Debug register) */
	void Reset(bool hard);

	/* Host (PCI) access to the 256-byte native I/O space. len is 1, 2 or 4. */
	uint32_t IORead(uint32_t off, unsigned len);
	void IOWrite(uint32_t off, uint32_t val, unsigned len);

	/* Host (PCI) access to local memory through BAR0, honouring MemEndian. */
	uint32_t MemRead(uint32_t off, unsigned len);
	void MemWrite(uint32_t off, uint32_t val, unsigned len);

	/* Run the RISC for up to "cycles" cycles. Returns the number used. The RISC stops
	 * early when it stalls on a FIFO, halts or is held. */
	uint32_t Run(uint32_t cycles);
	/* Advance the timer register for cycles spent stalled/idle */
	void Idle(uint32_t cycles) { timer += cycles; }

	/* true if Run() would make progress */
	bool Runnable() const;
	bool WaitingForInput() const { return stalled_in; }

	/* Vertical retrace start from the CRTC */
	void VerticalRetrace();

	/* Board hooks */
	void SetPciMemBase(uint32_t base) { pci_mem_base = base; }
	void SetRom(const uint8_t *data, uint32_t size);
	static bool IsRomAddress(uint32_t a);

	/* RISC-view memory accessors (big-endian addressing, wrapped to installed memory) */
	uint8_t  Read8(uint32_t a) const  { return mem[(a & mem_mask) ^ BXOR]; }
	uint16_t Read16(uint32_t a) const { return *(const uint16_t*)(mem + ((a & mem_mask & ~1u) ^ HXOR)); }
	uint32_t Read32(uint32_t a) const { return *(const uint32_t*)(mem + (a & mem_mask & ~3u)); }
	void Write8(uint32_t a, uint8_t v)   { mem[(a & mem_mask) ^ BXOR] = v; }
	void Write16(uint32_t a, uint16_t v) { *(uint16_t*)(mem + ((a & mem_mask & ~1u) ^ HXOR)) = v; }
	void Write32(uint32_t a, uint32_t v) { *(uint32_t*)(mem + (a & mem_mask & ~3u)) = v; }

	uint8_t *Memory() { return mem; }
	uint32_t MemSize() const { return mem_size; }
	uint8_t ModeReg() const { return mode; }
	uint8_t MemEndian() const { return memendian; }
	uint32_t BankSelect() const { return bank_select; }
	bool IrqAsserted() const { return (intr & intr_en) != 0; }

	/* statistics / diagnostics */
	uint64_t instructions;
	uint64_t pixels;
	unsigned trace_count; /* when non-zero, log this many instructions */
	unsigned ctl_log_left;  /* when non-zero, log this many RISC control register accesses */

	/* public for state readback and tests */
	uint32_t reg[256];
	uint32_t pc, npc;
	PixelEngine pe;

#if defined(WORDS_BIGENDIAN) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
	static const uint32_t BXOR = 0, HXOR = 0;
#else
	static const uint32_t BXOR = 3, HXOR = 2;
#endif

private:
	Board *board;
	Chip chip;
	uint8_t *mem;
	uint32_t mem_size, mem_mask;
	uint8_t rom[ROM_SIZE];
	bool have_rom;

	/* FIFOs */
	uint32_t in_fifo[IN_FIFO_SIZE];
	unsigned in_head, in_count;
	uint32_t out_fifo[OUT_FIFO_SIZE];
	unsigned out_head, out_count;

	/* host registers */
	uint8_t comm_sys;       /* SysStatus (host -> RISC) */
	uint8_t memendian;
	uint16_t intr, intr_en;
	uint8_t debug;
	uint8_t lowwatermark;
	uint8_t xbusctl;
	uint8_t vga_extend;
	uint8_t stateindex;
	uint16_t scratch;
	uint8_t mode, scratch1;
	uint32_t bank_select;
	uint32_t sclkpll;
	uint32_t pci_mem_base;
	uint8_t latch[0x80];    /* byte latch for partial accesses to word registers */

	/* DMA */
	uint32_t dma_cmdptr, dma_addr, dma_len;
	bool dma_busy, dma_xfer;

	/* RISC state */
	uint32_t flag;          /* writable flag bits */
	uint32_t seg, seed_out, scale_out, seed_index, clip;
	uint32_t timer;
	uint32_t riscintr;
	bool halted, stalled_in, stalled_out;
	bool forced_valid;      /* host wrote the decode instruction register */
	uint32_t forced_ir;
	bool step_pending;
	bool in_reset;

	unsigned undef_logged;

	/* host interface helpers */
	void UpdateIrq();
	void SetIntr(uint16_t bits);
	void PushInput(uint32_t v);
	uint32_t PopOutput();
	void WriteDebug(uint8_t v);
	uint32_t StateRead(unsigned index);
	void ByteWrite(uint32_t off, uint8_t v);
	uint8_t ByteRead(uint32_t off);
	void CatchUp(); /* run the RISC while the host polls for it, within Board::CatchUpBudget() */
	void DmaStart();
	void DmaPump();

	/* RISC */
	void Message(const char *fmt, ...);
	uint32_t Fetch(uint32_t a);
	bool StepOnce();
	unsigned Resolve(unsigned idx) const;
	uint32_t ReadReg(unsigned idx);
	uint32_t PeekReg(unsigned idx) const;
	void WriteReg(unsigned idx, uint32_t v);
	void ReadPair(unsigned idx, uint32_t &even, uint32_t &odd);
	void WritePair(unsigned idx, uint32_t even, uint32_t odd);
	uint32_t FlagRead() const;
	uint32_t LoadWord(uint32_t a);
	uint8_t LoadByte(uint32_t a);
	uint16_t LoadHalf(uint32_t a);
	void StoreWord(uint32_t a, uint32_t v);
	void StoreHalf(uint32_t a, uint16_t v);
	void StoreByte(uint32_t a, uint8_t v);
	uint32_t CtlRead(uint32_t a);
	void CtlWrite(uint32_t a, uint32_t v, unsigned len);
	void Seed(uint32_t x);
	void Execute(uint32_t ir, uint32_t cur);
	void Trace(uint32_t ir, uint32_t cur);
	unsigned FifoPops(uint32_t ir) const;

	/* pixel engine */
	void PEReset();
	void SprWrite(unsigned idx, uint32_t v);
	uint32_t PEStateRead(unsigned index) const;
	void StepParam(unsigned field, uint32_t value, uint32_t s1);
	void DrawParallel(uint8_t op, unsigned d, unsigned s2, unsigned s1);
	void DrawParam(uint8_t op, unsigned d, unsigned s2, unsigned s1);
	void SetParam(unsigned field, int32_t value);
	int32_t GetParam(unsigned field) const;
	void PixelParallel(int x, int y, int u, int v, uint32_t pat_bit_valid);
	void PixelFull(int x, int y);
	uint32_t RowOffset(uint32_t y, unsigned stride_field) const;
	uint32_t FetchTexel(int u, int v);
	uint32_t SampleTexture(int32_t u, int32_t v);
	uint32_t ReadDstARGB(uint32_t addr, unsigned fmt, unsigned bpp) const;
};

/* Pixel format helpers shared with the CRTC */
unsigned FormatBytes(unsigned fmt);         /* 0 for 4-bit formats */
uint32_t ExpandToARGB(uint32_t raw, unsigned fmt, bool bgr, bool nopad);

} /* namespace Verite */

#endif
