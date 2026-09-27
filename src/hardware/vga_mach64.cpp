/*
 *  ATI mach64 GT (3D RAGE) emulation: register file, I/O and memory
 *  decoding, accelerator CRTC, PLL, DAC, hardware cursor, PCI.
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
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 *  Primary reference: ATI mach64 Register Reference Guide, ATI-264VT and
 *  3D RAGE, RRG-G02700 rev 0.10 (1996), cited as [RRG] with page numbers.
 *  Structure and a number of register details follow 86Box's
 *  vid_ati_mach64.c (Sarah Walker, Miran Grca, Connor Hyde; GPL-2.0 or
 *  later), adapted to DOSBox-X and to the GT.
 */

#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "dosbox.h"
#include "logging.h"
#include "setup.h"
#include "inout.h"
#include "mem.h"
#include "paging.h"
#include "pic.h"
#include "vga.h"
#include "render.h"
#include "vga_mach64.h"

Mach64State mach64;

extern bool vga_8bit_dac;
extern uint32_t S3_LFB_BASE;
extern void VGA_DAC_UpdateColorPalette();
extern void write_p3c6(Bitu port,Bitu val,Bitu iolen);
extern void write_p3c7(Bitu port,Bitu val,Bitu iolen);
extern void write_p3c8(Bitu port,Bitu val,Bitu iolen);
extern void write_p3c9(Bitu port,Bitu val,Bitu iolen);
extern Bitu read_p3c6(Bitu port,Bitu iolen);
extern Bitu read_p3c7(Bitu port,Bitu iolen);
extern Bitu read_p3c8(Bitu port,Bitu iolen);
extern Bitu read_p3c9(Bitu port,Bitu iolen);
extern void VGA_StartUpdateLFB(void);
extern void VGA_SetupHandlers(void);
extern void VGA_DetermineMode_StandardVGA(void);

static const uint32_t M64_CHIP_ID_GT_A2 = 0x40004754;  /* ASIC ID 40h (NEC GT-A2), type 'GT' [RRG 4-17] */
static const uint32_t M64_CHIP_ID_GT_B  = 0x9A004754;  /* rev 9Ah (3D RAGE II, GT-B2U1); BIOS ASIC ID 479Ah, listed by macxw4.drv 4.35 as 3D-capable */
static uint32_t Mach64_ChipID(void) { return mach64_rage2 ? M64_CHIP_ID_GT_B : M64_CHIP_ID_GT_A2; }
static const double   M64_XTAL = 14318180.0;           /* XTALIN, PLL reference [RRG B-5] */

bool Mach64_Present(void) {
	return IS_VGA_ARCH && svgaCard == SVGA_ATI && atiCard == ATI_Mach64GT;
}

/* ---------------------------------------------------------------------- */
/* PLL [RRG appendix B]                                                     */
/* ---------------------------------------------------------------------- */

enum {
	PLL_MACRO_CNTL = 1,
	PLL_REF_DIV    = 2,
	PLL_GEN_CNTL   = 3,
	PLL_MCLK_FB_DIV = 4,
	PLL_VCLK_CNTL  = 5,
	PLL_VCLK_POST_DIV = 6,
	PLL_VCLK0_FB_DIV = 7,
	PLL_XCLK_CNTL  = 11,
	PLL_TEST_CNTL  = 14,
	PLL_TEST_COUNT = 15
};

static void Mach64_PLL_Reset(void) {
	/* power-up defaults [RRG B-2, B-3] */
	static const uint8_t defaults[16] = {
		0x00, 0xd4, 0x36, 0x4f, 0x97, 0x04, 0x6a, 0xbe,
		0xd6, 0xee, 0x88, 0x00, 0x41, 0x00, 0x00, 0x00
	};
	memcpy(mach64.pll, defaults, sizeof(defaults));
}

/* Pixel clock of VCLK entry n: 2 * R * N / (M * P) [RRG B-6] */
static Bitu Mach64_PLL_VCLK(unsigned int n) {
	const uint8_t vclk_cntl = mach64.pll[PLL_VCLK_CNTL];
	switch (vclk_cntl & 3) {
		case 0: return 33333333;               /* CPUCLK (PCI bus clock) */
		case 1: return 25175000;               /* DCLK: feature connector, nothing drives it */
		case 2: return (Bitu)M64_XTAL;
		default: break;
	}
	const unsigned int M = mach64.pll[PLL_REF_DIV];
	const unsigned int N = mach64.pll[PLL_VCLK0_FB_DIV + (n & 3)];
	const unsigned int P = 1u << ((mach64.pll[PLL_VCLK_POST_DIV] >> ((n & 3) * 2)) & 3);
	if (M < 2 || N < 2) return 25175000;
	return (Bitu)((2.0 * M64_XTAL * N) / (M * P));
}

/* ---------------------------------------------------------------------- */
/* Register read-back masks for the setup/control and CRTC registers        */
/* ---------------------------------------------------------------------- */

static uint32_t Mach64_Block0Mask(unsigned int reg) {
	switch (reg) {
		case M64_CRTC_H_TOTAL_DISP:     return 0x00FF01FF;
		case M64_CRTC_H_SYNC_STRT_WID:  return 0x003F17FF;
		case M64_CRTC_V_TOTAL_DISP:     return 0x07FF07FF;
		case M64_CRTC_V_SYNC_STRT_WID:  return 0x003F07FF;
		case M64_CRTC_VLINE_CRNT_VLINE: return 0x000007FF;
		case M64_CRTC_OFF_PITCH:        return 0xFFCFFFFF;
		case M64_CRTC_GEN_CNTL:         return 0xFFFFCFDF;
		case 0x08: case 0x09:           return 0xFFFFFFFF; /* DSP_CONFIG/DSP_ON_OFF on later chips; kept */
		case M64_OVR_CLR:               return 0xFFFFFFFF;
		case M64_OVR_WID_LEFT_RIGHT:    return 0x000F000F;
		case M64_OVR_WID_TOP_BOTTOM:    return 0x00FF00FF;
		case M64_CUR_CLR0:
		case M64_CUR_CLR1:              return 0xFFFFFFFF;
		case M64_CUR_OFFSET:            return 0x000FFFFF;
		case M64_CUR_HORZ_VERT_POSN:    return 0x07FF07FF;
		case M64_CUR_HORZ_VERT_OFF:     return 0x003F003F;
		case M64_GP_IO:                 return 0xFFFFFFFF;
		case M64_GP_IO_CNTL:            return 0x8000000F;
		case M64_SCRATCH_REG0:
		case M64_SCRATCH_REG1:          return 0xFFFFFFFF;
		case M64_CLOCK_CNTL:            return 0x00003E03;
		case M64_BUS_CNTL:              return 0xF85FDFFF; /* 21 and 23 are interrupt status */
		case M64_MEM_CNTL:              return 0x1F7FFFFF;
		case M64_MEM_VGA_WP_SEL:
		case M64_MEM_VGA_RP_SEL:        return 0x00FF00FF;
		case M64_DAC_CNTL:              return 0xBF00E10C;
		case M64_GEN_TEST_CNTL:         return 0x3FFF31B7;
		case M64_CONFIG_CNTL:           return 0x00080004; /* CFG_VGA_DIS, CFG_MEM_VGA_AP_EN */
		case M64_CONFIG_STAT0:          return 0x0000FFFF;
		default:                        return 0;
	}
}

/* Block 1 read-back masks [RRG chapter 5]; widths as in 86Box, taken from
 * ATI's M64DIAG register tables for the VT-A4. */
static uint32_t Mach64_Block1Mask(unsigned int reg) {
	switch (reg - 0x100) {
		case 0x00: case 0x01:           return 0x87FF07FF;
		case 0x02: case 0x03:
		case 0x04: case 0x05:           return 0x00FFFFFF;
		case 0x06:                      return 0x80000F77;
		case 0x08:                      return 0xFFFFFFFF;
		case 0x09:                      return 0xE000007F;
		case 0x0A:                      return 0x03FF03FF;
		case 0x0B:                      return 0x000000F2;
		case 0x0C:                      return 0x03FF0000;
		case 0x10: case 0x11:           return 0x03FF03FF;
		case 0x12:                      return 0xF00F000F;
		case 0x13:                      return 0x0000FF5F;
		case 0x14:                      return 0x000F00BF;
		case 0x15:                      return 0x80000000;
		case 0x16:                      return 0x00010F03;
		case 0x17:                      return 0x113F1FFF;
		case 0x18:                      return 0x033600FF;
		case 0x1A:                      return 0x0C0003FF;
		case 0x1B: case 0x1C:
		case 0x1D: case 0x1E:           return 0xFFFFFFFF;
		case 0x20: case 0x26:
		case 0x2B: case 0x2C:           return 0x003FFFF8;
		case 0x23: case 0x29:           return 0x00000FFE;
		case 0x50:                      return 0x0000FFFF;
		default:                        return 0;
	}
}

/* ---------------------------------------------------------------------- */
/* Display timing and state                                                 */
/* ---------------------------------------------------------------------- */

static inline uint32_t R(unsigned int reg) { return mach64.reg[reg]; }

