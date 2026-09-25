/*
 *  ATI mach64 GT (3D RAGE) emulation: scaler and 3D pipe.
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
 *  Written from RRG-G02700 [RRG] chapter 6 ("3D RAGE Scaler and 3D
 *  Operations Registers"); there is no other implementation of this chip's
 *  3D unit to follow. CPU software rendering, synchronous with the register
 *  write that starts the trapezoid (see vga_mach64_accel.cpp).
 *
 *  Where [RRG] leaves the behaviour open, the choice made is marked
 *  "ASSUMPTION" and collected in ATI-RAGE-NOTES.md section 6.
 */

#include <string.h>
#include <math.h>

#include "dosbox.h"
#include "logging.h"
#include "vga.h"
#include "vga_mach64.h"

static inline uint32_t R(unsigned int reg) { return mach64.reg[reg]; }
static inline int32_t sext(uint32_t v, int bits) { return (int32_t)(v << (32 - bits)) >> (32 - bits); }

extern uint32_t Mach64_ReadPixel(uint32_t addr, int sz);
extern void Mach64_WritePixel(uint32_t addr, int sz, uint32_t v);

/* ---------------------------------------------------------------------- */
/* Interpolator state                                                       */
/* ---------------------------------------------------------------------- */

/* Field positions from the register diagrams [RRG 6-8..6-14]:
 *   RED/GREEN/BLUE/ALPHA START, X_INC, Y_INC: S.8.12 in bits 24:4, so the
 *     integer colour sits at bit 16 of the register image
 *   Z START, X_INC, Y_INC: S.16.12 in bits 28:0 (integer at bit 12)
 *   S/T START: 10.11 in bits 25:5; XINC_START and Y_INC: S.11.16 in 27:0;
 *     X_INC2, Y_INC2, XY_INC2: S.10.16 in 26:0 - all with the binary point
 *     at bit 16 of the register image, 1024.0 being one wrap of the map
 * Everything is kept in these register units (64-bit, sign extended), and is
 * written back when the trapezoid ends. */
struct Interp {
	int64_t v, xinc, yinc;
};

struct TexCoord {
	int64_t v;       /* coordinate on the leading edge */
	int64_t dx;      /* current S_X_INC */
	int64_t dy;      /* current S_Y_INC */
	int64_t dxx, dyy, dxy; /* second derivatives */
};

static struct {
	/* on the leading edge */
	Interp r, g, b, a, z;
	TexCoord s, t;
	/* along the span */
	int64_t sr, sg, sb, sa, sz;
	int64_t srx, sgx, sbx, sax, szx;
	int64_t ss, sdsx, st, sdtx;     /* S/T and their per-pixel steps */
	int64_t sdsy, sdty;             /* S_Y_INC at the pixel, for the level of detail */
	int64_t sdxy_step, tdxy_step;
	int32_t dir;                    /* DST_X_DIR as +1/-1 */
} P;

static inline int64_t sx(unsigned int reg, int bits) { return (int64_t)sext(R(reg), bits); }

void Mach64_3D_TrapBegin(void) {
	P.r.v = sx(M64_RED_START, 25);   P.r.xinc = sx(M64_RED_X_INC, 25);   P.r.yinc = sx(M64_RED_Y_INC, 25);
	P.g.v = sx(M64_GREEN_START, 25); P.g.xinc = sx(M64_GREEN_X_INC, 25); P.g.yinc = sx(M64_GREEN_Y_INC, 25);
	P.b.v = sx(M64_BLUE_START, 25);  P.b.xinc = sx(M64_BLUE_X_INC, 25);  P.b.yinc = sx(M64_BLUE_Y_INC, 25);
	P.a.v = sx(M64_ALPHA_START, 25); P.a.xinc = sx(M64_ALPHA_X_INC, 25); P.a.yinc = sx(M64_ALPHA_Y_INC, 25);
	P.z.v = sx(M64_Z_START, 29);     P.z.xinc = sx(M64_Z_X_INC, 29);     P.z.yinc = sx(M64_Z_Y_INC, 29);

	P.s.v   = sx(M64_S_START, 26);
	P.s.dx  = sx(M64_S_XINC_START, 28);
	P.s.dy  = sx(M64_S_Y_INC, 28);
	P.s.dxx = sx(M64_S_X_INC2, 27);
	P.s.dyy = sx(M64_S_Y_INC2, 27);
	P.s.dxy = sx(M64_S_XY_INC2, 27);
	P.t.v   = sx(M64_T_START, 26);
	P.t.dx  = sx(M64_T_XINC_START, 28);
	P.t.dy  = sx(M64_T_Y_INC, 28);
	P.t.dxx = sx(M64_T_X_INC2, 27);
	P.t.dyy = sx(M64_T_Y_INC2, 27);
	P.t.dxy = sx(M64_T_XY_INC2, 27);
}

