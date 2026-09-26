/*
 *  ATI mach64 GT (3D RAGE) emulation: video BIOS support.
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
 *  References: mach64 BIOS Kit BIO-C012XX1-05 [BK] appendix A (extended ROM
 *  services, query structure); the ROM header tables as read by Linux atyfb
 *  (init_from_bios) and xf86-video-mach64 (atipreinit.c): ROM offset 48h
 *  points to the ROM table, whose word at +10h points to the clock table.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dosbox.h"
#include "logging.h"
#include "mem.h"
#include "inout.h"
#include "callback.h"
#include "regs.h"
#include "cpu.h"
#include "paging.h"
#include "int10.h"
#include "vga.h"
#include "vga_mach64.h"

extern bool vga_8bit_dac;

/* ---------------------------------------------------------------------- */
/* ROM tables                                                               */
/* ---------------------------------------------------------------------- */

/* Layout in the ROM header area (the rest of the ROM, from 100h, holds the
 * fonts and tables of the generic DOSBox-X VGA BIOS):
 *   048h  word: offset of the ROM table
 *   064h  far entry of the extended ROM services ([BK] A-11: "CALL xxxx:64h")
 *   0A0h  word: length of the ROM table (read at ROMTable-2)
 *   0A2h  ROM table (12h bytes; +10h: clock table offset)
 *   0C0h  clock table (20h bytes)
 */
static const unsigned int ROMTABLE = 0xA2;
static const unsigned int CLOCKTABLE = 0xC0;

static Bitu mach64_rom_cb = 0;
static uint16_t mach64_captable = 0;   /* ROM offset of the AL=07h table */

/* set while the services run from the ROM entry point, which ATI's Windows
 * driver calls directly from 16-bit protected mode */
static bool mach64_via_rom = false;

static Bitu Mach64_ROMServices_Callback(void) {
	/* the same services as INT 10h AH=A0h; AL selects the function */
	mach64_via_rom = true;
	Mach64_INT10_Extended();
	mach64_via_rom = false;
	return CBRET_NONE;
}

/* A far pointer passed by the caller: segment:offset, or selector:offset
 * from protected mode */
static PhysPt Mach64_FarPtr(uint16_t seg, uint16_t off) {
	if (cpu.pmode && !(reg_flags & FLAG_VM)) {
		Descriptor desc;
		if (!cpu.gdt.GetDescriptor(seg, desc)) return 0;
		return (PhysPt)(desc.GetBase() + off);
	}
	return PhysMake(seg, off);
}

/* The segment (or selector) of the ROM for pointers returned to the caller:
 * the ROM entry point runs in the caller's code segment for the ROM */
static uint16_t Mach64_ROMSeg(void) {
	return mach64_via_rom ? (uint16_t)SegValue(cs) : (uint16_t)0xC000;
}

void Mach64_SetupROM(uint32_t rom_base) {
	/* ROM table [atyfb]: length word before it, clock table pointer at +10h */
	phys_writew(rom_base + ROMTABLE - 2, 0x0012);
	for (unsigned int i = 0; i < 0x12; i++) phys_writeb(rom_base + ROMTABLE + i, 0);
	phys_writew(rom_base + 0x48, (uint16_t)ROMTABLE);
	phys_writew(rom_base + ROMTABLE + 0x10, (uint16_t)CLOCKTABLE);

	/* clock table (PLL_BLOCK_MACH64 in atyfb, ClockTable in xf86-video-mach64),
	 * frequencies in units of 10 kHz */
	for (unsigned int i = 0; i < 0x20; i++) phys_writeb(rom_base + CLOCKTABLE + i, 0);
	phys_writeb(rom_base + CLOCKTABLE + 0x00, 4);        /* programmable clock: internal PLL */
	phys_writew(rom_base + CLOCKTABLE + 0x02, 1000);     /* PCLK_min_freq 10 MHz */
	phys_writew(rom_base + CLOCKTABLE + 0x04, 13500);    /* PCLK_max_freq 135 MHz [RRG B-5] */
	phys_writeb(rom_base + CLOCKTABLE + 0x06, 3);        /* clock number to programme */
	phys_writew(rom_base + CLOCKTABLE + 0x08, 1432);     /* reference 14.32 MHz */
	phys_writew(rom_base + CLOCKTABLE + 0x0A, 0x36);     /* reference divider */
	phys_writew(rom_base + CLOCKTABLE + 0x0E, 4000);     /* MCLK power down */
	phys_writew(rom_base + CLOCKTABLE + 0x10, 6000);     /* MCLK max 60 MHz */
	phys_writew(rom_base + CLOCKTABLE + 0x12, 6000);     /* XCLK max */
	phys_writew(rom_base + CLOCKTABLE + 0x14, 6000);     /* SCLK */

	/* extended services entry: callback + RETF */
	if (mach64_rom_cb == 0) {
		mach64_rom_cb = CALLBACK_Allocate();
		CALLBACK_Setup(mach64_rom_cb, Mach64_ROMServices_Callback, CB_RETF, "ATI mach64 extended ROM services");
	}
	phys_writeb(rom_base + 0x64, 0xFE);
	phys_writeb(rom_base + 0x65, 0x38);
	phys_writew(rom_base + 0x66, (uint16_t)mach64_rom_cb);
	phys_writeb(rom_base + 0x68, 0xCB);

	/* SCRATCH_REG1: ROM segment location, (reg & 7Fh) * 80h + C000h [BK A-11] */
	Mach64_IOWriteReg(M64_SCRATCH_REG1, 0);

	/* CFG_MEM_VGA_AP_EN: the ATI BIOS leaves the registers mapped at
	 * B000:F800-FFFF (the hardware default is off [RRG 5-3]); ATI's Windows 9x
	 * driver polls FIFO_STAT there without enabling it */
	Mach64_IOWriteReg(M64_CONFIG_CNTL, Mach64_IOReadReg(M64_CONFIG_CNTL) | 0x4);
}