bool Mach64_ExtDisplayActive(void) {
	if (!Mach64_Present()) return false;
	return (R(M64_CRTC_GEN_CNTL) & (M64_CRTC_EXT_DISP_EN|M64_CRTC_ENABLE)) == (M64_CRTC_EXT_DISP_EN|M64_CRTC_ENABLE);
}

static unsigned int Mach64_CRTCPixWidth(void) {
	return (R(M64_CRTC_GEN_CNTL) >> 8) & 7;
}

static unsigned int Mach64_BytesPerPixel(unsigned int pw) {
	switch (pw) {
		case M64_PIX_15BPP: case M64_PIX_16BPP: return 2;
		case M64_PIX_24BPP: return 3;
		case M64_PIX_32BPP: return 4;
		default: return 1;
	}
}

Bitu Mach64_DisplayStart(void) {
	return (Bitu)(R(M64_CRTC_OFF_PITCH) & 0xFFFFF) * 8u;
}

bool Mach64_GetDisplay(Mach64_Display &d) {
	if (!Mach64_ExtDisplayActive()) return false;

	const uint32_t htd = R(M64_CRTC_H_TOTAL_DISP);
	const uint32_t hsw = R(M64_CRTC_H_SYNC_STRT_WID);
	const uint32_t vtd = R(M64_CRTC_V_TOTAL_DISP);
	const uint32_t vsw = R(M64_CRTC_V_SYNC_STRT_WID);
	const uint32_t gen = R(M64_CRTC_GEN_CNTL);

	/* horizontal values are in characters of 8 pixels [RRG 4-20] */
	d.htotal  = ((htd & 0x1FF) + 1u) * 8u;
	d.hdend   = (((htd >> 16) & 0xFF) + 1u) * 8u;
	d.hrstart = (((hsw & 0xFF) | ((hsw >> 4) & 0x100)) + 1u) * 8u + ((hsw >> 8) & 7);
	d.hrend   = d.hrstart + (((hsw >> 16) & 0x1F) ? ((hsw >> 16) & 0x1F) : 1u) * 8u;
	d.hbstart = d.hdend;
	d.hbend   = d.htotal;

	/* vertical values in lines */
	d.vtotal  = (vtd & 0x7FF) + 1u;
	d.vdend   = ((vtd >> 16) & 0x7FF) + 1u;
	d.vrstart = (vsw & 0x7FF) + 1u;
	d.vrend   = d.vrstart + (((vsw >> 16) & 0x1F) ? ((vsw >> 16) & 0x1F) : 1u);
	d.vbstart = d.vdend;
	d.vbend   = d.vtotal;

	/* sanitize: bad register values must not break the scanout */
	if (d.htotal < d.hdend + 8) d.htotal = d.hdend + 8;
	if (d.vtotal < d.vdend + 1) d.vtotal = d.vdend + 1;
	if (d.hrstart < d.hdend || d.hrstart >= d.htotal) { d.hrstart = d.hdend; d.hrend = d.hdend + 8; }
	if (d.hrend > d.htotal) d.hrend = d.htotal;
	if (d.vrstart < d.vdend || d.vrstart >= d.vtotal) { d.vrstart = d.vdend; d.vrend = d.vdend + 1; }
	if (d.vrend > d.vtotal) d.vrend = d.vtotal;

	d.doublescan = (gen & M64_CRTC_DBL_SCAN_EN) != 0;
	d.interlace  = (gen & M64_CRTC_INTERLACE_EN) != 0;

	d.pixclock = Mach64_PLL_VCLK(R(M64_CLOCK_CNTL) & 3);
	if (d.pixclock < 1000000) d.pixclock = 25175000;

	const unsigned int pw = Mach64_CRTCPixWidth();
	const Bitu pitch_pixels = (Bitu)((R(M64_CRTC_OFF_PITCH) >> 22) & 0x3FF) * 8u;
	if (pw == M64_PIX_4BPP)
		d.pitch_bytes = pitch_pixels / 2u;
	else
		d.pitch_bytes = pitch_pixels * Mach64_BytesPerPixel(pw);
	d.start_bytes = Mach64_DisplayStart();
	return true;
}

/* ---------------------------------------------------------------------- */
/* Scanline conversion with hardware cursor                                 */
/* ---------------------------------------------------------------------- */

extern uint32_t GFX_Rmask; extern unsigned char GFX_Rshift;
extern uint32_t GFX_Gmask; extern unsigned char GFX_Gshift;
extern uint32_t GFX_Bmask; extern unsigned char GFX_Bshift;
extern uint32_t GFX_Amask;

static inline uint32_t Mach64_HostRGB(unsigned int r, unsigned int g, unsigned int b) {
	return ((uint32_t)r << GFX_Rshift) | ((uint32_t)g << GFX_Gshift) | ((uint32_t)b << GFX_Bshift) | GFX_Amask;
}

/* Converts one displayed line at vidstart (byte offset into video memory)
 * into 32-bit host pixels and draws the hardware cursor over it. 'line' is
 * the display line (0 = top). */
uint8_t *Mach64_DrawLine(Bitu vidstart, Bitu line, uint8_t *templine) {
	uint32_t *out = (uint32_t*)templine;
	const Bitu width = vga.draw.width;
	const uint8_t *mem = vga.mem.linear;
	const Bitu mask = vga.mem.memmask;
	const unsigned int pw = Mach64_CRTCPixWidth();

	if (R(M64_CRTC_GEN_CNTL) & M64_CRTC_DISPLAY_DIS) {
		const uint32_t black = Mach64_HostRGB(0,0,0);
		for (Bitu x = 0; x < width; x++) out[x] = black;
		return templine;
	}

	switch (pw) {
		case M64_PIX_4BPP: {
			const bool lo_first = (R(M64_CRTC_GEN_CNTL) & (1u << 11)) != 0;
			for (Bitu x = 0; x < width; x += 2) {
				const uint8_t v = mem[(vidstart + (x >> 1)) & mask];
				const uint8_t a = lo_first ? (v & 0xF) : (v >> 4);
				const uint8_t b = lo_first ? (v >> 4) : (v & 0xF);
				out[x] = vga.dac.xlat32[a & vga.dac.pel_mask];
				if (x + 1 < width) out[x+1] = vga.dac.xlat32[b & vga.dac.pel_mask];
			}
			break;
		}
		case M64_PIX_15BPP:
			for (Bitu x = 0; x < width; x++) {
				const Bitu a = (vidstart + x * 2) & mask;
				const uint16_t v = (uint16_t)(mem[a] | (mem[(a + 1) & mask] << 8));
				const unsigned int r = (v >> 10) & 0x1F, g = (v >> 5) & 0x1F, b = v & 0x1F;
				out[x] = Mach64_HostRGB((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2));
			}
			break;
		case M64_PIX_16BPP:
			for (Bitu x = 0; x < width; x++) {
				const Bitu a = (vidstart + x * 2) & mask;
				const uint16_t v = (uint16_t)(mem[a] | (mem[(a + 1) & mask] << 8));
				const unsigned int r = (v >> 11) & 0x1F, g = (v >> 5) & 0x3F, b = v & 0x1F;
				out[x] = Mach64_HostRGB((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2));
			}
			break;
		case M64_PIX_24BPP:
			for (Bitu x = 0; x < width; x++) {
				const Bitu a = vidstart + x * 3;
				out[x] = Mach64_HostRGB(mem[(a + 2) & mask], mem[(a + 1) & mask], mem[a & mask]);
			}
			break;
		case M64_PIX_32BPP:
			for (Bitu x = 0; x < width; x++) {
				const Bitu a = vidstart + x * 4;
				out[x] = Mach64_HostRGB(mem[(a + 2) & mask], mem[(a + 1) & mask], mem[a & mask]);
			}
			break;
		case M64_PIX_8BPP:
		default:
			for (Bitu x = 0; x < width; x++)
				out[x] = vga.dac.xlat32[mem[(vidstart + x) & mask] & vga.dac.pel_mask];
			break;
	}

	/* Hardware cursor [RRG 4-33]: 64x64, 2 bits per pixel, 16 bytes per
	 * row, "reverse pixel order within each byte" (pixel 0 in bits 1:0).
	 * 00 = colour 0, 01 = colour 1, 10 = transparent, 11 = invert.
	 * CUR_HORZ_OFF skips the first pixels of each row; CUR_VERT_OFF only
	 * shortens the cursor to 64 - VERT_OFF rows from CUR_OFFSET (clipping at
	 * the top is done by moving CUR_OFFSET, as atyfb does; ATI's Windows 9x
	 * driver keeps its 32x32 pointer in rows 0-31, columns 32-63, with both
	 * offsets 32). */
	if (R(M64_GEN_TEST_CNTL) & 0x80) {
		const uint32_t posn = R(M64_CUR_HORZ_VERT_POSN);
		const uint32_t off  = R(M64_CUR_HORZ_VERT_OFF);
		const int cx = (int)(posn & 0x7FF), cy = (int)((posn >> 16) & 0x7FF);
		const int xoff = (int)(off & 0x3F), yoff = (int)((off >> 16) & 0x3F);
		const int row = (int)line - cy;
		if (row >= 0 && row < 64 - yoff) {
			const Bitu base = (Bitu)(R(M64_CUR_OFFSET) & 0xFFFFF) * 8u + (Bitu)row * 16u;
			const uint32_t c0 = R(M64_CUR_CLR0), c1 = R(M64_CUR_CLR1);
			const uint32_t col0 = Mach64_HostRGB((c0 >> 24) & 0xFF, (c0 >> 16) & 0xFF, (c0 >> 8) & 0xFF);
			const uint32_t col1 = Mach64_HostRGB((c1 >> 24) & 0xFF, (c1 >> 16) & 0xFF, (c1 >> 8) & 0xFF);
			const uint32_t invmask = GFX_Rmask | GFX_Gmask | GFX_Bmask;
			for (int px = xoff; px < 64; px++) {
				const int sx = cx + px - xoff;
				if (sx < 0) continue;
				if (sx >= (int)width) break;
				const uint8_t b = mem[(base + (Bitu)(px >> 2)) & mask];
				switch ((b >> ((px & 3) * 2)) & 3) {
					case 0: out[sx] = col0; break;
					case 1: out[sx] = col1; break;
					case 3: out[sx] ^= invmask; break;
					default: break;
				}
			}
		}
	}
	return templine;
}

