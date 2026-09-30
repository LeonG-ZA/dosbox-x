/*
 *  ATI mach64 GT (3D RAGE) emulation: GUI (draw) engine.
 *
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
 *  Register reference: RRG-G02700 [RRG], mach64 Programmer's Guide
 *  PRG888GX0-01 [PRG]. The rectangle/line pixel pipeline, host data
 *  unpacking, pattern handling and context loading are adapted from 86Box's
 *  vid_ati_mach64_accel.c (Sarah Walker, Miran Grca, Connor Hyde; GPL-2.0
 *  or later). The engine here is synchronous: an operation runs to its end
 *  inside the register write that starts it, or, when it takes host data,
 *  as the HOST_DATA writes arrive. There is no command FIFO or thread.
 *
 *  The GT trapezoid trajectory is not described in enough detail by [RRG]
 *  (chapter 4, "Destination (and Z) Trajectory"); see Mach64_TrapStart.
 */

#include <string.h>

#include <stdlib.h>
#include "dosbox.h"
#include "logging.h"
#include "vga.h"
#include "vga_mach64.h"
#include <string>

/* TEST HOOK: DOSBOX_X_M64SNAP collects one line per drawing operation for the frame */
std::string m64snap_log; bool m64snap_on = false; unsigned int m64snap_seq = 0;
extern unsigned int m64snap_zfail, m64snap_rej;
extern int64_t m64snap_u, m64snap_v; extern unsigned int m64snap_wl, m64snap_hl, m64snap_pl, m64snap_sl;

#define A (mach64.accel)

static inline uint32_t R(unsigned int reg) { return mach64.reg[reg]; }

static inline int32_t sext(uint32_t v, int bits) {
	return (int32_t)(v << (32 - bits)) >> (32 - bits);
}

/* pixel size codes */
enum { SZ_1 = 0, SZ_4 = 1, SZ_8 = 2, SZ_16 = 3, SZ_32 = 4 };

/* DP_*_PIX_WIDTH to storage size [RRG 4-89]; 24 bpp is drawn as 8 bpp with
 * DST_24_ROT_EN. YUV422 and aYUV444 are raw 32-bit data to the 2D engine. */
static int PixSize(unsigned int code) {
	switch (code & 0xF) {
		case M64_PIX_MONO:  return SZ_1;
		case M64_PIX_4BPP:  return SZ_4;
		case M64_PIX_15BPP:
		case M64_PIX_16BPP:
		case M64_PIX_ARGB4444: return SZ_16;
		case M64_PIX_32BPP:
		case M64_PIX_YUV422:
		case M64_PIX_AYUV444: return SZ_32;
		default:            return SZ_8;
	}
}

static inline int PixBits(int sz) {
	switch (sz) {
		case SZ_1: return 1;
		case SZ_4: return 4;
		case SZ_8: return 8;
		case SZ_16: return 16;
		default: return 32;
	}
}

/* offsets are in qwords; the engine addresses pixels */
static uint32_t OffsetPixels(uint32_t off_pitch, int sz) {
	const uint32_t bytes = (off_pitch & 0xFFFFF) << 3;
	switch (sz) {
		case SZ_1: return bytes << 3;
		case SZ_4: return bytes << 1;
		case SZ_8: return bytes;
		case SZ_16: return bytes >> 1;
		default: return bytes >> 2;
	}
}

/* ---------------------------------------------------------------------- */
/* Video memory access by pixel address                                     */
/* ---------------------------------------------------------------------- */

static inline bool ByteOrderLSB(void) {
	return (R(M64_DP_PIX_WIDTH) & M64_DP_BYTE_PIX_ORDER) != 0;
}

uint32_t Mach64_ReadPixel(uint32_t addr, int sz) {
	uint8_t *mem = vga.mem.linear;
	const uint32_t mask = (uint32_t)vga.mem.memmask;
	switch (sz) {
		case SZ_8:  return mem[addr & mask];
		case SZ_16: { const uint32_t a = (addr << 1) & mask; return mem[a] | (mem[(a + 1) & mask] << 8); }
		case SZ_32: { const uint32_t a = (addr << 2) & mask; return mem[a] | (mem[(a + 1) & mask] << 8) | (mem[(a + 2) & mask] << 16) | ((uint32_t)mem[(a + 3) & mask] << 24); }
		case SZ_4: {
			const uint8_t b = mem[(addr >> 1) & mask];
			if (ByteOrderLSB()) return (addr & 1) ? (b >> 4) : (b & 0xF);
			return (addr & 1) ? (b & 0xF) : (b >> 4);
		}
		default: {
			const uint8_t b = mem[(addr >> 3) & mask];
			if (ByteOrderLSB()) return (b >> (addr & 7)) & 1;
			return (b >> (7 - (addr & 7))) & 1;
		}
	}
}

void Mach64_WritePixel(uint32_t addr, int sz, uint32_t v) {
	uint8_t *mem = vga.mem.linear;
	const uint32_t mask = (uint32_t)vga.mem.memmask;
	switch (sz) {
		case SZ_8: mem[addr & mask] = (uint8_t)v; break;
		case SZ_16: { const uint32_t a = (addr << 1) & mask; mem[a] = (uint8_t)v; mem[(a + 1) & mask] = (uint8_t)(v >> 8); break; }
		case SZ_32: {
			const uint32_t a = (addr << 2) & mask;
			mem[a] = (uint8_t)v; mem[(a + 1) & mask] = (uint8_t)(v >> 8);
			mem[(a + 2) & mask] = (uint8_t)(v >> 16); mem[(a + 3) & mask] = (uint8_t)(v >> 24);
			break;
		}
		case SZ_4: {
			uint8_t &b = mem[(addr >> 1) & mask];
			const bool hi = ByteOrderLSB() ? ((addr & 1) != 0) : !(addr & 1);
			if (hi) b = (uint8_t)((b & 0x0F) | ((v & 0xF) << 4));
			else    b = (uint8_t)((b & 0xF0) | (v & 0xF));
			break;
		}
		default: {
			uint8_t &b = mem[(addr >> 3) & mask];
			const unsigned int bit = ByteOrderLSB() ? (addr & 7) : (7 - (addr & 7));
			if (v & 1) b |= (uint8_t)(1u << bit);
			else b &= (uint8_t)~(1u << bit);
			break;
		}
	}
}

/* ---------------------------------------------------------------------- */
/* Mixes [RRG 4-91]                                                         */
/* ---------------------------------------------------------------------- */

static uint32_t MixAverage(uint32_t s, uint32_t d) {
	/* mix 17h: (DST+SRC)/2, the carry chain broken where DP_CHAIN_MSK has a 1 [RRG 4-90] */
	const int bits = PixBits(A.dst_size) < 16 ? PixBits(A.dst_size) : 32;
	uint32_t chain = (R(M64_DP_CHAIN_MSK) & 0x7FFF) | 0x8000;
	if (bits < 16) chain |= 1u << (bits - 1);
	uint32_t ret = 0;
	int start = 0;
	for (int i = 0; i < bits; i++) {
		if (((chain >> (i & 15)) & 1) || i == bits - 1) {
			const int w = i - start + 1;
			const uint32_t m = (w >= 32) ? 0xFFFFFFFFu : ((1u << w) - 1);
			const uint64_t sum = (uint64_t)((s >> start) & m) + ((d >> start) & m);
			ret |= (uint32_t)((sum >> 1) & m) << start;
			start = i + 1;
		}
	}
	return ret;
}

static inline uint32_t Mix(int fn, uint32_t s, uint32_t d) {
	switch (fn) {
		case 0x0: return ~d;
		case 0x1: return 0;
		case 0x2: return 0xFFFFFFFFu;
		case 0x3: return d;
		case 0x4: return ~s;
		case 0x5: return s ^ d;
		case 0x6: return ~(s ^ d);
		case 0x7: return s;
		case 0x8: return ~(s & d);
		case 0x9: return ~s | d;
		case 0xA: return s | ~d;
		case 0xB: return s | d;
		case 0xC: return s & d;
		case 0xD: return s & ~d;
		case 0xE: return ~s & d;
		case 0xF: return ~(s | d);
		case 0x17: return MixAverage(s, d);
		default: return s;   /* 10h-16h, 18h-1Fh: saturating mixes not modelled; take the source */
	}
}

/* colour compare [RRG 4-100]: true = keep the destination */
static bool ColourCompare(uint32_t src, uint32_t dst, bool texel_src, uint32_t texel) {
	const uint32_t cntl = R(M64_CLR_CMP_CNTL);
	/* against the destination only the pixel's bits take part: drivers write the 16 bpp key and mask
	 * replicated into both halves (F83EF83Eh), as for the other colour registers */
	const int pb = PixBits(A.dst_size);
	const uint32_t pmask = (((cntl >> 24) & 3) != 0 || pb >= 32) ? 0xFFFFFFFFu : ((1u << pb) - 1);
	const uint32_t msk = R(M64_CLR_CMP_MSK) & pmask;
	const uint32_t clr = R(M64_CLR_CMP_CLR) & msk;
	uint32_t v;
	switch ((cntl >> 24) & 3) {
		case 1:  v = src; break;
		case 2:  v = texel_src ? texel : src; break;
		default: v = dst; break;
	}
	switch (cntl & 7) {
		case 1: return true;
		case 4: return (v & msk) != clr;
		case 5: return (v & msk) == clr;
		default: return false;
	}
}