/* Hardware capability list for AL=07h [PRG-215R3 A-5, BK A-13]: rows of
 * H_DISP (characters), DACMASK (1 shl dactype), MEMREQ ((DRAM requirement shl
 * 4) or VRAM requirement, in memory size codes), maximum dot clock in MHz and
 * pixel width (colour depth code as in AL=00h CL), ending with H_DISP 0. The
 * format type and the row length sit in the two bytes before the table.
 * ATI_HAL.DLL takes the first row whose H_DISP is not below the mode's and
 * then checks only the rows of that H_DISP, so the rows go from the narrowest
 * mode up (the deepest colour first). Returns the bytes used. */
static uint8_t MemCodeFor(uint32_t bytes) {
	static const uint32_t kb[] = { 512, 1024, 2048, 4096, 6144, 8192 };
	for (unsigned int i = 0; i < 6; i++) if (bytes <= kb[i] * 1024u) return (uint8_t)i;
	return 0xF;
}

unsigned int Mach64_SetupCapabilityTable(uint32_t rom_base, unsigned int at) {
	static const unsigned int widths[] = { 640, 800, 1024, 1280, 1600 };
	static const unsigned int heights[] = { 480, 600, 768, 1024, 1200 };
	static const struct { uint8_t code, bytespp; } depths[] = { { 6, 4 }, { 5, 3 }, { 4, 2 }, { 3, 2 }, { 2, 1 } };
	unsigned int n = at;
	phys_writeb(rom_base + n++, 0);        /* format type 0: DACMASK */
	phys_writeb(rom_base + n++, 5);        /* bytes per row */
	mach64_captable = (uint16_t)n;
	for (unsigned int r = 0; r < 5; r++) {
		for (const auto &d : depths) {
			const uint8_t m = MemCodeFor(widths[r] * heights[r] * d.bytespp);
			if (m == 0xF) continue;
			phys_writeb(rom_base + n++, (uint8_t)(widths[r] / 8u));
			phys_writeb(rom_base + n++, 0xFF);                 /* any DAC type */
			phys_writeb(rom_base + n++, (uint8_t)((m << 4) | m));
			phys_writeb(rom_base + n++, 135);                  /* PCLK_max_freq [RRG B-5] */
			phys_writeb(rom_base + n++, d.code);
		}
	}
	for (unsigned int i = 0; i < 5; i++) phys_writeb(rom_base + n++, 0);
	return n - at;
}

/* ---------------------------------------------------------------------- */
/* Mode setting                                                             */
/* ---------------------------------------------------------------------- */

static unsigned int PixWidthOf(VGAModes type) {
	switch (type) {
		case M_PACKED4: return M64_PIX_4BPP;
		case M_LIN15:   return M64_PIX_15BPP;
		case M_LIN16:   return M64_PIX_16BPP;
		case M_LIN24:   return M64_PIX_24BPP;
		case M_LIN32:   return M64_PIX_32BPP;
		case M_LIN8:    return M64_PIX_8BPP;
		default:        return 0;
	}
}

/* Programs VCLK entry 3 of the PLL for 'hz' [RRG B-6]. */
static void SetPixelClock(Bitu hz) {
	const double R = 14318180.0;
	const unsigned int M = 0x36;
	const double Q = ((double)hz * M) / (2.0 * R);
	unsigned int P = 8, pcode = 3;
	if (Q >= 127.5)     { P = 1; pcode = 0; }
	else if (Q >= 63.5) { P = 2; pcode = 1; }
	else if (Q >= 31.5) { P = 4; pcode = 2; }
	unsigned int N = (unsigned int)(Q * P + 0.5);
	if (N < 0x80) N = 0x80;
	if (N > 0xFF) N = 0xFF;

	auto pll_write = [](unsigned int a, unsigned int d) {
		const uint32_t cc = Mach64_IOReadReg(M64_CLOCK_CNTL) & 0x3u;
		Mach64_IOWriteReg(M64_CLOCK_CNTL, cc | (a << 10) | (1u << 9) | (d << 16));
	};
	auto pll_read = [](unsigned int a) -> unsigned int {
		const uint32_t cc = Mach64_IOReadReg(M64_CLOCK_CNTL) & 0x3u;
		Mach64_IOWriteReg(M64_CLOCK_CNTL, cc | (a << 10));
		return (Mach64_IOReadReg(M64_CLOCK_CNTL) >> 16) & 0xFF;
	};
	pll_write(2, M);                                     /* PLL_REF_DIV */
	pll_write(6, (pll_read(6) & 0x3F) | (pcode << 6));   /* VCLK_POST_DIV for VCLK3 */
	pll_write(10, N);                                    /* VCLK3_FB_DIV */
	pll_write(5, (pll_read(5) & ~0x07u) | 0x03);         /* VCLK_SRC_SEL = PLLVCLK, PLL running */
	Mach64_IOWriteReg(M64_CLOCK_CNTL, 3);                /* CLOCK_SEL 3, write disabled */
}