/* TEST HOOK (not for upstream): DOSBOX_X_AUTOSHOT="ms,ms,..." takes DOSBox-X
 * screenshots at those emulated times, for automated testing of the scanout. */
/* TEST HOOK (not for upstream): DOSBOX_X_EIPSAMPLE=1 logs CS:EIP and the code bytes every 30 frames */
#include "regs.h"
#include "cpu.h"
static void Mach64_EIPSample(void) {
	static int enabled = -1;
	static unsigned int frame = 0;
	if (enabled < 0) enabled = getenv("DOSBOX_X_EIPSAMPLE") ? 1 : 0;
	if (!enabled || (++frame % 30) != 0) return;
	/* DOSBOX_X_MEMDUMP=start,len,file: dump guest linear memory once, 60 s in */
	static bool dumped = false;
	const char *md = getenv("DOSBOX_X_MEMDUMP");
	static unsigned int inloop = 0;
	if (reg_eip >= 0xC1440000u && reg_eip < 0xC1450000u) inloop++;
	if (md && !dumped && inloop >= 5) {
		dumped = true;
		unsigned long start = 0, len = 0; char fn[256] = {0};
		if (sscanf(md, "%lx,%lx,%255s", &start, &len, fn) == 3) {
			FILE *f = fopen(fn, "wb");
			if (f) { for (unsigned long i = 0; i < len; i++) fputc(mem_readb((PhysPt)(start + i)), f); fclose(f); }
		}
	}
	const PhysPt lin = (PhysPt)(SegPhys(cs) + reg_eip);
	char bytes[64]; char *b = bytes;
	for (int i = 0; i < 16; i++) b += sprintf(b, "%02x", mem_readb(lin + i));
	LOG_MSG("EIP %04x:%08x pm=%d v86=%d lin=%08x eax=%08x ecx=%08x edx=%08x code=%s",
		(unsigned)SegValue(cs), (unsigned)reg_eip, cpu.pmode ? 1 : 0, (reg_flags & FLAG_VM) ? 1 : 0, (unsigned)lin,
		(unsigned)reg_eax, (unsigned)reg_ecx, (unsigned)reg_edx, bytes);
	if ((reg_eax & 0xFFFF0000u) == 0xCB6F0000u) {
		const PhysPt a = (PhysPt)(reg_eax & ~0xFFFu);
		LOG_MSG("EIP  lin %08x -> phys %08x (lfb base %08x, cfg %02x cmd %02x)", (unsigned)a,
			(unsigned)PAGING_GetPhysicalAddress(a), (unsigned)mach64.pci_lfb_base, mach64.pci_cfg40, mach64.pci_command);
	}
}

#include "vga_mach64_cov.cpp"   /* TEST HOOK (not for upstream) */

/* TEST HOOK (not for upstream): DOSBOX_X_AUTOKEYS="ms:keys;ms:keys" types keys at
 * those emulated times. keys: plain characters, {ENTER} {ESC} {TAB} {WIN+R}
 * {ALT+F4} {WAIT} (one empty step). */
#include "keyboard.h"
#include <vector>
#include <string>
static void Mach64_AutoKeys(void) {
	struct Ev { KBD_KEYS k; bool down; };
	struct Step { double t; std::string keys; };
	static bool parsed = false;
	static std::vector<Step> steps;
	static size_t next = 0;
	static std::vector<Ev> q;
	static size_t qp = 0;
	if (!parsed) {
		parsed = true;
		const char *e = getenv("DOSBOX_X_AUTOKEYS");
		std::string all = e ? e : "";
		size_t p0 = 0;
		while (p0 < all.size()) {
			size_t q1 = all.find(';', p0); if (q1 == std::string::npos) q1 = all.size();
			std::string it = all.substr(p0, q1 - p0);
			size_t c = it.find(':');
			if (c != std::string::npos) steps.push_back({ atof(it.substr(0, c).c_str()), it.substr(c + 1) });
			p0 = q1 + 1;
		}
	}
	if (qp < q.size()) {
		const Ev &ev = q[qp++];
		if (ev.k != KBD_NONE) KEYBOARD_AddKey(ev.k, ev.down);
		return;
	}
	if (next >= steps.size() || PIC_FullIndex() < steps[next].t) return;
	q.clear(); qp = 0;
	const std::string &k = steps[next++].keys;
	auto tap = [&](KBD_KEYS key, bool shift) {
		if (shift) q.push_back({ KBD_leftshift, true });
		q.push_back({ key, true }); q.push_back({ key, false });
		if (shift) q.push_back({ KBD_leftshift, false });
	};
	for (size_t i = 0; i < k.size(); i++) {
		const char ch = k[i];
		if (ch == '{') {
			size_t e2 = k.find('}', i); std::string n = k.substr(i + 1, e2 - i - 1); i = e2;
			if (n == "ENTER") tap(KBD_enter, false);
			else if (n == "ESC") tap(KBD_esc, false);
			else if (n == "TAB") tap(KBD_tab, false);
			else if (n == "WAIT") { for (int w = 0; w < 30; w++) q.push_back({ KBD_NONE, false }); }
			else if (n == "WIN+R") { q.push_back({ KBD_lwindows, true }); tap(KBD_r, false); q.push_back({ KBD_lwindows, false }); }
			else if (n == "ALT+F4") { q.push_back({ KBD_leftalt, true }); tap(KBD_f4, false); q.push_back({ KBD_leftalt, false }); }
			continue;
		}
		if (ch >= 'a' && ch <= 'z') { static const KBD_KEYS L[26] = { KBD_a,KBD_b,KBD_c,KBD_d,KBD_e,KBD_f,KBD_g,KBD_h,KBD_i,KBD_j,KBD_k,KBD_l,KBD_m,KBD_n,KBD_o,KBD_p,KBD_q,KBD_r,KBD_s,KBD_t,KBD_u,KBD_v,KBD_w,KBD_x,KBD_y,KBD_z }; tap(L[ch - 'a'], false); }
		else if (ch >= 'A' && ch <= 'Z') { static const KBD_KEYS L[26] = { KBD_a,KBD_b,KBD_c,KBD_d,KBD_e,KBD_f,KBD_g,KBD_h,KBD_i,KBD_j,KBD_k,KBD_l,KBD_m,KBD_n,KBD_o,KBD_p,KBD_q,KBD_r,KBD_s,KBD_t,KBD_u,KBD_v,KBD_w,KBD_x,KBD_y,KBD_z }; tap(L[ch - 'A'], true); }
		else if (ch >= '1' && ch <= '9') tap((KBD_KEYS)(KBD_1 + (ch - '1')), false);
		else if (ch == '0') tap(KBD_0, false);
		else if (ch == ' ') tap(KBD_space, false);
		else if (ch == '\\') tap(KBD_backslash, false);
		else if (ch == ':') tap(KBD_semicolon, true);
		else if (ch == '.') tap(KBD_period, false);
		else if (ch == '"') tap(KBD_quote, true);
		else if (ch == '~') tap(KBD_grave, true);
		else if (ch == '-') tap(KBD_minus, false);
		else if (ch == '_') tap(KBD_minus, true);
	}
	LOG_MSG("AUTOKEYS: typing '%s'", k.c_str());
}