/* The trajectory registers are live: store where the next trapezoid of the
 * same triangle continues. */
void Mach64_3D_TrapEnd(void) {
	auto put = [](unsigned int reg, int64_t v, uint32_t fieldmask) {
		mach64.reg[reg] = (uint32_t)v & fieldmask;
	};
	put(M64_RED_START, P.r.v, 0x01FFFFFF);
	put(M64_GREEN_START, P.g.v, 0x01FFFFFF);
	put(M64_BLUE_START, P.b.v, 0x01FFFFFF);
	put(M64_ALPHA_START, P.a.v, 0x01FFFFFF);
	put(M64_Z_START, P.z.v, 0x1FFFFFFF);
	put(M64_S_START, P.s.v, 0x03FFFFFF);
	put(M64_S_XINC_START, P.s.dx, 0x0FFFFFFF);
	put(M64_S_Y_INC, P.s.dy, 0x0FFFFFFF);
	put(M64_SCALE_Y_PITCH, P.s.dy, 0x0FFFFFFF);   /* the same register */
	put(M64_T_START, P.t.v, 0x03FFFFFF);
	put(M64_T_XINC_START, P.t.dx, 0x0FFFFFFF);
	put(M64_T_Y_INC, P.t.dy, 0x0FFFFFFF);
}

/* The leading edge moved one pixel in DST_X_DIR. X_INC2 is the change of
 * S_X_INC per X step; the mixed term also changes S_Y_INC (the quadratic's
 * cross term is shared). */
void Mach64_3D_EdgeStepX(void) {
	P.r.v += P.r.xinc; P.g.v += P.g.xinc; P.b.v += P.b.xinc; P.a.v += P.a.xinc; P.z.v += P.z.xinc;
	P.s.v += P.s.dx; P.s.dx += P.s.dxx; P.s.dy += P.s.dxy;
	P.t.v += P.t.dx; P.t.dx += P.t.dxx; P.t.dy += P.t.dxy;
}

/* The leading edge moved to the next scan line: S_Y_INC2 changes S_Y_INC,
 * S_XY_INC2 changes S_X_INC [RRG 6-8]. */
void Mach64_3D_EdgeStepY(void) {
	P.r.v += P.r.yinc; P.g.v += P.g.yinc; P.b.v += P.b.yinc; P.a.v += P.a.yinc; P.z.v += P.z.yinc;
	P.s.v += P.s.dy; P.s.dy += P.s.dyy; P.s.dx += P.s.dxy;
	P.t.v += P.t.dy; P.t.dy += P.t.dyy; P.t.dx += P.t.dxy;
}

/* S at n X steps from the leading edge, and the step from there to the
 * next pixel in screen order (dir = DST_X_DIR, +1 or -1) */
static inline int64_t QuadAt(int64_t start, int64_t inc, int64_t inc2, int64_t n) {
	return start + n * inc + ((n * (n - 1)) / 2) * inc2;
}
static inline int64_t QuadStep(int64_t inc, int64_t inc2, int64_t n, int32_t dir) {
	return dir > 0 ? inc + n * inc2 : -inc - (n - 1) * inc2;
}

/* Start of a span at the pixel n steps from the leading edge in DST_X_DIR;
 * the span then walks left to right on the screen. */
void Mach64_3D_SpanBegin(int32_t n, int32_t dir) {
	P.dir = dir;
	P.sr = P.r.v + P.r.xinc * n; P.srx = P.r.xinc * dir;
	P.sg = P.g.v + P.g.xinc * n; P.sgx = P.g.xinc * dir;
	P.sb = P.b.v + P.b.xinc * n; P.sbx = P.b.xinc * dir;
	P.sa = P.a.v + P.a.xinc * n; P.sax = P.a.xinc * dir;
	P.sz = P.z.v + P.z.xinc * n; P.szx = P.z.xinc * dir;
	P.ss = QuadAt(P.s.v, P.s.dx, P.s.dxx, n); P.sdsx = QuadStep(P.s.dx, P.s.dxx, n, dir);
	P.st = QuadAt(P.t.v, P.t.dx, P.t.dxx, n); P.sdtx = QuadStep(P.t.dx, P.t.dxx, n, dir);
	P.sdsy = P.s.dy + (int64_t)n * P.s.dxy;
	P.sdty = P.t.dy + (int64_t)n * P.t.dxy;
}