static void Mach64_DbgDriverVars(const char *why);
void Mach64_INT10_SetMode(const VideoModeBlock *mode) {
	if (getenv("DOSBOX_X_M64TRACE")) {
		LOG_MSG("M64BIOS setmode %03x %ux%u type %d", (unsigned)mode->mode, (unsigned)mode->swidth, (unsigned)mode->sheight, (int)mode->type);
		Mach64_DbgDriverVars("setmode");
		extern void Mach64_CovDump(const char *why);
		Mach64_CovDump("setmode");
	}
	if (mode->mode == 0x12 && getenv("DOSBOX_X_RAMDUMP")) {   /* TEST HOOK: guest RAM at each mode 12h set */
		extern HostPt MemBase;
		static unsigned int n = 0;
		char fn[512];
		snprintf(fn, sizeof(fn), "%s.%u", getenv("DOSBOX_X_RAMDUMP"), n++);
		FILE *f = fopen(fn, "wb");
		if (f) { fwrite(MemBase, 4096, MEM_TotalPages(), f); fclose(f); LOG_MSG("M64BIOS RAM dumped to %s", fn); }
	}
	const unsigned int pw = PixWidthOf(mode->type);
	const bool ext = pw != 0 && mode->mode > 0x13;

	if (!ext) {
		/* VGA display: the accelerator CRTC is off; 6-bit DAC */
		Mach64_IOWriteReg(M64_CRTC_GEN_CNTL, Mach64_IOReadReg(M64_CRTC_GEN_CNTL) & ~(M64_CRTC_EXT_DISP_EN | M64_CRTC_ENABLE));
		Mach64_IOWriteReg(M64_DAC_CNTL, Mach64_IOReadReg(M64_DAC_CNTL) & ~(1u << 8));
		Mach64_IOWriteReg(M64_MEM_VGA_WP_SEL, 0x00010000);
		Mach64_IOWriteReg(M64_MEM_VGA_RP_SEL, 0x00010000);
		return;
	}

	const unsigned int w = mode->swidth, h = mode->sheight;
	const bool dbl = (mode->vdispend >= 2 * h);           /* 200-line modes scan every line twice */
	const unsigned int vdisp = dbl ? 2 * h : h;
	unsigned int vtot = mode->vtotal;
	if (vtot < vdisp + 8) vtot = vdisp + vdisp / 10 + 8;

	/* horizontal in characters: display, sync after a small front porch, total ~1.25x */
	const unsigned int hd = w / 8u;
	unsigned int ht = (hd * 5u) / 4u;
	if (ht < hd + 8) ht = hd + 8;
	const unsigned int hss = hd + 2, hsw = (ht - hd) / 2 > 1 ? (ht - hd) / 2 : 1;
	const unsigned int vss = vdisp + (vtot - vdisp) / 4, vsw = 2;
	const unsigned int refresh = (w >= 640) ? 60 : 70;
	const Bitu clock = (Bitu)ht * 8u * vtot * refresh;

	/* blank the display while it is reprogrammed */
	Mach64_IOWriteReg(M64_CRTC_GEN_CNTL, Mach64_IOReadReg(M64_CRTC_GEN_CNTL) | M64_CRTC_DISPLAY_DIS);

	Mach64_IOWriteReg(M64_CRTC_H_TOTAL_DISP, ((hd - 1) << 16) | (ht - 1));
	Mach64_IOWriteReg(M64_CRTC_H_SYNC_STRT_WID, ((hsw & 0x1F) << 16) | ((hss - 1) & 0xFF) | ((((hss - 1) >> 8) & 1) << 12));
	Mach64_IOWriteReg(M64_CRTC_V_TOTAL_DISP, ((vdisp - 1) << 16) | (vtot - 1));
	Mach64_IOWriteReg(M64_CRTC_V_SYNC_STRT_WID, (vsw << 16) | (vss - 1));
	Mach64_IOWriteReg(M64_OVR_WID_LEFT_RIGHT, 0);
	Mach64_IOWriteReg(M64_OVR_WID_TOP_BOTTOM, 0);
	Mach64_IOWriteReg(M64_OVR_CLR, 0);

	const unsigned int pitch_px = (mode->pitch != 0) ? mode->pitch : w;
	Mach64_IOWriteReg(M64_CRTC_OFF_PITCH, (((pitch_px + 7) / 8) & 0x3FF) << 22);

	SetPixelClock(clock);

	/* 8 bpp uses the palette with a 6-bit DAC, as VGA software expects */
	Mach64_IOWriteReg(M64_DAC_CNTL, Mach64_IOReadReg(M64_DAC_CNTL) & ~(1u << 8));

	/* the CPU window pages through video memory from 0 */
	Mach64_IOWriteReg(M64_MEM_VGA_WP_SEL, 0x00010000);
	Mach64_IOWriteReg(M64_MEM_VGA_RP_SEL, 0x00010000);

	uint32_t gen = (pw << 8) | M64_CRTC_EXT_DISP_EN | M64_CRTC_ENABLE;
	if (dbl) gen |= M64_CRTC_DBL_SCAN_EN;
	gen |= (1u << 30);                                   /* VGA_XCRT_CNT_EN */
	Mach64_IOWriteReg(M64_CRTC_GEN_CNTL, gen);

	/* draw engine defaults for the mode (the BIOS kit's mode set leaves the
	 * engine for the driver; this is only for DOS software that assumes a
	 * sane state) */
	const uint32_t dp_pw = (pw == M64_PIX_24BPP) ? M64_PIX_8BPP : pw;
	const uint32_t pitch_eng = (pw == M64_PIX_24BPP) ? pitch_px * 3 : pitch_px;
	Mach64_WriteReg(M64_DST_OFF_PITCH, (((pitch_eng + 7) / 8) & 0x3FF) << 22, 0xFFFFFFFFu);
	Mach64_WriteReg(M64_SRC_OFF_PITCH, (((pitch_eng + 7) / 8) & 0x3FF) << 22, 0xFFFFFFFFu);
	Mach64_WriteReg(M64_DP_PIX_WIDTH, dp_pw | (dp_pw << 8) | (dp_pw << 16) | (dp_pw << 28), 0xFFFFFFFFu);
	Mach64_WriteReg(M64_SC_LEFT_RIGHT, (((pitch_eng - 1) & 0x1FFF) << 16), 0xFFFFFFFFu);
	Mach64_WriteReg(M64_SC_TOP_BOTTOM, ((0x7FFF) << 16), 0xFFFFFFFFu);
	Mach64_WriteReg(M64_DP_WRITE_MSK, 0xFFFFFFFFu, 0xFFFFFFFFu);
	Mach64_WriteReg(M64_DST_CNTL, M64_DST_X_DIR | M64_DST_Y_DIR, 0xFFFFFFFFu);
}