void Mach64_AutoShot(void) {
	Mach64_EIPSample();
	Mach64_CovInit();
	Mach64_AutoKeys();
	if (getenv("DOSBOX_X_M64TRACE")) {   /* TEST HOOK: watch the visible screen */
		static unsigned int fr = 0; static unsigned int last = 0xFFFFFFFF;
		if (++fr % 70 == 0) {
			const Bitu fb = (Bitu)(R(M64_CRTC_OFF_PITCH) & 0xFFFFF) * 8u;
			unsigned int nz = 0; for (Bitu i = 0; i < 640 * 480 * 2; i += 4) if (vga.mem.linear[(fb + i) & vga.mem.memmask]) nz++;
			if (nz != last) { LOG_MSG("M64 SCREEN t=%.0f nonzero=%u", PIC_FullIndex(), nz); last = nz; }
		}
	}
	static bool parsed = false;
	static double shots[16];
	static unsigned int nshots = 0, next = 0;
	if (!parsed) {
		parsed = true;
		const char *e = getenv("DOSBOX_X_AUTOSHOT");
		while (e && *e && nshots < 16) {
			shots[nshots++] = atof(e);
			const char *c = strchr(e, ',');
			e = c ? c + 1 : NULL;
		}
	}
	if (next < nshots && PIC_FullIndex() >= shots[next]) {
		extern void CAPTURE_ScreenShotEvent(bool pressed);
		CAPTURE_ScreenShotEvent(true);
		Mach64_CovDump("screenshot");
		{
			const Bitu cb = (Bitu)(R(M64_CUR_OFFSET) & 0xFFFFF) * 8u;
			char hex[80]; size_t hn = 0;
			for (unsigned int i = 0; i < 32; i++) hn += (size_t)snprintf(hex + hn, sizeof(hex) - hn, "%02x", vga.mem.linear[(cb + 16 * 32 + i) & vga.mem.memmask]);
			LOG_MSG("M64 CURSOR gen_test=%08x off=%08x posn=%08x hvoff=%08x clr0=%08x clr1=%08x crtc_gen=%08x row32: %s",
				(unsigned)R(M64_GEN_TEST_CNTL), (unsigned)R(M64_CUR_OFFSET), (unsigned)R(M64_CUR_HORZ_VERT_POSN), (unsigned)R(M64_CUR_HORZ_VERT_OFF),
				(unsigned)R(M64_CUR_CLR0), (unsigned)R(M64_CUR_CLR1), (unsigned)R(M64_CRTC_GEN_CNTL), hex);
			{
				const uint32_t op = R(M64_CRTC_OFF_PITCH);
				const Bitu fb = (Bitu)(op & 0xFFFFF) * 8u;
				char px[200]; size_t pn = 0;
				for (unsigned int i = 0; i < 16; i++) pn += (size_t)snprintf(px + pn, sizeof(px) - pn, "%02x", vga.mem.linear[(fb + 640 * 2 * 100 + 200 + i) & vga.mem.memmask]);
				unsigned int nz = 0; for (Bitu i = 0; i < 640 * 480 * 2; i++) if (vga.mem.linear[(fb + i) & vga.mem.memmask]) nz++;
				LOG_MSG("M64 SCAN off_pitch=%08x htd=%08x vtd=%08x dac=%08x gen_test=%08x mem_cntl=%08x draw %ux%u line100: %s nonzero=%u pel_mask=%02x",
					(unsigned)op, (unsigned)R(M64_CRTC_H_TOTAL_DISP), (unsigned)R(M64_CRTC_V_TOTAL_DISP), (unsigned)R(M64_DAC_CNTL), (unsigned)R(M64_GEN_TEST_CNTL), (unsigned)R(M64_MEM_CNTL),
					(unsigned)vga.draw.width, (unsigned)vga.draw.height, px, nz, (unsigned)vga.dac.pel_mask);
				char blk[400]; size_t bn = 0;
				for (Bitu bb = 0; bb < vga.mem.memsize; bb += 0x40000) { unsigned int c = 0; for (Bitu i = 0; i < 0x40000; i++) if (vga.mem.linear[bb + i]) c++; bn += (size_t)snprintf(blk + bn, sizeof(blk) - bn, " %u", c); }
				LOG_MSG("M64 SCAN nonzero per 256K:%s", blk);
				for (unsigned int c = 0; c < 4; c++) {
					const Bitu ca = vga.mem.memsize - (c + 1) * 256;
					char cx[300]; size_t cn = 0;
					for (unsigned int d = 0; d < 30; d++) { uint32_t v; memcpy(&v, &vga.mem.linear[ca + d * 4], 4); cn += (size_t)snprintf(cx + cn, sizeof(cx) - cn, " %08x", v); }
					LOG_MSG("M64 CONTEXT %u @%06x:%s", c, (unsigned)ca, cx);
				}
			}
		}
		next++;
	}
}

void Mach64_VerticalRetrace(void) {
	if (!Mach64_Present()) return;
	/* CRTC_VBLANK_INT latches at the vertical blank [RRG 4-26]. No interrupt
	 * is delivered: the PCI function reports no interrupt pin. */
	mach64.reg[M64_CRTC_INT_CNTL] |= (1u << 2);
	mach64.vblank_seen++;
}

bool Mach64_InVerticalBlank(void) {
	const double timeInFrame = PIC_FullIndex() - vga.draw.delay.framestart;
	return timeInFrame >= vga.draw.delay.vdend;
}

static uint32_t Mach64_CurrentLine(void) {
	const double timeInFrame = PIC_FullIndex() - vga.draw.delay.framestart;
	if (vga.draw.delay.htotal <= 0) return 0;
	Bitu l = (Bitu)(timeInFrame / vga.draw.delay.htotal);
	return (uint32_t)(l & 0x7FF);
}

/* ---------------------------------------------------------------------- */
/* Mapping                                                                  */
/* ---------------------------------------------------------------------- */

static void Mach64_UpdateMapping(void) {
	/* CFG_MEM_AP_SIZE is 2x8MB and CFG_MEM_AP_LOC follows BAR0 [RRG 4-16] */
	mach64.reg[M64_CONFIG_CNTL] = (mach64.reg[M64_CONFIG_CNTL] & 0x00080004) | 2u |
		(((mach64.pci_lfb_base >> 22) << 4) & 0x3FF0);
	VGA_StartUpdateLFB();
	VGA_SetupHandlers();
}

void Mach64_LFBInfo(uint32_t &base, bool &enabled) {
	base = mach64.pci_lfb_base;
	enabled = (mach64.pci_command & 0x2) && base != 0;
}

/* ---------------------------------------------------------------------- */
/* Register access                                                          */
/* ---------------------------------------------------------------------- */

static void Mach64_DACCntlChanged(void) {
	const bool eight = (R(M64_DAC_CNTL) & (1u << 8)) != 0;
	if (vga_8bit_dac != eight) {
		vga_8bit_dac = eight;
		VGA_DAC_UpdateColorPalette();
	}
}

static uint32_t Mach64_ReadDACRegs(unsigned int lane) {
	/* DAC_REGS: W_INDEX, DATA, MASK, R_INDEX in bytes 0..3 [RRG 4-39] */
	switch (lane) {
		case 0: return (uint32_t)read_p3c8(0x3c8, 1) & 0xFF;
		case 1: return (uint32_t)read_p3c9(0x3c9, 1) & 0xFF;
		case 2: return (uint32_t)read_p3c6(0x3c6, 1) & 0xFF;
		default: return (uint32_t)read_p3c7(0x3c7, 1) & 0xFF;
	}
}

static void Mach64_WriteDACRegs(unsigned int lane, uint8_t v) {
	switch (lane) {
		case 0: write_p3c8(0x3c8, v, 1); break;
		case 1: write_p3c9(0x3c9, v, 1); break;
		case 2: write_p3c6(0x3c6, v, 1); break;
		default: write_p3c7(0x3c7, v, 1); break;
	}
}