/* ---------------------------------------------------------------------- */
/* Operation setup                                                          */
/* ---------------------------------------------------------------------- */

enum { SRC_BG = 0, SRC_FG = 1, SRC_HOST = 2, SRC_BLIT = 3, SRC_PAT = 4 };
enum { MONO_1 = 0, MONO_PAT = 1, MONO_HOST = 2, MONO_BLIT = 3 };

static void SetupCommon(void) {
	const uint32_t pw = R(M64_DP_PIX_WIDTH);
	A.dst_size  = PixSize(pw & 0xF);
	A.src_size  = PixSize((pw >> 8) & 0xF);
	A.host_size = PixSize((pw >> 16) & 0xF);

	A.src_pitch  = ((R(M64_SRC_OFF_PITCH) >> 22) & 0x3FF) << 3;
	A.src_offset = OffsetPixels(R(M64_SRC_OFF_PITCH), A.src_size);
	A.dst_pitch  = ((R(M64_DST_OFF_PITCH) >> 22) & 0x3FF) << 3;
	A.dst_offset = OffsetPixels(R(M64_DST_OFF_PITCH), A.dst_size);
	A.poly_offset = OffsetPixels(R(M64_SRC_OFF_PITCH), SZ_1);

	A.mix_fg = (R(M64_DP_MIX) >> 16) & 0x1F;
	A.mix_bg = R(M64_DP_MIX) & 0x1F;
	A.source_bg  = R(M64_DP_SRC) & 7;
	A.source_fg  = (R(M64_DP_SRC) >> 8) & 7;
	A.source_mix = (R(M64_DP_SRC) >> 16) & 3;
	A.source_host = A.source_bg == SRC_HOST || A.source_fg == SRC_HOST || A.source_mix == MONO_HOST;

	const uint32_t pat = R(M64_PAT_CNTL);
	if (pat & 1) {
		/* mono 8x8 pattern, by DP_BYTE_PIX_ORDER [PRG 2-36] */
		for (int y = 0; y < 8; y++) {
			const uint32_t t = (y & 4) ? R(M64_PAT_REG1) : R(M64_PAT_REG0);
			for (int x = 0; x < 8; x++) {
				const int bit = ByteOrderLSB() ? x : (7 - x);
				A.pattern[y][x] = (uint8_t)((t >> (bit + ((y & 3) << 3))) & 1);
			}
		}
	}
	for (int i = 0; i < 4; i++) {
		A.pattern_clr4x2[0][i] = (uint8_t)(R(M64_PAT_REG0) >> (i * 8));
		A.pattern_clr4x2[1][i] = (uint8_t)(R(M64_PAT_REG1) >> (i * 8));
		A.pattern_clr8x1[i]     = (uint8_t)(R(M64_PAT_REG0) >> (i * 8));
		A.pattern_clr8x1[i + 4] = (uint8_t)(R(M64_PAT_REG1) >> (i * 8));
	}

	A.sc_left   = sext(R(M64_SC_LEFT) & 0x1FFF, 13);
	A.sc_right  = sext(R(M64_SC_RIGHT) & 0x1FFF, 13);
	A.sc_top    = sext(R(M64_SC_TOP) & 0x7FFF, 15);
	A.sc_bottom = sext(R(M64_SC_BOTTOM) & 0x7FFF, 15);

	A.xinc = (R(M64_DST_CNTL) & M64_DST_X_DIR) ? 1 : -1;
	A.yinc = (R(M64_DST_CNTL) & M64_DST_Y_DIR) ? 1 : -1;
	A.row_ended = false;
}

/* packed 24 bpp: the colour component of the current byte [PRG 4-7] */
static inline uint32_t RotByte(uint32_t v) {
	return (v >> (8 * A.rot)) & 0xFF;
}

/* The source colour of one pixel (before the mix). */
static uint32_t SourceColour(int sel, int32_t dst_x, int32_t dst_y, int32_t src_x, int32_t src_y, uint32_t host, bool rot) {
	switch (sel) {
		case SRC_HOST: return host;
		case SRC_BLIT:
			if (R(M64_SRC_CNTL) & M64_SRC_LINEAR_EN)
				return Mach64_ReadPixel(A.src_offset + (uint32_t)src_x, A.src_size);
			return Mach64_ReadPixel(A.src_offset + (uint32_t)(src_y * (int32_t)A.src_pitch + src_x), A.src_size);
		case SRC_FG: return rot ? RotByte(R(M64_DP_FRGD_CLR)) : R(M64_DP_FRGD_CLR);
		case SRC_PAT:
			if (R(M64_PAT_CNTL) & 2) return A.pattern_clr4x2[dst_y & 1][dst_x & 3];
			if (R(M64_PAT_CNTL) & 4) return A.pattern_clr8x1[dst_x & 7];
			return R(M64_DP_FRGD_CLR);
		case SRC_BG:
		default:
			return rot ? RotByte(R(M64_DP_BKGD_CLR)) : R(M64_DP_BKGD_CLR);
	}
}

/* Writes one destination pixel through the data path: mix, colour compare,
 * write mask, scissors. */
static void DataPathPixel(int32_t dx, int32_t dy, bool mix, uint32_t src, bool rot) {
	if (dx < A.sc_left || dx > A.sc_right || dy < A.sc_top || dy > A.sc_bottom) return;
	const uint32_t addr = A.dst_offset + (uint32_t)(dy * (int32_t)A.dst_pitch + dx);
	uint32_t dst = Mach64_ReadPixel(addr, A.dst_size);
	if (ColourCompare(src, dst, false, 0)) return;
	const uint32_t wm = rot ? RotByte(R(M64_DP_WRITE_MSK)) : R(M64_DP_WRITE_MSK);
	const uint32_t res = Mix(mix ? A.mix_fg : A.mix_bg, src, dst);
	Mach64_WritePixel(addr, A.dst_size, (res & wm) | (dst & ~wm));
}

static void StartFill(void) {
	SetupCommon();
	A.dst_x = 0;
	A.dst_y = 0;
	A.dst_x_start = sext(R(M64_DST_X) & 0x1FFF, 13);
	A.dst_y_start = sext(R(M64_DST_Y) & 0x7FFF, 15);
	A.dst_width  = (int32_t)(R(M64_DST_WIDTH) & 0x1FFF);
	A.dst_height = (int32_t)(R(M64_DST_HEIGHT) & 0x7FFF);
	A.x_count = A.dst_width;

	A.src_x = 0;
	A.src_y = 0;
	A.src_x_start = sext(R(M64_SRC_X) & 0x1FFF, 13);
	A.src_y_start = sext(R(M64_SRC_Y) & 0x7FFF, 15);
	A.src_width1  = (int32_t)(R(M64_SRC_WIDTH1) & 0x1FFF);
	A.src_height1 = (int32_t)(R(M64_SRC_HEIGHT1) & 0x7FFF);
	A.src_width2  = (int32_t)(R(M64_SRC_WIDTH2) & 0x1FFF);
	A.src_height2 = (int32_t)(R(M64_SRC_HEIGHT2) & 0x7FFF);
	A.src_x_count = (R(M64_SRC_CNTL) & M64_SRC_LINEAR_EN) ? 0x7FFFFFF : A.src_width1;
	A.src_y_count = (R(M64_SRC_CNTL) & M64_SRC_PATT_EN) ? A.src_height1 : 0x7FFFFFF;
	if (A.src_x_count <= 0) A.src_x_count = 0x7FFFFFF;
	if (A.src_y_count <= 0) A.src_y_count = 0x7FFFFFF;

	A.rot0 = (int32_t)(((((R(M64_DST_CNTL) >> 8) & 7) * 4) + (A.dst_x_start & 3)) % 3);
	A.rot = A.rot0;
	A.poly_draw = false;
	A.op = M64_OP_RECT;
	A.busy = A.dst_width > 0 && A.dst_height > 0;
}

/* Scaler/3D pipe used for this operation? (SCALE_3D_FCN [RRG 6-4]) */
static inline unsigned int Scale3DFcn(void) {
	return (R(M64_SCALE_3D_CNTL) >> 6) & 3;
}

extern void Mach64_Scale_Begin(void);
extern bool Mach64_Scale_Pixel(int32_t col, int32_t row, uint32_t dst, uint32_t &out);