void Mach64_INT10_SetPitch(uint32_t pitch_pixels) {
	const uint32_t offp = Mach64_IOReadReg(M64_CRTC_OFF_PITCH);
	Mach64_IOWriteReg(M64_CRTC_OFF_PITCH, (offp & 0x000FFFFFu) | (((pitch_pixels / 8u) & 0x3FF) << 22));
}

void Mach64_INT10_SetDisplayStart(uint32_t byte_offset) {
	const uint32_t offp = Mach64_IOReadReg(M64_CRTC_OFF_PITCH);
	Mach64_IOWriteReg(M64_CRTC_OFF_PITCH, (offp & 0xFFC00000u) | ((byte_offset >> 3) & 0xFFFFF));
}

uint32_t Mach64_INT10_GetDisplayStart(void) {
	return (Mach64_IOReadReg(M64_CRTC_OFF_PITCH) & 0xFFFFF) << 3;
}

void Mach64_INT10_SetBank(unsigned int bank) {
	/* VESA window granularity 64 KB = two 32 KB pages */
	const uint32_t p = (uint32_t)(bank * 2u) & 0xFF;
	const uint32_t v = p | (((p + 1) & 0xFF) << 16);
	Mach64_IOWriteReg(M64_MEM_VGA_WP_SEL, v);
	Mach64_IOWriteReg(M64_MEM_VGA_RP_SEL, v);
}

unsigned int Mach64_INT10_GetBank(void) {
	return (Mach64_IOReadReg(M64_MEM_VGA_WP_SEL) & 0xFF) / 2u;
}

/* ---------------------------------------------------------------------- */
/* Extended ROM services [BK A-11..A-17]                                    */
/* ---------------------------------------------------------------------- */

/* Resolution codes (AL=0, CH) and the CRTC timings the mode tables report
 * (VESA DMT 60 Hz timings; [BK A-20] mode table layout). Horizontal values
 * in pixels, vertical in lines, dot clock in 10 kHz. */
struct Mach64_Res {
	uint8_t code; uint16_t w, h;
	uint16_t htotal, hss, hsw, vtotal, vss, vsw, clock;
	bool hneg, vneg;
};
static const Mach64_Res mach64_res[] = {
	{ 0x12,  640,  480,  800,  656,  96,  525,  490, 2,  2518, true,  true  },
	{ 0x6A,  800,  600, 1056,  840, 128,  628,  601, 4,  4000, false, false },
	{ 0x55, 1024,  768, 1344, 1048, 136,  806,  771, 6,  6500, true,  true  },
	{ 0x83, 1280, 1024, 1688, 1328, 112, 1066, 1025, 3, 10800, false, false },
};

/* A000h: program the accelerator CRTC [PRG-215R3 A-2] from the built-in
 * timings (CH = resolution code) or from a table in the caller's memory
 * (CH = 81h): the mode table layout of AL=09h [BK A-20] from its offset 8 on,
 * CRTC_H_TOTAL at +06h (as ATI_HAL.DLL passes it). CL[3:0] is the
 * pixel depth (the CRTC_PIX_WIDTH codes), CL[4] the 8-bit DAC, CL[7:6] the
 * pitch: 0 = 1024, 1 = unchanged, 2 = the display width. The display stays
 * as it is until AL=01h. */