static uint32_t Mach64_ReadRegInner(unsigned int reg) {
	reg &= 0x1FF;
	if (reg >= 0x100) {
		if (reg == M64_VMC_STATUS) return 0;
		if (reg == M64_EXT_CRTC_GEN_CNTL_R) return 0;
		return mach64.reg[reg];
	}
	switch (reg) {
		case M64_CRTC_VLINE_CRNT_VLINE:
			return (mach64.reg[reg] & 0x7FF) | (Mach64_CurrentLine() << 16);
		case M64_CRTC_INT_CNTL: {
			uint32_t v = mach64.reg[reg] & ~0x61u;
			if (Mach64_InVerticalBlank()) v |= 1u;          /* CRTC_VBLANK */
			if (Mach64_CurrentLine() & 1) v |= 0x20u;       /* CRTC_VLINE_SYNC */
			return v;
		}
		case M64_CLOCK_CNTL:
			return (mach64.reg[reg] & 0x00003E03) | ((uint32_t)mach64.pll[(mach64.reg[reg] >> 10) & 0xF] << 16);
		case M64_DAC_REGS:
			return Mach64_ReadDACRegs(0) | (Mach64_ReadDACRegs(1) << 8) | (Mach64_ReadDACRegs(2) << 16) | (Mach64_ReadDACRegs(3) << 24);
		case M64_DAC_CNTL:
			return (mach64.reg[reg] & 0xBF00E10C) | (1u << 16); /* DAC_TYPE 1: internal 24-bit [RRG 4-40] */
		case M64_CONFIG_CHIP_ID:
			return Mach64_ChipID();
		case M64_CRC_SIG:
			return 0;
		default:
			break;
	}
	if (reg >= 0x40) {
		switch (reg) {
			case M64_FIFO_STAT:
				return 0;                                   /* empty, no error */
			case M64_GUI_STAT: {
				/* FIFO_CNT: all entries free. Rage II: 64 (assumed; Time Warriors' ATI3DCIF
				 * waits for >= 48 free entries before each batch on the GT-B) */
				uint32_t v = (mach64_rage2 ? 64u : 32u) << 16;
				if (Mach64_Accel_Busy()) v |= 1u;
				const int32_t x  = (int32_t)(R(M64_DST_X) << 19) >> 19;
				const int32_t y  = (int32_t)(R(M64_DST_Y) << 17) >> 17;
				const int32_t sl = (int32_t)(R(M64_SC_LEFT) << 19) >> 19;
				const int32_t sr = (int32_t)(R(M64_SC_RIGHT) << 19) >> 19;
				const int32_t st = (int32_t)(R(M64_SC_TOP) << 17) >> 17;
				const int32_t sb = (int32_t)(R(M64_SC_BOTTOM) << 17) >> 17;
				if (x < sl) v |= 1u << 8;
				if (x > sr) v |= 1u << 9;
				if (y < st) v |= 1u << 10;
				if (y > sb) v |= 1u << 11;
				return v;
			}
			case M64_DST_Y_X:
			case M64_DST_Y_X_ALIAS:
				return (R(M64_DST_Y) & 0x7FFF) | ((R(M64_DST_X) & 0x1FFF) << 16);
			case M64_DST_HEIGHT_WIDTH:
				return (R(M64_DST_HEIGHT) & 0x7FFF) | ((R(M64_DST_WIDTH) & 0xFFFF) << 16);
			case M64_DST_X_WIDTH:
				return (R(M64_DST_X) & 0x1FFF) | ((R(M64_DST_WIDTH) & 0xFFFF) << 16);
			case M64_LEAD_BRES_LNTH:
				return R(M64_DST_BRES_LNTH);
			case M64_SRC_Y_X:
				return (R(M64_SRC_Y) & 0x7FFF) | ((R(M64_SRC_X) & 0x1FFF) << 16);
			case M64_SRC_HEIGHT1_WIDTH1:
				return (R(M64_SRC_HEIGHT1) & 0x7FFF) | ((R(M64_SRC_WIDTH1) & 0x1FFF) << 16);
			case M64_SRC_Y_X_START:
				return (R(M64_SRC_Y_START) & 0x7FFF) | ((R(M64_SRC_X_START) & 0x1FFF) << 16);
			case M64_SRC_HEIGHT2_WIDTH2:
				return (R(M64_SRC_HEIGHT2) & 0x7FFF) | ((R(M64_SRC_WIDTH2) & 0x1FFF) << 16);
			case M64_SC_LEFT_RIGHT:
				return (R(M64_SC_LEFT) & 0x1FFF) | ((R(M64_SC_RIGHT) & 0x1FFF) << 16);
			case M64_SC_TOP_BOTTOM:
				return (R(M64_SC_TOP) & 0x7FFF) | ((R(M64_SC_BOTTOM) & 0x7FFF) << 16);
			case M64_GUI_TRAJ_CNTL:
				return (R(M64_DST_CNTL) & 0xFFFF) | ((R(M64_SRC_CNTL) & 0xFF) << 16) |
					((R(M64_PAT_CNTL) & 7) << 24) | ((R(M64_HOST_CNTL) & 3) << 28);
			default:
				if (reg >= M64_HOST_DATA0 && reg <= M64_HOST_DATA15) return 0;
				return mach64.reg[reg];
		}
	}
	return mach64.reg[reg];
}

/* TEST HOOK (not for upstream): DOSBOX_X_M64TRACE=N logs the first N register accesses */
static long m64_trace_left = -1;
static inline bool Mach64_Trace(void) {
	if (m64_trace_left < 0) {
		const char *e = getenv("DOSBOX_X_M64TRACE");
		m64_trace_left = e ? atol(e) : 0;
	}
	if (m64_trace_left == 0) return false;
	m64_trace_left--;
	return true;
}

/* TEST HOOK: DOSBOX_X_M64RING=N keeps the last N register accesses and writes them to
 * DOSBOX_X_M64RINGOUT (default m64ring.log) when DOSBox-X exits */
struct M64Ring {
	struct E { uint32_t reg, val, mask, pc, n; bool w; };
	std::vector<E> buf; size_t pos = 0; bool full = false; bool init = false;
	bool on() {
		if (!init) { init = true; const char *e = getenv("DOSBOX_X_M64RING"); if (e && atol(e) > 0) buf.resize((size_t)atol(e)); }
		return !buf.empty();
	}
	void add(bool w, uint32_t reg, uint32_t val, uint32_t mask) { const uint32_t pc = (uint32_t)SegPhys(cs) + (uint32_t)reg_eip;
		if (pos || full) { E &l = buf[pos ? pos - 1 : buf.size() - 1]; if (l.w == w && l.reg == reg && l.val == val && l.mask == mask && l.pc == pc) { l.n++; return; } }
		buf[pos] = { reg, val, mask, pc, 1, w }; if (++pos == buf.size()) { pos = 0; full = true; } }
	~M64Ring() {
		if (buf.empty()) return;
		const char *o = getenv("DOSBOX_X_M64RINGOUT"); FILE *f = fopen(o ? o : "m64ring.log", "w"); if (!f) return;
		const size_t n = full ? buf.size() : pos;
		for (size_t i = 0; i < n; i++) { const E &e = buf[(full ? pos : 0) + i < buf.size() ? (full ? pos : 0) + i : (full ? pos : 0) + i - buf.size()];
			if (e.w) fprintf(f, "W %03x = %08x mask %08x pc %08x x%u\n", e.reg, e.val, e.mask, e.pc, e.n); else fprintf(f, "R %03x = %08x pc %08x x%u\n", e.reg, e.val, e.pc, e.n); }
		fclose(f);
	}
};
static M64Ring m64ring;

uint32_t Mach64_ReadReg(unsigned int reg) {
	const uint32_t v = Mach64_ReadRegInner(reg);
	if (Mach64_Trace()) LOG_MSG("M64T R %03x = %08x", reg & 0x1FF, v);
	if (m64ring.on()) m64ring.add(false, reg & 0x1FF, v, 0);
	return v;
}

void Mach64_WriteReg(unsigned int reg, uint32_t val, uint32_t mask) {
	reg &= 0x1FF;
	val &= mask;
	if (Mach64_Trace()) LOG_MSG("M64T W %03x = %08x mask %08x", reg, val, mask);
	if (m64ring.on()) m64ring.add(true, reg, val, mask);

	if (reg >= 0x40 && reg < 0x100) {
		Mach64_Accel_Write(reg, val, mask);
		return;
	}

	if (reg >= 0x100) {
		const uint32_t m = Mach64_Block1Mask(reg) & mask;
		mach64.reg[reg] = (mach64.reg[reg] & ~m) | (val & m);
		return;
	}

	const uint32_t old = mach64.reg[reg];

	switch (reg) {
		case M64_CRTC_INT_CNTL: {
			/* enables are read/write, status bits one above them acknowledge on 1 [RRG 4-26] */
			const uint32_t en = 0x0055000Au & mask;
			const uint32_t ack = val & 0x00AA0014u;
			uint32_t v = (old & ~en) | (val & en);
			v &= ~ack;
			mach64.reg[reg] = v;
			return;
		}
		case M64_DAC_REGS:
			for (unsigned int lane = 0; lane < 4; lane++)
				if (mask & (0xFFu << (lane * 8))) Mach64_WriteDACRegs(lane, (uint8_t)(val >> (lane * 8)));
			return;
		case M64_CLOCK_CNTL: {
			const uint32_t m = 0x00003E03u & mask;
			mach64.reg[reg] = (old & ~m) | (val & m);
			/* PLL_DATA is written only with PLL_WR_EN [RRG 4-38] */
			if ((mask & 0x00FF0000u) && (mach64.reg[reg] & (1u << 9))) {
				const unsigned int a = (mach64.reg[reg] >> 10) & 0xF;
				uint8_t d = (uint8_t)(val >> 16);
				if (a == PLL_TEST_COUNT) d = 0;
				if (a == PLL_TEST_CNTL && ((R(M64_GEN_TEST_CNTL) >> 16) & 0xF) != 0xB) d = 0;
				if (mach64.pll[a] != d) {
					mach64.pll[a] = d;
					if (Mach64_ExtDisplayActive()) VGA_StartResize();
				}
			}
			if (((old ^ mach64.reg[reg]) & 3) && Mach64_ExtDisplayActive()) VGA_StartResize();
			return;
		}
		case M64_BUS_CNTL: {
			const uint32_t m = Mach64_Block0Mask(reg) & mask;
			mach64.reg[reg] = (old & ~m) | (val & m);
			if ((old ^ mach64.reg[reg]) & (1u << 27)) Mach64_UpdateMapping(); /* BUS_EXT_REG_EN */
			return;
		}
		case M64_GEN_TEST_CNTL: {
			const uint32_t m = Mach64_Block0Mask(reg) & mask;
			mach64.reg[reg] = (old & ~m) | (val & m);
			/* GEN_GUI_EN (bit 8): 0 resets the draw engine */
			if ((old & 0x100) && !(mach64.reg[reg] & 0x100)) Mach64_Accel_Reset();
			if (((mach64.reg[reg] >> 16) & 0xF) != 0xB) mach64.pll[PLL_TEST_CNTL] = 0;
			return;
		}
		case M64_CONFIG_CNTL: {
			const uint32_t m = Mach64_Block0Mask(reg) & mask;
			mach64.reg[reg] = (old & ~m) | (val & m);
			if ((old ^ mach64.reg[reg]) & 0x4) VGA_SetupHandlers();
			return;
		}
		case M64_MEM_VGA_WP_SEL:
		case M64_MEM_VGA_RP_SEL: {
			const uint32_t m = Mach64_Block0Mask(reg) & mask;
			mach64.reg[reg] = (old & ~m) | (val & m);
			if (old != mach64.reg[reg]) VGA_SetupHandlers();
			return;
		}
		case M64_CONFIG_CHIP_ID:
		case M64_CRC_SIG:
			return;
		default:
			break;
	}

	const uint32_t m = Mach64_Block0Mask(reg) & mask;
	mach64.reg[reg] = (old & ~m) | (val & m);

	switch (reg) {
		case M64_CRTC_H_TOTAL_DISP:
		case M64_CRTC_H_SYNC_STRT_WID:
		case M64_CRTC_V_TOTAL_DISP:
		case M64_CRTC_V_SYNC_STRT_WID:
			if (old != mach64.reg[reg] && Mach64_ExtDisplayActive()) VGA_StartResize();
			break;
		case M64_CRTC_OFF_PITCH:
			/* the offset is latched per frame by the scanout; the pitch needs a resize */
			if (((old ^ mach64.reg[reg]) & 0xFFC00000u) && Mach64_ExtDisplayActive()) VGA_StartResize();
			break;
		case M64_CRTC_GEN_CNTL:
			if (old != mach64.reg[reg]) {
				VGA_DetermineMode();
				VGA_SetupHandlers();
				VGA_StartResize();
			}
			break;
		case M64_DAC_CNTL:
			Mach64_DACCntlChanged();
			break;
		default:
			break;
	}
}