/* One destination pixel of a rectangle fill or blit; true when finished. */
static bool RectPixel(uint32_t host) {
	const bool rot = (R(M64_DST_CNTL) & M64_DST_24_ROT_EN) != 0;
	const int32_t dx = A.dst_x + A.dst_x_start;
	const int32_t dy = A.dst_y + A.dst_y_start;
	int32_t sx = (R(M64_SRC_CNTL) & M64_SRC_LINEAR_EN) ? A.src_x : (A.src_x + A.src_x_start);
	int32_t sy = A.src_y + A.src_y_start;
	if (R(M64_SRC_CNTL) & M64_SRC_TRACK_DST) { sx = dx; sy = dy; }

	bool mix = false;
	switch (A.source_mix) {
		case MONO_HOST: mix = (host & 1) != 0; break;
		case MONO_PAT:
			if (rot) mix = A.pattern[dy & 7][((dx - ((dx < 0) ? 2 : 0)) / 3) & 7] != 0;
			else mix = A.pattern[dy & 7][dx & 7] != 0;
			break;
		case MONO_BLIT:
			if (R(M64_SRC_CNTL) & M64_SRC_LINEAR_EN) mix = Mach64_ReadPixel(A.src_offset + (uint32_t)sx, SZ_1) != 0;
			else mix = Mach64_ReadPixel(A.src_offset + (uint32_t)(sy * (int32_t)A.src_pitch + sx), SZ_1) != 0;
			break;
		default: mix = true; break;
	}

	bool draw = true;
	if (R(M64_DST_CNTL) & M64_DST_POLYGON_EN) {
		/* alternate polygon fill: the boundary toggles the fill [PRG 2-43] */
		const bool was = A.poly_draw;
		const bool bound = Mach64_ReadPixel(A.poly_offset + (uint32_t)(sy * (int32_t)A.src_pitch + sx), SZ_1) != 0;
		if (bound) A.poly_draw = !A.poly_draw;
		draw = was || A.poly_draw;
		if (bound && was && !A.poly_draw && (R(M64_DST_CNTL) & M64_DST_POLYGON_RTEDGE_DIS)) draw = false;
	}

	if (draw) {
		if (Scale3DFcn() == 1) {
			/* stretch blit through the scaler [RRG 6-1] */
			if (dx >= A.sc_left && dx <= A.sc_right && dy >= A.sc_top && dy <= A.sc_bottom) {
				const uint32_t addr = A.dst_offset + (uint32_t)(dy * (int32_t)A.dst_pitch + dx);
				const uint32_t dst = Mach64_ReadPixel(addr, A.dst_size);
				uint32_t out;
				if (Mach64_Scale_Pixel(A.dst_x * A.xinc, A.dst_y * A.yinc, dst, out)) {
					const uint32_t wm = R(M64_DP_WRITE_MSK);
					Mach64_WritePixel(addr, A.dst_size, (out & wm) | (dst & ~wm));
				}
			}
		}
		else {
			const uint32_t src = SourceColour(mix ? A.source_fg : A.source_bg, dx, dy, sx, sy, host, rot);
			DataPathPixel(dx, dy, mix, src, rot);
		}
	}

	/* next pixel: source, then destination */
	if (R(M64_SRC_CNTL) & M64_SRC_LINEAR_EN)
		A.src_x++;
	else {
		A.src_x += A.xinc;
		if (--A.src_x_count <= 0) {
			A.src_x = 0;
			if ((R(M64_SRC_CNTL) & (M64_SRC_PATT_ROT_EN|M64_SRC_PATT_EN)) == (M64_SRC_PATT_ROT_EN|M64_SRC_PATT_EN)) {
				A.src_x_start = sext(R(M64_SRC_X_START) & 0x1FFF, 13);
				A.src_x_count = A.src_width2 > 0 ? A.src_width2 : 0x7FFFFFF;
			}
			else A.src_x_count = A.src_width1 > 0 ? A.src_width1 : 0x7FFFFFF;
		}
	}
	A.dst_x += A.xinc;
	A.rot = (A.xinc > 0) ? ((A.rot + 1) % 3) : ((A.rot + 2) % 3);

	if (--A.x_count > 0) return false;

	/* end of a row */
	A.x_count = A.dst_width;
	A.rot = A.rot0;
	A.dst_x = 0;
	A.dst_y += A.yinc;
	A.src_x_start = sext(R(M64_SRC_X) & 0x1FFF, 13);
	A.src_x_count = (R(M64_SRC_CNTL) & M64_SRC_LINEAR_EN) ? 0x7FFFFFF : (A.src_width1 > 0 ? A.src_width1 : 0x7FFFFFF);

	if (R(M64_SRC_CNTL) & M64_SRC_LINEAR_EN) {
		/* SRC_BYTE_ALIGN: 1 and 4 bpp linear sources skip to the next byte [RRG 4-62] */
		if (R(M64_SRC_CNTL) & M64_SRC_BYTE_ALIGN) {
			const int sz = (A.source_mix == MONO_BLIT || A.src_size == SZ_1) ? SZ_1 : A.src_size;
			if (sz == SZ_1 || sz == SZ_4) {
				const uint32_t per = (sz == SZ_1) ? 8 : 2;
				uint32_t a = A.src_offset + (uint32_t)A.src_x;
				a = (a + per - 1) & ~(per - 1);
				A.src_x = (int32_t)(a - A.src_offset);
			}
		}
	}
	else {
		A.src_x = 0;
		A.src_y += A.yinc;
		if (--A.src_y_count <= 0) {
			A.src_y = 0;
			if ((R(M64_SRC_CNTL) & (M64_SRC_PATT_ROT_EN|M64_SRC_PATT_EN)) == (M64_SRC_PATT_ROT_EN|M64_SRC_PATT_EN)) {
				A.src_y_start = sext(R(M64_SRC_Y_START) & 0x7FFF, 15);
				A.src_y_count = A.src_height2 > 0 ? A.src_height2 : 0x7FFFFFF;
			}
			else A.src_y_count = A.src_height1 > 0 ? A.src_height1 : 0x7FFFFFF;
		}
	}
	A.poly_draw = false;
	A.row_ended = true;

	if (--A.dst_height > 0) return false;

	/* finished; tiling side effects [RRG 4-49] */
	A.busy = false;
	if (R(M64_DST_CNTL) & M64_DST_X_TILE) {
		const int32_t x = sext(R(M64_DST_X) & 0x1FFF, 13) + A.xinc * (int32_t)(R(M64_DST_WIDTH) & 0x1FFF);
		mach64.reg[M64_DST_X] = (uint32_t)x & 0x1FFF;
	}
	if (R(M64_DST_CNTL) & M64_DST_Y_TILE) {
		const int32_t y = sext(R(M64_DST_Y) & 0x7FFF, 15) + A.yinc * (int32_t)(R(M64_DST_HEIGHT) & 0x7FFF);
		mach64.reg[M64_DST_Y] = (uint32_t)y & 0x7FFF;
	}
	return true;
}

static bool line3d = false;   /* the running line takes the 3D pixel path (see LinePixel) */

static void StartLine(void) {
	SetupCommon();
	A.dst_x = sext(R(M64_DST_X) & 0x1FFF, 13);
	A.dst_y = sext(R(M64_DST_Y) & 0x7FFF, 15);
	A.src_x = sext(R(M64_SRC_X) & 0x1FFF, 13);
	A.src_y = sext(R(M64_SRC_Y) & 0x7FFF, 15);
	A.x_count = (int32_t)(R(M64_DST_BRES_LNTH) & 0x7FFF);
	A.err = sext(R(M64_DST_BRES_ERR) & 0x3FFFF, 18);
	A.inc = sext(R(M64_DST_BRES_INC) & 0x3FFFF, 18);
	A.dec = sext(R(M64_DST_BRES_DEC) & 0x3FFFF, 18);
	A.op = M64_OP_LINE;
	A.busy = A.x_count > 0;
	line3d = A.busy && Scale3DFcn() >= 2 && ((R(M64_DP_SRC) >> 8) & 7) == 5;
	if (line3d) Mach64_3D_TrapBegin();
}

/* sign of a zero Bresenham error [RRG 4-48, BRES_SIGN_AUTO on GT] */
static inline bool ErrPositive(int32_t err, bool sign_bit, bool xdir_pos, bool ydir_pos, bool ymajor) {
	if (err > 0) return true;
	if (err < 0) return false;
	if (R(M64_DST_CNTL) & M64_BRES_SIGN_AUTO)
		return ymajor ? !xdir_pos : !ydir_pos;
	return !sign_bit;
}

/* One pixel of a Bresenham line; true when finished. No 24 bpp lines [PRG 4-7]. */
/* ASSUMPTION: a line with DP_SRC FRGD_SRC 5 while the 3D engine is on takes
 * its colour from the 3D pixel path [RRG 4-94], the interpolators stepping one
 * X step (DST_X_DIR) or Y step per pixel the line moves, as on a trapezoid
 * edge. Assault Rigs draws its sky's triangle edges as such lines; drawn with
 * the plain foreground colour they showed as black lines. */