void Mach64_3D_SpanStep(void) {
	P.sr += P.srx; P.sg += P.sgx; P.sb += P.sbx; P.sa += P.sax; P.sz += P.szx;
	P.ss += P.sdsx; P.sdsx += P.s.dxx;
	P.st += P.sdtx; P.sdtx += P.t.dxx;
	P.sdsy += P.s.dxy * P.dir;
	P.sdty += P.t.dxy * P.dir;
}

/* colour interpolator (register units) to 8 bits */
static inline int Clamp8(int64_t v) {
	v >>= 16;
	return v < 0 ? 0 : (v > 255 ? 255 : (int)v);
}

/* ---------------------------------------------------------------------- */
/* Texel formats (DP_SCALE_PIX_WIDTH, DP_PIX_WIDTH 31:28 [RRG 4-90])        */
/* ---------------------------------------------------------------------- */

struct ARGB { int a, r, g, b; };

static int TexelBytes(unsigned int fmt) {
	switch (fmt) {
		case M64_PIX_8BPP: case M64_PIX_RGB332: case M64_PIX_Y8: return 1;
		case M64_PIX_15BPP: case M64_PIX_16BPP: case M64_PIX_ARGB4444: case M64_PIX_YUV422: return 2;
		case M64_PIX_32BPP: case M64_PIX_AYUV444: return 4;
		default: return 2;
	}
}

static inline int ex5(unsigned int v) { return (int)((v << 3) | (v >> 2)); }
static inline int ex6(unsigned int v) { return (int)((v << 2) | (v >> 4)); }
static inline int ex4(unsigned int v) { return (int)((v << 4) | v); }

static ARGB YUV(int y, int u, int v) {
	/* ITU-R BT.601; with APPLE_YUV_MODE UV are signed without offset [RRG 6-5] */
	if (!(R(M64_SCALE_3D_CNTL) & (1u << 10))) { u -= 128; v -= 128; }
	else { u = (int8_t)u; v = (int8_t)v; }
	ARGB c;
	c.a = 255;
	c.r = y + ((359 * v) >> 8);
	c.g = y - ((88 * u + 183 * v) >> 8);
	c.b = y + ((453 * u) >> 8);
	c.r = c.r < 0 ? 0 : c.r > 255 ? 255 : c.r;
	c.g = c.g < 0 ? 0 : c.g > 255 ? 255 : c.g;
	c.b = c.b < 0 ? 0 : c.b > 255 ? 255 : c.b;
	return c;
}

extern bool vga_8bit_dac;

static ARGB FetchTexel(uint32_t base, uint32_t pitch, uint32_t x, uint32_t y, unsigned int fmt) {
	const uint8_t *mem = vga.mem.linear;
	const uint32_t mask = (uint32_t)vga.mem.memmask;
	const uint32_t addr = base + (y * pitch + x) * (uint32_t)TexelBytes(fmt);
	ARGB c; c.a = 255;
	switch (fmt) {
		case M64_PIX_8BPP: {
			/* pseudocolour through the RAMDAC palette [RRG 4-90] */
			const uint8_t i = mem[addr & mask];
			const int sh = vga_8bit_dac ? 0 : 2;
			c.r = (vga.dac.rgb[i].red << sh) & 0xFF;
			c.g = (vga.dac.rgb[i].green << sh) & 0xFF;
			c.b = (vga.dac.rgb[i].blue << sh) & 0xFF;
			if (sh) { c.r |= c.r >> 6; c.g |= c.g >> 6; c.b |= c.b >> 6; }
			break;
		}
		case M64_PIX_RGB332: {
			const uint8_t v = mem[addr & mask];
			c.r = ((v >> 5) & 7) * 255 / 7; c.g = ((v >> 2) & 7) * 255 / 7; c.b = (v & 3) * 255 / 3;
			break;
		}
		case M64_PIX_Y8: {
			const uint8_t v = mem[addr & mask];
			c.r = c.g = c.b = v;
			break;
		}
		case M64_PIX_15BPP: {
			const uint16_t v = (uint16_t)(mem[addr & mask] | (mem[(addr + 1) & mask] << 8));
			c.a = (v & 0x8000) ? 255 : 0;
			c.r = ex5((v >> 10) & 0x1F); c.g = ex5((v >> 5) & 0x1F); c.b = ex5(v & 0x1F);
			break;
		}
		case M64_PIX_16BPP: {
			const uint16_t v = (uint16_t)(mem[addr & mask] | (mem[(addr + 1) & mask] << 8));
			c.r = ex5((v >> 11) & 0x1F); c.g = ex6((v >> 5) & 0x3F); c.b = ex5(v & 0x1F);
			break;
		}
		case M64_PIX_ARGB4444: {
			const uint16_t v = (uint16_t)(mem[addr & mask] | (mem[(addr + 1) & mask] << 8));
			c.a = ex4((v >> 12) & 0xF); c.r = ex4((v >> 8) & 0xF); c.g = ex4((v >> 4) & 0xF); c.b = ex4(v & 0xF);
			break;
		}
		case M64_PIX_32BPP:
			c.b = mem[addr & mask]; c.g = mem[(addr + 1) & mask]; c.r = mem[(addr + 2) & mask]; c.a = mem[(addr + 3) & mask];
			break;
		case M64_PIX_YUV422: {
			/* YUYV: the pixel pair shares U and V */
			const uint32_t pair = base + (y * pitch + (x & ~1u)) * 2u;
			const int y0 = mem[(pair + ((x & 1) ? 2 : 0)) & mask];
			const int u = mem[(pair + 1) & mask], v = mem[(pair + 3) & mask];
			c = YUV(y0, u, v);
			break;
		}
		case M64_PIX_AYUV444: {
			const int v = mem[addr & mask], u = mem[(addr + 1) & mask], yy = mem[(addr + 2) & mask];
			const int a = mem[(addr + 3) & mask];
			c = YUV(yy, u, v);
			c.a = a;
			break;
		}
		default:
			c.r = c.g = c.b = 0;
			break;
	}
	return c;
}