/* ---------------------------------------------------------------------- */
/* I/O decoding: sparse (base 2ECh/1CCh/1C8h) and block (PCI BAR1)          */
/* ---------------------------------------------------------------------- */

/* sparse I/O select -> block 0 dword offset [RRG chapter 4 register headers] */
static const int8_t m64_sparse_map[32] = {
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,   /* 00-07 CRTC */
	0x10, 0x11, 0x12, 0x18, 0x19, 0x1A, 0x1B, 0x1C,   /* 08-0F overscan, cursor */
	0x20, 0x21, 0x24, 0x28, 0x2C, 0x2D, 0x2E, 0x30,   /* 10-17 */
	0x31, 0x34, 0x37, 0x38, 0x39, 0x3A, 0x1E, 0x00    /* 18-1F */
};

static Bitu Mach64_IORead(unsigned int reg, Bitu port, Bitu iolen) {
	const unsigned int lane = (unsigned int)(port & 3);
	uint32_t v = Mach64_ReadReg(reg);
	/* DAC_REGS reads have side effects: read only the lanes asked for */
	if (reg == M64_DAC_REGS) {
		Bitu r = 0;
		for (unsigned int i = 0; i < iolen && lane + i < 4; i++) r |= (Bitu)Mach64_ReadDACRegs(lane + i) << (i * 8);
		return r;
	}
	v >>= lane * 8;
	if (iolen == 1) return v & 0xFF;
	if (iolen == 2) return v & 0xFFFF;
	return v;
}

static void Mach64_IOWrite(unsigned int reg, Bitu port, Bitu val, Bitu iolen) {
	const unsigned int lane = (unsigned int)(port & 3);
	uint32_t mask = (iolen == 1) ? 0xFFu : (iolen == 2) ? 0xFFFFu : 0xFFFFFFFFu;
	mask <<= lane * 8;
	Mach64_WriteReg(reg, (uint32_t)val << (lane * 8), mask);
}

static Bitu Mach64_SparseRead(Bitu port, Bitu iolen) {
	const unsigned int sel = (unsigned int)((port - mach64.io_sparse_base_active) >> 10) & 0x1F;
	return Mach64_IORead((unsigned int)m64_sparse_map[sel], port, iolen);
}

static void Mach64_SparseWrite(Bitu port, Bitu val, Bitu iolen) {
	const unsigned int sel = (unsigned int)((port - mach64.io_sparse_base_active) >> 10) & 0x1F;
	Mach64_IOWrite((unsigned int)m64_sparse_map[sel], port, val, iolen);
}

static Bitu Mach64_BlockRead(Bitu port, Bitu iolen) {
	const unsigned int off = (unsigned int)(port - mach64.io_block_base_active) & 0xFF;
	return Mach64_IORead(off >> 2, port, iolen);
}

static void Mach64_BlockWrite(Bitu port, Bitu val, Bitu iolen) {
	const unsigned int off = (unsigned int)(port - mach64.io_block_base_active) & 0xFF;
	Mach64_IOWrite(off >> 2, port, val, iolen);
}

static void Mach64_UpdateIO(void) {
	if (mach64.io_sparse_base_active) {
		for (unsigned int s = 0; s < 32; s++) {
			IO_FreeReadHandler((s << 10) + mach64.io_sparse_base_active, IO_MA, 4);
			IO_FreeWriteHandler((s << 10) + mach64.io_sparse_base_active, IO_MA, 4);
		}
		mach64.io_sparse_base_active = 0;
	}
	if (mach64.io_block_base_active) {
		IO_FreeReadHandler(mach64.io_block_base_active, IO_MA, 256);
		IO_FreeWriteHandler(mach64.io_block_base_active, IO_MA, 256);
		mach64.io_block_base_active = 0;
	}
	if (!(mach64.pci_command & 1)) return;

	if (mach64.pci_cfg40 & 0x4) {
		const uint32_t b = mach64.pci_io_base & 0xFF00u;
		if (b != 0) {
			mach64.io_block_base_active = b;
			IO_RegisterReadHandler(b, Mach64_BlockRead, IO_MA, 256);
			IO_RegisterWriteHandler(b, Mach64_BlockWrite, IO_MA, 256);
		}
	}
	else {
		static const uint32_t bases[4] = { 0x2EC, 0x1CC, 0x1C8, 0x2EC };
		const uint32_t b = bases[mach64.pci_cfg40 & 3];
		mach64.io_sparse_base_active = b;
		for (unsigned int s = 0; s < 32; s++) {
			IO_RegisterReadHandler((s << 10) + b, Mach64_SparseRead, IO_MA, 4);
			IO_RegisterWriteHandler((s << 10) + b, Mach64_SparseWrite, IO_MA, 4);
		}
	}
}

/* The BIOS reaches the registers through the I/O ports the card decodes, as
 * ATI's BIOS does, so that a driver trapping them (Windows in a DOS box)
 * sees the accesses. Registers without an I/O address are written directly. */
static Bitu Mach64_IOPort(unsigned int reg) {
	if (reg >= 0x40) return 0;
	if (mach64.io_block_base_active) return mach64.io_block_base_active + reg * 4u;
	if (mach64.io_sparse_base_active) {
		for (unsigned int s = 0; s < 31; s++)
			if ((unsigned int)m64_sparse_map[s] == reg && (s == 0 || reg != 0))
				return mach64.io_sparse_base_active + (s << 10);
	}
	return 0;
}

void Mach64_IOWriteReg(unsigned int reg, uint32_t val) {
	const Bitu port = Mach64_IOPort(reg);
	if (port) IO_WriteD(port, val);
	else Mach64_WriteReg(reg, val, 0xFFFFFFFFu);
}

uint32_t Mach64_IOReadReg(unsigned int reg) {
	const Bitu port = Mach64_IOPort(reg);
	if (port) return IO_ReadD(port);
	return Mach64_ReadReg(reg);
}

/* ---------------------------------------------------------------------- */
/* Memory mapped registers                                                  */
/* ---------------------------------------------------------------------- */

/* MMIO access at a byte offset within the 2 KB register area
 * (0x000-0x3FF block 1, 0x400-0x7FF block 0). */
static uint32_t Mach64_MMIORead(unsigned int off, unsigned int len) {
	const unsigned int reg = ((off & 0x400) ? 0 : 0x100) + ((off & 0x3FF) >> 2);
	const unsigned int lane = off & 3;
	if (lane + len > 4) {
		uint32_t v = 0;
		for (unsigned int i = 0; i < len; i++) v |= Mach64_MMIORead(off + i, 1) << (i * 8);
		return v;
	}
	uint32_t v;
	if (reg == M64_DAC_REGS) {
		v = 0;
		for (unsigned int i = 0; i < len; i++) v |= Mach64_ReadDACRegs(lane + i) << (i * 8);
		return v;
	}
	v = Mach64_ReadReg(reg) >> (lane * 8);
	if (len == 1) return v & 0xFF;
	if (len == 2) return v & 0xFFFF;
	return v;
}

static void Mach64_MMIOWrite(unsigned int off, uint32_t val, unsigned int len) {
	const unsigned int reg = ((off & 0x400) ? 0 : 0x100) + ((off & 0x3FF) >> 2);
	const unsigned int lane = off & 3;
	if (lane + len > 4) {
		for (unsigned int i = 0; i < len; i++) Mach64_MMIOWrite(off + i, (val >> (i * 8)) & 0xFF, 1);
		return;
	}
	uint32_t mask = (len == 1) ? 0xFFu : (len == 2) ? 0xFFFFu : 0xFFFFFFFFu;
	/* HOST_DATA: each write is consumed as it arrives, at its own width */
	if (reg >= M64_HOST_DATA0 && reg <= M64_HOST_DATA15) {
		Mach64_Accel_HostData(val & mask, (int)len * 8);
		return;
	}
	Mach64_WriteReg(reg, val << (lane * 8), mask << (lane * 8));
}