static bool LinePixel(uint32_t host) {
	const uint32_t dcntl = R(M64_DST_CNTL);
	const bool ymajor = (dcntl & M64_DST_Y_MAJOR) != 0;
	const bool err_pos = ErrPositive(A.err, (dcntl & M64_DST_BRES_SIGN) != 0, A.xinc > 0, A.yinc > 0, ymajor);
	bool draw = true;
	int32_t dx = A.dst_x;

	bool mix = false;
	switch (A.source_mix) {
		case MONO_HOST: mix = (host & 1) != 0; break;
		case MONO_PAT: mix = A.pattern[A.dst_y & 7][A.dst_x & 7] != 0; break;
		case MONO_BLIT: mix = Mach64_ReadPixel(A.src_offset + (uint32_t)(A.src_y * (int32_t)A.src_pitch + A.src_x), SZ_1) != 0; break;
		default: mix = true; break;
	}

	if (dcntl & M64_DST_POLYGON_EN) {
		/* polygon outline: one pixel a scan line, saturated to the left scissor [RRG 4-49] */
		draw = ymajor || err_pos;
		if (dx < A.sc_left) dx = A.sc_left;
	}
	if (A.x_count == 1 && !(dcntl & M64_DST_LAST_PEL)) draw = false;

	if (draw && line3d) {
		if (dx >= A.sc_left && dx <= A.sc_right && A.dst_y >= A.sc_top && A.dst_y <= A.sc_bottom && A.dst_y >= 0) {
			const uint32_t addr = A.dst_offset + (uint32_t)(A.dst_y * (int32_t)A.dst_pitch + dx);
			const uint32_t dst = Mach64_ReadPixel(addr, A.dst_size);
			const bool keep = ((R(M64_CLR_CMP_CNTL) >> 24) & 3) == 0 && ColourCompare(0, dst, false, 0);
			uint32_t out;
			Mach64_3D_SpanBegin(0, A.xinc > 0 ? 1 : -1);
			if (!keep && Mach64_3D_Pixel(dx, A.dst_y, addr, A.dst_size, dst, out)) {
				const uint32_t wm = R(M64_DP_WRITE_MSK);
				Mach64_WritePixel(addr, A.dst_size, (out & wm) | (dst & ~wm));
				if (m64snap_on && m64snap_log.size() < (32u << 20)) { char b[80]; snprintf(b, sizeof(b), "  LP %d,%d dst=%04x out=%04x\n", dx, A.dst_y, (unsigned)dst, (unsigned)out); m64snap_log += b; }   /* TEST HOOK */
			}
		}
	}
	else if (draw) {
		const uint32_t src = SourceColour(mix ? A.source_fg : A.source_bg, dx, A.dst_y, A.src_x, A.src_y, host, false);
		DataPathPixel(dx, A.dst_y, mix, src, false);
	}

	if (--A.x_count <= 0) {
		A.busy = false;
		/* DST_X and DST_Y rest on the last pixel [PRG 2-43] */
		mach64.reg[M64_DST_X] = (uint32_t)A.dst_x & 0x1FFF;
		mach64.reg[M64_DST_Y] = (uint32_t)A.dst_y & 0x7FFF;
		mach64.reg[M64_DST_BRES_ERR] = (uint32_t)A.err & 0x3FFFF;
		if (line3d) { Mach64_3D_TrapEnd(); line3d = false; }
		return true;
	}

	A.src_x += (R(M64_SRC_CNTL) & M64_SRC_LINE_X_DIR) ? 1 : -1;
	const int32_t ox = A.dst_x, oy = A.dst_y;
	if (ymajor) {
		A.dst_y += A.yinc;
		if (err_pos) { A.err += A.dec; A.dst_x += A.xinc; }
		else A.err += A.inc;
	}
	else {
		A.dst_x += A.xinc;
		if (err_pos) { A.err += A.dec; A.dst_y += A.yinc; }
		else A.err += A.inc;
	}
	if (line3d) {
		if (A.dst_x != ox) Mach64_3D_EdgeStepX();
		if (A.dst_y != oy) Mach64_3D_EdgeStepY();
	}
	return false;
}

/* ---------------------------------------------------------------------- */
/* Trapezoids (GT)                                                          */
/* ---------------------------------------------------------------------- */

static unsigned int trap_log_count = 0;

/* One scan line of a trapezoid edge. [RRG] documents the registers
 * (err/inc/dec, "INC must be positive, DEC negative", LNTH = "trapezoid
 * leading edge length") but not the stepping. Per scan line INC is added
 * once, then while the error is positive (a zero error counts as positive
 * unless the edge's BRES_SIGN bit says otherwise) the edge steps one pixel in
 * its X direction and adds DEC, so edges flatter than 45 degrees move several
 * pixels per line. This is the order the ATI Windows 9x Direct3D HAL's edge
 * terms need, as established by 86GPTBox's Rage II+ emulation against
 * captured HAL command streams (vid_ati_mach64_3d_part3.inc,
 * r3d_edge_step_scanline). Returns the X steps taken (signed). */
static int32_t EdgeStep(int32_t &x, int32_t &err, int32_t inc, int32_t dec, int32_t dir, bool zero_negative) {
	const int32_t old = x;
	err += inc;
	for (int steps = 0; steps < 8192; steps++) {
		const bool pos = (err > 0) || (err == 0 && !zero_negative);
		if (!pos) break;
		x += dir;
		err += dec;
		/* malformed terms (DEC not negative): stop instead of running away */
		if (dec >= 0) break;
	}
	return x - old;
}