static inline ARGB Lerp(const ARGB &p, const ARGB &q, int f /* 0..256 */) {
	ARGB c;
	c.a = p.a + (((q.a - p.a) * f) >> 8);
	c.r = p.r + (((q.r - p.r) * f) >> 8);
	c.g = p.g + (((q.g - p.g) * f) >> 8);
	c.b = p.b + (((q.b - p.b) * f) >> 8);
	return c;
}

/* ---------------------------------------------------------------------- */
/* Texture sampling                                                         */
/* ---------------------------------------------------------------------- */

static unsigned int TexFormat(void) { return (R(M64_DP_PIX_WIDTH) >> 28) & 0xF; }

/* ASSUMPTION: S and T are in texels of a 1024-texel map (10 integer bits
 * [RRG 6-8]), i.e. one wrap of any map is 1024.0; a map of 2^n texels uses
 * the top n integer bits. */
static const int TEX_COORD_BITS = 10;

static ARGB SampleLevel(unsigned int level, int64_t s16, int64_t t16, bool bilinear) {
	const uint32_t tsp = R(M64_TEX_SIZE_PITCH);
	const unsigned int maxsize = (tsp >> 4) & 0xF;     /* log2 of the largest map */
	unsigned int pitch_log = tsp & 0xF;
	unsigned int height_log = (tsp >> 8) & 0xF;
	if (maxsize > 10) return ARGB{255, 0, 0, 0};
	if (level > maxsize) level = maxsize;
	const unsigned int shrink = maxsize - level;
	unsigned int w_log = maxsize - shrink;
	unsigned int h_log = (height_log > shrink) ? height_log - shrink : 0;
	if (h_log > w_log) h_log = w_log;
	pitch_log = (pitch_log > shrink) ? pitch_log - shrink : 0;
	if (pitch_log < w_log && level == maxsize) pitch_log = w_log;

	const uint32_t base = R(M64_TEX_0_OFF + level) & 0x7FFFFF;
	const uint32_t pitch = 1u << pitch_log;
	const unsigned int fmt = TexFormat();

	/* coordinate in texels of this level, 8 fractional bits */
	const int sh = 16 + TEX_COORD_BITS - (int)w_log - 8;
	const int shv = 16 + TEX_COORD_BITS - (int)w_log - 8;   /* T uses the width scale as well: square coordinate space */
	int64_t u = (sh >= 0) ? (s16 >> sh) : (s16 << -sh);
	int64_t v = (shv >= 0) ? (t16 >> shv) : (t16 << -shv);
	const uint32_t wmask = (1u << w_log) - 1, hmask = (1u << h_log) - 1;

	if (!bilinear) {
		return FetchTexel(base, pitch, (uint32_t)(u >> 8) & wmask, (uint32_t)(v >> 8) & hmask, fmt);
	}
	u -= 128; v -= 128;
	const uint32_t x0 = (uint32_t)(u >> 8) & wmask, y0 = (uint32_t)(v >> 8) & hmask;
	const uint32_t x1 = (x0 + 1) & wmask, y1 = (y0 + 1) & hmask;
	const int fu = (int)(u & 0xFF), fv = (int)(v & 0xFF);
	const ARGB c00 = FetchTexel(base, pitch, x0, y0, fmt), c10 = FetchTexel(base, pitch, x1, y0, fmt);
	const ARGB c01 = FetchTexel(base, pitch, x0, y1, fmt), c11 = FetchTexel(base, pitch, x1, y1, fmt);
	return Lerp(Lerp(c00, c10, fu), Lerp(c01, c11, fu), fv);
}