static bool Mach64_Block1Enabled(void) {
	return (R(M64_BUS_CNTL) & (1u << 27)) != 0;   /* BUS_EXT_REG_EN [RRG 4-8] */
}

/* The register pages and the big endian aperture. The 16 MB BAR holds two
 * 8 MB apertures, little endian then big endian [RRG 2-2]; the last 4 KB page
 * of each has video memory in its first 2 KB, block 1 at +800h (while
 * BUS_EXT_REG_EN) and block 0 at +C00h. The rest of the big endian aperture is
 * video memory; the byte swapping it would apply is not emulated. */
class Mach64_ApertureRegPage : public PageHandler {
public:
	Mach64_ApertureRegPage() : PageHandler(PFLAG_NOCODE) {}

	uint8_t readb(PhysPt addr) override { return (uint8_t)rd(addr, 1); }
	uint16_t readw(PhysPt addr) override { return (uint16_t)rd(addr, 2); }
	uint32_t readd(PhysPt addr) override { return rd(addr, 4); }
	void writeb(PhysPt addr, uint8_t val) override { wr(addr, val, 1); }
	void writew(PhysPt addr, uint16_t val) override { wr(addr, val, 2); }
	void writed(PhysPt addr, uint32_t val) override { wr(addr, val, 4); }
private:
	/* register offset from block 1, or -1 for video memory */
	static int RegOffset(uint32_t ap) {
		if ((ap & 0x7FF000u) != 0x7FF000u) return -1;
		const unsigned int off = ap & 0xFFFu;
		if (off < 0x800 || (off < 0xC00 && !Mach64_Block1Enabled())) return -1;
		return (int)(off - 0x800);
	}
	static uint32_t rd(PhysPt addr, unsigned int len) {
		const uint32_t ap = (uint32_t)PAGING_GetPhysicalAddress(addr) & 0xFFFFFFu;
		if (((ap & 0xFFFu) + len) > 0x1000) len = 0x1000 - (ap & 0xFFFu);
		const int r = RegOffset(ap);
		if (r >= 0) return Mach64_MMIORead((unsigned int)r, len);
		uint32_t v = 0;
		const Bitu vaddr = ap & 0x7FFFFFu;
		for (unsigned int i = 0; i < len; i++)
			v |= (uint32_t)((vaddr + i < vga.mem.memsize) ? vga.mem.linear[vaddr + i] : 0xFF) << (i * 8);
		return v;
	}
	static void wr(PhysPt addr, uint32_t val, unsigned int len) {
		const uint32_t ap = (uint32_t)PAGING_GetPhysicalAddress(addr) & 0xFFFFFFu;
		if (((ap & 0xFFFu) + len) > 0x1000) len = 0x1000 - (ap & 0xFFFu);
		const int r = RegOffset(ap);
		if (r >= 0) { Mach64_MMIOWrite((unsigned int)r, val, len); return; }
		const Bitu vaddr = ap & 0x7FFFFFu;
		for (unsigned int i = 0; i < len; i++)
			if (vaddr + i < vga.mem.memsize) vga.mem.linear[vaddr + i] = (uint8_t)(val >> (i * 8));
	}
};

static Mach64_ApertureRegPage m64_aperture_regs;

PageHandler *Mach64_GetMMIOHandler(void) { return &m64_aperture_regs; }

/* The register window in the VGA aperture at B000:F800-FFFF while
 * CFG_MEM_VGA_AP_EN is set [RRG 2-2]. The rest of the page stays whatever
 * the VGA memory handler makes of it. */
class Mach64_VGARegPage : public PageHandler {
public:
	Mach64_VGARegPage() : PageHandler(PFLAG_NOCODE) {}
	PageHandler *under = NULL;

	uint8_t readb(PhysPt addr) override { return (uint8_t)rd(addr, 1); }
	uint16_t readw(PhysPt addr) override { return (uint16_t)rd(addr, 2); }
	uint32_t readd(PhysPt addr) override { return rd(addr, 4); }
	void writeb(PhysPt addr, uint8_t val) override { wr(addr, val, 1); }
	void writew(PhysPt addr, uint16_t val) override { wr(addr, val, 2); }
	void writed(PhysPt addr, uint32_t val) override { wr(addr, val, 4); }
private:
	bool isreg(unsigned int off) const {
		return off >= 0xC00 || (off >= 0x800 && Mach64_Block1Enabled());
	}
	uint32_t rd(PhysPt addr, unsigned int len) {
		const PhysPt pa = PAGING_GetPhysicalAddress(addr);
		const unsigned int off = (unsigned int)(pa & 0xFFF);
		if (isreg(off) && off + len <= 0x1000) return Mach64_MMIORead(off - 0x800, len);
		uint32_t v = 0;
		for (unsigned int i = 0; i < len; i++) {
			const unsigned int o = off + i;
			uint8_t b = 0xFF;
			if (o < 0x1000) {
				if (isreg(o)) b = (uint8_t)Mach64_MMIORead(o - 0x800, 1);
				else if (under) {
					if (under->flags & PFLAG_READABLE) b = under->GetHostReadPt((PageNum)(pa >> 12))[o];
					else b = under->readb(addr + i);
				}
			}
			v |= (uint32_t)b << (i * 8);
		}
		return v;
	}
	void wr(PhysPt addr, uint32_t val, unsigned int len) {
		const PhysPt pa = PAGING_GetPhysicalAddress(addr);
		const unsigned int off = (unsigned int)(pa & 0xFFF);
		if (isreg(off) && off + len <= 0x1000) { Mach64_MMIOWrite(off - 0x800, val, len); return; }
		for (unsigned int i = 0; i < len; i++) {
			const unsigned int o = off + i;
			const uint8_t b = (uint8_t)(val >> (i * 8));
			if (o >= 0x1000) break;
			if (isreg(o)) Mach64_MMIOWrite(o - 0x800, b, 1);
			else if (under) {
				if (under->flags & PFLAG_WRITEABLE) under->GetHostWritePt((PageNum)(pa >> 12))[o] = b;
				else under->writeb(addr + i, b);
			}
		}
	}
};

static Mach64_VGARegPage m64_vga_regs;

/* Called at the end of VGA_SetupHandlers: bank offsets for the A0000 window
 * and the register window at BF800h. */
void Mach64_SetupHandlers(void) {
	if (!Mach64_Present()) return;
	/* Not while the VGA maps B8000-BFFFF (text modes): the window would sit on
	 * text memory, which the mode set clears in full. ATI's BIOS leaves the
	 * window enabled and the Windows 9x driver uses it in mode 12h (A0000
	 * 64K map); how the chip resolves the overlap in text modes is not
	 * documented. */
	if ((R(M64_CONFIG_CNTL) & 0x4) && ((vga.gfx.miscellaneous >> 2) & 3) != 3) {
		PageHandler *cur = MEM_GetPageHandler(0xBF);
		if (cur != &m64_vga_regs) m64_vga_regs.under = cur;
		MEM_SetPageHandler(0xBF, 1, &m64_vga_regs);
	}
}

/* MEM_VGA_WP_SEL / MEM_VGA_RP_SEL: 32 KB pages [RRG 4-12]. DOSBox-X maps one
 * contiguous window, so the page of the lower half is used for all of it. */
void Mach64_GetBanks(uint32_t &rd, uint32_t &wr) {
	rd = (R(M64_MEM_VGA_RP_SEL) & 0xFF) * 0x8000u;
	wr = (R(M64_MEM_VGA_WP_SEL) & 0xFF) * 0x8000u;
}

/* ---------------------------------------------------------------------- */
/* SVGA driver hooks                                                        */
/* ---------------------------------------------------------------------- */

static void Mach64_DetermineMode(void) {
	if (Mach64_ExtDisplayActive()) {
		switch (Mach64_CRTCPixWidth()) {
			case M64_PIX_4BPP:  VGA_SetMode(M_PACKED4); break;
			case M64_PIX_15BPP: VGA_SetMode(M_LIN15); break;
			case M64_PIX_16BPP: VGA_SetMode(M_LIN16); break;
			case M64_PIX_24BPP: VGA_SetMode(M_LIN24); break;
			case M64_PIX_32BPP: VGA_SetMode(M_LIN32); break;
			default:            VGA_SetMode(M_LIN8); break;
		}
		return;
	}
	VGA_DetermineMode_StandardVGA();
}

static Bitu Mach64_GetClock(void) {
	/* VGA display: the clock select comes from GENMO bits 3:2 [RRG 4-38];
	 * the four PLL entries power up as 25.175 / 28.322 / ... MHz */
	return Mach64_PLL_VCLK((vga.misc_output >> 2) & 3);
}

static bool Mach64_HWCursorActive(void) {
	return false; /* drawn by Mach64_DrawLine, not the S3 cursor code */
}

static bool Mach64_AcceptsMode(Bitu mode) {
	return VideoModeMemSize(mode) < vga.mem.vbe_memsize;
}