static void TrapezoidRun(void) {
	const uint32_t dcntl = R(M64_DST_CNTL);
	SetupCommon();

	int32_t lead_x = sext(R(M64_DST_X) & 0x1FFF, 13);
	int32_t y      = sext(R(M64_DST_Y) & 0x7FFF, 15);
	int32_t trail_x = sext(R(M64_DST_HEIGHT) & 0x1FFF, 13);   /* TRAIL_X aliases DST_HEIGHT[12:0] */
	int32_t lead_err = sext(R(M64_DST_BRES_ERR) & 0x3FFFF, 18);
	const int32_t lead_inc = sext(R(M64_DST_BRES_INC) & 0x3FFFF, 18);
	const int32_t lead_dec = sext(R(M64_DST_BRES_DEC) & 0x3FFFF, 18);
	int32_t trail_err = sext(R(M64_TRAIL_BRES_ERR) & 0x3FFFF, 18);
	const int32_t trail_inc = sext(R(M64_TRAIL_BRES_INC) & 0x3FFFF, 18);
	const int32_t trail_dec = sext(R(M64_TRAIL_BRES_DEC) & 0x3FFFF, 18);
	const int32_t lines = (int32_t)(R(M64_DST_BRES_LNTH) & 0x7FFF);
	const int32_t lead_dir  = (dcntl & M64_DST_X_DIR) ? 1 : -1;
	const int32_t trail_dir = (dcntl & M64_TRAIL_X_DIR) ? 1 : -1;
	const bool lead_zneg  = (dcntl & M64_DST_BRES_SIGN) != 0;
	const bool trail_zneg = (dcntl & M64_TRAIL_BRES_SIGN) != 0;
	/* TRAP_FILL_DIR: 1 = the trailing edge is right of the leading edge [RRG 4-48] */
	const bool fill_lr = (dcntl & M64_TRAP_FILL_DIR) != 0;
	/* DST_POLYGON_RTEDGE_DIS leaves out the right edge pixel [RRG 4-48] */
	const bool rtedge_dis = (dcntl & M64_DST_POLYGON_RTEDGE_DIS) != 0;
	const bool use3d = Scale3DFcn() >= 2;

	if (trap_log_count < 64) {
		trap_log_count++;
		LOG(LOG_VGAMISC, LOG_NORMAL)("MACH64 TRAP #%u lead=(%d,%d) err=%d inc=%d dec=%d dir=%d trail=%d err=%d inc=%d dec=%d dir=%d lines=%d dst_cntl=%08x 3d=%08x",
			trap_log_count, lead_x, y, lead_err, lead_inc, lead_dec, lead_dir, trail_x, trail_err, trail_inc, trail_dec, trail_dir, lines,
			dcntl, R(M64_SCALE_3D_CNTL));
		if (use3d)
			LOG(LOG_VGAMISC, LOG_NORMAL)("MACH64 TRAP #%u R=%08x/%08x/%08x A=%08x Z=%08x S=%08x/%08x/%08x T=%08x/%08x/%08x tex=%03x",
				trap_log_count, R(M64_RED_START), R(M64_RED_X_INC), R(M64_RED_Y_INC), R(M64_ALPHA_START), R(M64_Z_START),
				R(M64_S_START), R(M64_S_XINC_START), R(M64_S_Y_INC), R(M64_T_START), R(M64_T_XINC_START), R(M64_T_Y_INC),
				R(M64_TEX_SIZE_PITCH));
	}

	/* bound the work of malformed commands: the span never leaves the pitch
	 * or the scissors, and rows past the end of video memory are skipped */
	int32_t xmin = A.sc_left < 0 ? 0 : A.sc_left;
	int32_t xmax = A.sc_right;
	if ((int32_t)A.dst_pitch > 0 && xmax >= (int32_t)A.dst_pitch) xmax = (int32_t)A.dst_pitch - 1;

	if (use3d) Mach64_3D_TrapBegin();
	uint32_t sn_drawn = 0, sn_clip = 0, sn_keep = 0, sn_rows = 0; int32_t sn_x0 = 1 << 20, sn_x1 = -(1 << 20), sn_y0 = 1 << 20, sn_y1 = -(1 << 20);   /* TEST HOOK */
	const unsigned int sn_z0 = m64snap_zfail, sn_r0 = m64snap_rej;
	const int32_t sn_lx = lead_x, sn_tx = trail_x, sn_y = y, sn_le = lead_err, sn_te = trail_err;

	for (int32_t l = 0; l < lines; l++) {
		/* ASSUMPTION: the right edge's x is drawn (DST_POLYGON_RTEDGE_DIS
		 * [RRG 4-48] leaves it out), except for a vertical trailing edge on the
		 * right (TRAP_FILL_DIR 1, TRAIL_BRES_INC 0). ATI3DCIF's text quads end on
		 * such an edge one past the glyph: drawing it showed the next glyph's first
		 * column (Wipeout and Assault Rigs text). A vertical leading edge on the
		 * right keeps its x: Wipeout's pilot logo tiles end there and had gaps
		 * without it. A trailing edge stepping left with a positive error (not
		 * yet stepped: the first row of a triangle) lies left of its x, so the x
		 * is left out too (the dot at the top right of Wipeout's glyphs).
		 * Otherwise sloped edges keep the x (Assault Rigs' meshes cracked). */
		const int32_t lo = fill_lr ? lead_x : trail_x;
		int32_t hi = fill_lr ? trail_x : lead_x;
		static int oldedge = -1; if (oldedge < 0) oldedge = getenv("DOSBOX_X_M64OLDEDGE") ? 1 : 0;   /* TEST HOOK: always draw the right x */
		if (rtedge_dis || (!oldedge && fill_lr && (trail_inc == 0 || (trail_dir < 0 && trail_err > 0)))) hi--;
		if (m64snap_on && lo <= hi) { if (y < A.sc_top || y > A.sc_bottom || y < 0) sn_clip += (uint32_t)(hi - lo + 1); else { if (lo < xmin) sn_clip += (uint32_t)(xmin - lo); if (hi > xmax) sn_clip += (uint32_t)(hi - xmax); } }
		if (lo <= hi && y >= A.sc_top && y <= A.sc_bottom && y >= 0) {
			const int32_t dl = lo < xmin ? xmin : lo;
			int32_t dh = hi + 1;
			if (dh > xmax + 1) dh = xmax + 1;
			if (dl < dh) {
				/* attributes are defined relative to the leading edge, stepping in DST_X_DIR */
				if (use3d) Mach64_3D_SpanBegin((dl - lead_x) * lead_dir, lead_dir);
				for (int32_t x = dl; x < dh; x++) {
					const uint32_t addr = A.dst_offset + (uint32_t)(y * (int32_t)A.dst_pitch + x);
					const uint32_t dst = Mach64_ReadPixel(addr, A.dst_size);
					if (use3d) {
						uint32_t out;
						/* colour compare on the destination [RRG 4-100] (the texel source,
						 * CLR_CMP_SRC 2, is tested in Mach64_3D_Pixel). ATI3DCIF sprites draw
						 * a keyed mask, then a filtered pass only where the mask colour is */
						const bool keep = ((R(M64_CLR_CMP_CNTL) >> 24) & 3) == 0 && ColourCompare(0, dst, false, 0);
						if (keep) sn_keep++;
						const bool sn_ok = !keep && Mach64_3D_Pixel(x, y, addr, A.dst_size, dst, out);
						if (m64snap_on && (l == 0 || l == lines - 1) && Scale3DFcn() == 2 && m64snap_wl < m64snap_sl && m64snap_log.size() < (32u << 20)) { char b[200]; snprintf(b, sizeof(b), "  BP %d,%d row=%d/%d ok=%d u=%lld v=%lld wl=%u hl=%u sl=%u out=%04x\n", x, y, (int)l, (int)lines, (int)sn_ok, (long long)m64snap_u, (long long)m64snap_v, m64snap_wl, m64snap_hl, m64snap_sl, sn_ok ? (unsigned)out : 0u); m64snap_log += b; }   /* TEST HOOK */
						if (m64snap_on && x == dh - 1 && Scale3DFcn() == 2 && m64snap_log.size() < (32u << 20)) { char b[200]; snprintf(b, sizeof(b), "  RP %d,%d ok=%d u=%lld v=%lld wl=%u hl=%u pl=%u sl=%u out=%04x dst=%04x\n", x, y, (int)sn_ok, (long long)m64snap_u, (long long)m64snap_v, m64snap_wl, m64snap_hl, m64snap_pl, m64snap_sl, sn_ok ? (unsigned)out : 0u, (unsigned)dst); m64snap_log += b; }   /* TEST HOOK */
						if (sn_ok) { sn_drawn++; if (x < sn_x0) sn_x0 = x; if (x > sn_x1) sn_x1 = x; if (y < sn_y0) sn_y0 = y; if (y > sn_y1) sn_y1 = y; }
						if (sn_ok) {
							/* DP_SRC FRGD_SRC [RRG 4-94]: 5 = the 3D/scaler data; 0/1 put the
							 * BKGD/FRGD colour where the texel passed (ATI3DCIF's sprite mask) */
							const unsigned int fsrc = (R(M64_DP_SRC) >> 8) & 7;
							if (fsrc == 0) out = R(M64_DP_BKGD_CLR);
							else if (fsrc == 1) out = R(M64_DP_FRGD_CLR);
							const uint32_t wm = R(M64_DP_WRITE_MSK);
							Mach64_WritePixel(addr, A.dst_size, (out & wm) | (dst & ~wm));
						}
						Mach64_3D_SpanStep();
					}
					else {
						const uint32_t src = SourceColour(A.source_fg, x, y, x, y, 0, false);
						DataPathPixel(x, y, true, src, false);
					}
				}
			}
		}

		/* next scan line: both edges, then the interpolators follow the
		 * leading edge, one Y step and one X step per pixel it moved */
		if (lo <= hi) sn_rows++;
		const int32_t lead_dx = EdgeStep(lead_x, lead_err, lead_inc, lead_dec, lead_dir, lead_zneg);
		EdgeStep(trail_x, trail_err, trail_inc, trail_dec, trail_dir, trail_zneg);
		if (use3d) {
			Mach64_3D_EdgeStepY();
			const int32_t steps = lead_dx < 0 ? -lead_dx : lead_dx;
			for (int32_t s = 0; s < steps; s++) Mach64_3D_EdgeStepX();
		}
		y += A.yinc;
	}

	/* The trajectory registers are live: the Windows HAL draws the second
	 * trapezoid of a triangle by loading only TRAIL_X and the new trailing
	 * edge, the leading edge and the interpolators continuing from here. */
	mach64.reg[M64_DST_X] = (uint32_t)lead_x & 0x1FFF;
	mach64.reg[M64_DST_Y] = (uint32_t)y & 0x7FFF;
	mach64.reg[M64_DST_HEIGHT] = (mach64.reg[M64_DST_HEIGHT] & ~0x1FFFu) | ((uint32_t)trail_x & 0x1FFF);
	mach64.reg[M64_DST_BRES_ERR] = (uint32_t)lead_err & 0x3FFFF;
	mach64.reg[M64_TRAIL_BRES_ERR] = (uint32_t)trail_err & 0x3FFFF;
	if (use3d) Mach64_3D_TrapEnd();
	if (m64snap_on && m64snap_log.size() < (32u << 20)) {   /* TEST HOOK */
		char b[1024];
		snprintf(b, sizeof(b), "T %u dst=%08x y=%d n=%d lead=%d,%d/%d/%d trail=%d,%d/%d/%d end=%d,%d dcntl=%08x 3d=%08x z=%08x tex=%03x t0=%06x cmp=%08x src=%03x sc=%d-%d,%d-%d rows=%u drawn=%u clip=%u keep=%u zf=%u rej=%u bb=%d,%d-%d,%d R=%08x G=%08x B=%08x A=%08x Z=%08x/%08x/%08x S=%08x/%08x T=%08x/%08x\n",
			m64snap_seq++, (unsigned)R(M64_DST_OFF_PITCH), sn_y, lines, sn_lx, sn_le, lead_inc, lead_dec * lead_dir, sn_tx, sn_te, trail_inc, trail_dec * trail_dir, lead_x, trail_x,
			(unsigned)dcntl, (unsigned)R(M64_SCALE_3D_CNTL), (unsigned)R(M64_Z_CNTL), (unsigned)(R(M64_TEX_SIZE_PITCH) & 0xFFF), (unsigned)(R(M64_TEX_0_OFF + ((R(M64_TEX_SIZE_PITCH) >> 4) & 0xF)) & 0xFFFFFF),
			(unsigned)R(M64_CLR_CMP_CNTL), (unsigned)(R(M64_DP_SRC) & 0xFFF), A.sc_left, A.sc_right, A.sc_top, A.sc_bottom,
			sn_rows, sn_drawn, sn_clip, sn_keep, m64snap_zfail - sn_z0, m64snap_rej - sn_r0, sn_x0, sn_y0, sn_x1, sn_y1,
			(unsigned)R(M64_RED_START), (unsigned)R(M64_GREEN_START), (unsigned)R(M64_BLUE_START), (unsigned)R(M64_ALPHA_START),
			(unsigned)R(M64_Z_START), (unsigned)R(M64_Z_X_INC), (unsigned)R(M64_Z_Y_INC), (unsigned)R(M64_S_START), (unsigned)R(M64_S_XINC_START), (unsigned)R(M64_T_START), (unsigned)R(M64_T_XINC_START));
		m64snap_log += b;
	}
	A.busy = false;
}