static ARGB SampleTexture(void) {
	const uint32_t c3 = R(M64_SCALE_3D_CNTL);
	const uint32_t tsp = R(M64_TEX_SIZE_PITCH);
	const unsigned int maxsize = (tsp >> 4) & 0xF;
	const bool bilinear_mag = (c3 & (1u << 25)) != 0;
	const unsigned int blend_fcn = (c3 >> 26) & 3;
	const bool mip = !(c3 & (1u << 24));

	/* level of detail from the texel step per pixel, in texels of the largest map */
	double lod = 0.0;
	if (mip && maxsize > 0) {
		const double scale = (double)(1u << maxsize) / (double)(1u << TEX_COORD_BITS) / 65536.0;
		const double dsx = fabs((double)P.sdsx) * scale, dtx = fabs((double)P.sdtx) * scale;
		const double dsy = fabs((double)P.sdsy) * scale, dty = fabs((double)P.sdty) * scale;
		double rho = dsx > dtx ? dsx : dtx;
		if (dsy > rho) rho = dsy;
		if (dty > rho) rho = dty;
		if (rho > 1.0) lod = log2(rho);
	}

	if (lod <= 0.0) {
		/* magnification: the largest map, 2x2 blend if BILINEAR_TEX_EN [RRG 6-6] */
		return SampleLevel(maxsize, P.ss, P.st, bilinear_mag);
	}

	int ilod = (int)lod;
	if (ilod > (int)maxsize) ilod = (int)maxsize;
	const unsigned int level = maxsize - (unsigned int)ilod;
	switch (blend_fcn) {
		case 1: {  /* blend between the two closest maps */
			const unsigned int l2 = level > 0 ? level - 1 : 0;
			const ARGB a = SampleLevel(level, P.ss, P.st, false);
			const ARGB b = SampleLevel(l2, P.ss, P.st, false);
			return Lerp(a, b, (int)((lod - ilod) * 256.0));
		}
		case 2:  /* 2x2 blend within the nearest map */
			return SampleLevel(level, P.ss, P.st, true);
		case 3:  /* 2x2 blend within the next-to-nearest map */
			return SampleLevel(level > 0 ? level - 1 : 0, P.ss, P.st, true);
		default: /* nearest map */
			return SampleLevel(level, P.ss, P.st, false);
	}
}

/* ---------------------------------------------------------------------- */
/* Destination formats                                                      */
/* ---------------------------------------------------------------------- */

static const int dither4[4][4] = {
	{ 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 }
};

static ARGB UnpackDst(uint32_t v, unsigned int fmt) {
	ARGB c; c.a = 255;
	switch (fmt) {
		case M64_PIX_15BPP: c.a = (v & 0x8000) ? 255 : 0; c.r = ex5((v >> 10) & 0x1F); c.g = ex5((v >> 5) & 0x1F); c.b = ex5(v & 0x1F); break;
		case M64_PIX_16BPP: c.r = ex5((v >> 11) & 0x1F); c.g = ex6((v >> 5) & 0x3F); c.b = ex5(v & 0x1F); break;
		case M64_PIX_ARGB4444: c.a = ex4((v >> 12) & 0xF); c.r = ex4((v >> 8) & 0xF); c.g = ex4((v >> 4) & 0xF); c.b = ex4(v & 0xF); break;
		case M64_PIX_32BPP: c.b = v & 0xFF; c.g = (v >> 8) & 0xFF; c.r = (v >> 16) & 0xFF; c.a = (v >> 24) & 0xFF; break;
		case M64_PIX_RGB332: case M64_PIX_8BPP:
			c.r = ((v >> 5) & 7) * 255 / 7; c.g = ((v >> 2) & 7) * 255 / 7; c.b = (v & 3) * 255 / 3; break;
		case M64_PIX_Y8: c.r = c.g = c.b = (int)(v & 0xFF); break;
		default: c.r = c.g = c.b = 0; break;
	}
	return c;
}