static bool Mach64_BIOSTrace(void);
static bool Mach64_LoadCRTC(uint8_t ch, uint8_t cl, PhysPt table) {
	const unsigned int depth = cl & 0xF;
	if (depth < M64_PIX_4BPP || depth > M64_PIX_32BPP) return false;
	unsigned int ht, hd, hss, hsw, vt, vd, vss, vsw, hdly = 0;
	uint16_t flags = 0;
	uint32_t clock_10k;
	if (ch == 0x81) {
		if (!table) return false;
		table -= 8;                 /* read it with the mode table offsets */
		if (Mach64_BIOSTrace()) {
			char hex[100]; size_t n = 0;
			for (unsigned int i = 8; i < 0x24; i++) n += (size_t)snprintf(hex + n, sizeof(hex) - n, "%02x", (unsigned)mem_readb(table + i));
			LOG_MSG("M64BIOS A000 table %s", hex);
		}
		flags = mem_readw(table + 0x0C);
		ht = mem_readb(table + 0x0E); hd = mem_readb(table + 0x0F);
		hss = mem_readb(table + 0x10); hsw = mem_readb(table + 0x11);
		vt = mem_readw(table + 0x12); vd = mem_readw(table + 0x14);
		vss = mem_readw(table + 0x16); vsw = mem_readb(table + 0x18);
		clock_10k = mem_readw(table + 0x1A);
		hdly = (mem_readw(table + 0x1C) >> 8) & 7;
	}
	else {
		const Mach64_Res *r = NULL;
		for (const auto &x : mach64_res) if (x.code == ch) r = &x;
		if (!r) return false;
		ht = r->htotal / 8 - 1; hd = r->w / 8 - 1; hss = r->hss / 8 - 1;
		hsw = (r->hsw / 8) | (r->hneg ? 0x20 : 0);
		vt = r->vtotal - 1u; vd = r->h - 1u; vss = r->vss - 1u;
		vsw = r->vsw | (r->vneg ? 0x20 : 0);
		clock_10k = r->clock;
	}
	if (hd == 0 || vd == 0 || ht < hd || vt < vd) return false;

	uint32_t gen = Mach64_IOReadReg(M64_CRTC_GEN_CNTL);
	Mach64_IOWriteReg(M64_CRTC_GEN_CNTL, gen | M64_CRTC_DISPLAY_DIS);
	Mach64_IOWriteReg(M64_CRTC_H_TOTAL_DISP, (hd << 16) | ht);
	Mach64_IOWriteReg(M64_CRTC_H_SYNC_STRT_WID, (hss & 0xFF) | (hdly << 8) | ((hsw & 0x1F) << 16) | ((hsw & 0x20) ? (1u << 21) : 0));
	Mach64_IOWriteReg(M64_CRTC_V_TOTAL_DISP, (vd << 16) | vt);
	Mach64_IOWriteReg(M64_CRTC_V_SYNC_STRT_WID, vss | ((vsw & 0x1F) << 16) | ((vsw & 0x20) ? (1u << 21) : 0));
	Mach64_IOWriteReg(M64_OVR_WID_LEFT_RIGHT, 0);
	Mach64_IOWriteReg(M64_OVR_WID_TOP_BOTTOM, 0);

	const unsigned int pitchsel = (cl >> 6) & 3;
	if (pitchsel != 1) {
		const uint32_t pitch_px = (pitchsel == 0) ? 1024u : (hd + 1u) * 8u;
		Mach64_IOWriteReg(M64_CRTC_OFF_PITCH, ((pitch_px / 8u) & 0x3FF) << 22);
	}
	if (clock_10k) SetPixelClock((Bitu)clock_10k * 10000u);
	if (cl & 0x10) Mach64_IOWriteReg(M64_DAC_CNTL, Mach64_IOReadReg(M64_DAC_CNTL) | (1u << 8));

	gen &= ~(0x700u | M64_CRTC_DBL_SCAN_EN | M64_CRTC_INTERLACE_EN | M64_CRTC_DISPLAY_DIS);
	gen |= (depth << 8) | M64_CRTC_ENABLE;
	if (flags & 0x100) gen |= M64_CRTC_DBL_SCAN_EN;
	if (flags & 0x200) gen |= M64_CRTC_INTERLACE_EN;
	Mach64_IOWriteReg(M64_CRTC_GEN_CNTL, gen);
	return true;
}

/* AL=01h: accelerator (coprocessor) or VGA display, and the DAC width */
static void Mach64_SetDisplay(bool coprocessor, bool dac8) {
	uint32_t gen = Mach64_IOReadReg(M64_CRTC_GEN_CNTL);
	if (coprocessor) gen |= M64_CRTC_EXT_DISP_EN | M64_CRTC_ENABLE;
	else gen &= ~M64_CRTC_EXT_DISP_EN;
	Mach64_IOWriteReg(M64_CRTC_GEN_CNTL, gen);
	const uint32_t dac = Mach64_IOReadReg(M64_DAC_CNTL);
	Mach64_IOWriteReg(M64_DAC_CNTL, (coprocessor && dac8) ? (dac | (1u << 8)) : (dac & ~(1u << 8)));
}

static VGAModes ModeTypeOfDepth(unsigned int cl) {
	switch (cl & 0xF) {
		case 1: return M_PACKED4;
		case 2: return M_LIN8;
		case 3: return M_LIN15;
		case 4: return M_LIN16;
		case 5: return M_LIN24;
		case 6: return M_LIN32;
		default: return M_ERROR;
	}
}

/* ASIC identification for the query functions: [7:0] revision, [15:8] type */
static uint16_t AsicID(void) {
	/* 4700h = GT-A2 [PRG-215R3 table A-6]; ATI's driver (macxw4.drv 4.35) only
	 * reports 3D (ATI3DCIF) for the IDs in that table */
	return 0x4700;
}