/* ---------------------------------------------------------------------- */
/* Running operations and host data                                         */
/* ---------------------------------------------------------------------- */

static bool OpPixel(uint32_t host) {
	if (A.op == M64_OP_LINE) return LinePixel(host);
	return RectPixel(host);
}

static void RunToEnd(void) {
	/* a destination of up to 8192 x 32767 pixels: bounded */
	while (A.busy && !OpPixel(0)) {}
	A.busy = false;
}

static void Begin(void) {
	if (A.busy && A.source_host) {
		/* a new operation while one still waits for host data: the old one is abandoned */
		A.busy = false;
	}
}

static void StartFillOp(void) {
	{   /* TEST HOOK (not for upstream): log the first fills */
		static int n = -1;
		if (n < 0) n = getenv("DOSBOX_X_M64TRACE") ? 600 : 0;
		if (n > 0) {
			n--;
			LOG_MSG("M64FILL dst_off_pitch=%08x yx=%08x hw=%08x pixw=%08x mix=%08x src=%08x frgd=%08x sc_lr=%08x sc_tb=%08x dst_cntl=%08x wmsk=%08x",
				(unsigned)R(M64_DST_OFF_PITCH), (unsigned)R(M64_DST_Y_X), (unsigned)R(M64_DST_HEIGHT_WIDTH), (unsigned)R(M64_DP_PIX_WIDTH), (unsigned)R(M64_DP_MIX),
				(unsigned)R(M64_DP_SRC), (unsigned)R(M64_DP_FRGD_CLR), (unsigned)R(M64_SC_LEFT_RIGHT), (unsigned)R(M64_SC_TOP_BOTTOM), (unsigned)R(M64_DST_CNTL), (unsigned)R(M64_DP_WRITE_MSK));
		}
	}
	if (m64snap_on && m64snap_log.size() < (32u << 20)) {   /* TEST HOOK */
		char b[400];
		snprintf(b, sizeof(b), "F %u dst=%08x yx=%08x hw=%08x dcntl=%08x 3d=%08x src=%08x mix=%08x frgd=%08x pixw=%08x cmp=%08x sc=%d-%d,%d-%d\n", m64snap_seq++,
			(unsigned)R(M64_DST_OFF_PITCH), (unsigned)R(M64_DST_Y_X), (unsigned)R(M64_DST_HEIGHT_WIDTH), (unsigned)R(M64_DST_CNTL), (unsigned)R(M64_SCALE_3D_CNTL),
			(unsigned)R(M64_DP_SRC), (unsigned)R(M64_DP_MIX), (unsigned)R(M64_DP_FRGD_CLR), (unsigned)R(M64_DP_PIX_WIDTH), (unsigned)R(M64_CLR_CMP_CNTL),
			(int)sext(R(M64_SC_LEFT) & 0x1FFF, 13), (int)sext(R(M64_SC_RIGHT) & 0x1FFF, 13), (int)R(M64_SC_TOP), (int)R(M64_SC_BOTTOM));
		m64snap_log += b;
	}
	Begin();
	StartFill();
	if (Scale3DFcn() == 1) Mach64_Scale_Begin();
	if (A.busy && !A.source_host) RunToEnd();
	if (getenv("DOSBOX_X_M64TRACE")) {   /* TEST HOOK */
		static int n = 600;
		if (n > 0) { n--;
			LOG_MSG("M64FILL done sc %d-%d %d-%d dst %u,%u %ux%u busy=%d host=%d word@dst=%04x",
				(int)R(M64_SC_LEFT), (int)R(M64_SC_RIGHT), (int)R(M64_SC_TOP), (int)R(M64_SC_BOTTOM),
				(unsigned)R(M64_DST_X), (unsigned)R(M64_DST_Y), (unsigned)R(M64_DST_WIDTH), (unsigned)R(M64_DST_HEIGHT), (int)A.busy, (int)A.source_host,
				(unsigned)(vga.mem.linear[((R(M64_DST_OFF_PITCH) & 0xFFFFF) * 8) & vga.mem.memmask] | (vga.mem.linear[(((R(M64_DST_OFF_PITCH) & 0xFFFFF) * 8) + 1) & vga.mem.memmask] << 8)));
		}
	}
}

static void StartLineOp(void) {
	Begin();
	if (R(M64_DST_BRES_LNTH) & 0x8000) {
		/* DRAW_TRAP */
		TrapezoidRun();
		return;
	}
	if (m64snap_on && m64snap_log.size() < (32u << 20)) {   /* TEST HOOK */
		char b[800];
		snprintf(b, sizeof(b), "L %u dst=%08x x=%d y=%d lnth=%08x err=%08x inc=%08x dec=%08x dcntl=%08x src=%08x mix=%08x frgd=%08x 3d=%08x z=%08x tex=%03x t0=%06x R=%08x/%08x/%08x G=%08x B=%08x A=%08x S=%08x/%08x/%08x T=%08x/%08x/%08x\n", m64snap_seq++,
			(unsigned)R(M64_DST_OFF_PITCH), (int)sext(R(M64_DST_X) & 0x1FFF, 13), (int)sext(R(M64_DST_Y) & 0x7FFF, 15), (unsigned)R(M64_DST_BRES_LNTH), (unsigned)R(M64_DST_BRES_ERR), (unsigned)R(M64_DST_BRES_INC),
			(unsigned)R(M64_DST_BRES_DEC), (unsigned)R(M64_DST_CNTL), (unsigned)R(M64_DP_SRC), (unsigned)R(M64_DP_MIX), (unsigned)R(M64_DP_FRGD_CLR),
			(unsigned)R(M64_SCALE_3D_CNTL), (unsigned)R(M64_Z_CNTL), (unsigned)(R(M64_TEX_SIZE_PITCH) & 0xFFF), (unsigned)(R(M64_TEX_0_OFF + ((R(M64_TEX_SIZE_PITCH) >> 4) & 0xF)) & 0xFFFFFF),
			(unsigned)R(M64_RED_START), (unsigned)R(M64_RED_X_INC), (unsigned)R(M64_RED_Y_INC), (unsigned)R(M64_GREEN_START), (unsigned)R(M64_BLUE_START), (unsigned)R(M64_ALPHA_START),
			(unsigned)R(M64_S_START), (unsigned)R(M64_S_XINC_START), (unsigned)R(M64_S_Y_INC), (unsigned)R(M64_T_START), (unsigned)R(M64_T_XINC_START), (unsigned)R(M64_T_Y_INC));
		m64snap_log += b;
	}
	StartLine();
	if (A.busy && !A.source_host) RunToEnd();
}

/* The pixels one HOST_DATA write carries, in the order the engine takes
 * them [PRG 2-35]; see 86Box mach64_host_pixels. */
static int HostPixels(uint32_t val, int bits, uint32_t *out, uint8_t *byte_of) {
	int w = (A.source_mix == MONO_HOST) ? 1 : PixBits(A.host_size);
	const bool ltr = (A.op == M64_OP_LINE) || (A.xinc > 0);
	const bool order = ByteOrderLSB();
	const int nbytes = bits >> 3;
	int n = 0;

	if ((R(M64_HOST_CNTL) & 2) && w >= 16) {
		/* HOST_BIG_ENDIAN_EN [RRG 4-104] */
		if (w == 16) val = ((val & 0x00FF00FFu) << 8) | ((val & 0xFF00FF00u) >> 8);
		else val = (val >> 24) | ((val >> 8) & 0xFF00u) | ((val << 8) & 0xFF0000u) | (val << 24);
	}

	if (w >= 8) {
		const int per = w >> 3;
		int cnt = nbytes / per;
		if (!cnt) { cnt = 1; w = bits; }
		for (int i = 0; i < cnt; i++) {
			const int idx = ltr ? i : (cnt - 1 - i);
			out[n] = (w >= 32) ? val : ((val >> (idx * w)) & ((1u << w) - 1));
			byte_of[n] = (uint8_t)(idx * (w >> 3));
			n++;
		}
	}
	else {
		const int per = 8 / w;
		const bool hi = (!order && ltr) || (order && !ltr);
		const uint32_t m = (1u << w) - 1;
		for (int b = 0; b < nbytes; b++) {
			const int bi = ltr ? b : (nbytes - 1 - b);
			const uint8_t byte = (uint8_t)(val >> (bi * 8));
			for (int k = 0; k < per; k++) {
				const int pos = hi ? (per - 1 - k) : k;
				out[n] = (byte >> (pos * w)) & m;
				byte_of[n] = (uint8_t)b;
				n++;
			}
		}
	}
	return n;
}

void Mach64_Accel_HostData(uint32_t val, int bits) {
	if (!A.busy || !A.source_host) return;
	uint32_t pix[32];
	uint8_t byte_of[32];
	const int n = HostPixels(val, bits, pix, byte_of);
	for (int i = 0; i < n && A.busy; i++) {
		if (A.row_ended) {
			A.row_ended = false;
			/* HOST_BYTE_ALIGN: 1 and 4 bpp host data resumes at the next byte after a row [RRG 4-78] */
			if ((R(M64_HOST_CNTL) & 1) && A.op == M64_OP_RECT && i > 0) {
				const int w = (A.source_mix == MONO_HOST) ? 1 : PixBits(A.host_size);
				if (w < 8) {
					while (i < n && byte_of[i] == byte_of[i - 1]) i++;
					if (i >= n) break;
				}
			}
		}
		OpPixel(pix[i]);
	}
}