static void Mach64_Reset(void) {
	memset(mach64.reg, 0, sizeof(mach64.reg));
	Mach64_PLL_Reset();

	mach64.reg[M64_CRTC_GEN_CNTL] = 0;
	mach64.reg[M64_BUS_CNTL] = 0x08000000u | 0xFFu;   /* block 1 on, default wait states */
	mach64.reg[M64_DAC_CNTL] = 0;
	mach64.reg[M64_GEN_TEST_CNTL] = 0x100;             /* GUI engine enabled */

	/* MEM_SIZE [RRG 4-10] */
	unsigned int msz = 3;
	if (vga.mem.memsize <= 512*1024) msz = 0;
	else if (vga.mem.memsize <= 1024*1024) msz = 1;
	else if (vga.mem.memsize <= 2048*1024) msz = 2;
	mach64.reg[M64_MEM_CNTL] = msz | (3u << 11);

	/* CONFIG_STAT0: EDO DRAM, VGA enabled [RRG 4-18] */
	mach64.reg[M64_CONFIG_STAT0] = 2u | (1u << 4);
	mach64.reg[M64_CONFIG_CNTL] = 2u;

	/* engine defaults */
	mach64.reg[M64_DST_CNTL] = M64_DST_X_DIR | M64_DST_Y_DIR;
	mach64.reg[M64_DP_WRITE_MSK] = 0xFFFFFFFFu;
	mach64.reg[M64_SC_RIGHT] = 0x1FFF;
	mach64.reg[M64_SC_BOTTOM] = 0x7FFF;
	Mach64_Accel_Reset();
}

void SVGA_Setup_Mach64(void) {
	svga.write_p3d5 = nullptr;
	svga.read_p3d5 = nullptr;
	svga.write_p3c5 = nullptr;
	svga.read_p3c5 = nullptr;
	svga.write_p3c0 = nullptr;
	svga.read_p3c1 = nullptr;
	svga.set_video_mode = nullptr;
	svga.determine_mode = &Mach64_DetermineMode;
	svga.set_clock = nullptr;
	svga.get_clock = &Mach64_GetClock;
	svga.hardware_cursor_active = &Mach64_HWCursorActive;
	svga.accepts_mode = &Mach64_AcceptsMode;

	vga.max_svga_width = 2048;
	vga.max_svga_height = 2048;

	/* 3D RAGE boards: 2 MB or 4 MB [RRG 4-10, MEM_SIZE] */
	if (vga.mem.memsize == 0)
		vga.mem.memsize = vga.mem.memsize_original = 4*1024*1024;
	if (vga.mem.memsize_original < 3*1024*1024)
		vga.mem.memsize = 2*1024*1024;
	else
		vga.mem.memsize = 4*1024*1024;

	memset(&mach64, 0, sizeof(mach64));
	Mach64_Reset();

	mach64.pci_lfb_base = S3_LFB_BASE & 0xFF000000u;
	if (mach64.pci_lfb_base == 0) mach64.pci_lfb_base = 0xE0000000u;
	mach64.pci_io_base = 0xD800;
	mach64.pci_cfg40 = 0x4;                             /* block I/O decoding */
	mach64.pci_command = 0x3;                           /* I/O and memory enabled */
	mach64.pci_rom_bar = 0;

	Mach64_UpdateIO();
	mach64.reg[M64_CONFIG_CNTL] = 2u | (((mach64.pci_lfb_base >> 22) << 4) & 0x3FF0);

	extern void PCI_AddSVGAMach64_Device(void);
	PCI_AddSVGAMach64_Device();
}

/* ---------------------------------------------------------------------- */
/* PCI configuration space [RRG chapter 7]                                  */
/* ---------------------------------------------------------------------- */

uint8_t Mach64_PCI_ConfigRead_Inner(uint8_t regnum);
uint8_t Mach64_PCI_ConfigRead(uint8_t regnum) {
	const uint8_t v = Mach64_PCI_ConfigRead_Inner(regnum);
	if (Mach64_Trace()) LOG_MSG("M64T PCI R %02x = %02x", regnum, v);
	return v;
}

uint8_t Mach64_PCI_ConfigRead_Inner(uint8_t regnum) {
	switch (regnum) {
		case 0x00: return 0x02;                     /* vendor 1002h */
		case 0x01: return 0x10;
		case 0x02: return 0x54;                     /* device 4754h 'GT' */
		case 0x03: return 0x47;
		case 0x04: return (uint8_t)(mach64.pci_command & 0x03);
		case 0x05: return 0;
		case 0x06: return 0x80;                     /* fast back-to-back capable */
		case 0x07: return 0x02;                     /* DEVSEL medium */
		case 0x08: return (uint8_t)(Mach64_ChipID() >> 24);   /* ASIC ID */
		case 0x09: return 0x00;
		case 0x0A: return (R(M64_CONFIG_CNTL) & (1u << 19)) ? 0x80 : 0x00;
		case 0x0B: return 0x03;                     /* display controller */
		case 0x10: return 0x00;                     /* BAR0: memory, 32-bit, not prefetchable */
		case 0x11: return 0x00;
		case 0x12: return 0x00;
		case 0x13: return (uint8_t)(mach64.pci_lfb_base >> 24);
		case 0x14: return (uint8_t)((mach64.pci_io_base & 0xFC) | 0x01); /* BAR1: I/O */
		case 0x15: return (uint8_t)(mach64.pci_io_base >> 8);
		case 0x16: return (uint8_t)(mach64.pci_io_base >> 16);
		case 0x17: return (uint8_t)(mach64.pci_io_base >> 24);
		case 0x2C: case 0x2D: case 0x2E: case 0x2F:
			return (uint8_t)(mach64.pci_adapter_id >> ((regnum - 0x2C) * 8));
		case 0x30: case 0x31: case 0x32: case 0x33:
			return (uint8_t)(mach64.pci_rom_bar >> ((regnum - 0x30) * 8));
		case 0x3C: return mach64.pci_int_line;
		case 0x3D: return 0x00;                     /* interrupt disabled by strap [RRG 7-4] */
		case 0x40: return mach64.pci_cfg40;
		default: return 0x00;
	}
}

void Mach64_PCI_ConfigWrite(uint8_t regnum, uint8_t val) {
	if (Mach64_Trace()) LOG_MSG("M64T PCI W %02x = %02x", regnum, val);
	switch (regnum) {
		case 0x04:
			if ((mach64.pci_command ^ val) & 0x3) {
				mach64.pci_command = val & 0x03;
				Mach64_UpdateIO();
				Mach64_UpdateMapping();
			}
			break;
		case 0x13:
			/* BAR0 is 16 MB: bits 23:0 read as zero */
			if ((uint8_t)(mach64.pci_lfb_base >> 24) != val) {
				mach64.pci_lfb_base = (uint32_t)val << 24;
				if (mach64.pci_lfb_base) S3_LFB_BASE = mach64.pci_lfb_base;
				Mach64_UpdateMapping();
			}
			break;
		case 0x15: case 0x16: case 0x17: {
			const unsigned int sh = (regnum - 0x14) * 8;
			const uint32_t nb = (mach64.pci_io_base & ~(0xFFu << sh)) | ((uint32_t)val << sh);
			if (nb != mach64.pci_io_base) {
				mach64.pci_io_base = nb & 0xFFFFFF00u;
				Mach64_UpdateIO();
			}
			break;
		}
		case 0x2C: case 0x2D: case 0x2E: case 0x2F: {
			const unsigned int sh = (regnum - 0x2C) * 8;
			mach64.pci_adapter_id = (mach64.pci_adapter_id & ~(0xFFu << sh)) | ((uint32_t)val << sh);
			break;
		}
		case 0x30: case 0x31: case 0x32: case 0x33: {
			/* the ROM is the emulated VGA BIOS at C0000; the BAR is stored only */
			const unsigned int sh = (regnum - 0x30) * 8;
			const uint32_t m = (regnum == 0x30) ? 0x01u : (regnum == 0x31) ? 0x80u : 0xFFu;
			mach64.pci_rom_bar = (mach64.pci_rom_bar & ~(0xFFu << sh)) | ((uint32_t)(val & m) << sh);
			break;
		}
		case 0x3C:
			mach64.pci_int_line = val;
			break;
		case 0x40:
			if ((mach64.pci_cfg40 ^ val) & 0x07) {
				mach64.pci_cfg40 = val & 0x07;
				Mach64_UpdateIO();
			}
			else mach64.pci_cfg40 = val & 0x07;
			break;
		default:
			break;
	}
}

/* ---------------------------------------------------------------------- */
/* Savestate                                                                */
/* ---------------------------------------------------------------------- */

void POD_Save_VGA_Mach64(std::ostream &stream) {
	WRITE_POD(&mach64, mach64);
}

void POD_Load_VGA_Mach64(std::istream &stream) {
	const uint32_t sparse = mach64.io_sparse_base_active, block = mach64.io_block_base_active;
	READ_POD(&mach64, mach64);
	if (!Mach64_Present()) return;
	/* the I/O handlers registered now are those of the running session */
	const uint32_t want_sparse = mach64.io_sparse_base_active, want_block = mach64.io_block_base_active;
	mach64.io_sparse_base_active = sparse;
	mach64.io_block_base_active = block;
	(void)want_sparse; (void)want_block;
	Mach64_UpdateIO();
	Mach64_UpdateMapping();
	Mach64_DACCntlChanged();
}