static uint8_t MemSizeCode(void) {
	if (vga.mem.memsize <= 512*1024) return 0;
	if (vga.mem.memsize <= 1024*1024) return 1;
	if (vga.mem.memsize <= 2048*1024) return 2;
	if (vga.mem.memsize <= 4096*1024) return 3;
	return 5;
}

static uint16_t ApertureMB(void) {
	uint32_t base; bool en;
	Mach64_LFBInfo(base, en);
	return (uint16_t)(base >> 20);
}

static void BuildQuery(PhysPt buf, bool with_modes, unsigned int &size) {
	const unsigned int nmodes = with_modes ? (unsigned int)(sizeof(mach64_res) / sizeof(mach64_res[0])) : 0;
	size = 0x20 + nmodes * 0x24;
	if (!buf) return;
	for (unsigned int i = 0; i < size; i++) mem_writeb(buf + i, 0);
	mem_writew(buf + 0x00, (uint16_t)size);
	mem_writeb(buf + 0x02, 1);                       /* revision */
	mem_writeb(buf + 0x03, (uint8_t)nmodes);
	mem_writew(buf + 0x04, 0x20);                    /* offset to the mode tables */
	mem_writeb(buf + 0x06, 0x24);                    /* size of a mode table */
	mem_writeb(buf + 0x07, 1);                       /* VGA enabled */
	mem_writew(buf + 0x08, AsicID());
	mem_writeb(buf + 0x0A, 0);                       /* VGA boundary: full access */
	mem_writeb(buf + 0x0B, MemSizeCode());
	mem_writeb(buf + 0x0C, 0x00);                    /* internal DAC */
	mem_writeb(buf + 0x0D, 2);                       /* EDO DRAM */
	mem_writeb(buf + 0x0E, 7);                       /* PCI */
	mem_writeb(buf + 0x0F, 0);
	mem_writew(buf + 0x10, ApertureMB());
	mem_writeb(buf + 0x12, 0);
	mem_writeb(buf + 0x13, 0x42);                    /* 8M aperture, predefined (see AL=6) */
	mem_writeb(buf + 0x14, 0x80 | 0x20 | 0x04 | 0x02 | 0x01); /* 32bpp xRGB, RGB 24bpp, 555, 565 */
	mem_writeb(buf + 0x15, 0x60);                    /* gamma correction, 256 greyscale */
	mem_writeb(buf + 0x16, (mach64.pci_cfg40 & 4) ? 1 : 0); /* I/O address type (AL=12h) */
	uint32_t iobase = (mach64.pci_cfg40 & 4) ? (mach64.pci_io_base & 0xFF00) : 0x2EC;
	mem_writew(buf + 0x18, (uint16_t)iobase);
	for (unsigned int m = 0; m < nmodes; m++) {
		const Mach64_Res &r = mach64_res[m];
		const PhysPt t = buf + 0x20 + m * 0x24;
		/* deepest colour that fits in video memory */
		uint8_t depth = 6;
		static const unsigned int bytespp[7] = { 0, 1, 1, 2, 2, 3, 4 };
		while (depth > 2 && (uint32_t)r.w * r.h * bytespp[depth] > vga.mem.memsize) depth--;
		mem_writew(t + 0x00, r.w);
		mem_writew(t + 0x02, r.h);
		mem_writeb(t + 0x04, depth);
		mem_writeb(t + 0x05, r.code);
		mem_writew(t + 0x06, 0);                     /* not from the EEPROM */
		mem_writew(t + 0x0C, 0);                     /* no interlace, double scan, ... */
		mem_writeb(t + 0x0E, (uint8_t)(r.htotal / 8 - 1));       /* CRTC_H_TOTAL */
		mem_writeb(t + 0x0F, (uint8_t)(r.w / 8 - 1));            /* CRTC_H_DISP */
		mem_writeb(t + 0x10, (uint8_t)(r.hss / 8 - 1));          /* CRTC_H_SYNC_STRT */
		mem_writeb(t + 0x11, (uint8_t)((r.hsw / 8) | (r.hneg ? 0x20 : 0))); /* CRTC_H_SYNC_WID, _POL */
		mem_writew(t + 0x12, (uint16_t)(r.vtotal - 1));          /* CRTC_V_TOTAL */
		mem_writew(t + 0x14, (uint16_t)(r.h - 1));               /* CRTC_V_DISP */
		mem_writew(t + 0x16, (uint16_t)(r.vss - 1));             /* CRTC_V_SYNC_STRT */
		mem_writeb(t + 0x18, (uint8_t)(r.vsw | (r.vneg ? 0x20 : 0))); /* CRTC_V_SYNC_WID, _POL */
		mem_writeb(t + 0x19, 3);                     /* CLOCK_CNTL: the programmable clock */
		mem_writew(t + 0x1A, r.clock);
	}
}

static bool FindMode(unsigned int w, unsigned int h, VGAModes type, Bitu &modenum) {
	extern VideoModeBlock ModeList_VGA[];
	for (unsigned int i = 0; ModeList_VGA[i].mode != 0xFFFF; i++) {
		const VideoModeBlock &b = ModeList_VGA[i];
		if (b.mode >= 0x100 && b.swidth == w && b.sheight == h && b.type == type) { modenum = b.mode; return true; }
	}
	return false;
}

/* TEST HOOK (not for upstream): log the BIOS calls while DOSBOX_X_M64TRACE is set */
static bool Mach64_BIOSTrace(void) {
	static int on = -1;
	if (on < 0) on = getenv("DOSBOX_X_M64TRACE") != NULL;
	return on != 0;
}