bool Mach64_Accel_Busy(void) {
	return A.busy;
}

void Mach64_Accel_Reset(void) {
	memset(&A, 0, sizeof(A));
}

/* ---------------------------------------------------------------------- */
/* Contexts [RRG 4-102]                                                     */
/* ---------------------------------------------------------------------- */

static void LoadContext(void) {
	uint32_t cntl = R(M64_CONTEXT_LOAD_CNTL);
	if (cntl & (1u << 31)) return;
	const uint32_t chunks = (uint32_t)(vga.mem.memsize >> 8);
	/* register of each context save area dword [PRG, Draw Engine Contexts] */
	static const uint16_t regs[28] = {
		0, 0, M64_DST_OFF_PITCH, M64_DST_Y_X, M64_DST_HEIGHT_WIDTH, M64_DST_BRES_ERR, M64_DST_BRES_INC,
		M64_DST_BRES_DEC, M64_SRC_OFF_PITCH, M64_SRC_Y_X, M64_SRC_HEIGHT1_WIDTH1, M64_SRC_Y_X_START,
		M64_SRC_HEIGHT2_WIDTH2, M64_PAT_REG0, M64_PAT_REG1, M64_SC_LEFT_RIGHT, M64_SC_TOP_BOTTOM,
		M64_DP_BKGD_CLR, M64_DP_FRGD_CLR, M64_DP_WRITE_MSK, M64_DP_CHAIN_MSK, M64_DP_PIX_WIDTH,
		M64_DP_MIX, M64_DP_SRC, M64_CLR_CMP_CLR, M64_CLR_CMP_MSK, M64_CLR_CMP_CNTL, M64_GUI_TRAJ_CNTL
	};
	unsigned int cmd = (cntl >> 16) & 3;
	for (unsigned int chain = 0; cmd && chain < 1024; chain++) {
		const uint32_t addr = ((chunks - 1 - ((cntl & 0x7FFF) % chunks)) * 256) & (uint32_t)vga.mem.memmask;
		const uint32_t cmask = Mach64_ReadPixel(addr >> 2, SZ_32);
		mach64.reg[M64_CONTEXT_MASK] = cmask;
		for (unsigned int e = 2; e < 28; e++) {
			if (!(cmask & (1u << e))) continue;
			const uint32_t v = Mach64_ReadPixel((addr >> 2) + e, SZ_32);
			if (e == 4) {
				/* stored, not written: a write would start the fill early */
				mach64.reg[M64_DST_HEIGHT] = v & 0x7FFF;
				mach64.reg[M64_DST_WIDTH] = (v >> 16) & 0xFFFF;
				mach64.reg[M64_DST_BRES_LNTH] = (mach64.reg[M64_DST_BRES_LNTH] & ~0x1FFFu) | ((v >> 16) & 0x1FFF);
			}
			else Mach64_Accel_Write(regs[e], v, 0xFFFFFFFFu);
		}
		if (cmd == 2) StartFillOp();
		else if (cmd == 3) StartLineOp();
		cntl = Mach64_ReadPixel((addr >> 2) + 0x1C, SZ_32);
		cmd = (cntl >> 16) & 3;
	}
	mach64.reg[M64_CONTEXT_LOAD_CNTL] = cntl & 0x8003FFFFu;
}

/* ---------------------------------------------------------------------- */
/* Register writes (dword offsets 0x40-0xFF)                                */
/* ---------------------------------------------------------------------- */

static inline void Store(unsigned int reg, uint32_t val, uint32_t mask, uint32_t fieldmask) {
	const uint32_t m = mask & fieldmask;
	mach64.reg[reg] = (mach64.reg[reg] & ~m) | (val & m);
}

