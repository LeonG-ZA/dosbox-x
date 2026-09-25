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

#include <string.h>

#include "dosbox.h"
#include "logging.h"
#include "mem.h"
#include "inout.h"
#include "callback.h"
#include "regs.h"
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

static Bitu Mach64_ROMServices_Callback(void) {
	/* the same services as INT 10h AH=A0h; AL selects the function */
	Mach64_INT10_Extended();
	return CBRET_NONE;
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

void Mach64_INT10_SetMode(const VideoModeBlock *mode) {
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

/* resolution codes (AL=0, CH) */
struct Mach64_Res { uint8_t code; uint16_t w, h; };
static const Mach64_Res mach64_res[] = {
	{ 0x12, 640, 480 }, { 0x6A, 800, 600 }, { 0x55, 1024, 768 }, { 0x83, 1280, 1024 }, { 0x84, 1600, 1200 }
};

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
	return 0x4740;   /* 'G' (GT), ASIC ID 40h */
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
		const PhysPt t = buf + 0x20 + m * 0x24;
		mem_writew(t + 0, mach64_res[m].w);
		mem_writew(t + 2, mach64_res[m].h);
		mem_writeb(t + 4, 6);                        /* maximum depth: 32 bpp */
		mem_writeb(t + 5, mach64_res[m].code);
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

void Mach64_INT10_Extended(void) {
	const uint8_t fn = reg_al;
	switch (fn) {
		case 0x00:   /* load coprocessor CRTC parameters */
		case 0x02: { /* ... and set the display mode */
			unsigned int w = 0, h = 0;
			for (const auto &r : mach64_res) if (r.code == reg_ch) { w = r.w; h = r.h; }
			const VGAModes t = ModeTypeOfDepth(reg_cl);
			Bitu modenum;
			if (w == 0 || t == M_ERROR || !FindMode(w, h, t, modenum)) { reg_ah = 1; break; }
			if (fn == 0x02) {
				if (!INT10_SetVideoMode((uint16_t)modenum)) { reg_ah = 1; break; }
			}
			reg_ah = 0;
			break;
		}
		case 0x01:   /* set display mode: CL bit 0 = coprocessor */
			if (reg_cl & 1) {
				Mach64_IOWriteReg(M64_CRTC_GEN_CNTL, Mach64_IOReadReg(M64_CRTC_GEN_CNTL) | M64_CRTC_EXT_DISP_EN | M64_CRTC_ENABLE);
				if (reg_cl & 0x80) Mach64_IOWriteReg(M64_DAC_CNTL, Mach64_IOReadReg(M64_DAC_CNTL) | (1u << 8));
			}
			else {
				Mach64_IOWriteReg(M64_CRTC_GEN_CNTL, Mach64_IOReadReg(M64_CRTC_GEN_CNTL) & ~M64_CRTC_EXT_DISP_EN);
				Mach64_IOWriteReg(M64_DAC_CNTL, Mach64_IOReadReg(M64_DAC_CNTL) & ~(1u << 8));
			}
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
		case 0x07:   /* hardware capability list: empty table */
			reg_ah = 2;
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
			BuildQuery(PhysMake(reg_dx, reg_bx), (reg_cl & 1) != 0, size);
			reg_ah = 0;
			break;
		}
		case 0x0A:   /* clock chip frequency table */
			reg_al = 4;                              /* internal PLL */
			reg_dx = 0xC000;
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