static uint32_t PackDst(ARGB c, unsigned int fmt, int32_t x, int32_t y) {
	const uint32_t c3 = R(M64_SCALE_3D_CNTL);
	const bool dither = (c3 & (1u << 2)) != 0;
	const bool round = (c3 & (1u << 4)) != 0;
	auto q = [&](int v, int bits) -> int {
		/* reduce an 8-bit channel to 'bits' bits: dither, round or truncate [RRG 6-4] */
		const int drop = 8 - bits;
		if (drop <= 0) return v;
		if (dither) v += (dither4[y & 3][x & 3] << drop) >> 4;
		else if (round) v += 1 << (drop - 1);
		if (v > 255) v = 255;
		return v >> drop;
	};
	switch (fmt) {
		case M64_PIX_15BPP: return (uint32_t)((c.a >= 128 ? 0x8000 : 0) | (q(c.r,5) << 10) | (q(c.g,5) << 5) | q(c.b,5));
		case M64_PIX_16BPP: return (uint32_t)((q(c.r,5) << 11) | (q(c.g,6) << 5) | q(c.b,5));
		case M64_PIX_ARGB4444: return (uint32_t)((q(c.a,4) << 12) | (q(c.r,4) << 8) | (q(c.g,4) << 4) | q(c.b,4));
		case M64_PIX_32BPP: return (uint32_t)((c.a << 24) | (c.r << 16) | (c.g << 8) | c.b);
		case M64_PIX_Y8: return (uint32_t)((c.r * 77 + c.g * 150 + c.b * 29) >> 8);
		case M64_PIX_RGB332:
		case M64_PIX_8BPP:
		default: {
			/* 8 bpp: RGB332; RED_DITHER_MAX limits red to 0-223 of the LUT [RRG 6-5] */
			int r = q(c.r, 3);
			if ((c3 & (1u << 14)) && r > 6) r = 6;
			return (uint32_t)((r << 5) | (q(c.g,3) << 2) | q(c.b,2));
		}
	}
}

/* ---------------------------------------------------------------------- */
/* Z buffer [RRG 4-60, 4-61]                                                */
/* ---------------------------------------------------------------------- */

/* ASSUMPTION: 16-bit Z (the integer part of the S.16.12 interpolator,
 * clamped), as on the 3D RAGE II; the GT-A2 guide gives no format. */
static bool ZTest(int32_t x, int32_t y, uint32_t &zaddr, uint16_t &znew) {
	const uint32_t zc = R(M64_Z_CNTL);
	int64_t zi = P.sz >> 12;
	if (zi < 0) zi = 0;
	if (zi > 0xFFFF) zi = 0xFFFF;
	znew = (uint16_t)zi;
	const uint32_t zop = R(M64_Z_OFF_PITCH);
	const uint32_t zpitch = ((zop >> 22) & 0x3FF) << 3;
	zaddr = ((zop & 0xFFFFF) << 2) + (uint32_t)(y * (int32_t)zpitch + x);   /* in 16-bit units */
	if (!(zc & 1)) return true;
	const uint16_t zold = (uint16_t)Mach64_ReadPixel(zaddr, 3 /* 16-bit */);
	switch ((zc >> 4) & 7) {
		case 0: return false;
		case 1: return znew < zold;
		case 2: return znew <= zold;
		case 3: return znew == zold;
		case 4: return znew >= zold;
		case 5: return znew > zold;
		case 6: return znew != zold;
		default: return true;
	}
}

/* ---------------------------------------------------------------------- */
/* Per pixel                                                                */
/* ---------------------------------------------------------------------- */

static int BlendFactor(unsigned int sel, int as, int other, bool src_side) {
	/* ALPHA_BLND_SRC / ALPHA_BLND_DST [RRG 6-5]; 'other' is the colour
	 * channel of the other operand (destination for SRC, source for DST) */
	(void)src_side;
	switch (sel) {
		case 0: return 0;
		case 1: return 256;
		case 2: return other + (other >> 7);
		case 3: return 256 - (other + (other >> 7));
		case 4: return as + (as >> 7);
		case 5: return 256 - (as + (as >> 7));
		default: return 256;
	}
}