void Mach64_Accel_Write(unsigned int reg, uint32_t val, uint32_t mask) {
	val &= mask;
	const bool top = (mask & 0xFF000000u) != 0;   /* initiators fire on the write of their top byte */

	if (reg >= M64_HOST_DATA0 && reg <= M64_HOST_DATA15) {
		int bits = 32;
		if (mask == 0xFFu) bits = 8;
		else if (mask == 0xFFFFu) bits = 16;
		Mach64_Accel_HostData(val >> ((mask & 0xFFFFu) ? 0 : 16), bits);
		return;
	}

	if (reg == M64_DST_X_Y || reg == M64_DST_WIDTH_HEIGHT) {
		/* Rage II: X/width in 15:0, Y/height in 31:16 (not in RRG-G02700; 86Box). The
		 * GT-B driver uses them for host-data blits after DP_SET_GUI_ENGINE. */
		if (!mach64_rage2) return;
		if (reg == M64_DST_X_Y) {
			Store(M64_DST_X, val, mask, 0x1FFF);
			Store(M64_DST_Y, val >> 16, mask >> 16, 0x7FFF);
		}
		else {
			Store(M64_DST_WIDTH, val, mask, 0xFFFF);
			Store(M64_DST_BRES_LNTH, val, mask, 0x1FFF);
			Store(M64_DST_HEIGHT, val >> 16, mask >> 16, 0x7FFF);
			if (top) StartFillOp();
		}
		return;
	}
	if (reg == M64_TEX_PALETTE) {
		/* Rage II CI4/CI8 texture palette, separate from the display DAC. Not in
		 * RRG-G02700; format from 86Box (ATI Windows 95 HAL uploads). */
		extern void Mach64_TexPaletteWrite(uint32_t v);
		Store(reg, val, mask, 0xFFFFFFFFu);
		if (mach64_rage2 && top) Mach64_TexPaletteWrite(R(M64_TEX_PALETTE));
		return;
	}
	if (reg == M64_DP_SET_GUI_ENGINE) {
		/* 3D RAGE II shortcut that loads a set of engine defaults in one write. Not in
		 * RRG-G02700 (3D RAGE I); layout follows 86Box's mach64_recalc_dp_set_engine
		 * (GPL-2.0-or-later). The GT-B driver writes it before each solid fill/blit. */
		if (!mach64_rage2 || !top) return;
		static const unsigned int pitches[16] = { 320, 320, 352, 384, 640, 800, 896, 512, 1024, 1152, 1280, 400, 832, 1600, 448, 2048 };
		const uint32_t v = mach64.reg[M64_DP_SET_GUI_ENGINE] = val;
		const uint32_t F = 0xFFFFFFFFu;
		Store(M64_DST_Y, 0, F, 0x7FFF); Store(M64_DST_X, 0, F, 0x1FFF);
		Store(M64_DST_HEIGHT, 0, F, 0x7FFF); Store(M64_DST_WIDTH, 0, F, 0xFFFF);
		Store(M64_SRC_Y, 0, F, 0x7FFF); Store(M64_SRC_X, 0, F, 0x1FFF);
		Store(M64_SRC_Y_START, 0, F, 0x7FFF); Store(M64_SRC_X_START, 0, F, 0x1FFF);
		Store(M64_SC_TOP, 0, F, 0x7FFF); Store(M64_SC_BOTTOM, 0x3FFF, F, 0x7FFF);
		Store(M64_SC_LEFT, 0, F, 0x1FFF); Store(M64_SC_RIGHT, 0x0FFF, F, 0x1FFF);   /* 86Box: 1FFFh, but the scissors are signed here */
		Store(M64_DP_WRITE_MSK, F, F, F);
		Store(M64_CLR_CMP_CLR, 0, F, F); Store(M64_CLR_CMP_CNTL, 0, F, 0x03000007);   /* colour compare off (ref/86gptbox) */
		Store(M64_SRC_CNTL, R(M64_SRC_CNTL) & ~((3u << 13) | (1u << 5) | (1u << 12)), F, 0x0000609F);
		Store(M64_DST_CNTL, R(M64_DST_CNTL) & ~(7u << 13), F, 0x0002FFFF);
		uint32_t pw = R(M64_DP_PIX_WIDTH) & (1u << 13);
		pw |= (v >> 3) & 7;
		if (v & (1u << 6)) pw |= ((v >> 3) & 7) << 8;
		Store(M64_DP_PIX_WIDTH, pw, F, F);
		uint32_t op = (262144u * ((v >> 7) & 3)) & 0xFFFFF;
		op |= ((pitches[(v >> 10) & 0xF] * ((v & (1u << 14)) ? 2 : 1)) / 8) << 22;
		Store(M64_DST_OFF_PITCH, op, F, 0xFFCFFFFFu);
		Store(M64_SRC_OFF_PITCH, (v & (1u << 15)) ? op : 0, F, 0xFFCFFFFFu);
		static const uint32_t srcsz[4] = { 0x00080008, 0x00200001, 0x00180008, 0 };
		if (((v >> 16) & 3) != 3) {
			const uint32_t hw = srcsz[(v >> 16) & 3];
			Store(M64_SRC_HEIGHT1, hw, F, 0x7FFF); Store(M64_SRC_WIDTH1, hw >> 16, F, 0x1FFF);
			Store(M64_SRC_HEIGHT2, hw, F, 0x7FFF); Store(M64_SRC_WIDTH2, hw >> 16, F, 0x1FFF);
		}
		/* drawing combination: DP_SRC, DP_MIX, GUI_TRAJ_CNTL (0 and 14 unknown) */
		static const uint32_t combo[16][3] = {
			{ 0, 0, 0 }, { 0x100, 0x070003, 0x23 }, { 0x200, 0x70007, 3 }, { 0x20100, 0x70007, 3 },
			{ 0x100, 0x70007, 3 }, { 0x10100, 0x70007, 0x01000003 }, { 0x100, 0x70007, 3 }, { 0x300, 0x70007, 0x30003 },
			{ 0x300, 0x70007, 0 }, { 0x300, 0x70007, 1 }, { 0x300, 0x70007, 2 }, { 0x300, 0x70007, 3 },
			{ 0x20100, 0x70003, 0x1004001B }, { 0x20100, 0x70003, 0x0004001B }, { 0, 0, 0 }, { 0x300, 0x70007, 0x0004001B } };
		const unsigned int c = (v >> 20) & 0xF;
		if (c != 0 && c != 14) {
			Store(M64_DP_SRC, combo[c][0], F, 0x00030707);
			Store(M64_DP_MIX, combo[c][1], F, 0x001F001F);
			Mach64_Accel_Write(M64_GUI_TRAJ_CNTL, combo[c][2], F);
		}
		return;
	}

	switch (reg) {
		case M64_DST_Y_X:
		case M64_DST_Y_X_ALIAS:
			Store(M64_DST_Y, val, mask, 0x7FFF);
			Store(M64_DST_X, val >> 16, mask >> 16, 0x1FFF);
			return;
		case M64_DST_HEIGHT_WIDTH:
			Store(M64_DST_HEIGHT, val, mask, 0x7FFF);
			Store(M64_DST_WIDTH, val >> 16, mask >> 16, 0xFFFF);
			Store(M64_DST_BRES_LNTH, val >> 16, mask >> 16, 0x1FFF);
			if (top) StartFillOp();
			return;
		case M64_DST_X_WIDTH:
			Store(M64_DST_X, val, mask, 0x1FFF);
			Store(M64_DST_WIDTH, val >> 16, mask >> 16, 0xFFFF);
			Store(M64_DST_BRES_LNTH, val >> 16, mask >> 16, 0x1FFF);
			if (top) StartFillOp();
			return;
		case M64_DST_WIDTH:
			/* bit 15 is write only (DRAW_TRAP alias), bit 31 suppresses the fill [RRG 4-54] */
			Store(M64_DST_WIDTH, val, mask, 0x7FFF);
			Store(M64_DST_BRES_LNTH, val, mask, 0x7FFF);
			if (mask & 0x8000u) mach64.reg[M64_DST_BRES_LNTH] = (mach64.reg[M64_DST_BRES_LNTH] & ~0x8000u) | (val & 0x8000u);
			if (top && !(val & 0x80000000u)) {
				if (val & 0x8000u) StartLineOp();
				else StartFillOp();
			}
			return;
		case M64_DST_BRES_LNTH:
		case M64_LEAD_BRES_LNTH: {
			/* [RRG 4-46]: bits 31 and 15 decide what is loaded and started */
			const bool dis = (val & 0x80000000u) != 0;
			const bool trap = (val & 0x8000u) != 0;
			Store(M64_DST_BRES_LNTH, val, mask, 0x7FFF | 0x8000);
			Store(M64_DST_WIDTH, val, mask, 0x7FFF);
			if (!(trap && !dis))    /* TRAIL_X (28:16) is not written for "0 1" */
				Store(M64_DST_HEIGHT, val >> 16, mask >> 16, 0x1FFF);
			if (top && !(dis && !trap)) StartLineOp();
			return;
		}
		case M64_SRC_Y_X:
			Store(M64_SRC_Y, val, mask, 0x7FFF);
			Store(M64_SRC_X, val >> 16, mask >> 16, 0x1FFF);
			return;
		case M64_SRC_HEIGHT1_WIDTH1:
			Store(M64_SRC_HEIGHT1, val, mask, 0x7FFF);
			Store(M64_SRC_WIDTH1, val >> 16, mask >> 16, 0x1FFF);
			return;
		case M64_SRC_Y_X_START:
			Store(M64_SRC_Y_START, val, mask, 0x7FFF);
			Store(M64_SRC_X_START, val >> 16, mask >> 16, 0x1FFF);
			return;
		case M64_SRC_HEIGHT2_WIDTH2:
			Store(M64_SRC_HEIGHT2, val, mask, 0x7FFF);
			Store(M64_SRC_WIDTH2, val >> 16, mask >> 16, 0x1FFF);
			return;
		case M64_SC_LEFT_RIGHT:
			Store(M64_SC_LEFT, val, mask, 0x1FFF);
			Store(M64_SC_RIGHT, val >> 16, mask >> 16, 0x1FFF);
			return;
		case M64_SC_TOP_BOTTOM:
			Store(M64_SC_TOP, val, mask, 0x7FFF);
			Store(M64_SC_BOTTOM, val >> 16, mask >> 16, 0x7FFF);
			return;
		case M64_GUI_TRAJ_CNTL:
			/* composite of DST_CNTL, SRC_CNTL, PAT_CNTL and HOST_CNTL [RRG 4-104] */
			Store(M64_DST_CNTL, val, mask, 0xFFFF);
			Store(M64_SRC_CNTL, val >> 16, mask >> 16, 0x9F);
			Store(M64_PAT_CNTL, val >> 24, mask >> 24, 0x7);
			Store(M64_HOST_CNTL, val >> 28, mask >> 28, 0x3);
			return;
		case M64_CONTEXT_LOAD_CNTL:
			Store(reg, val, mask, 0x8003FFFFu);
			if (top) LoadContext();
			return;
		case M64_FIFO_STAT:
		case M64_GUI_STAT:
			return;
		/* field widths for read-back */
		case M64_DST_OFF_PITCH:
		case M64_SRC_OFF_PITCH:
		case M64_Z_OFF_PITCH:
			Store(reg, val, mask, 0xFFCFFFFFu);
			return;
		case M64_DST_X:    Store(reg, val, mask, 0x1FFF); return;
		case M64_DST_Y:    Store(reg, val, mask, 0x7FFF); return;
		case M64_DST_HEIGHT: Store(reg, val, mask, 0x7FFF); return;
		case M64_DST_BRES_ERR: case M64_DST_BRES_INC: case M64_DST_BRES_DEC:
		case M64_TRAIL_BRES_ERR: case M64_TRAIL_BRES_INC: case M64_TRAIL_BRES_DEC:
			Store(reg, val, mask, 0x3FFFF);
			return;
		case M64_DST_CNTL: Store(reg, val, mask, 0x0002FFFF); return;
		case M64_Z_CNTL:   Store(reg, val, mask, 0x00000173); return;
		case M64_SRC_X: case M64_SRC_WIDTH1: case M64_SRC_X_START: case M64_SRC_WIDTH2:
			Store(reg, val, mask, 0x1FFF);
			return;
		case M64_SRC_Y: case M64_SRC_HEIGHT1: case M64_SRC_Y_START: case M64_SRC_HEIGHT2:
			Store(reg, val, mask, 0x7FFF);
			return;
		case M64_SRC_CNTL: Store(reg, val, mask, 0x0000609F); return;
		case M64_HOST_CNTL: Store(reg, val, mask, 0x3); return;
		case M64_PAT_CNTL: Store(reg, val, mask, 0x7); return;
		case M64_SC_LEFT: case M64_SC_RIGHT: Store(reg, val, mask, 0x1FFF); return;
		case M64_SC_TOP: case M64_SC_BOTTOM: Store(reg, val, mask, 0x7FFF); return;
		case M64_DP_MIX: Store(reg, val, mask, 0x001F001F); return;
		case M64_DP_SRC: Store(reg, val, mask, 0x00030707); return;
		case M64_CLR_CMP_CNTL: Store(reg, val, mask, 0x03000007); return;
		case M64_TEX_SIZE_PITCH: Store(reg, val, mask, 0x00000FFF); return;
		/* scaler registers with two addresses [RRG chapter 3]: one register each */
		case M64_SCALE_Y_PITCH: case M64_S_Y_INC:
			Store(M64_SCALE_Y_PITCH, val, mask, 0xFFFFFFFFu);
			Store(M64_S_Y_INC, val, mask, 0xFFFFFFFFu);
			return;
		case M64_SCALE_X_INC: case M64_RED_X_INC:
			Store(M64_SCALE_X_INC, val, mask, 0xFFFFFFFFu);
			Store(M64_RED_X_INC, val, mask, 0xFFFFFFFFu);
			return;
		case M64_SCALE_Y_INC: case M64_GREEN_X_INC:
			Store(M64_SCALE_Y_INC, val, mask, 0xFFFFFFFFu);
			Store(M64_GREEN_X_INC, val, mask, 0xFFFFFFFFu);
			return;
		default:
			Store(reg, val, mask, 0xFFFFFFFFu);
			return;
	}
}
