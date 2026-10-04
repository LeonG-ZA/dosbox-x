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

/* Rendition Verite V1000: RISC processor, pixel engine and host interface.
 *
 * This is a low level emulation: the microcode that the guest driver uploads is executed
 * instruction by instruction, and the pixel engine is driven by the RISC's spr, step and
 * draw instructions exactly as on the real chip. Nothing here depends on recognising a
 * particular microcode.
 *
 * References:
 *   [SPEC]  Rendition V2200 Technical Specification (1159-001, ECN 0058, RevB 1/98),
 *           "including RISC programming". Documents the instruction set, the register
 *           file, the pixel engine and the host registers, including the V1000
 *           compatibility mode (step/draw/rdraw) that V1000 microcode uses.
 *   [XORG]  xf86-video-rendition (MIT license): commonregs.h (I/O map), v1krisc.c (how a
 *           host holds, single steps and forces instructions into the RISC), vloaduc.c
 *           (ELF microcode loading), vmodes.c (MemEndian per colour depth).
 *   [UC]    The V1000 2D microcode shipped with [XORG] (v10002d.uc), whose symbols were
 *           used to confirm the semantics below.
 *
 * Behaviour that [SPEC] leaves open, and the choice made here (each confirmed against
 * [UC] where it is exercised there):
 *   - getpc, jmpl, rjmpl and jmprl link to their own address + 8 (past the delay slot);
 *     the [XORG] context switch code's dispatch table relies on this for getpc.
 *   - The seed ROM returns floor(2^62 / m), m being the operand shifted left until bit 31
 *     is set. [UC]'s line setup refines it with one Newton step in 0.24 fixed point,
 *     which only converges with this scaling. It is computed exactly instead of from a
 *     13-bit ROM index.
 *   - draw/drawp decrement Cnt by the pixels drawn; rdraw/rdrawp draw Cnt pixels and leave
 *     Cnt at 0. Pixels taken from XShadow advance it by one pixel in XDir.
 *   - Parallel draws walk the source one pixel per destination pixel in XDir; the dU
 *     register value is not used (the microcode sets it to 0 or 0xfff80000 by convention).
 *   - Pair 2 values 0..7 of DrawCtl are not listed in [SPEC] table 2.9c; they are taken to
 *     be the same as pair 3/pair 4 values 0..7 (logged when used).
 *   - The icache and the line buffer are not emulated: memory is always coherent.
 *   - The pixel engine is synchronous: drawing completes inside the draw instruction, so
 *     PixelBusy always reads 0.
 */

#include "verite_core.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <algorithm>