bool Mach64_3D_Pixel(int32_t x, int32_t y, uint32_t /*dst_addr*/, int /*dst_size*/, uint32_t dst, uint32_t &out) {
	const uint32_t c3 = R(M64_SCALE_3D_CNTL);
	const unsigned int fcn = (c3 >> 6) & 3;
	const unsigned int dfmt = R(M64_DP_PIX_WIDTH) & 0xF;

	uint32_t zaddr = 0; uint16_t znew = 0;
	if (!ZTest(x, y, zaddr, znew)) return false;

	ARGB ic;   /* interpolated colour */
	ic.r = Clamp8(P.sr); ic.g = Clamp8(P.sg); ic.b = Clamp8(P.sb); ic.a = Clamp8(P.sa);

	ARGB c = ic;
	int as = ic.a;
	if (fcn == 2) {
		ARGB t = SampleTexture();
		if (c3 & (1u << 30)) {           /* TEX_MAP_AEN: alphas in the texture */
			if (c3 & (1u << 28)) {       /* TEX_AMASK_AEN: only bit 0 of the alpha */
				const bool bit = (t.a & 0x80) != 0 || (t.a & 1) != 0;
				if (!(c3 & (1u << 29))) {     /* 3D-DDI mode: 0 = not drawn, 1 = interpolator alpha */
					if (!bit) return false;
					t.a = ic.a;
				}
				else t.a = bit ? 255 : ic.a;  /* blended edge mode */
			}
		}
		else t.a = 255;

		/* colour key on the texel (CLR_CMP_SRC 2 [RRG 4-100]) */
		const uint32_t ccc = R(M64_CLR_CMP_CNTL);
		if (((ccc >> 24) & 3) == 2 && ((ccc & 7) >= 4 || (ccc & 7) == 1)) {
			/* ASSUMPTION: the key is compared against the texel as 8:8:8 RGB */
			const uint32_t msk = R(M64_CLR_CMP_MSK);
			const uint32_t key = R(M64_CLR_CMP_CLR) & msk;
			const uint32_t tv = (uint32_t)((t.r << 16) | (t.g << 8) | t.b) & msk;
			const bool eq = tv == key;
			if ((ccc & 7) == 1 || ((ccc & 7) == 5 && eq) || ((ccc & 7) == 4 && !eq)) return false;
		}

		ARGB tc = t;
		if (c3 & (1u << 13)) { tc.r = ic.r; tc.g = ic.g; tc.b = ic.b; }   /* COLOR_OVERRIDE */
		switch ((c3 >> 22) & 3) {
			case 1:   /* modulate */
				c.r = (tc.r * (ic.r + 1)) >> 8; c.g = (tc.g * (ic.g + 1)) >> 8; c.b = (tc.b * (ic.b + 1)) >> 8;
				break;
			case 2: { /* alpha decal */
				const int ta = t.a + (t.a >> 7);
				c.r = (tc.r * ta + ic.r * (256 - ta)) >> 8;
				c.g = (tc.g * ta + ic.g * (256 - ta)) >> 8;
				c.b = (tc.b * ta + ic.b * (256 - ta)) >> 8;
				break;
			}
			default:  /* no lighting */
				c.r = tc.r; c.g = tc.g; c.b = tc.b;
				break;
		}
		/* the alpha source: texel alpha, or the interpolator for alpha decal with blending */
		as = ((c3 >> 22) & 3) == 2 ? ic.a : t.a;
		if (!(c3 & (1u << 30))) as = ic.a;
	}
	c.a = as;

	/* ALPHA_FOG_EN [RRG 6-4]: 1 = blend with the destination; 2 = fog, the
	 * fog colour (DP_FOG_CLR) "in place of the destination". The fog factor is
	 * the ALPHA/FOG interpolator: out = src * f + fog * (1 - f), as the ATI
	 * Direct3D HAL's vertex fog needs (86GPTBox part3, af == 2). */
	const unsigned int af = (c3 >> 11) & 3;
	if (af == 2) {
		const uint32_t fc = R(M64_DP_FRGD_CLR);
		const int f = ic.a + (ic.a >> 7);
		c.r = (c.r * f + (int)((fc >> 16) & 0xFF) * (256 - f)) >> 8;
		c.g = (c.g * f + (int)((fc >> 8) & 0xFF) * (256 - f)) >> 8;
		c.b = (c.b * f + (int)(fc & 0xFF) * (256 - f)) >> 8;
	}
	else if (af == 1) {
		const ARGB d = UnpackDst(dst, dfmt);
		const unsigned int fs = (c3 >> 16) & 7, fd = (c3 >> 19) & 7;
		int r = (c.r * BlendFactor(fs, as, d.r, true) + d.r * BlendFactor(fd, as, c.r, false)) >> 8;
		int g = (c.g * BlendFactor(fs, as, d.g, true) + d.g * BlendFactor(fd, as, c.g, false)) >> 8;
		int b = (c.b * BlendFactor(fs, as, d.b, true) + d.b * BlendFactor(fd, as, c.b, false)) >> 8;
		c.r = r > 255 ? 255 : r; c.g = g > 255 ? 255 : g; c.b = b > 255 ? 255 : b;
	}

	out = PackDst(c, dfmt, x, y);

	/* Z write: Z_MASK [RRG 4-61] */
	if ((R(M64_Z_CNTL) & 1) && (R(M64_Z_CNTL) & 0x100))
		Mach64_WritePixel(zaddr, 3, znew);
	return true;
}