/* TEST HOOK: ati_m64.drv's data segment, from its A009h buffer at DS:3FF8h */
static PhysPt m64_dbg_ds = 0;
/* linear to physical through the guest page tables, without faulting */
static bool Mach64_DbgLinToPhys(PhysPt lin, uint32_t &phys) {
	if (!paging.enabled) { phys = lin; return true; }
	const uint32_t pde = phys_readd((paging.cr3 & 0xFFFFF000u) + ((lin >> 22) << 2));
	if (!(pde & 1)) return false;
	if (pde & 0x80) { phys = (pde & 0xFFC00000u) | (lin & 0x3FFFFFu); return true; }
	const uint32_t pte = phys_readd((pde & 0xFFFFF000u) + (((lin >> 12) & 0x3FF) << 2));
	if (!(pte & 1)) return false;
	phys = (pte & 0xFFFFF000u) | (lin & 0xFFFu);
	return true;
}
static uint32_t Mach64_DbgRead(PhysPt lin, unsigned int len) {
	uint32_t v = 0;
	for (unsigned int i = 0; i < len; i++) {
		uint32_t p;
		if (!Mach64_DbgLinToPhys(lin + i, p)) return 0xDEADDEAD;
		v |= (uint32_t)phys_readb(p) << (8 * i);
	}
	return v;
}
static void Mach64_DbgDriverVars(const char *why) {
	if (!Mach64_BIOSTrace() || m64_dbg_ds == 0) return;
	static const uint16_t offs[] = { 0x2831, 0x2b32, 0x2b34, 0x2b36, 0x27fb, 0x27fd, 0x27ff, 0x2a68, 0x2992, 0x2b93, 0x2b95, 0x2b4b, 0x4003, 0x4bbc, 0x3e07, 0x2b68 };
	char buf[512]; size_t n = 0;
	for (auto o : offs) n += (size_t)snprintf(buf + n, sizeof(buf) - n, " [%04x]=%04x", o, (unsigned)Mach64_DbgRead(m64_dbg_ds + o, 2));
	LOG_MSG("M64DRV %s ds=%08x%s [3dca]=%08x", why, (unsigned)m64_dbg_ds, buf, (unsigned)Mach64_DbgRead(m64_dbg_ds + 0x3dca, 4));
}