namespace Verite {

namespace {

/* Read-only constant registers ([SPEC] table 2.2b) */
const uint32_t const_regs[32] = {
	0x00000000, 0x00000000, 0x00008000, 0x80000000, /* zero zer1 if0_5 msb */
	0xFF000000, 0x00FF0000, 0x0000FF00, 0x000000FF, /* b0msk b1msk b2msk b3msk */
	0xFFFF0000, 0x0000FFFF, 0xFF00FF00, 0x00FF00FF, /* h0msk h1msk b0b2msk b1b3msk */
	0x00000000, 0xFFFFFFFF, 0x00000000, 0x00010000, /* zer2 minus1 zer3 if1_0 */
	0xFFFFFFFF, 0x00010000, 0x007FFFFF, 0x02000000, /* allfs bit16 fpfrac bit25 */
	0x00800000, 0x00020000, 0x0000B000, 0x0000B800, /* bit23 bit17 b000 b800 */
	0x0000C000, 0x0000C800, 0x0000D000, 0x0000D800, /* c000 c800 d000 d800 */
	0x0000E000, 0x0000E800, 0x0000F000, 0x0000F800  /* e000 e800 f000 f800 */
};

inline uint32_t swap_mode(uint32_t v, unsigned m) {
	switch (m & 3) {
		case 1: return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
		case 2: return ((v >> 8) & 0x00FF00FFu) | ((v << 8) & 0xFF00FF00u);
		case 3: return (v >> 16) | (v << 16);
		default: return v;
	}
}

/* x86 byte lane -> RISC byte lane, per MemEndian / DMA swap mode */
const uint32_t host_lane_xor[4] = { 3, 0, 2, 1 };

inline int32_t sext16(uint32_t v) { return (int32_t)(int16_t)(uint16_t)v; }

inline uint32_t next_pc(uint32_t a) {
	/* the RISC does not increment across a 16MB boundary [SPEC] 2 */
	return (a & 0xFF000000u) | ((a + 4u) & 0x00FFFFFFu);
}

inline uint32_t rel_target(uint32_t slot, int32_t words) {
	return (slot & 0xFF000000u) | ((slot + (uint32_t)words * 4u) & 0x00FFFFFFu);
}

inline uint32_t abs_target(uint32_t field) {
	/* the msb of the 24-bit address selects ROM */
	return (field & 0x800000u) ? (0xFE000000u | ((field & 0x7FFFFFu) << 2)) : ((field & 0x7FFFFFu) << 2);
}

inline unsigned clz32(uint32_t v) {
	unsigned n = 0;
	if (v == 0) return 32;
	while (!(v & 0x80000000u)) { v <<= 1; n++; }
	return n;
}

inline uint8_t clamp255(int32_t v) { return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

/* parameter 16.16 (or 21.11 for Z) to the 0..255 value the pixel engine sees */
inline uint8_t param8(int32_t v) { return clamp255(v >> 16); }

/* Operand usage per opcode, for FIFO stall checks */
enum {
	USE_S1 = 0x01, USE_S2 = 0x02, USE_D = 0x04,
	VEC_S1 = 0x08, VEC_S2 = 0x10, VEC_D = 0x20
};

uint8_t op_use[256];
bool op_use_ready = false;

void init_op_use() {
	if (op_use_ready) return;
	memset(op_use, 0, sizeof(op_use));
	for (unsigned i = 0x00; i <= 0x0F; i++) op_use[i] = USE_S2 | USE_D;
	op_use[0x0E] = USE_S2;
	for (unsigned i = 0x10; i <= 0x1F; i++) op_use[i] = USE_S1 | USE_S2 | USE_D;
	op_use[0x1E] = USE_S1 | USE_S2;
	for (unsigned i = 0x20; i <= 0x2E; i++) op_use[i] = VEC_S1 | VEC_S2 | VEC_D | USE_D;
	op_use[0x2E] = VEC_S1 | VEC_S2;
	for (unsigned i = 0x30; i <= 0x3E; i++) op_use[i] = USE_S1 | VEC_S2 | VEC_D | USE_D;
	op_use[0x3E] = USE_S1 | VEC_S2;
	for (unsigned i = 0x40; i <= 0x4D; i++) op_use[i] = USE_S2 | USE_D;
	op_use[0x4F] = USE_S2;
	op_use[0x52] = USE_S1 | USE_D;
	for (unsigned i = 0x53; i <= 0x5E; i++) op_use[i] = USE_S1 | USE_S2 | USE_D;
	op_use[0x5F] = USE_S1 | USE_S2;
	for (unsigned i = 0x60; i <= 0x65; i++) op_use[i] = USE_S1;
	op_use[0x6B] = USE_D;
	op_use[0x6F] = USE_S1 | USE_D;
	op_use[0x70] = op_use[0x71] = op_use[0x72] = USE_S1 | USE_D;
	op_use[0x73] = USE_S1;
	op_use[0x74] = op_use[0x75] = USE_S1 | VEC_D | USE_D;
	op_use[0x76] = op_use[0x77] = USE_D;
	op_use[0x78] = op_use[0x79] = op_use[0x7A] = USE_S1 | USE_S2;
	op_use[0x7C] = USE_S1 | VEC_S2;
	op_use[0x7D] = op_use[0x7E] = op_use[0x7F] = USE_S1 | USE_S2;
	for (unsigned i = 0xC0; i <= 0xC3; i++) op_use[i] = USE_S1 | VEC_D | USE_D;
	op_use[0xC4] = USE_S1 | USE_S2;
	for (unsigned i = 0xD0; i <= 0xD7; i++) op_use[i] = USE_S1 | USE_S2 | USE_D;
	for (unsigned i = 0xD8; i <= 0xDF; i++) op_use[i] = VEC_S1 | VEC_S2 | VEC_D | USE_D;
	op_use_ready = true;
}

/* Parameter identifiers for DrawCtl pairs */
enum Param {
	P_UNUSED = 0, /* "*": register updated, value not used by the pixel engine */
	P_NA,         /* not applicable */
	P_X, P_Y, P_Z, P_U, P_V, P_Q, P_R, P_G, P_B, P_A, P_I, P_F
};

struct PairDef { uint8_t shadow, even, odd; };

/* [SPEC] table 2.9c. shadow: P_X or P_Y if that coordinate comes from XShadow */
const PairDef pair1_tab[16] = {
	{P_NA, P_X, P_UNUSED}, {P_X, P_Q, P_UNUSED}, {P_NA, P_X, P_Y}, {P_X, P_Q, P_Y},
	{P_NA, P_X, P_U},      {P_X, P_Q, P_U},      {P_NA, P_X, P_R}, {P_X, P_Q, P_R},
	{P_NA, P_X, P_A},      {P_X, P_Q, P_A},      {P_NA, P_Z, P_X}, {P_NA, P_NA, P_NA},
	{P_NA, P_X, P_F},      {P_X, P_Q, P_F},      {P_NA, P_Y, P_U}, {P_Y, P_Q, P_U}
};
const PairDef pair34_tab[16] = {
	{0, P_G, P_B}, {0, P_Z, P_A},       {0, P_Z, P_UNUSED}, {0, P_UNUSED, P_F},
	{0, P_Z, P_F}, {0, P_NA, P_NA},     {0, P_R, P_F},      {0, P_R, P_A},
	{0, P_R, P_UNUSED}, {0, P_Z, P_R},  {0, P_UNUSED, P_A}, {0, P_F, P_A},
	{0, P_I, P_UNUSED}, {0, P_Z, P_I},  {0, P_I, P_A},      {0, P_I, P_F}
};
const PairDef pair2_tab[32] = {
	/* 0..7: assumed identical to pair 3/4 */
	{0, P_G, P_B}, {0, P_Z, P_A},       {0, P_Z, P_UNUSED}, {0, P_UNUSED, P_F},
	{0, P_Z, P_F}, {0, P_NA, P_NA},     {0, P_R, P_F},      {0, P_R, P_A},
	{0, P_X, P_V}, {0, P_Y, P_V},       {0, P_R, P_V},      {0, P_UNUSED, P_V},
	{0, P_I, P_V}, {0, P_Z, P_V},       {0, P_A, P_V},      {0, P_Z, P_U},
	{0, P_G, P_B}, {0, P_Z, P_A},       {0, P_Z, P_UNUSED}, {0, P_UNUSED, P_F},
	{0, P_Z, P_F}, {0, P_F, P_V},       {0, P_R, P_F},      {0, P_R, P_A},
	{0, P_R, P_UNUSED}, {0, P_Z, P_R},  {0, P_UNUSED, P_A}, {0, P_F, P_A},
	{0, P_I, P_UNUSED}, {0, P_Z, P_I},  {0, P_I, P_A},      {0, P_I, P_F}
};

/* 4x4 dither matrix [SPEC] table 4.8, indexed [y][x] */
const uint8_t dither_tab[4][4] = {
	{ 0, 12,  3, 15},
	{ 7, 11,  4,  8},
	{ 2, 13,  1, 14},
	{10,  6,  9,  5}
};

inline uint32_t expand(uint32_t c, unsigned bits, bool nopad) {
	if (bits >= 8) return c & 0xFF;
	if (bits == 0) return 0;
	const uint32_t max = (1u << bits) - 1u;
	c &= max;
	if (nopad) return c << (8 - bits);
	return (c * 255u + max / 2u) / max;
}

inline uint32_t argb(uint32_t a, uint32_t r, uint32_t g, uint32_t b) {
	return (a << 24) | (r << 16) | (g << 8) | b;
}

} /* anonymous namespace */

/* ------------------------------------------------------------------------------------ */
/* pixel formats                                                                        */

unsigned FormatBytes(unsigned fmt) {
	switch (fmt & 0xF) {
		case 1: case 2: case 3: return 1;
		case 4: case 5: case 6: return 2;
		case 0xC: case 0xD: return 4;
		default: return 0; /* 4-bit indexed, reserved */
	}
}

uint32_t ExpandToARGB(uint32_t raw, unsigned fmt, bool bgr, bool nopad) {
	uint32_t a = 255, r = 0, g = 0, b = 0;
	switch (fmt & 0xF) {
		case 1: /* 332 */
			r = expand(raw >> 5, 3, nopad); g = expand(raw >> 2, 3, nopad); b = expand(raw, 2, nopad);
			if (bgr) { /* red in the LSBs */
				b = expand(raw >> 5, 3, nopad); g = expand(raw >> 2, 3, nopad); r = expand(raw, 2, nopad);
			}
			return argb(a, r, g, b);
		case 2: /* 8-bit intensity */
			raw &= 0xFF;
			return argb(a, raw, raw, raw);
		case 3: /* 8-bit alpha, r = g = b = 255 */
			return argb(raw & 0xFF, 255, 255, 255);
		case 4: /* 565 */
			r = expand(raw >> 11, 5, nopad); g = expand(raw >> 5, 6, nopad); b = expand(raw, 5, nopad);
			break;
		case 5: /* 4444 */
			a = expand(raw >> 12, 4, nopad); r = expand(raw >> 8, 4, nopad);
			g = expand(raw >> 4, 4, nopad); b = expand(raw, 4, nopad);
			break;
		case 6: /* 1555 */
			a = (raw & 0x8000) ? 255 : 0; r = expand(raw >> 10, 5, nopad);
			g = expand(raw >> 5, 5, nopad); b = expand(raw, 5, nopad);
			break;
		case 0xC: /* 8888 */
			a = raw >> 24; r = (raw >> 16) & 0xFF; g = (raw >> 8) & 0xFF; b = raw & 0xFF;
			break;
		default:
			break;
	}
	if (bgr) std::swap(r, b);
	return argb(a, r, g, b);
}

/* Pack ARGB8888 into a destination format, with optional dithering */
static uint32_t PackARGB(uint32_t c, unsigned fmt, bool bgr, bool dither, unsigned dval) {
	uint32_t a = c >> 24, r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
	if (bgr) std::swap(r, b);
	/* [SPEC] 4.8: ColorN = min(Color8 + (dither << (4 - (8 - N)) ... ), 255) >> (8 - N) */
	auto q = [&](uint32_t v, unsigned bits) -> uint32_t {
		if (dither && bits < 8) {
			const int sh = (int)bits - 4; /* dither value is 0..15 for 4-bit output */
			uint32_t d = sh >= 0 ? (dval >> sh) : (dval << -sh);
			v = std::min<uint32_t>(v + d, 255u);
		}
		return v >> (8 - bits);
	};
	switch (fmt & 0xF) {
		case 1:
			if (bgr) return (q(b, 3) << 5) | (q(g, 3) << 2) | q(r, 2); /* r/b were swapped above */
			return (q(r, 3) << 5) | (q(g, 3) << 2) | q(b, 2);
		case 2: return (r * 77 + g * 151 + b * 28) >> 8;
		case 3: return a;
		case 4: return (q(r, 5) << 11) | (q(g, 6) << 5) | q(b, 5);
		case 5: return (q(a, 4) << 12) | (q(r, 4) << 8) | (q(g, 4) << 4) | q(b, 4);
		case 6: return ((a >= 128) ? 0x8000u : 0u) | (q(r, 5) << 10) | (q(g, 5) << 5) | q(b, 5);
		case 0xC: return (a << 24) | (r << 16) | (g << 8) | b;
		default: return 0;
	}
}

static uint32_t Rop(unsigned rop, uint32_t s, uint32_t d) {
	switch (rop & 0xF) {
		case 0x0: return 0;
		case 0x1: return ~(s | d);
		case 0x2: return ~s & d;
		case 0x3: return ~s;
		case 0x4: return s & ~d;
		case 0x5: return ~d;
		case 0x6: return s ^ d;
		case 0x7: return ~(s & d);
		case 0x8: return s & d;
		case 0x9: return ~(s ^ d);
		case 0xA: return d;
		case 0xB: return ~s | d;
		case 0xC: return s;
		case 0xD: return s | ~d;
		case 0xE: return s | d;
		default:  return 0xFFFFFFFFu;
	}
}

/* ALU modes that read the destination ([SPEC] 4.4) */
static inline bool RopReadsDst(unsigned rop) {
	switch (rop & 0xF) {
		case 0x0: case 0x3: case 0xC: case 0xF: return false;
		default: return true;
	}
}

/* ------------------------------------------------------------------------------------ */
/* construction, reset                                                                  */

Core::Core(Board *b, Chip c, uint32_t size) : board(b), chip(c) {
	init_op_use();
	uint32_t s = 1u << 20;
	while (s < size && s < (16u << 20)) s <<= 1;
	mem_size = s;
	mem_mask = s - 1u;
	mem = (uint8_t*)calloc(1, mem_size);
	memset(rom, 0xFF, sizeof(rom));
	have_rom = false;
	pci_mem_base = 0;
	instructions = 0;
	pixels = 0;
	trace_count = 0;
	ctl_log_left = 0;
	undef_logged = 0;
	Reset(true);
}

Core::~Core() {
	free(mem);
}

void Core::SetRom(const uint8_t *data, uint32_t size) {
	memset(rom, 0xFF, sizeof(rom));
	if (data && size) memcpy(rom, data, std::min<uint32_t>(size, ROM_SIZE));
	have_rom = data != NULL && size != 0;
}

void Core::Message(const char *fmt, ...) {
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (board) board->Message(buf);
}

void Core::Reset(bool hard) {
	/* [SPEC] 12: FIFOs cleared, RISC held at 0xfffffff0, V1000 compatibility mode */
	in_head = in_count = 0;
	out_head = out_count = 0;
	memset(reg, 0, sizeof(reg));
	pc = 0xFFFFFFF0u;
	npc = next_pc(pc);
	flag = FLAG_ICACHEOFF;
	seg = 0; seed_out = 0; scale_out = 0; seed_index = 0; clip = 0;
	riscintr = 0;
	halted = false;
	stalled_in = stalled_out = false;
	forced_valid = false;
	forced_ir = 0;
	step_pending = false;
	dma_cmdptr = dma_addr = dma_len = 0;
	dma_busy = dma_xfer = false;
	comm_sys = 0;
	memendian = 0;
	intr = 0;
	intr_en = 0;
	debug = DEBUG_HOLDRISC;
	lowwatermark = 0;
	stateindex = 0;
	in_reset = false;
	memset(latch, 0, sizeof(latch));
	if (hard) {
		timer = 0;
		xbusctl = 0;
		vga_extend = 4;
		scratch = 0;
		scratch1 = 0;
		mode = MODE_VGA;
		bank_select = 0;
		sclkpll = 0;
		memset(mem, 0, mem_size);
	}
	PEReset();
	UpdateIrq();
}

void Core::PEReset() {
	memset(&pe, 0, sizeof(pe));
	pe.pmask = 0xFFFFFFFFu;
	pe.yuv_contrast = 41;
	pe.src_mask = 0x07FF07FFu; /* no wrapping until the microcode sets it */
	pe.scissor_x = 0x000007FFu;
	pe.scissor_y = 0x000007FFu;
}

/* ------------------------------------------------------------------------------------ */
/* host interface                                                                       */

void Core::UpdateIrq() {
	if (board) board->IrqUpdate((intr & intr_en) != 0);
}

void Core::SetIntr(uint16_t bits) {
	const uint16_t old = intr;
	intr |= bits;
	if (intr != old) UpdateIrq();
}

void Core::PushInput(uint32_t v) {
	if (in_count >= IN_FIFO_SIZE) {
		/* a well behaved host checks FIFOInFree first; let the RISC drain the FIFO */
		Run(100000);
		if (in_count >= IN_FIFO_SIZE) {
			SetIntr(INTR_FIFOERROR);
			Message("Verite: input FIFO overflow, word %08x dropped (RISC pc %08x%s)", v, pc,
				(debug & DEBUG_HOLDRISC) ? ", held" : "");
			return;
		}
	}
	in_fifo[(in_head + in_count) % IN_FIFO_SIZE] = v;
	in_count++;
	stalled_in = false;
}

uint32_t Core::PopOutput() {
	if (out_count == 0) {
		/* the RISC may be about to produce the data */
		Run(100000);
		if (out_count == 0) {
			SetIntr(INTR_FIFOERROR);
			return 0xFFFFFFFFu;
		}
	}
	const uint32_t v = out_fifo[out_head];
	out_head = (out_head + 1) % OUT_FIFO_SIZE;
	out_count--;
	stalled_out = false;
	return v;
}

uint32_t Core::IORead(uint32_t off, unsigned len) {
	off &= 0xFF;
	if (off >= IO_EXT_FIRST) return board ? board->ExtRegRead(off, len) : 0xFFFFFFFFu;
	if (off < 0x10) {
		/* output FIFO: a read of the first byte of the word pops it */
		const unsigned base = off & 0x0C;
		if ((off & 3) == 0) {
			const uint32_t v = swap_mode(PopOutput(), base >> 2);
			for (unsigned i = 0; i < 4; i++) latch[base + i] = (uint8_t)(v >> (8 * i));
		}
		uint32_t r = 0;
		for (unsigned i = 0; i < len; i++) r |= (uint32_t)latch[(off + i) & 0x0F] << (8 * i);
		return r;
	}
	uint32_t r = 0;
	for (unsigned i = 0; i < len; i++) r |= (uint32_t)ByteRead(off + i) << (8 * i);
	return r;
}

void Core::CatchUp() {
	const uint32_t n = board ? board->CatchUpBudget() : 20000u;
	if (n) Run(n);
}

uint8_t Core::ByteRead(uint32_t off) {
	switch (off) {
		case IO_FIFOINFREE: {
			/* the host polls this while waiting for room: let the RISC catch up */
			if (in_count >= IN_FIFO_SIZE - 1 && Runnable()) CatchUp();
			const unsigned free_entries = IN_FIFO_SIZE - in_count;
			return (uint8_t)std::min(free_entries, 31u);
		}
		case IO_FIFOOUTVALID:
			if (out_count == 0 && Runnable()) CatchUp();
			return (uint8_t)std::min(out_count, 7u);
		case IO_COMM:
			return (uint8_t)((comm_sys & 0x0F) | (flag & FLAG_STATUS_MASK));
		case IO_MEMENDIAN:
			return (uint8_t)((memendian & 3) | (dma_busy ? 0x80 : 0));
		case IO_INTR:     return (uint8_t)intr;
		case IO_INTR + 1: return (uint8_t)(intr >> 8);
		case IO_INTREN:     return (uint8_t)intr_en;
		case IO_INTREN + 1: return (uint8_t)(intr_en >> 8);
		case IO_DEBUG:
			if (step_pending) {
				/* a step waiting on the FIFO completes once the FIFO is ready */
				Run(0);
			}
			return debug;
		case IO_LOWWATERMARK: return lowwatermark;
		case IO_STATUS: {
			const bool held = (debug & DEBUG_HOLDRISC) != 0;
			uint8_t s = 0x04 | 0x08; /* pixel and fill engines idle */
			if (halted) s |= 0x01;
			if (held) s |= 0x02;
			if (stalled_in) s |= 0x10;
			if (stalled_out) s |= 0x20;
			if (halted || held || stalled_in || stalled_out) s |= 0x80;
			return s;
		}
		case IO_XBUSCTL: return xbusctl;
		case IO_DMACMDPTR:     return (uint8_t)((dma_cmdptr & 0xFC) | (dma_busy ? 1 : 0));
		case IO_DMACMDPTR + 1: return (uint8_t)(dma_cmdptr >> 8);
		case IO_DMACMDPTR + 2: return (uint8_t)(dma_cmdptr >> 16);
		case IO_DMACMDPTR + 3: return (uint8_t)(dma_cmdptr >> 24);
		case IO_DMAADDRESS: case IO_DMAADDRESS + 1: case IO_DMAADDRESS + 2: case IO_DMAADDRESS + 3:
			return (uint8_t)(dma_addr >> (8 * (off - IO_DMAADDRESS)));
		case IO_DMALENGTH: case IO_DMALENGTH + 1: case IO_DMALENGTH + 2: case IO_DMALENGTH + 3:
			return (uint8_t)(dma_len >> (8 * (off - IO_DMALENGTH)));
		case IO_VGAEXTEND: return vga_extend;
		case IO_MEMBASE: return (uint8_t)(pci_mem_base >> 24);
		case IO_STATEINDEX: return stateindex;
		case IO_STATEDATA: {
			const uint32_t v = StateRead(stateindex);
			for (unsigned i = 0; i < 4; i++) latch[IO_STATEDATA + i] = (uint8_t)(v >> (8 * i));
			return (uint8_t)v;
		}
		case IO_STATEDATA + 1: case IO_STATEDATA + 2: case IO_STATEDATA + 3:
			return latch[off];
		case IO_SCLKPLL: case IO_SCLKPLL + 1: case IO_SCLKPLL + 2: case IO_SCLKPLL + 3:
			return (uint8_t)(sclkpll >> (8 * (off - IO_SCLKPLL)));
		case IO_SCRATCH:     return (uint8_t)scratch;
		case IO_SCRATCH + 1: return (uint8_t)(scratch >> 8);
		case IO_MODE: return mode;
		case IO_SCRATCH1: return scratch1;
		case IO_BANKSELECT: case IO_BANKSELECT + 1: case IO_BANKSELECT + 2: case IO_BANKSELECT + 3:
			return (uint8_t)(bank_select >> (8 * (off - IO_BANKSELECT)));
		default:
			return 0;
	}
}

void Core::IOWrite(uint32_t off, uint32_t val, unsigned len) {
	off &= 0xFF;
	if (off >= IO_EXT_FIRST) {
		if (board) board->ExtRegWrite(off, val, len);
		return;
	}
	if (off < 0x10) {
		/* input FIFO, pushed when the last byte of the word is written */
		for (unsigned i = 0; i < len; i++) {
			const unsigned o = (off + i) & 0x0F;
			latch[o] = (uint8_t)(val >> (8 * i));
			if ((o & 3) == 3) {
				const unsigned base = o & 0x0C;
				const uint32_t w = (uint32_t)latch[base] | ((uint32_t)latch[base + 1] << 8) |
					((uint32_t)latch[base + 2] << 16) | ((uint32_t)latch[base + 3] << 24);
				PushInput(swap_mode(w, base >> 2));
			}
		}
		return;
	}
	for (unsigned i = 0; i < len; i++) ByteWrite(off + i, (uint8_t)(val >> (8 * i)));
}

void Core::ByteWrite(uint32_t off, uint8_t v) {
	latch[off & 0x7F] = v;
	auto latched32 = [&](uint32_t base) -> uint32_t {
		return (uint32_t)latch[base] | ((uint32_t)latch[base + 1] << 8) |
			((uint32_t)latch[base + 2] << 16) | ((uint32_t)latch[base + 3] << 24);
	};
	switch (off) {
		case IO_COMM:
			comm_sys = v & 0x0F;
			break;
		case IO_MEMENDIAN:
			if ((v & 3) != memendian) {
				memendian = v & 3;
				if (board) board->MemEndianChanged();
			}
			break;
		case IO_INTR:
			intr &= (uint16_t)~v;
			if (v & INTR_RISC) flag &= ~FLAG_SIGNALSYS;
			UpdateIrq();
			break;
		case IO_INTR + 1:
			intr &= (uint16_t)~((uint16_t)v << 8);
			UpdateIrq();
			break;
		case IO_INTREN:
			intr_en = (uint16_t)((intr_en & 0xFF00) | v);
			UpdateIrq();
			break;
		case IO_INTREN + 1:
			intr_en = (uint16_t)((intr_en & 0x00FF) | ((uint16_t)v << 8));
			UpdateIrq();
			break;
		case IO_DEBUG:
			WriteDebug(v);
			break;
		case IO_LOWWATERMARK:
			lowwatermark = v & 0x1F;
			break;
		case IO_XBUSCTL:
			xbusctl = v;
			break;
		case IO_DMACMDPTR + 3:
			dma_cmdptr = latched32(IO_DMACMDPTR) & ~3u;
			DmaStart();
			break;
		case IO_VGAEXTEND:
			vga_extend = v;
			break;
		case IO_STATEINDEX:
			stateindex = v;
			break;
		case IO_STATEDATA + 3:
			/* [SPEC] 5.2: writes update the decode instruction register */
			forced_ir = latched32(IO_STATEDATA);
			forced_valid = true;
			break;
		case IO_SCLKPLL + 3:
			sclkpll = latched32(IO_SCLKPLL);
			break;
		case IO_SCRATCH:     scratch = (uint16_t)((scratch & 0xFF00) | v); break;
		case IO_SCRATCH + 1: scratch = (uint16_t)((scratch & 0x00FF) | ((uint16_t)v << 8)); break;
		case IO_MODE: {
			const uint8_t old = mode;
			mode = v & 0x0F;
			if ((mode & MODE_DMA_EN) && dma_busy) DmaPump();
			if (old != mode && board) board->ModeChanged();
			break;
		}
		case IO_SCRATCH1:
			scratch1 = v;
			break;
		case IO_BANKSELECT + 3:
			/* the A0000 window handler reads BankSelect on every access */
			bank_select = latched32(IO_BANKSELECT) & 0x80FFE000u;
			break;
		default:
			break;
	}
}

void Core::WriteDebug(uint8_t v) {
	const bool was_reset = (debug & DEBUG_SOFTRESET) != 0;
	if ((v & DEBUG_SOFTRESET) && !was_reset) {
		Reset(false);
	}
	in_reset = (v & DEBUG_SOFTRESET) != 0;
	debug = (uint8_t)((v & ~DEBUG_STEPRISC) | (debug & DEBUG_STEPRISC));
	if (v & DEBUG_STEPRISC) {
		debug |= DEBUG_STEPRISC;
		step_pending = true;
		Run(0);
	}
}

uint32_t Core::StateRead(unsigned index) {
	switch (index) {
		case 128: /* decode IR */
			return forced_valid ? forced_ir : Fetch(pc);
		case 129: /* decode PC */
			return pc;
		case 130: { /* decode S1: register file value selected by the IR's S1 field */
			const uint32_t ir = forced_valid ? forced_ir : Fetch(pc);
			return PeekReg(Resolve(ir & 0xFF));
		}
		default:
			return PEStateRead(index);
	}
}

void Core::DmaStart() {
	dma_xfer = false;
	if (dma_cmdptr == 0) {
		dma_busy = false;
		return;
	}
	dma_busy = true;
	if (mode & MODE_DMA_EN) DmaPump();
}

void Core::DmaPump() {
	if (!board) return;
	unsigned guard = 0;
	while (dma_busy && (mode & MODE_DMA_EN) && in_count < IN_FIFO_SIZE) {
		if (!dma_xfer) {
			if (++guard > 4096) break; /* malformed list: try again later */
			const uint32_t a = board->DmaRead32(dma_cmdptr);
			const uint32_t l = board->DmaRead32(dma_cmdptr + 4);
			if (l & 0x80000000u) {
				if (a == 0) { /* null link terminates the list */
					dma_busy = false;
					SetIntr(INTR_DMA);
					break;
				}
				dma_cmdptr = a & ~3u;
				continue;
			}
			if (a == 0) {
				dma_busy = false;
				SetIntr(INTR_DMA);
				break;
			}
			dma_addr = a & ~3u;
			dma_len = l & 0x00FFFFFFu;
			dma_xfer = true;
		}
		while ((dma_len & 0x00FFFFFCu) != 0 && in_count < IN_FIFO_SIZE) {
			const uint32_t w = board->DmaRead32(dma_addr);
			in_fifo[(in_head + in_count) % IN_FIFO_SIZE] = swap_mode(w, dma_len & 3);
			in_count++;
			stalled_in = false;
			dma_addr += 4;
			dma_len -= 4;
		}
		if ((dma_len & 0x00FFFFFCu) == 0) {
			dma_xfer = false;
			dma_cmdptr += 8;
		}
	}
}

uint32_t Core::MemRead(uint32_t off, unsigned len) {
	if (len == 4 && (off & 3) == 0)
		return swap_mode(Read32(off), memendian);
	const uint32_t x = host_lane_xor[memendian & 3];
	uint32_t r = 0;
	for (unsigned i = 0; i < len; i++) {
		const uint32_t a = off + i;
		r |= (uint32_t)Read8((a & ~3u) | ((a & 3u) ^ x)) << (8 * i);
	}
	return r;
}

void Core::MemWrite(uint32_t off, uint32_t val, unsigned len) {
	if (len == 4 && (off & 3) == 0) {
		Write32(off, swap_mode(val, memendian));
		return;
	}
	const uint32_t x = host_lane_xor[memendian & 3];
	for (unsigned i = 0; i < len; i++) {
		const uint32_t a = off + i;
		Write8((a & ~3u) | ((a & 3u) ^ x), (uint8_t)(val >> (8 * i)));
	}
}

void Core::VerticalRetrace() {
	riscintr |= 1u;
	SetIntr(INTR_VERT);
}

/* ------------------------------------------------------------------------------------ */
/* RISC                                                                                 */

bool Core::Runnable() const {
	if (in_reset) return false;
	if (step_pending) return true;
	if (debug & DEBUG_HOLDRISC) return false;
	if (halted) return false;
	return true;
}

uint32_t Core::Run(uint32_t cycles) {
	uint32_t used = 0;
	if (dma_busy) DmaPump();
	/* a pending single step runs regardless of hold */
	if (step_pending && !in_reset) {
		if (StepOnce()) {
			step_pending = false;
			debug &= ~DEBUG_STEPRISC;
			used++;
		}
		if (cycles == 0) return used;
	}
	while (used < cycles) {
		if (in_reset || halted || (debug & DEBUG_HOLDRISC)) break;
		if (dma_busy && in_count < IN_FIFO_SIZE) DmaPump();
		if (!StepOnce()) break;
		used++;
	}
	timer += used;
	return used;
}

/* The BIOS ROM: [SPEC] table 5 places the 128K bytes at 0xfffe0000. The V1000 decodes it in the
 * whole top 16M bytes, mirrored, except for the memory-mapped registers: Creative's CGL modules
 * copy a RISC loader from 0xfff10000 (ROM offset 0x10000). */
bool Core::IsRomAddress(uint32_t a) {
	return a >= 0xFF000000u && !(a >= 0xFFE00000u && a < 0xFFE01000u);
}

uint32_t Core::Fetch(uint32_t a) {
	if (a < 0x01000000u) return Read32(a);
	if (IsRomAddress(a)) {
		const uint32_t o = a & (ROM_SIZE - 4);
		return ((uint32_t)rom[o] << 24) | ((uint32_t)rom[o + 1] << 16) | ((uint32_t)rom[o + 2] << 8) | rom[o + 3];
	}
	return 0xFFFFFFFFu;
}

unsigned Core::Resolve(unsigned idx) const {
	if ((idx & 0xC0) == 0x40) {
		const unsigned sel = (idx >> 4) & 3;
		return 128u | (((seg >> (sel * 4)) & 7u) << 4) | (idx & 15u);
	}
	return idx;
}

uint32_t Core::FlagRead() const {
	uint32_t f = flag & (FLAG_SIGNALSYS | FLAG_ICACHEOFF | FLAG_STATUS_MASK | FLAG_FIFOOUTFORCE);
	f |= (uint32_t)(comm_sys & 0x0F) << 8;
	if (out_count < OUT_FIFO_SIZE) f |= FLAG_FIFOOUTRDY;
	if (in_count > 0) f |= FLAG_FIFOINRDY;
	if (pe.pick_valid) f |= FLAG_PICKVALID;
	if (pe.pick_hit) f |= FLAG_PICKHIT;
	if (intr & INTR_RISC) f |= FLAG_SIGNALSYS;
	return f;
}

/* Register read without side effects */
uint32_t Core::PeekReg(unsigned idx) const {
	if (idx >= 64) return reg[idx];
	if (idx < 32) return const_regs[idx];
	switch (idx) {
		case R_SWFIFO: case R_NPSWFIFO:
			return in_count ? swap_mode(in_fifo[in_head], 1) : 0;
		case R_FIFO: case R_NPFIFO:
			return in_count ? in_fifo[in_head] : 0;
		case R_SEED:  return seed_out;
		case R_SEG:   return (seg & 0x7777u) | 0x8888u | (seed_index << 19);
		case R_SCALE: return scale_out;
		case R_FLAG:  return FlagRead();
		case R_CLIP:  return clip & 0xFC;
		case R_TIMER: return timer;
		case R_WINCLIP: return clip & 0x3C;
		case R_RISCINTR: {
			uint32_t r = riscintr & 0x003FC03Fu;
			if ((r & 0x3F) & (r >> 16)) r |= 0x80000000u;
			return r;
		}
		case 56: return 0xFFFFF000u;
		case 57: return 0xFFFFF800u;
		default: return 0;
	}
}

uint32_t Core::ReadReg(unsigned idx) {
	if (idx >= 64) return reg[idx];
	if (idx == R_FIFO || idx == R_SWFIFO) {
		uint32_t v = 0;
		if (in_count) {
			v = in_fifo[in_head];
			in_head = (in_head + 1) % IN_FIFO_SIZE;
			in_count--;
			/* free entries rose above the low water mark */
			if (IN_FIFO_SIZE - in_count == (unsigned)lowwatermark + 1u) SetIntr(INTR_FIFOLOW);
		}
		return idx == R_SWFIFO ? swap_mode(v, 1) : v;
	}
	return PeekReg(idx);
}

void Core::WriteReg(unsigned idx, uint32_t v) {
	if (idx >= 64) {
		reg[idx] = v;
		return;
	}
	switch (idx) {
		case R_FIFO:
			if (out_count < OUT_FIFO_SIZE) {
				out_fifo[(out_head + out_count) % OUT_FIFO_SIZE] = v;
				out_count++;
			}
			break;
		case R_SEG:
			seg = v & 0x7777u;
			break;
		case R_FLAG:
			flag = (flag & ~(FLAG_ICACHEOFF | FLAG_STATUS_MASK | FLAG_FIFOOUTFORCE)) |
				(v & (FLAG_ICACHEOFF | FLAG_STATUS_MASK | FLAG_FIFOOUTFORCE));
			if (v & FLAG_SIGNALSYS) SetIntr(INTR_RISC);
			break;
		case R_TIMER:
			timer = v;
			break;
		case R_DRAWCTL:
			pe.drawctl = v & 0xFFFFu;
			break;
		case R_RISCINTR:
			riscintr &= ~(v & 0x3Fu);
			riscintr = (riscintr & ~0x003F0000u) | (v & 0x003F0000u);
			break;
		default:
			break; /* constants and read-only registers */
	}
}

/* Vector source pair. Pair 32/33 is the FIFO: one pop, swapped data in the even half. */
void Core::ReadPair(unsigned idx, uint32_t &even, uint32_t &odd) {
	idx = Resolve(idx & ~1u);
	if (idx == R_SWFIFO) {
		odd = ReadReg(R_FIFO);
		even = swap_mode(odd, 1);
		return;
	}
	if (idx == R_NPSWFIFO) {
		odd = PeekReg(R_NPFIFO);
		even = swap_mode(odd, 1);
		return;
	}
	even = ReadReg(idx);
	odd = ReadReg(idx + 1);
}

void Core::WritePair(unsigned idx, uint32_t even, uint32_t odd) {
	idx = Resolve(idx & ~1u);
	WriteReg(idx, even);
	WriteReg(idx + 1, odd);
}

/* Input FIFO entries an instruction pops (stall check). Returns 0x100 + n for peeks. */
unsigned Core::FifoPops(uint32_t ir) const {
	const unsigned op = ir >> 24;
	const unsigned use = op_use[op];
	const unsigned s1 = ir & 0xFF, s2 = (ir >> 8) & 0xFF;
	unsigned pops = 0;
	bool peek = false;
	auto scalar = [&](unsigned i) {
		if (i == R_FIFO || i == R_SWFIFO) pops++;
		else if (i == R_NPFIFO || i == R_NPSWFIFO) peek = true;
	};
	auto vector = [&](unsigned i) {
		i &= ~1u;
		if (i == R_SWFIFO) pops++;
		else if (i == R_NPSWFIFO) peek = true;
	};
	if (use & VEC_S2) vector(s2); else if (use & USE_S2) scalar(s2);
	if (use & VEC_S1) vector(s1); else if (use & USE_S1) scalar(s1);
	if (peek && pops == 0) return 1;
	return pops;
}

bool Core::StepOnce() {
	const uint32_t ir = forced_valid ? forced_ir : Fetch(pc);
	const unsigned op = ir >> 24;

	/* stall on an empty input FIFO or a full output FIFO */
	const unsigned need = FifoPops(ir);
	if (need > in_count) {
		stalled_in = true;
		return false;
	}
	stalled_in = false;
	if ((op_use[op] & USE_D) && !(op_use[op] & VEC_D) && ((ir >> 16) & 0xFF) == R_FIFO &&
		out_count >= OUT_FIFO_SIZE && !(flag & FLAG_FIFOOUTFORCE)) {
		stalled_out = true;
		return false;
	}
	stalled_out = false;

	forced_valid = false;
	const uint32_t cur = pc;
	pc = npc;
	npc = next_pc(pc);
	if (trace_count) {
		trace_count--;
		Trace(ir, cur);
	}
	Execute(ir, cur);
	instructions++;
	return true;
}

void Core::Trace(uint32_t ir, uint32_t cur) {
	Message("Verite: %08x: %08x  [%02x] s1=%08x s2=%08x", cur, ir, ir >> 24,
		PeekReg(Resolve(ir & 0xFF)), PeekReg(Resolve((ir >> 8) & 0xFF)));
}

void Core::Seed(uint32_t x) {
	/* [SPEC] 3.5: scale = position of the most significant 1 + 1; seed = left aligned
	 * reciprocal of the normalised input; seg[31:19] = 13-bit normalised index. */
	if (x == 0) {
		scale_out = 0;
		seed_out = 0xFFFFFFFFu;
		seed_index = 0;
		return;
	}
	const unsigned lz = clz32(x);
	scale_out = 32u - lz;
	const uint32_t m = x << lz;
	seed_index = m >> 19;
	const uint64_t s = ((uint64_t)1 << 62) / m;
	seed_out = s > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)s;
}

uint32_t Core::CtlRead(uint32_t a) {
	if (a >= 0xFFE00400u && a < 0xFFE00800u) {
		const uint32_t io = 0x80u + ((a - 0xFFE00400u) >> 3);
		const uint32_t v = board ? board->ExtRegRead(io, (io < 0xB0 || io >= 0xC0) ? 4 : 1) : 0;
		if (ctl_log_left) { ctl_log_left--; Message("Verite ctl rd %08x = %08x (pc %08x)", a, v, pc); }
		return v;
	}
	if (a >= 0xFFE00800u && a < 0xFFE01000u)
		return PEStateRead((a - 0xFFE00800u) >> 3);
	return 0;
}

void Core::CtlWrite(uint32_t a, uint32_t v, unsigned len) {
	if (ctl_log_left) { ctl_log_left--; Message("Verite ctl wr %08x = %08x/%u (pc %08x)", a, v, len, pc); }
	if (a >= 0xFFE00400u && a < 0xFFE00800u) {
		const uint32_t io = 0x80u + ((a - 0xFFE00400u) >> 3);
		/* RAMDAC registers are bytes, except that a word write to the palette data register
		 * stores a whole RGB triple ([SPEC] 7) */
		const bool dac = io >= 0xB0 && io < 0xC0;
		if (board) board->ExtRegWrite(io, (dac && !(io == 0xB1 && len == 4)) ? (v & 0xFF) : v, (dac && !(io == 0xB1 && len == 4)) ? 1 : len);
		return;
	}
	/* 0xffe00700 sub-vendor ID, 0xffe00704 66MHz: not needed */
}

uint32_t Core::LoadWord(uint32_t a) {
	if (a < 0x01000000u) return Read32(a);
	if (IsRomAddress(a)) return Fetch(a & ~3u);
	return CtlRead(a & ~3u);
}

uint16_t Core::LoadHalf(uint32_t a) {
	if (a < 0x01000000u) return Read16(a);
	const uint32_t w = LoadWord(a & ~3u);
	return (uint16_t)((a & 2) ? w : (w >> 16));
}

uint8_t Core::LoadByte(uint32_t a) {
	if (a < 0x01000000u) return Read8(a);
	const uint32_t w = LoadWord(a & ~3u);
	return (uint8_t)(w >> (24 - 8 * (a & 3)));
}

void Core::StoreWord(uint32_t a, uint32_t v) {
	if (a < 0x01000000u) Write32(a, v);
	else CtlWrite(a & ~3u, v, 4);
}

void Core::StoreHalf(uint32_t a, uint16_t v) {
	if (a < 0x01000000u) Write16(a, v);
	else CtlWrite(a & ~3u, v, 2);
}

void Core::StoreByte(uint32_t a, uint8_t v) {
	if (a < 0x01000000u) Write8(a, v);
	else CtlWrite(a & ~3u, v, 1);
}

static uint32_t AluOp(unsigned fn, uint32_t a, uint32_t b) {
	switch (fn & 0xF) {
		case 0x0: return a + b;
		case 0x1: return a - b;
		case 0x2: return a & ~b;
		case 0x3: return b - a;
		case 0x4: return a & b;
		case 0x5: return a | b;
		case 0x6: return ~(a | b);
		case 0x7: return a ^ b;
		case 0x8: return (uint32_t)((int64_t)(int32_t)a * (int32_t)b);
		case 0x9: return (uint32_t)(((int64_t)(int32_t)a * (int32_t)b) >> 8);
		case 0xA: return (uint32_t)(((int64_t)(int32_t)a * (int32_t)b) >> 16);
		case 0xB: return (uint32_t)(((int64_t)(int32_t)a * (int32_t)b) >> 24);
		case 0xC: return (uint32_t)(((int64_t)(int32_t)a * (int32_t)b) >> 32);
		case 0xD: return (uint32_t)(((int64_t)(int32_t)a * (int32_t)b) >> 31);
		case 0xF: {
			/* f2i: int(S2 * 2^S1), rounded toward -infinity, saturating */
			float f;
			memcpy(&f, &a, 4);
			if (f != f) return 0x7FFFFFFFu;
			const double d = floor(ldexp((double)f, (int)(b & 31)));
			if (d >= 2147483647.0) return 0x7FFFFFFFu;
			if (d <= -2147483648.0) return 0x80000000u;
			return (uint32_t)(int32_t)d;
		}
		default: return 0;
	}
}

void Core::Execute(uint32_t ir, uint32_t cur) {
	const unsigned op = ir >> 24;
	const unsigned d = (ir >> 16) & 0xFF;
	const unsigned s2 = (ir >> 8) & 0xFF;
	const unsigned s1 = ir & 0xFF;
	const uint32_t imm8 = s1;
	const uint32_t imm16 = ir & 0xFFFF;

	switch (op) {
	/* ---- ALU, immediate S1 ---- */
	case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07:
	case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0F: {
		const uint32_t a = ReadReg(Resolve(s2));
		WriteReg(Resolve(d), AluOp(op, a, imm8));
		break;
	}
	case 0x0E: { /* clip S2, #imm */
		const int32_t a = (int32_t)ReadReg(Resolve(s2));
		clip = ((clip << 1) & 0xFC) | ((a > (int32_t)imm8) ? 4u : 0u);
		break;
	}
	/* ---- ALU, register S1 ---- */
	case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
	case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1F: {
		const uint32_t a = ReadReg(Resolve(s2));
		const uint32_t b = ReadReg(Resolve(s1));
		WriteReg(Resolve(d), AluOp(op, a, b));
		break;
	}
	case 0x1E: { /* clip S2, S1 */
		const int32_t a = (int32_t)ReadReg(Resolve(s2));
		const int32_t b = (int32_t)ReadReg(Resolve(s1));
		clip = ((clip << 1) & 0xFC) | ((a > b) ? 4u : 0u);
		break;
	}
	/* ---- vector ---- */
	case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27:
	case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: {
		uint32_t ae, ao, be, bo;
		ReadPair(s2, ae, ao);
		ReadPair(s1, be, bo);
		WritePair(d, AluOp(op, ae, be), AluOp(op, ao, bo));
		break;
	}
	case 0x2E: case 0x3E: { /* clipv, clipvs: even pair first */
		uint32_t ae, ao, be, bo;
		ReadPair(s2, ae, ao);
		if (op == 0x2E) ReadPair(s1, be, bo);
		else be = bo = ReadReg(Resolve(s1));
		clip = ((clip << 1) & 0xFC) | (((int32_t)ae > (int32_t)be) ? 4u : 0u);
		clip = ((clip << 1) & 0xFC) | (((int32_t)ao > (int32_t)bo) ? 4u : 0u);
		break;
	}
	case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37:
	case 0x38: case 0x39: case 0x3A: case 0x3B: case 0x3C: case 0x3D: {
		uint32_t ae, ao;
		ReadPair(s2, ae, ao);
		const uint32_t b = ReadReg(Resolve(s1)); /* a FIFO S1 pops once for both */
		WritePair(d, AluOp(op, ae, b), AluOp(op, ao, b));
		break;
	}
	/* ---- scalar ALU, table 2.3b ---- */
	case 0x40: WriteReg(Resolve(d), ReadReg(Resolve(s2)) + (imm8 << 16)); break; /* addif */
	case 0x41: WriteReg(Resolve(d), ReadReg(Resolve(s2)) - (imm8 << 16)); break; /* subif */
	case 0x43: WriteReg(Resolve(d), (imm8 << 16) - ReadReg(Resolve(s2))); break; /* subfif */
	case 0x4B: WriteReg(Resolve(d), ReadReg(Resolve(s2)) + (imm8 << 8)); break;  /* addsl8 */
	case 0x42: case 0x53: { /* seedsr */
		const uint32_t a = ReadReg(Resolve(s2));
		const uint32_t b = (op == 0x42) ? imm8 : ReadReg(Resolve(s1));
		Seed(a);
		const int sh = (int)scale_out - (int)b;
		uint32_t r;
		if (sh >= 32) r = 0;
		else if (sh >= 0) r = seed_out >> sh;
		else if (sh > -32) r = seed_out << -sh;
		else r = 0;
		WriteReg(Resolve(d), r);
		break;
	}
	case 0x44: case 0x54: case 0x45: case 0x55: case 0x46: case 0x56: case 0x47: case 0x57:
	case 0x48: case 0x58: case 0x49: case 0x59: case 0x4A: case 0x5A: case 0x4C: case 0x5C:
	case 0x4D: case 0x5D: case 0x5E: {
		const uint32_t a = ReadReg(Resolve(s2));
		const uint32_t b = (op < 0x50) ? imm8 : ReadReg(Resolve(s1));
		uint32_t r = 0;
		switch (op & 0x0F) {
			case 0x4: { const unsigned n = b & 31; r = n ? ((a >> n) | (a << (32 - n))) : a; break; }
			case 0x5: r = a << (b & 31); break;
			case 0x6: r = (uint32_t)((int32_t)a >> (b & 31)); break;
			case 0x7: r = a >> (b & 31); break;
			case 0x8: r = ((int32_t)a < (int32_t)b) ? 1u : 0u; break;
			case 0x9: r = (a < b) ? 1u : 0u; break;
			case 0xA: r = (a == b) ? 1u : 0u; break;
			case 0xC: r = ((int32_t)a < (int32_t)b) ? a : b; break;
			case 0xD: r = ((int32_t)a > (int32_t)b) ? a : b; break;
			case 0xE: r = (a + b) & 0xFFFF0000u; break; /* at */
		}
		WriteReg(Resolve(d), r);
		break;
	}
	case 0x52: { /* abs */
		const int32_t a = (int32_t)ReadReg(Resolve(s1));
		WriteReg(Resolve(d), (uint32_t)(a < 0 ? -a : a));
		break;
	}
	case 0x4E: /* sprii pixreg, immed16 */
		SprWrite(s1, (ir >> 8) & 0xFFFF);
		break;
	case 0x4F: /* spri pixreg, S2 */
		SprWrite(s1, ReadReg(Resolve(s2)));
		break;
	case 0x5F: { /* spr (S1), S2 */
		const uint32_t idx = ReadReg(Resolve(s1));
		SprWrite(idx & 0xFF, ReadReg(Resolve(s2)));
		break;
	}
	/* ---- branches and jumps ---- */
	case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: {
		const int32_t v = (int32_t)ReadReg(Resolve(s1));
		bool take = false;
		switch (op) {
			case 0x60: take = v == 0; break;
			case 0x61: take = v != 0; break;
			case 0x62: take = v >= 0; break;
			case 0x63: take = v < 0; break;
			case 0x64: take = v > 0; break;
			case 0x65: take = v <= 0; break;
		}
		if (take) npc = rel_target(pc, sext16(ir >> 8));
		break;
	}
	case 0x68: case 0x6A: { /* rjmp, rjmpl: 24-bit relative */
		int32_t off = (int32_t)(ir << 8) >> 8;
		if (op == 0x6A) WriteReg(R_RA, cur + 8);
		npc = rel_target(pc, off);
		break;
	}
	case 0x6B: /* getpc */
		WriteReg(Resolve(d), cur + 8);
		break;
	case 0x6C: case 0x6E: /* jmp, jmpl */
		if (op == 0x6E) WriteReg(R_RA, cur + 8);
		npc = abs_target(ir & 0xFFFFFF);
		break;
	case 0x6D: /* halt */
		halted = true;
		SetIntr(INTR_HALT);
		break;
	case 0x6F: { /* jmprl D, (S1) */
		const uint32_t t = ReadReg(Resolve(s1));
		WriteReg(Resolve(d), cur + 8);
		npc = t & ~3u;
		break;
	}
	/* ---- loads and stores ---- */
	case 0x70: WriteReg(Resolve(d), LoadByte(ReadReg(Resolve(s1)) + s2)); break;
	case 0x71: WriteReg(Resolve(d), LoadHalf(ReadReg(Resolve(s1)) + s2 * 2)); break;
	case 0x72: WriteReg(Resolve(d), LoadWord(ReadReg(Resolve(s1)) + s2 * 4)); break;
	case 0x73: (void)ReadReg(Resolve(s1)); break; /* pre: prefetch */
	case 0x74: case 0x75: {
		const uint32_t a = ReadReg(Resolve(s1)) + s2 * 8;
		WritePair(d, LoadWord(a), LoadWord(a + 4));
		break;
	}
	case 0x76: WriteReg(Resolve(d), imm16); break;
	case 0x77: WriteReg(Resolve(d), imm16 << 16); break;
	case 0x78: StoreByte(ReadReg(Resolve(s1)) + d, (uint8_t)ReadReg(Resolve(s2))); break;
	case 0x79: StoreHalf(ReadReg(Resolve(s1)) + d * 2, (uint16_t)ReadReg(Resolve(s2))); break;
	case 0x7A: StoreWord(ReadReg(Resolve(s1)) + d * 4, ReadReg(Resolve(s2))); break;
	case 0x7C: {
		const uint32_t a = ReadReg(Resolve(s1)) + d * 8;
		uint32_t e, o;
		ReadPair(s2, e, o);
		StoreWord(a, e);
		StoreWord(a + 4, o);
		break;
	}
	case 0x7D: case 0x7E: case 0x7F: { /* sy, scr, scb: scatter four bytes */
		const uint32_t a = ReadReg(Resolve(s1)) + d * 8;
		const uint32_t v = ReadReg(Resolve(s2));
		static const unsigned start[3] = { 0, 1, 3 };
		static const unsigned step[3] = { 2, 4, 4 };
		const unsigned k = op - 0x7D;
		for (unsigned i = 0; i < 4; i++)
			StoreByte(a + start[k] + step[k] * i, (uint8_t)(v >> (24 - 8 * i)));
		break;
	}
	/* ---- unpack ---- */
	case 0xC0: case 0xC1: case 0xC2: case 0xC3: {
		const uint32_t a = ReadReg(Resolve(s1));
		uint32_t e = 0, o = 0;
		switch (op) {
			case 0xC0: e = a & 0xFFFF0000u; o = a << 16; break;
			case 0xC1: e = a << 16; o = a & 0xFFFF0000u; break;
			case 0xC2: e = a & 0xFF0000u; o = (a >> 8) & 0xFF0000u; break;
			case 0xC3: e = (a << 8) & 0xFF0000u; o = (a << 16) & 0xFF0000u; break;
		}
		WritePair(d, e, o);
		break;
	}
	case 0xC4: { /* sort S3, S2, S1 */
		const unsigned i3 = Resolve(d), i2 = Resolve(s2), i1 = Resolve(s1);
		struct E { int32_t v; unsigned idx; int order; } e[3] = {
			{(int32_t)ReadReg(i1), i1, 0}, {(int32_t)ReadReg(i2), i2, 1}, {(int32_t)ReadReg(i3), i3, 2} };
		std::sort(e, e + 3, [](const E &x, const E &y) { return x.v != y.v ? x.v < y.v : x.order < y.order; });
		seg = (seg & 0x7000u) | (((e[0].idx >> 4) & 7u)) | (((e[1].idx >> 4) & 7u) << 4) | (((e[2].idx >> 4) & 7u) << 8);
		const bool small = (int64_t)e[2].v - 1 < (int64_t)e[0].v;
		for (unsigned i = 0; i < 4; i++) clip = ((clip << 1) & 0xFC) | (small ? 4u : 0u);
		break;
	}
	/* ---- step (V1000 compatibility mode) ---- */
	case 0xD0: case 0xD1: case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6: case 0xD7: {
		const uint32_t a = ReadReg(Resolve(s2));
		const uint32_t b = ReadReg(Resolve(s1));
		static const uint8_t even_f[8] = { P_X, P_Q, P_R, P_I, P_F, P_G, P_Z, 0xFF };
		static const uint8_t odd_f[8]  = { P_Y, P_Q, P_V, P_U, P_F, P_B, P_A, 0xFF };
		const unsigned k = op - 0xD0;
		StepParam((s2 & 1) ? odd_f[k] : even_f[k], a, b);
		WriteReg(Resolve(d), a + b);
		break;
	}
	case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF: {
		uint32_t ae, ao, be, bo;
		ReadPair(s2, ae, ao);
		ReadPair(s1, be, bo);
		static const uint8_t even_f[8] = { P_X, P_Q, P_R, P_I, P_Z, P_G, P_Z, 0xFF };
		static const uint8_t odd_f[8]  = { P_Y, P_U, P_V, P_V, P_V, P_B, P_A, P_UNUSED };
		const unsigned k = op - 0xD8;
		StepParam(even_f[k], ae, be);
		StepParam(odd_f[k], ao, bo);
		WritePair(d, ae + be, ao + bo);
		break;
	}
	/* ---- drawing ---- */
	case 0xE0: case 0xE1: case 0xE4: case 0xE5:
		DrawParallel((uint8_t)op, d, s2, s1);
		break;
	case 0xF0: case 0xF1: case 0xF2: case 0xF3: case 0xF8: case 0xF9: case 0xFA: case 0xFB:
	case 0xFC: case 0xFD:
		DrawParam((uint8_t)op, d, s2, s1);
		break;
	default:
		/* triangle and fill engine instructions (V2x00) and undefined opcodes */
		if (undef_logged < 32) {
			undef_logged++;
			Message("Verite: unimplemented RISC opcode %02x (instruction %08x at %08x), ignored", op, ir, cur);
		}
		break;
	}
}

/* ------------------------------------------------------------------------------------ */
/* pixel engine                                                                         */

void Core::SprWrite(unsigned idx, uint32_t v) {
	auto field = [](uint32_t &r, uint32_t mask, uint32_t val) { r = (r & ~mask) | (val & mask); };
	switch (idx) {
		case 0:  pe.src_base = v & 0xFFFFFFu; break;
		case 1:  pe.src_mode = v & 0x3FFFFu; break;
		case 2:  pe.src_mask = v & 0x07FF07FFu; break;
		case 3:  pe.stride = v & 0x777777u; break;
		case 4:  pe.dst_base = v & 0xFFFFFCu; break;
		case 5:  pe.dst_mode = v & 0x3FF7FFFu; break;
		case 6:  pe.dst_fmt = v & 0xFu; break;
		case 7:  pe.pmask = v; break;
		case 8:  pe.pattern = v; pe.pat_offset = 0; break;
		case 10: /* IPat: pattern from an arbitrary byte address */
			pe.pattern = ((uint32_t)Read8(v) << 24) | ((uint32_t)Read8(v + 1) << 16) |
				((uint32_t)Read8(v + 2) << 8) | Read8(v + 3);
			pe.pat_offset = 0;
			break;
		case 12: pe.pat_mode = v & 0x3001Fu; break;
		case 13: pe.pat_offset = v & 31u; break;
		case 14: pe.scissor_x = v & 0x0FFF0FFFu; break;
		case 15: pe.scissor_y = v & 0x0FFF0FFFu; break;
		case 16: pe.z_base = v & 0xFFFFFEu; break;
		case 17: pe.dither_offset = v & 0x33u; break;
		case 19: pe.fg_color = v; break;
		case 20: pe.bg_color = v; break;
		case 21: pe.fog_color = v & 0xFFFFFFu; break;
		case 22: pe.dst_color = v; break;
		case 23: pe.chroma_color = v; break;
		case 24: pe.yuv_contrast = v & 0xFFu; break;
		case 26: pe.chroma_mask = v; break;
		case 27: pe.fg_color = (pe.fg_color & 0xFF000000u) | (v & 0x00FFFFFFu); break;
		case 28: pe.pick_valid = (v >> 16) & 1; pe.pick_hit = (v >> 17) & 1; break;
		case 29: pe.alpha_thres = v & 0xFFu; break;
		case 30: pe.spec_color = v & 0xFFFFFFu; break;
		case 31: break; /* Sync: the pixel engine is synchronous */
		case 32: case 33: case 34: case 35: case 36: case 37: case 38: case 39:
			pe.tex4pal[idx - 32] = v;
			break;
		/* bit field writes: the field keeps its register bit position */
		case 48: field(pe.src_mode, 0x0000Fu, v); break;
		case 49: field(pe.src_mode, 0x00070u, v); break;
		case 50: field(pe.src_mode, 0x00080u, v); break;
		case 51: field(pe.src_mode, 0x00200u, v); break;
		case 52: field(pe.src_mode, 0x00400u, v); break;
		case 53: field(pe.src_mode, 0x00800u, v); break;
		case 54: field(pe.src_mode, 0x01000u, v); break;
		case 55: field(pe.src_mode, 0x02000u, v); break;
		case 61: field(pe.src_mode, 0x04000u, v); break;
		case 62: field(pe.src_mode, 0x08000u, v); break;
		case 63: field(pe.src_mode, 0x10000u, v); break;
		case 83: field(pe.src_mode, 0x20000u, v); break;
		case 56: field(pe.src_mask, 0x000007FFu, v); break;
		case 57: field(pe.src_mask, 0x07FF0000u, v); break;
		case 58: field(pe.stride, 0x000077u, v); break;
		case 59: field(pe.stride, 0x007700u, v); break;
		case 60: field(pe.stride, 0x770000u, v); break;
		case 64: field(pe.dst_mode, 0x000000Fu, v); break;
		case 65: field(pe.dst_mode, 0x00000F0u, v); break;
		case 66: field(pe.dst_mode, 0x0000F00u, v); break;
		case 67: field(pe.dst_mode, 0x0007000u, v); break;
		case 68: field(pe.dst_mode, 0x0010000u, v); break;
		case 69: field(pe.dst_mode, 0x0040000u, v); break;
		case 70: field(pe.dst_mode, 0x0080000u, v); break;
		case 71: field(pe.dst_mode, 0x0100000u, v); break;
		case 72: field(pe.dst_mode, 0x0200000u, v); break;
		case 73: field(pe.dst_mode, 0x0400000u, v); break;
		case 74: field(pe.dst_mode, 0x0800000u, v); break;
		case 75: field(pe.dst_mode, 0x1000000u, v); break;
		case 76: field(pe.dst_mode, 0x2000000u, v); break;
		case 80: field(pe.pat_mode, 0x0001Fu, v); break;
		case 81: field(pe.pat_mode, 0x10000u, v); break;
		case 82: field(pe.pat_mode, 0x20000u, v); break;
		default:
			if (undef_logged < 32) {
				undef_logged++;
				Message("Verite: write to unknown pixel engine register %u = %08x", idx, v);
			}
			break;
	}
}

uint32_t Core::PEStateRead(unsigned index) const {
	switch (index) {
		case 0:  return pe.src_base;
		case 1:  return pe.src_mode;
		case 2:  return pe.src_mask;
		case 3:  return pe.stride;
		case 4:  return pe.dst_base;
		case 5:  return pe.dst_mode;
		case 6:  return pe.dst_fmt;
		case 7:  return pe.pmask;
		case 8:  return pe.pattern;
		case 12: return pe.pat_mode;
		case 13: return pe.pat_offset;
		case 14: return pe.scissor_x;
		case 15: return pe.scissor_y;
		case 16: return pe.z_base;
		case 17: return pe.dither_offset;
		case 19: return pe.fg_color;
		case 20: return pe.bg_color;
		case 21: return pe.fog_color;
		case 22: return pe.dst_color;
		case 23: return pe.chroma_color;
		case 24: return pe.yuv_contrast;
		case 26: return pe.chroma_mask;
		case 28: return (pe.pick_valid ? 0x10000u : 0u) | (pe.pick_hit ? 0x20000u : 0u);
		case 29: return pe.alpha_thres;
		case 30: return pe.spec_color;
		case 32: case 33: case 34: case 35: case 36: case 37: case 38: case 39:
			return pe.tex4pal[index - 32];
		case 112: return (uint32_t)pe.x_shadow & 0xFFFF0000u;
		case 113: return (uint32_t)pe.y & 0xFFFF0000u;
		case 114: return ((uint32_t)pe.z & 0x07FFF800u) | (pe.z_ovf ? 0x08000000u : 0u);
		case 117: return (uint32_t)param8(pe.f) << 16;
		case 119: return ((uint32_t)pe.u & 0x07FFFF00u);
		case 121: return ((uint32_t)pe.v & 0x07FFFF00u);
		case 123: return pe.u_dir ? 0x80000000u : 0u;
		case 125: return pe.v_dir ? 0x80000000u : 0u;
		case 131: return pe.drawctl & 0xFFFFu;
		case 132: return ((uint32_t)pe.x_shadow & 0xFFFF0000u) | (pe.x_dir ? 1u : 0u);
		case 133: return pe.cnt << 16;
		default:  return 0;
	}
}

void Core::SetParam(unsigned field, int32_t value) {
	switch (field) {
		case P_X: pe.x_param = value; break;
		case P_Y: pe.y = value; break;
		case P_Z:
			pe.z = value;
			pe.z_ovf = ((uint32_t)value & 0xF8000000u) != 0 && (pe.src_mode & 0x1000u);
			break;
		case P_U: pe.u = value; break;
		case P_V: pe.v = value; break;
		case P_Q:
			pe.q = (uint32_t)value;
			break;
		case P_R: pe.r = value; pe.fg_color = (pe.fg_color & 0xFF00FFFFu) | ((uint32_t)param8(value) << 16); break;
		case P_G: pe.g = value; pe.fg_color = (pe.fg_color & 0xFFFF00FFu) | ((uint32_t)param8(value) << 8); break;
		case P_B: pe.b = value; pe.fg_color = (pe.fg_color & 0xFFFFFF00u) | (uint32_t)param8(value); break;
		case P_A: pe.a = value; pe.fg_color = (pe.fg_color & 0x00FFFFFFu) | ((uint32_t)param8(value) << 24); break;
		case P_I: {
			pe.r = pe.g = pe.b = value;
			const uint32_t i = param8(value);
			pe.fg_color = (pe.fg_color & 0xFF000000u) | (i << 16) | (i << 8) | i;
			break;
		}
		case P_F: pe.f = value; break;
		default: break;
	}
}

int32_t Core::GetParam(unsigned field) const {
	switch (field) {
		case P_X: return pe.x_param;
		case P_Y: return pe.y;
		case P_Z: return pe.z;
		case P_U: return pe.u;
		case P_V: return pe.v;
		case P_Q: return (int32_t)pe.q;
		case P_R: return pe.r;
		case P_G: return pe.g;
		case P_B: return pe.b;
		case P_A: return pe.a;
		case P_I: return pe.r;
		case P_F: return pe.f;
		default:  return 0;
	}
}

void Core::StepParam(unsigned field, uint32_t value, uint32_t s1) {
	const bool neg = (int32_t)s1 < 0;
	switch (field) {
		case P_X:
			pe.x_shadow = (int32_t)value;
			pe.x_dir = neg;
			break;
		case P_U:
			/* SwapUV exchanges the meaning of U and V as they are received */
			if (pe.src_mode & 0x400u) { pe.v = (int32_t)value; pe.v_dir = neg; }
			else { pe.u = (int32_t)value; pe.u_dir = neg; }
			break;
		case P_V:
			if (pe.src_mode & 0x400u) { pe.u = (int32_t)value; pe.u_dir = neg; }
			else { pe.v = (int32_t)value; pe.v_dir = neg; }
			break;
		case P_Q:
			/* step q is also a seed operation, used by the RISC for reciprocals */
			Seed(value);
			pe.q = value;
			break;
		case 0xFF: /* Cnt: b31:b16 of S2 */
			pe.cnt = value >> 16;
			break;
		case P_UNUSED:
			break;
		default:
			SetParam(field, (int32_t)value);
			break;
	}
}

uint32_t Core::RowOffset(uint32_t y, unsigned field) const {
	static const uint8_t s0a[8] = { 0, 8, 9, 10, 2, 8, 9, 10 };
	static const uint8_t s0b[8] = { 0, 0, 0, 0, 0, 9, 6, 7 };
	static const uint8_t s1t[8] = { 0, 4, 5, 6, 7, 10, 11, 12 };
	const unsigned f0 = field & 7, f1 = (field >> 4) & 7;
	uint32_t o = 0;
	if (f0) o += y << s0a[f0];
	if (s0b[f0]) o += y << s0b[f0];
	if (f1) o += y << s1t[f1];
	return o;
}

/* Raw pixel value of a 1/2/4 byte pixel at a RISC byte address */
static inline uint32_t ReadRaw(const Core &c, uint32_t a, unsigned bpp) {
	switch (bpp) {
		case 1: return c.Read8(a);
		case 2: return c.Read16(a);
		default: return c.Read32(a);
	}
}

static inline void WriteRaw(Core &c, uint32_t a, unsigned bpp, uint32_t v) {
	switch (bpp) {
		case 1: c.Write8(a, (uint8_t)v); break;
		case 2: c.Write16(a, (uint16_t)v); break;
		default: c.Write32(a, v); break;
	}
}

/* Lane of a replicated 32-bit value for pixel x; the left pixel is in the MSBs */
static inline uint32_t Lane(uint32_t word, int x, unsigned bpp) {
	switch (bpp) {
		case 1: return (word >> (24 - 8 * (x & 3))) & 0xFFu;
		case 2: return (x & 1) ? (word & 0xFFFFu) : (word >> 16);
		default: return word;
	}
}

static inline uint32_t Replicate(uint32_t v, unsigned bpp) {
	switch (bpp) {
		case 1: return (v & 0xFF) * 0x01010101u;
		case 2: return (v & 0xFFFF) * 0x00010001u;
		default: return v;
	}
}

void Core::DrawParallel(uint8_t op, unsigned d, unsigned s2, unsigned s1) {
	/* drawp/rdrawp: S2 offset 1 = U. drawpxy/rdrawpxy: S2 offset 0 = H, offset 1 = Y */
	const bool xy = (op == 0xE4 || op == 0xE5);
	const bool repeat = (op & 1) != 0;
	uint32_t se, so, de, dodd;
	ReadPair(s2, se, so);
	ReadPair(s1, de, dodd);

	const unsigned bpp = std::max(1u, FormatBytes(pe.dst_fmt));
	uint32_t total = pe.cnt & 0xFFFFu;
	const uint32_t cnt0 = total;
	const int32_t x0 = pe.x_shadow;
	int y = xy ? ((int32_t)so >> 16) : (pe.y >> 16);
	if (xy) pe.y = (int32_t)so;
	int u = xy ? (pe.u >> 16) : ((int32_t)so >> 16);
	const int v = pe.v >> 16;
	const int step = pe.x_dir ? -1 : 1;
	int x = pe.x_shadow >> 16;

	if (total != 0 && !repeat) {
		/* one 32-bit block per single parallel draw */
		const unsigned per = 4u / bpp;
		uint32_t n = pe.x_dir ? (uint32_t)(x % (int)per) + 1u : per - (uint32_t)(x % (int)per);
		if (x < 0) n = 1;
		total = std::min(total, n);
	}

	const bool use_src = ((pe.src_mode >> 4) & 7) != 0;
	const bool pat_en = (pe.pat_mode & 0x10000u) != 0;
	const bool pat_opaque = (pe.pat_mode & 0x20000u) != 0;
	const bool chroma = (pe.src_mode & 0x800u) != 0 && use_src;
	const unsigned rop = pe.dst_mode & 0xF;
	const bool dst_rd_disable = (pe.dst_mode & 0x800000u) != 0;
	const unsigned sx0 = (pe.scissor_x >> 16) & 0xFFF, sx1 = pe.scissor_x & 0xFFF;
	const unsigned sy0 = (pe.scissor_y >> 16) & 0xFFF, sy1 = pe.scissor_y & 0xFFF;
	const unsigned sbpp = std::max(1u, FormatBytes((pe.src_mode & 0xF) ? (pe.src_mode & 0xF) : pe.dst_fmt));
	const bool y_in = y >= 0 && (unsigned)y >= sy0 && (unsigned)y < sy1 && y < 2048;
	const uint32_t drow = pe.dst_base + (y >= 0 ? RowOffset((uint32_t)y, pe.stride >> 8) : 0);
	/* [SPEC] 4.2: UMask/VMask apply to U and V as the pixel engine receives them, also
	 * for parallel draws. The 2D microcode's colour brush fill relies on it: it draws
	 * from an 8x8 brush in memory with SrcMask = 7,7 (Windows 98 tooltips). */
	const int umask = (int)(pe.src_mask & 0x7FFu), vmask = (int)((pe.src_mask >> 16) & 0x7FFu);
	const uint32_t srow = pe.src_base + RowOffset((uint32_t)(v & vmask), pe.stride);
	const uint32_t cmask = pe.chroma_mask, ccol = pe.chroma_color & cmask;

	for (uint32_t i = 0; i < total; i++, x += step, u += step) {
		bool fg = true;
		if (pat_en) {
			fg = ((pe.pattern >> (31 - pe.pat_offset)) & 1u) != 0;
			pe.pat_offset = (pe.pat_offset != (pe.pat_mode & 0x1F)) ? pe.pat_offset + 1 : 0;
			if (!fg && !pat_opaque) continue;
		}
		if (!y_in || x < 0 || (unsigned)x < sx0 || (unsigned)x >= sx1 || x >= 2048) continue;
		uint32_t s;
		if (!fg) s = Lane(pe.bg_color, x, bpp);
		else if (use_src) {
			s = ReadRaw(*this, srow + (uint32_t)(u & umask) * sbpp, sbpp);
			if (chroma && (Replicate(s, sbpp) & cmask) == ccol) continue;
		}
		else s = Lane(pe.fg_color, x, bpp);
		const uint32_t addr = drow + (uint32_t)x * bpp;
		uint32_t dv = dst_rd_disable ? Lane(pe.dst_color, x, bpp) : ReadRaw(*this, addr, bpp);
		uint32_t r = Rop(rop, s, dv);
		const uint32_t pm = Lane(pe.pmask, x, bpp);
		r = (r & pm) | (dv & ~pm);
		WriteRaw(*this, addr, bpp, r);
		pixels++;
	}

	if (xy) {
		/* the span engine resets Cnt and XShadow; the register pair is updated once */
		pe.cnt = cnt0;
		pe.x_shadow = x0;
		WritePair(d, se + de, so + dodd);
		return;
	}
	pe.x_shadow = x0 + (int32_t)((pe.x_dir ? -(int32_t)total : (int32_t)total) * 65536);
	if (repeat) {
		pe.cnt = 0;
		return;
	}
	const bool update = pe.cnt != 0;
	pe.cnt = (pe.cnt - total) & 0xFFFFu;
	if (update) WritePair(d, se + de, so + dodd);
}

void Core::DrawParam(uint8_t op, unsigned d, unsigned s2, unsigned s1) {
	const bool repeat = (op & 1) != 0;
	unsigned npairs;
	switch (op & 0xFE) {
		case 0xF0: npairs = 1; break;
		case 0xF2: npairs = 2; break;
		case 0xF8: npairs = 3; break;
		case 0xFA: npairs = 4; break;
		default:   npairs = 5; break; /* draw5: pairs 1..4 plus G, B */
	}
	const unsigned p1 = pe.drawctl & 0xF, p2 = (pe.drawctl >> 4) & 0x1F;
	const unsigned p3 = (pe.drawctl >> 9) & 0xF, p4 = (pe.drawctl >> 13) & 0x7;
	if (p1 == 11) return; /* V2000 mode encoding, illegal for draw */
	if (npairs >= 2 && p2 < 8 && undef_logged < 32) {
		undef_logged++;
		Message("Verite: DrawCtl pair 2 value %u (undocumented, assumed same as pair 3)", p2);
	}

	uint8_t fields[10];
	unsigned nf = 0;
	const PairDef &d1 = pair1_tab[p1];
	fields[nf++] = d1.even; fields[nf++] = d1.odd;
	if (npairs >= 2) { fields[nf++] = pair2_tab[p2].even; fields[nf++] = pair2_tab[p2].odd; }
	if (npairs >= 3) { fields[nf++] = pair34_tab[p3].even; fields[nf++] = pair34_tab[p3].odd; }
	if (npairs >= 4) { fields[nf++] = pair34_tab[p4 & 7].even; fields[nf++] = pair34_tab[p4 & 7].odd; }
	if (npairs >= 5) { fields[nf++] = P_G; fields[nf++] = P_B; }

	uint32_t val[10], dlt[10];
	for (unsigned i = 0; i < nf; i += 2) {
		ReadPair(s2 + i, val[i], val[i + 1]);
		ReadPair(s1 + i, dlt[i], dlt[i + 1]);
	}

	const int16_t cnt = (int16_t)(uint16_t)pe.cnt;
	if (cnt <= 0) return;
	const unsigned n = repeat ? (unsigned)cnt : 1u;
	const uint8_t shadow = d1.shadow; /* P_X / P_Y: that coordinate is iterated from XShadow */
	const int32_t sstep = pe.x_dir ? -65536 : 65536;

	uint32_t cur[10];
	for (unsigned i = 0; i < nf; i++) cur[i] = val[i];

	for (unsigned k = 0; k < n; k++) {
		bool has_q = false;
		uint32_t q_ahead = 0;
		for (unsigned i = 0; i < nf; i++) {
			if (fields[i] == P_Q) { has_q = true; q_ahead = cur[i]; }
			else if (fields[i] != P_UNUSED && fields[i] != P_NA) SetParam(fields[i], (int32_t)cur[i]);
		}
		/* X comes from XShadow (pair 1 odd), or from the X parameter. Pair 1 value 15
		 * iterates Y from XShadow instead (vertical spans). */
		int px = pe.x_shadow >> 16, py = pe.y >> 16;
		if (shadow == P_Y) { py = pe.x_shadow >> 16; px = pe.x_param >> 16; }
		else if (shadow != P_X) px = pe.x_param >> 16;
		PixelFull(px, py);
		/* Q is one pixel ahead: this pixel used the Q sent before (step q or the previous
		 * pixel); the Q given with this pixel is used by the next one. */
		if (has_q) pe.q = q_ahead;
		for (unsigned i = 0; i < nf; i++) cur[i] += dlt[i];
		if (shadow == P_X || shadow == P_Y) pe.x_shadow += sstep;
	}

	if (repeat) {
		pe.cnt = 0;
	}
	else {
		pe.cnt = (pe.cnt - 1u) & 0xFFFFu;
		for (unsigned i = 0; i < nf; i += 2) WritePair(d + i, val[i] + dlt[i], val[i + 1] + dlt[i + 1]);
	}
}

uint32_t Core::FetchTexel(int u, int v) {
	const unsigned fmt = (pe.src_mode & 0xF) ? (pe.src_mode & 0xF) : pe.dst_fmt;
	const bool bgr = (pe.src_mode & 0x2000u) != 0;
	const bool nopad = (pe.src_mode & 0x200u) != 0;
	const uint32_t row = pe.src_base + RowOffset((uint32_t)v, pe.stride);
	uint32_t raw;
	switch (fmt) {
		case 8: case 9: case 0xA: { /* 4-bit indexed through Tex4Pal, first texel in the high nibble */
			const uint8_t byte = Read8(row + ((uint32_t)u >> 1));
			const unsigned idx = (u & 1) ? (byte & 0xF) : (byte >> 4);
			const uint32_t pair = pe.tex4pal[idx >> 1];
			raw = (idx & 1) ? (pair & 0xFFFF) : (pair >> 16);
			static const uint8_t pal_fmt[3] = { 4, 5, 6 };
			return ExpandToARGB(raw, pal_fmt[fmt - 8], bgr, nopad);
		}
		case 0xD: { /* 32-bit 4:2:2 Y0 Cr Y1 Cb: one word per two texels */
			const uint32_t w = Read32(row + (((uint32_t)u >> 1) << 2));
			const uint32_t y = (u & 1) ? ((w >> 8) & 0xFF) : (w >> 24);
			const uint32_t cr = (w >> 16) & 0xFF, cb = w & 0xFF;
			return argb(255, cr, y, cb); /* fields map to Cr = r, Y = g, Cb = b */
		}
		default: {
			const unsigned bpp = FormatBytes(fmt);
			if (bpp == 0) return 0;
			raw = ReadRaw(*this, row + (uint32_t)u * bpp, bpp);
			if ((pe.src_mode & 0x800u) && (Replicate(raw, bpp) & pe.chroma_mask) == (pe.chroma_color & pe.chroma_mask)) {
				/* chroma key match: transparent black */
				return (pe.src_mode & 0x4000u) ? argb(0, 128, 16, 128) : 0;
			}
			return ExpandToARGB(raw, fmt, bgr, nopad);
		}
	}
}

uint32_t Core::SampleTexture(int32_t u, int32_t v) {
	const int umask = (int)(pe.src_mask & 0x7FF), vmask = (int)((pe.src_mask >> 16) & 0x7FF);
	const bool uclamp = (pe.src_mode & 0x8000u) != 0, vclamp = (pe.src_mode & 0x10000u) != 0;
	auto wrap = [](int c, int mask, bool clampit) -> int {
		if (clampit) return c < 0 ? 0 : (c > mask ? mask : c);
		return c & mask;
	};
	if (!(pe.src_mode & 0x80u)) {
		return FetchTexel(wrap(u >> 16, umask, uclamp), wrap(v >> 16, vmask, vclamp));
	}
	/* bilinear with 8-bit fractions */
	const int u0 = u >> 16, v0 = v >> 16;
	const unsigned fu = ((uint32_t)u >> 8) & 0xFF, fv = ((uint32_t)v >> 8) & 0xFF;
	const uint32_t t00 = FetchTexel(wrap(u0, umask, uclamp), wrap(v0, vmask, vclamp));
	const uint32_t t10 = FetchTexel(wrap(u0 + 1, umask, uclamp), wrap(v0, vmask, vclamp));
	const uint32_t t01 = FetchTexel(wrap(u0, umask, uclamp), wrap(v0 + 1, vmask, vclamp));
	const uint32_t t11 = FetchTexel(wrap(u0 + 1, umask, uclamp), wrap(v0 + 1, vmask, vclamp));
	uint32_t out = 0;
	for (unsigned sh = 0; sh < 32; sh += 8) {
		const uint32_t a = (t00 >> sh) & 0xFF, b = (t10 >> sh) & 0xFF;
		const uint32_t c = (t01 >> sh) & 0xFF, e = (t11 >> sh) & 0xFF;
		const uint32_t top = a * (256 - fu) + b * fu;
		const uint32_t bot = c * (256 - fu) + e * fu;
		const uint32_t val = (top * (256 - fv) + bot * fv) >> 16;
		out |= (val & 0xFF) << sh;
	}
	return out;
}

uint32_t Core::ReadDstARGB(uint32_t addr, unsigned fmt, unsigned bpp) const {
	const uint32_t raw = ReadRaw(*this, addr, bpp);
	return ExpandToARGB(raw, fmt, (pe.dst_mode & 0x1000000u) != 0, (pe.dst_mode & 0x400000u) != 0);
}

void Core::PixelFull(int x, int y) {
	/* pattern position advances for every pixel, even scissored ones */
	bool fg = true;
	if (pe.pat_mode & 0x10000u) {
		fg = ((pe.pattern >> (31 - pe.pat_offset)) & 1u) != 0;
		pe.pat_offset = (pe.pat_offset != (pe.pat_mode & 0x1F)) ? pe.pat_offset + 1 : 0;
		if (!fg && !(pe.pat_mode & 0x20000u)) return;
	}
	const uint32_t q = pe.q;

	const unsigned sx0 = (pe.scissor_x >> 16) & 0xFFF, sx1 = pe.scissor_x & 0xFFF;
	const unsigned sy0 = (pe.scissor_y >> 16) & 0xFFF, sy1 = pe.scissor_y & 0xFFF;
	if (x < 0 || y < 0 || x >= 2048 || y >= 2048) return;
	if ((unsigned)x < sx0 || (unsigned)x >= sx1 || (unsigned)y < sy0 || (unsigned)y >= sy1) return;
	if (pe.z_ovf) return;

	const unsigned fmt = pe.dst_fmt & 0xF;
	const unsigned bpp = FormatBytes(fmt);
	if (bpp == 0) return;
	const uint32_t dmode = pe.dst_mode;

	/* texture */
	const unsigned func = (pe.src_mode >> 4) & 7;
	uint32_t tex = 0xFFFFFFFFu;
	if (func != 0) {
		int32_t u = pe.u, v = pe.v;
		const bool persp = (pe.drawctl & 1) != 0 && ((pe.drawctl & 0xF) != 11);
		if (persp && q != 0) {
			u = (int32_t)(((int64_t)u << 24) / (int64_t)q);
			v = (int32_t)(((int64_t)v << 24) / (int64_t)q);
		}
		tex = SampleTexture(u, v);
	}

	/* colour */
	uint32_t col = fg ? pe.fg_color : pe.bg_color;
	uint32_t src;
	{
		const uint32_t ca = col >> 24, cr = (col >> 16) & 0xFF, cg = (col >> 8) & 0xFF, cb = col & 0xFF;
		const uint32_t ta = tex >> 24, tr = (tex >> 16) & 0xFF, tg = (tex >> 8) & 0xFF, tb = tex & 0xFF;
		switch (func) {
			case 0: src = col; break;
			case 1: src = tex; break;
			case 2: /* decal */
				src = argb(ca, (cr * (255 - ta) + tr * ta) / 255, (cg * (255 - ta) + tg * ta) / 255,
					(cb * (255 - ta) + tb * ta) / 255);
				break;
			case 3: /* modulate */
				src = argb((ta * ca) / 255, (tr * cr) / 255, (tg * cg) / 255, (tb * cb) / 255);
				break;
			default:
				src = tex;
				break;
		}
	}

	/* fog: NewColor = f * (SrcColor - FogColor) + FogColor, f = 255 means no fog */
	if ((dmode & 0x200000u) && !(dmode & 0x40000u)) {
		const int32_t f = param8(pe.f);
		uint32_t o = src & 0xFF000000u;
		for (unsigned sh = 0; sh < 24; sh += 8) {
			const int32_t s = (int32_t)((src >> sh) & 0xFF), fc = (int32_t)((pe.fog_color >> sh) & 0xFF);
			o |= (uint32_t)clamp255(fc + (f * (s - fc)) / 255) << sh;
		}
		src = o;
	}

	/* alpha test */
	if ((dmode & 0x2000000u) && (src >> 24) <= pe.alpha_thres) return;

	/* Z buffer */
	const unsigned zmode = (dmode >> 12) & 7;
	if (zmode == 7) return;
	uint32_t zaddr = 0;
	const uint16_t newz = (uint16_t)((uint32_t)pe.z >> 11);
	if (zmode != 0 || (dmode & 0x10000u)) {
		zaddr = pe.z_base + RowOffset((uint32_t)y, pe.stride >> 16) + (uint32_t)x * 2u;
		if (zmode != 0) {
			const uint16_t oldz = Read16(zaddr);
			bool pass;
			switch (zmode) {
				case 1: pass = newz < oldz; break;
				case 2: pass = newz == oldz; break;
				case 3: pass = newz <= oldz; break;
				case 4: pass = newz > oldz; break;
				case 5: pass = newz != oldz; break;
				case 6: pass = newz >= oldz; break;
				default: pass = true; break;
			}
			if (!pass) return;
		}
	}
	pe.pick_hit = true;

	const uint32_t addr = pe.dst_base + RowOffset((uint32_t)y, pe.stride >> 8) + (uint32_t)x * bpp;
	const unsigned rop = dmode & 0xF;
	const bool blend = (dmode & 0x80000u) != 0;
	const bool yuv = (dmode & 0x40000u) != 0;
	const bool need_dst = !(dmode & 0x800000u) && (blend || RopReadsDst(rop) ||
		((pe.pmask != 0xFFFFFFFFu) && pe.pmask != 0));
	uint32_t draw_raw = need_dst ? ReadRaw(*this, addr, bpp) : 0;
	uint32_t dst = (dmode & 0x800000u) ? pe.dst_color :
		(need_dst ? ExpandToARGB(draw_raw, fmt, (dmode & 0x1000000u) != 0, (dmode & 0x400000u) != 0) : 0);

	uint32_t out = src;
	if (yuv) {
		/* [SPEC] 4.15: Cr = r, Y = g, Cb = b; YUVBlack is FogColor green */
		const int32_t Y = (int32_t)((src >> 8) & 0xFF), Cr = (int32_t)((src >> 16) & 0xFF), Cb = (int32_t)(src & 0xFF);
		const int32_t black = (int32_t)((pe.fog_color >> 8) & 0xFF);
		const int32_t sY = ((int32_t)pe.yuv_contrast + 256) * (Y - black) + 128;
		const int32_t R = (int32_t)floor((sY + 407.0 * (Cr - 128)) / 255.0);
		const int32_t G = (int32_t)floor((sY - 207.0 * (Cr - 128) - 100.0 * (Cb - 128)) / 255.0);
		const int32_t B = (int32_t)floor((sY + 515.0 * (Cb - 128)) / 255.0);
		out = argb(src >> 24, clamp255(R), clamp255(G), clamp255(B));
	}
	else if (blend) {
		const unsigned sf = (dmode >> 4) & 0xF, df = (dmode >> 8) & 0xF;
		const int32_t sa = (int32_t)(src >> 24), da = (int32_t)(dst >> 24);
		uint32_t o = 0;
		for (unsigned sh = 0; sh < 32; sh += 8) {
			const int32_t s = (int32_t)((src >> sh) & 0xFF), dd = (int32_t)((dst >> sh) & 0xFF);
			const bool alpha_ch = (sh == 24);
			auto factor = [&](unsigned fsel, bool for_src) -> int32_t {
				switch (fsel) {
					case 0: return for_src ? dd : s;          /* BlendDstColor / BlendSrcColor */
					case 1: return 255 - (for_src ? dd : s);
					case 2: return sa;
					case 3: return 255 - sa;
					case 4: return da;
					case 5: return 255 - da;
					case 6: return 0;
					case 7: return 255;
					case 8: return alpha_ch ? 255 : std::min(sa, 255 - da);
					case 9: return alpha_ch ? 0 : 255 - std::min(sa, 255 - da);
					case 10: return for_src ? s : dd;         /* BlendSrcColor / BlendDstColor */
					case 11: return 255 - (for_src ? s : dd);
					default: return 255;
				}
			};
			int32_t r;
			if (sf == 15 || df == 15) {
				/* add signed */
				r = s + dd - 128;
			}
			else {
				r = (s * factor(sf, true) + dd * factor(df, false)) / 255;
			}
			o |= (uint32_t)clamp255(r) << sh;
		}
		out = o;
	}

	/* dither, pack, raster op, plane mask */
	const unsigned dval = dither_tab[(y + ((pe.dither_offset >> 4) & 3)) & 3][(x + (pe.dither_offset & 3)) & 3];
	uint32_t raw = PackARGB(out, fmt, (dmode & 0x1000000u) != 0, (dmode & 0x100000u) != 0, dval);
	if (rop != 0xC) raw = Rop(rop, raw, draw_raw);
	if (need_dst) {
		const uint32_t pm = Lane(pe.pmask, x, bpp);
		raw = (raw & pm) | (draw_raw & ~pm);
	}
	WriteRaw(*this, addr, bpp, raw);
	if (zaddr && (dmode & 0x10000u)) Write16(zaddr, newz);
	pixels++;
}

} /* namespace Verite */