/* ---------------------------------------------------------------------- */
/* Scaler (SCALE_3D_FCN = 1) [RRG 6-1..6-7]                                 */
/* ---------------------------------------------------------------------- */

static struct {
	int64_t hacc0, vacc0;   /* accumulators at the start, integer at bit 16 */
	int64_t xinc, yinc;
} S;

void Mach64_Scale_Begin(void) {
	/* SCALE_HACC/VACC: sign, 4.12 in bits 20:4; SCALE_X/Y_INC: 4.12 unsigned in
	 * bits 19:4 [RRG 6-2..6-7]; so the integer part is at bit 16 */
	S.hacc0 = sext(R(M64_RED_START) & 0x001FFFF0u, 21);
	S.vacc0 = sext(R(M64_SCALE_VACC) & 0x001FFFF0u, 21);
	S.xinc = R(M64_SCALE_X_INC) & 0x000FFFF0u;
	S.yinc = R(M64_SCALE_Y_INC) & 0x000FFFF0u;
}

/* col/row: destination pixel within the rectangle, counted from its start */
bool Mach64_Scale_Pixel(int32_t col, int32_t row, uint32_t dst, uint32_t &out) {
	const uint32_t c3 = R(M64_SCALE_3D_CNTL);
	const unsigned int fmt = TexFormat();
	const uint32_t base = R(M64_TEX_0_OFF) & 0x7FFFF8;             /* SCALE_Y_OFF */
	const uint32_t pitch = R(M64_SCALE_Y_PITCH) & 0xFFF;
	const int32_t w = (int32_t)(R(M64_TEX_7_OFF) & 0xFFF);         /* SCALE_WIDTH */
	const int32_t h = (int32_t)(R(M64_TEX_8_OFF) & 0xFFF);         /* SCALE_HEIGHT */
	if (w <= 0 || h <= 0 || pitch == 0) return false;

	int64_t hx = S.hacc0 + (int64_t)(col < 0 ? -col : col) * S.xinc;
	int64_t vy = S.vacc0 + (int64_t)(row < 0 ? -row : row) * S.yinc;
	if (hx < 0) hx = 0;
	if (vy < 0) vy = 0;
	int32_t x0 = (int32_t)(hx >> 16), y0 = (int32_t)(vy >> 16);
	if (x0 >= w) x0 = w - 1;
	if (y0 >= h) y0 = h - 1;

	ARGB c;
	if (c3 & (1u << 8)) {   /* SCALE_PIX_REP: replicate */
		c = FetchTexel(base, pitch, (uint32_t)x0, (uint32_t)y0, fmt);
	}
	else {
		/* 2-tap 5-bit coefficient linear blend in each direction [RRG 6-1] */
		const int32_t x1 = x0 + 1 < w ? x0 + 1 : x0, y1 = y0 + 1 < h ? y0 + 1 : y0;
		const int fx = (int)((hx >> 11) & 0x1F) << 3, fy = (int)((vy >> 11) & 0x1F) << 3;
		const ARGB a = FetchTexel(base, pitch, (uint32_t)x0, (uint32_t)y0, fmt);
		const ARGB b = FetchTexel(base, pitch, (uint32_t)x1, (uint32_t)y0, fmt);
		const ARGB cc = FetchTexel(base, pitch, (uint32_t)x0, (uint32_t)y1, fmt);
		const ARGB d = FetchTexel(base, pitch, (uint32_t)x1, (uint32_t)y1, fmt);
		c = Lerp(Lerp(a, b, fx), Lerp(cc, d, fx), fy);
	}

	const unsigned int dfmt = R(M64_DP_PIX_WIDTH) & 0xF;
	if (((c3 >> 11) & 3) == 1) {
		const ARGB dd = UnpackDst(dst, dfmt);
		const int as = c.a;
		const unsigned int fs = (c3 >> 16) & 7, fd = (c3 >> 19) & 7;
		int r = (c.r * BlendFactor(fs, as, dd.r, true) + dd.r * BlendFactor(fd, as, c.r, false)) >> 8;
		int g = (c.g * BlendFactor(fs, as, dd.g, true) + dd.g * BlendFactor(fd, as, c.g, false)) >> 8;
		int b = (c.b * BlendFactor(fs, as, dd.b, true) + dd.b * BlendFactor(fd, as, c.b, false)) >> 8;
		c.r = r > 255 ? 255 : r; c.g = g > 255 ? 255 : g; c.b = b > 255 ? 255 : b;
	}
	out = PackDst(c, dfmt, col, row);
	return true;
}

bool Mach64_3D_Enabled(void) {
	return ((R(M64_SCALE_3D_CNTL) >> 6) & 3) >= 2;
}