void Mach64_INT10_Extended(void) {
	const uint8_t fn = reg_al;
	if (Mach64_BIOSTrace()) {
		const PhysPt stk = (PhysPt)SegPhys(ss) + (cpu.stack.big ? reg_esp : reg_sp);
		LOG_MSG("M64BIOS A0%02X bx=%04x cx=%04x dx=%04x%s cs=%04x:%04x ds=%04x ret=%04x:%04x", fn, (unsigned)reg_bx, (unsigned)reg_cx, (unsigned)reg_dx,
			mach64_via_rom ? " (ROM entry)" : "", (unsigned)SegValue(cs), (unsigned)reg_ip, (unsigned)SegValue(ds),
			mach64_via_rom ? (unsigned)Mach64_DbgRead(stk + 2, 2) : 0u, mach64_via_rom ? (unsigned)Mach64_DbgRead(stk, 2) : 0u);
		Mach64_DbgDriverVars("call");
		static bool dumped = false;
		if (mach64_via_rom && !dumped) {
			dumped = true;
			const uint16_t rcs = (uint16_t)Mach64_DbgRead(stk + 2, 2), rip = (uint16_t)Mach64_DbgRead(stk, 2);
			Descriptor desc;
			if (cpu.gdt.GetDescriptor(rcs, desc)) {
				char hex[200]; size_t n = 0;
				for (int i = -48; i < 16; i++) n += (size_t)snprintf(hex + n, sizeof(hex) - n, "%02x", (unsigned)(Mach64_DbgRead((PhysPt)desc.GetBase() + (uint16_t)(rip + i), 1) & 0xFF));
				LOG_MSG("M64DRV caller code base %08x limit %x bytes[-48..16] %s", (unsigned)desc.GetBase(), (unsigned)desc.GetLimit(), hex);
			}
			if (cpu.gdt.GetDescriptor(SegValue(ds), desc)) {
				char hex[200]; size_t n = 0;
				for (int i = 0; i < 64; i++) n += (size_t)snprintf(hex + n, sizeof(hex) - n, "%02x", (unsigned)(Mach64_DbgRead((PhysPt)desc.GetBase() + i, 1) & 0xFF));
				LOG_MSG("M64DRV caller ds base %08x limit %x bytes[0..64] %s", (unsigned)desc.GetBase(), (unsigned)desc.GetLimit(), hex);
			}
		}
		if (mach64_via_rom && m64_dbg_ds == 0 && fn == 0x09) {
			/* the driver calls with its own DS loaded: check for " 761295520" at DS:29EEh */
			const PhysPt dsb = (PhysPt)SegPhys(ds);
			LOG_MSG("M64DRV A009 caller ds=%04x base %08x, sig %08x", (unsigned)SegValue(ds), (unsigned)dsb, (unsigned)Mach64_DbgRead(dsb + 0x29ee, 4));
			if (Mach64_DbgRead(dsb + 0x29ee, 4) == 0x31363720u) m64_dbg_ds = dsb;
		}
	}
	switch (fn) {
		case 0x00: { /* load coprocessor CRTC parameters */
			const PhysPt table = (reg_ch == 0x81) ? Mach64_FarPtr(reg_dx, reg_bx) : 0;
			reg_ah = Mach64_LoadCRTC(reg_ch, reg_cl, table) ? 0 : 1;
			break;
		}
		case 0x02: { /* ... and set the display mode */
			if (reg_ch == 0x81 || reg_ch == 0x80) {
				const PhysPt table = (reg_ch == 0x81) ? Mach64_FarPtr(reg_dx, reg_bx) : 0;
				if (!Mach64_LoadCRTC(reg_ch, reg_cl, table)) { reg_ah = 1; break; }
				Mach64_SetDisplay(true, (reg_cl & 0x10) != 0);
				reg_cl &= ~0x20;
				reg_ah = 0;
				break;
			}
			/* a built-in mode: the VBE mode of the same resolution and depth, so
			 * that the rest of the BIOS knows about it */
			unsigned int w = 0, h = 0;
			for (const auto &r : mach64_res) if (r.code == reg_ch) { w = r.w; h = r.h; }
			const VGAModes t = ModeTypeOfDepth(reg_cl);
			Bitu modenum;
			if (w == 0 || t == M_ERROR || !FindMode(w, h, t, modenum)) { reg_ah = 1; break; }
			if (!INT10_SetVideoMode((uint16_t)modenum)) { reg_ah = 1; break; }
			reg_ah = 0;
			break;
		}
		case 0x01:   /* set display mode: CL bit 0 = coprocessor, bit 7 = 8-bit DAC */
			Mach64_SetDisplay((reg_cl & 1) != 0, (reg_cl & 0x80) != 0);
			reg_cl &= ~0x20;                         /* CRTC parameters not doubled */
			reg_ah = 0;
			break;
		case 0x03:   /* read EEPROM: no EEPROM contents */
			reg_bx = 0;
			reg_ah = 0;
			break;
		case 0x04:   /* write EEPROM */
			reg_ah = 0;
			break;
		case 0x05:   /* memory aperture service: the PCI aperture is always enabled */
			reg_ah = 0;
			break;
		case 0x06: { /* short query */
			reg_al = 0x02 | 0x40;                    /* 8M aperture, address predefined */
			reg_bx = ApertureMB();
			reg_ch = 0x80 | 0x20 | 0x04 | 0x02 | 0x01;
			reg_cl = MemSizeCode();
			reg_dx = AsicID();
			reg_ah = 0;
			break;
		}
		case 0x07:   /* hardware capability list */
			if (mach64_captable == 0) { reg_ah = 2; break; }
			reg_dx = Mach64_ROMSeg();
			reg_bx = mach64_captable;
			reg_al = 0;
			reg_ah = 0;
			break;
		case 0x08: { /* size of the query structure */
			unsigned int size;
			BuildQuery(0, (reg_cl & 1) != 0, size);
			reg_cx = (uint16_t)size;
			reg_ah = 0;
			break;
		}
		case 0x09: { /* query device into DX:BX */
			unsigned int size;
			const PhysPt buf = Mach64_FarPtr(reg_dx, reg_bx);
			if (buf == 0) { reg_ah = 1; break; }
			BuildQuery(buf, (reg_cl & 1) != 0, size);
			reg_ah = 0;
			break;
		}
		case 0x0A:   /* clock chip frequency table */
			reg_al = 4;                              /* internal PLL */
			reg_dx = Mach64_ROMSeg();
			reg_bx = 0x0000;                         /* no fixed frequency table */
			reg_cx = (uint16_t)CLOCKTABLE;
			reg_ah = 0;
			break;
		case 0x0B:   /* program clock entry: BX in 10 kHz */
			SetPixelClock((Bitu)reg_bx * 10000u);
			reg_al = 4;
			reg_ah = 0;
			break;
		case 0x0C:   /* DPMS set */
			Mach64_IOWriteReg(M64_CRTC_GEN_CNTL, (Mach64_IOReadReg(M64_CRTC_GEN_CNTL) & ~0x0Cu) |
				(((reg_cl & 3) == 1 || (reg_cl & 3) == 3) ? 0x04u : 0) | (((reg_cl & 3) >= 2) ? 0x08u : 0));
			reg_ah = 0;
			break;
		case 0x0D:   /* DPMS get */
			reg_cl = 0;
			reg_ah = 0;
			break;
		case 0x0E: case 0x0F: case 0x10:
			reg_cl = 0;
			reg_ah = 0;
			break;
		case 0x11:   /* external storage: none */
			reg_cl = 0x80;
			reg_ch = 0;
			reg_dx = 0;
			reg_bx = 0;
			reg_ah = 0;
			break;
		case 0x12:   /* short query: I/O base and type */
			if (mach64.pci_cfg40 & 4) { reg_cx = 1; reg_dx = (uint16_t)(mach64.pci_io_base & 0xFF00); }
			else { reg_cx = 0; reg_dx = 0x2EC; }
			reg_ax = 0;
			reg_bx = 0;
			break;
		case 0x13:   /* DDC: not supported */
			reg_bx = 0;
			reg_al = 0;
			reg_ah = 0;
			break;
		default:
			LOG(LOG_INT10, LOG_NORMAL)("ATI mach64 extended BIOS function AL=%02xh not supported", fn);
			reg_ah = 2;
			break;
	}
}
