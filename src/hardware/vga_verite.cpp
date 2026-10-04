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

/* Rendition Verite V1000 board (machine=svga_verite).
 *
 * The V1000 is a single chip VGA + 2D/3D accelerator. Standard VGA modes are handled by
 * DOSBox-X's VGA emulation. In "native" mode (Mode register VGA_Mode bit clear) the chip's
 * own CRTC scans its 4MB of local memory, and the RISC processor (verite_core.cpp) runs
 * whatever microcode the guest's driver uploaded.
 *
 * This file is the board around the chip:
 *   - PCI device 1163:0001: BAR0 = 16MB local memory window, BAR1 = 256 bytes of I/O
 *   - native I/O registers 0x80..0xff: CRTC, memory control, the external Bt485-compatible
 *     RAMDAC (palette, hardware cursor) and the external serially programmed pixel PLL
 *   - the native display path (vga.native, drawn by vga_draw.cpp)
 *   - the VESA BIOS hooks: like the real Rendition BIOS, VESA modes use native mode
 *   - scheduling of the RISC, interrupts and PCI bus master DMA
 *
 * References: see verite_core.cpp. In addition, from [XORG]: vmodes.c (CRTC timing
 * fields, the V1000 PLL serial protocol and frequency formula), vramdac.c (Bt485 register
 * use, cursor RAM loading, cursor position offset), rendition.c (MemEndian per depth).
 */

#include <algorithm>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <string>

#include "dosbox.h"
#include "logging.h"
#include "setup.h"
#include "control.h"
#include "inout.h"
#include "mem.h"
#include "paging.h"
#include "pic.h"
#include "pci_bus.h"
#include "regs.h"
#include "vga.h"
#include "../ints/int10.h"
#include "verite.h"
#include "verite_core.h"

extern bool vga_8bit_dac;
void write_p3c6(Bitu port, Bitu val, Bitu iolen);
Bitu read_p3c6(Bitu port, Bitu iolen);
void write_p3c7(Bitu port, Bitu val, Bitu iolen);
Bitu read_p3c7(Bitu port, Bitu iolen);
void write_p3c8(Bitu port, Bitu val, Bitu iolen);
Bitu read_p3c8(Bitu port, Bitu iolen);
void write_p3c9(Bitu port, Bitu val, Bitu iolen);
Bitu read_p3c9(Bitu port, Bitu iolen);
void VGA_DAC_UpdateColorPalette();

namespace {

const uint16_t RENDITION_VENDOR_ID = 0x1163;
const uint16_t V1000_DEVICE_ID     = 0x0001;
const uint32_t LOCAL_MEMORY        = 4u << 20;  /* V1000 boards shipped with 4MB */
const uint32_t MEM_BAR_SIZE        = 16u << 20;
const uint32_t IO_BAR_SIZE         = 256;
const uint32_t DEFAULT_IO_BASE     = 0xE000;

/* CRTC control register bits [SPEC] 5.14 */
enum {
	CRTCCTL_SCRNFMT        = 0x0000000Fu,
	CRTCCTL_FIFO128        = 0x00000010u,
	CRTCCTL_VIDEOENABLE    = 0x00001000u,
	CRTCCTL_FRAMEDISPLAYED = 0x00004000u,
	CRTCCTL_FRAMEBGR       = 0x00008000u,
	CRTCCTL_LINEDOUBLE     = 0x00020000u,
	CRTCCTL_FRAMESWITCHED  = 0x00040000u,
	CRTCCTL_FIFO256        = 0x00800000u
};

class VeriteBoard;
VeriteBoard *board = NULL;

class VeriteBoard : public Verite::Board {
public:
	Verite::Core *core;
	bool debug;
	int trace_left;   /* host accesses still to log (verite_trace) */
	uint8_t dma_en_saved; /* Mode DMA_EN across a BIOS mode set */
	int irq;
	uint32_t mem_bar, io_bar;
	double risc_hz;
	double risc_time; /* emulated time (PIC_FullIndex, ms) the RISC has been run up to */

	/* CRTC */
	uint32_t crtc_test, crtc_ctl, crtc_horz, crtc_horz_pending, crtc_vert;
	uint32_t framebase_a, framebase_b, framebase_b_pending, crtc_offset;
	uint32_t memctl[4];
	uint8_t devregs[0x40]; /* 0xc0..0xff: PLL (device 0), video input, device 1 */

	/* Bt485 */
	uint8_t dac_cr0, dac_cr1, dac_cr2, dac_cr3;
	uint8_t dac_waddr;            /* shadow of the palette write address */
	uint16_t cursor_addr;         /* cursor RAM address */
	/* 64x64 cursor: 10-bit address (A9:A8 from CR3). 32x32 cursor (Bt484 compatible): 256
	 * bytes, A9:A8 ignored and the address wraps from FFh to 00h. The Windows Rendition
	 * drivers rely on the wrap: they load plane 1 from 80h and then plane 0. */
	bool Cursor64() const { return (dac_cr3 & 0x04) != 0; }
	unsigned CursorRamIndex() const { return Cursor64() ? (cursor_addr & 0x3FFu) : (cursor_addr & 0xFFu); }
	void NextCursorAddr() {
		cursor_addr = Cursor64() ? (uint16_t)((cursor_addr + 1) & 0x3FF)
		                         : (uint16_t)((cursor_addr & 0x300) | ((cursor_addr + 1) & 0xFF));
	}
	uint8_t curcol_addr, curcol_comp;
	uint8_t curcol[4][3];
	uint8_t cursor_ram[1024];
	uint16_t cursor_x, cursor_y, cursor_x_pending, cursor_y_pending;

	/* pixel PLL: 20 bits shifted in serially, latched by a read */
	uint32_t pll_shift, pll_value;
	unsigned pll_bits; /* bits shifted in since the last latch (debug) */

	/* scheduling */
	bool irq_asserted;
	unsigned irq_log_left;  /* debug: IRQ changes still to log */
	uint64_t irq_raises;    /* debug */
	unsigned stat_frames;

	VeriteBoard() : core(NULL), debug(false), trace_left(0), dma_en_saved(0), irq(0), mem_bar(0), io_bar(0), risc_hz(25e6), risc_time(0) {
		ResetRegs();
		irq_asserted = false;
		irq_log_left = 40;
		irq_raises = 0;
		stat_frames = 0;
	}

	void ResetRegs() {
		crtc_test = 0;
		crtc_ctl = 0x300; /* VSyncHi, HSyncHi set by reset */
		/* 640x480 as power-on default: fp/sync/bp 16/96/48 pixels, 10/2/33 lines */
		crtc_horz = crtc_horz_pending = (1u << 21) | (11u << 16) | (5u << 9) | 79u;
		crtc_vert = (9u << 20) | (1u << 17) | (32u << 11) | 479u;
		framebase_a = framebase_b = framebase_b_pending = 0;
		crtc_offset = 0;
		memset(memctl, 0, sizeof(memctl));
		memset(devregs, 0, sizeof(devregs));
		dac_cr0 = dac_cr1 = dac_cr2 = dac_cr3 = 0;
		dac_waddr = 0;
		cursor_addr = 0;
		curcol_addr = curcol_comp = 0;
		memset(curcol, 0, sizeof(curcol));
		memset(cursor_ram, 0, sizeof(cursor_ram));
		cursor_x = cursor_y = cursor_x_pending = cursor_y_pending = 0;
		pll_shift = 0;
		pll_bits = 0;
		pll_value = (9u << 10) | (0u << 8) | 8u; /* M=11 N=10 P=0: 31.5MHz */
	}

	/* ---- Board interface ---- */
	uint32_t ExtRegRead(uint32_t off, unsigned len) override;
	void ExtRegWrite(uint32_t off, uint32_t val, unsigned len) override;
	void IrqUpdate(bool asserted) override;
	uint32_t DmaRead32(uint32_t phys) override { return phys_readd(phys); }
	void ModeChanged() override;
	void MemEndianChanged() override { PAGING_ClearTLB(); }
	uint32_t CatchUpBudget() override;
	void Message(const char *msg) override { LOG_MSG("%s", msg); }

	/* ---- helpers ---- */
	uint32_t Reg32Read(uint32_t off);
	void Reg32Write(uint32_t off, uint32_t val);
	uint8_t DacRead(unsigned r);
	void DacWrite(unsigned r, uint8_t v);
	double PixelClock() const;
	void UpdateDisplay();
	bool Native() const { return core && !(core->ModeReg() & Verite::MODE_VGA); }
	struct Timing {
		unsigned width, height, htotal, vtotal, hsync_start, hsync_end, vsync_start, vsync_end;
		double clock;
	};
	Timing CrtcTiming() const;
	void ScanPosition(unsigned &x, unsigned &y, Timing &t) const;
	uint32_t CrtcStatus() const;
	bool InVBlank() const;
};

/* ---------------------------------------------------------------------------------- */
/* native display                                                                     */

uint32_t line_buf[4096];

inline uint32_t Rgb(uint32_t r, uint32_t g, uint32_t b) { return (r << 16) | (g << 8) | b; }

/* Bt485 hardware cursor overlay for screen line y */
void DrawCursor(uint32_t *out, unsigned width, int y) {
	const uint8_t mode = board->dac_cr2 & 3;
	if (mode == 0) return;
	const int size = (board->dac_cr3 & 4) ? 64 : 32;
	/* the position registers give the cursor's bottom right corner + 1: (CX-64, CY-64) for
	 * the 64x64 cursor as in X.org, (CX-32, CY-32) for the 32x32 one the Windows drivers use */
	const int cy0 = (int)board->cursor_y - size, cx0 = (int)board->cursor_x - size;
	if (board->cursor_x == 0 && board->cursor_y == 0) return; /* (0,0) is off screen */
	const int row = y - cy0;
	if (row < 0 || row >= size) return;
	const unsigned bytes_per_row = (unsigned)size / 8u;
	const unsigned plane = (unsigned)(size * size / 8);
	for (int col = 0; col < size; col++) {
		const int x = cx0 + col;
		if (x < 0 || x >= (int)width) continue;
		const unsigned byte = (unsigned)row * bytes_per_row + (unsigned)col / 8u;
		const unsigned bit = 7u - ((unsigned)col & 7u);
		const unsigned p0 = (board->cursor_ram[byte] >> bit) & 1u;
		const unsigned p1 = (board->cursor_ram[plane + byte] >> bit) & 1u;
		const unsigned v = (p1 << 1) | p0;
		auto color = [&](unsigned i) { return Rgb(board->curcol[i][0], board->curcol[i][1], board->curcol[i][2]); };
		switch (mode) {
			case 1: /* three colour: 0 transparent, 1..3 cursor colours 1..3 */
				if (v) out[x] = color(v);
				break;
			case 2: /* XGA: 00 = colour 1, 01 = colour 2, 10 = transparent, 11 = complement
			         * (the Windows 98 Rendition driver: plane 1 = AND mask, plane 0 = XOR mask) */
				if (v < 2) out[x] = color(v + 1);
				else if (v == 3) out[x] ^= 0x00FFFFFFu;
				break;
			default: /* X Window: 10 = colour 1, 11 = colour 2 */
				if (v == 2) out[x] = color(1);
				else if (v == 3) out[x] = color(2);
				break;
		}
	}
}

template <unsigned FMT>
uint8_t *DrawNative(Bitu vidstart, Bitu /*line*/) {
	const unsigned width = (std::min<unsigned>)((unsigned)vga.draw.width, 4096);
	uint32_t *out = line_buf;
	Verite::Core *c = board->core;
	if (!(board->crtc_ctl & CRTCCTL_VIDEOENABLE)) {
		memset(out, 0, width * 4);
		return (uint8_t*)out;
	}
	uint32_t a = (uint32_t)vidstart;
	const bool bgr = (board->crtc_ctl & CRTCCTL_FRAMEBGR) != 0;
	switch (FMT) {
		case 2: /* 8-bit indexed through the RAMDAC palette */
			for (unsigned x = 0; x < width; x++, a++) out[x] = vga.dac.xlat32[c->Read8(a)];
			break;
		case 1: /* 8-bit 332 */
			for (unsigned x = 0; x < width; x++, a++)
				out[x] = Verite::ExpandToARGB(c->Read8(a), 1, bgr, false) & 0xFFFFFFu;
			break;
		case 4: case 5: case 6:
			for (unsigned x = 0; x < width; x++, a += 2)
				out[x] = Verite::ExpandToARGB(c->Read16(a), FMT, bgr, false) & 0xFFFFFFu;
			break;
		default: /* 8888 */
			for (unsigned x = 0; x < width; x++, a += 4) {
				const uint32_t p = c->Read32(a);
				out[x] = bgr ? (((p & 0xFF) << 16) | (p & 0xFF00) | ((p >> 16) & 0xFF)) : (p & 0xFFFFFFu);
			}
			break;
	}
	if (board->dac_cr2 & 3) {
		const uint32_t pitch = vga.native.pitch ? vga.native.pitch : 1;
		const int y = (int)(((uint32_t)vidstart - vga.native.start) / pitch);
		DrawCursor(out, width, y);
	}
	return (uint8_t*)out;
}

double VeriteBoard::PixelClock() const {
	/* [XORG] vmodes.c: value = (M-2)<<10 | P<<8 | (N-2), f = 14.318 * 2 * M / N / 2^P MHz.
	 * M is 8 bits wide: Rendition's BIOS and Windows drivers use M above 129 (640x480 at
	 * 25.175 MHz is 3463Ah, M = 211), which X.org's search never produces. */
	const unsigned m = ((pll_value >> 10) & 0xFF) + 2, p = (pll_value >> 8) & 3, n = (pll_value & 0xFF) + 2;
	double f = 14.31818e6 * 2.0 * m / n / (double)(1u << p);
	if (dac_cr3 & 0x08) f *= 2.0; /* Bt485 clock doubler */
	if (f < 5e6 || f > 250e6) f = 25.175e6;
	return f;
}

void VeriteBoard::UpdateDisplay() {
	VGA_NativeCRTC &n = vga.native;
	const VGA_NativeCRTC before = n;
	const bool was = n.active;
	n.active = Native();
	if (n.active) {
		const Timing t = CrtcTiming();
		const unsigned hac = t.width;
		n.width = t.width;
		n.height = t.height;
		n.htotal = t.htotal;
		n.vtotal = t.vtotal;
		n.hsync_start = t.hsync_start;
		n.hsync_end = t.hsync_end;
		n.vsync_start = t.vsync_start;
		n.vsync_end = t.vsync_end;
		n.line_double = (crtc_ctl & CRTCCTL_LINEDOUBLE) != 0;
		n.clock = t.clock;
		const unsigned fmt = crtc_ctl & CRTCCTL_SCRNFMT;
		const unsigned bpp = (std::max)(1u, Verite::FormatBytes(fmt));
		/* [SPEC] 5.14 CRTCOffset: bytes from the last video FIFO request of a line to the
		 * first of the next one */
		const uint32_t fifo = (crtc_ctl & CRTCCTL_FIFO256) ? 256u : ((crtc_ctl & CRTCCTL_FIFO128) ? 128u : 64u);
		const uint32_t w = hac * bpp;
		uint32_t last;
		if (w % fifo) last = w - w % fifo;
		else last = (framebase_a & 7) ? w : w - fifo;
		n.pitch = (crtc_offset & 0xFFFF) + last;
		switch (fmt) {
			case 1:  n.draw_line = DrawNative<1>; break;
			case 2:  n.draw_line = DrawNative<2>; break;
			case 4:  n.draw_line = DrawNative<4>; break;
			case 5:  n.draw_line = DrawNative<5>; break;
			case 6:  n.draw_line = DrawNative<6>; break;
			default: n.draw_line = DrawNative<12>; break;
		}
	}
	const bool changed = n.active != before.active || (n.active && (n.width != before.width ||
		n.height != before.height || n.htotal != before.htotal || n.vtotal != before.vtotal ||
		n.hsync_start != before.hsync_start || n.hsync_end != before.hsync_end ||
		n.vsync_start != before.vsync_start || n.vsync_end != before.vsync_end ||
		n.clock != before.clock || n.line_double != before.line_double ||
		n.pitch != before.pitch || n.draw_line != before.draw_line));
	if (!changed) return;
	if (debug && n.active) {
		LOG_MSG("Verite: native %ux%u fmt %u pitch %u, total %ux%u, %.3f MHz (%.2f Hz)%s",
			n.width, n.height, crtc_ctl & CRTCCTL_SCRNFMT, n.pitch, n.htotal, n.vtotal, n.clock / 1e6,
			n.clock / ((double)n.htotal * n.vtotal), n.line_double ? " line doubled" : "");
	}
	else if (debug) {
		LOG_MSG("Verite: VGA mode");
	}
	if (n.active != was) VGA_SetupHandlers();
	VGA_DetermineMode();
	/* the palette lookup bypasses the attribute controller while native */
	if (n.active != was) VGA_DAC_UpdateColorPalette();
	VGA_StartResize();
}

VeriteBoard::Timing VeriteBoard::CrtcTiming() const {
	/* [SPEC] 5.14, [XORG] vmodes.c HORZ()/VERT(). Vertical values count output scanlines
	 * (the Rendition BIOS programs 400 of 525 lines for line doubled 320x200). */
	Timing t;
	const uint32_t h = crtc_horz, v = crtc_vert;
	const unsigned hac = ((h & 0xFF) + 1) * 8, hbp = (((h >> 9) & 0x3F) + 1) * 8;
	const unsigned hsy = (((h >> 16) & 0x1F) + 1) * 8, hfp = (((h >> 21) & 0x7) + 1) * 8;
	const unsigned vac = (v & 0x7FF) + 1, vbp = ((v >> 11) & 0x3F) + 1;
	const unsigned vsy = ((v >> 17) & 0x7) + 1, vfp = ((v >> 20) & 0x3F) + 1;
	t.width = hac;
	t.height = vac;
	t.htotal = hac + hfp + hsy + hbp;
	t.vtotal = vac + vfp + vsy + vbp;
	t.hsync_start = hac + hfp;
	t.hsync_end = t.hsync_start + hsy;
	t.vsync_start = vac + vfp;
	t.vsync_end = t.vsync_start + vsy;
	t.clock = PixelClock();
	return t;
}

/* Native CRTC scan position. While native mode is on screen it follows the VGA frame
 * timer (which runs on the native timing); otherwise the native CRTC free-runs. */
void VeriteBoard::ScanPosition(unsigned &x, unsigned &y, Timing &t) const {
	t = CrtcTiming();
	double pos; /* pixels into the frame */
	if (vga.native.active && vga.draw.delay.htotal > 0) {
		double ms = fmod(PIC_FullIndex() - vga.draw.delay.framestart, vga.draw.delay.vtotal);
		if (ms < 0) ms += vga.draw.delay.vtotal;
		pos = ms / vga.draw.delay.htotal * t.htotal;
	}
	else {
		const double frame_px = (double)t.htotal * t.vtotal;
		pos = fmod(PIC_FullIndex() * t.clock / 1000.0, frame_px);
	}
	y = (unsigned)(pos / t.htotal);
	x = (unsigned)fmod(pos, (double)t.htotal);
	if (y >= t.vtotal) y = t.vtotal - 1;
}

bool VeriteBoard::InVBlank() const {
	unsigned x, y;
	Timing t;
	ScanPosition(x, y, t);
	return y >= t.height;
}

uint32_t VeriteBoard::CrtcStatus() const {
	/* [SPEC] 5.14 CRTCStatus: section (1 front porch, 3 sync, 2 back porch, 0 active) and
	 * counts left in that section */
	unsigned x, y;
	Timing n;
	ScanPosition(x, y, n);
	unsigned vs, vleft;
	if (y < n.height) { vs = 0; vleft = n.height - y; }
	else if (y < n.vsync_start) { vs = 1; vleft = n.vsync_start - y; }
	else if (y < n.vsync_end) { vs = 3; vleft = n.vsync_end - y; }
	else { vs = 2; vleft = (n.vtotal > y) ? n.vtotal - y : 0; }
	unsigned hs, hleft;
	if (x < n.width) { hs = 0; hleft = n.width - x; }
	else if (x < n.hsync_start) { hs = 1; hleft = n.hsync_start - x; }
	else if (x < n.hsync_end) { hs = 3; hleft = n.hsync_end - x; }
	else { hs = 2; hleft = n.htotal > x ? n.htotal - x : 0; }
	return ((uint32_t)vs << 22) | ((vleft & 0x7FF) << 11) | ((uint32_t)hs << 9) | ((hleft / 8) & 0xFF);
}

/* ---------------------------------------------------------------------------------- */
/* native I/O registers 0x80..0xff                                                    */

uint32_t VeriteBoard::Reg32Read(uint32_t off) {
	switch (off) {
		case 0x80: {
			uint32_t v = crtc_test & 0xFFFFu;
			const uint32_t s = CrtcStatus();
			if ((s >> 22) != 0 || ((s >> 9) & 3) != 0) v |= 0x10000u; /* not display enable */
			if ((s >> 22) != 0) v |= 0x40000u;                         /* vertical blank */
			return v;
		}
		case 0x84: return crtc_ctl;
		case 0x88: return crtc_horz;
		case 0x8C: return crtc_vert;
		case 0x90: return framebase_b;
		case 0x94: return framebase_a;
		case 0x98: return crtc_offset;
		case 0x9C: return CrtcStatus();
		case 0xA0: case 0xA4: case 0xA8: case 0xAC: return memctl[(off - 0xA0) >> 2];
		default: return 0;
	}
}

void VeriteBoard::Reg32Write(uint32_t off, uint32_t val) {
	switch (off) {
		case 0x80: crtc_test = val & 0xFFFFu; break;
		case 0x84: {
			const uint32_t ro = CRTCCTL_FRAMEDISPLAYED | CRTCCTL_FRAMESWITCHED | 0x10000u;
			const uint32_t old = crtc_ctl;
			crtc_ctl = (crtc_ctl & ro) | (val & ~ro);
			if ((old ^ crtc_ctl) & ~ro) UpdateDisplay();
			break;
		}
		case 0x88:
			/* takes effect when CRTCVert is written */
			crtc_horz_pending = val & 0x00FFFEFFu;
			break;
		case 0x8C:
			crtc_vert = val & 0x03FFFFFFu;
			crtc_horz = crtc_horz_pending;
			UpdateDisplay();
			break;
		case 0x90:
			framebase_b_pending = val & 0xFFFFFFu;
			break;
		case 0x94:
			/* latched at the next vertical sync; FrameDisplayed and (outside vertical blank)
			 * FrameSwitched are cleared by the write */
			framebase_a = val & 0xFFFFFFu;
			framebase_b = framebase_b_pending;
			crtc_ctl &= ~CRTCCTL_FRAMEDISPLAYED;
			if (!InVBlank()) crtc_ctl &= ~CRTCCTL_FRAMESWITCHED;
			if (vga.native.active && vga.native.pitch == 0) UpdateDisplay();
			break;
		case 0x98:
			crtc_offset = val & 0xFFFFu;
			if (vga.native.active) UpdateDisplay();
			break;
		case 0x9C: break;
		case 0xA0: case 0xA4: case 0xA8: case 0xAC: memctl[(off - 0xA0) >> 2] = val; break;
		default: break;
	}
}

uint8_t VeriteBoard::DacRead(unsigned r) {
	switch (r) {
		case 0x0: return (uint8_t)read_p3c8(0x3C8, 1);
		case 0x1: return (uint8_t)read_p3c9(0x3C9, 1);
		case 0x2: return (uint8_t)read_p3c6(0x3C6, 1);
		case 0x3: return (uint8_t)read_p3c7(0x3C7, 1);
		case 0x4: return curcol_addr;
		case 0x5: {
			const uint8_t v = curcol[curcol_addr & 3][curcol_comp];
			if (++curcol_comp == 3) { curcol_comp = 0; curcol_addr = (curcol_addr + 1) & 3; }
			return v;
		}
		case 0x6: return dac_cr0;
		case 0x7: return curcol_addr;
		case 0x8: return dac_cr1;
		case 0x9: return dac_cr2;
		case 0xA:
			/* command register 3 when unlocked by CR0 bit 7 and write address 1 */
			if ((dac_cr0 & 0x80) && dac_waddr == 0x01) return dac_cr3;
			return 0x20; /* status: Bt485 id/revision bits */
		case 0xB: {
			const uint8_t v = cursor_ram[CursorRamIndex()];
			NextCursorAddr();
			return v;
		}
		case 0xC: return (uint8_t)cursor_x;
		case 0xD: return (uint8_t)(cursor_x >> 8);
		case 0xE: return (uint8_t)cursor_y;
		default:  return (uint8_t)(cursor_y >> 8);
	}
}

void VeriteBoard::DacWrite(unsigned r, uint8_t v) {
	switch (r) {
		case 0x0:
			dac_waddr = v;
			cursor_addr = (uint16_t)(((dac_cr3 & 3u) << 8) | v);
			write_p3c8(0x3C8, v, 1);
			break;
		case 0x1: write_p3c9(0x3C9, v, 1); break;
		case 0x2: write_p3c6(0x3C6, v, 1); break;
		case 0x3: write_p3c7(0x3C7, v, 1); break;
		case 0x4: case 0x7: curcol_addr = v & 3; curcol_comp = 0; break;
		case 0x5:
			curcol[curcol_addr & 3][curcol_comp] = v;
			if (++curcol_comp == 3) { curcol_comp = 0; curcol_addr = (curcol_addr + 1) & 3; }
			break;
		case 0x6: {
			const bool dac8 = (v & 0x02) != 0;
			dac_cr0 = v;
			if (dac8 != vga_8bit_dac) {
				vga_8bit_dac = dac8;
				VGA_DAC_UpdateColorPalette();
			}
			break;
		}
		case 0x8: dac_cr1 = v; break;
		case 0x9: dac_cr2 = v; break;
		case 0xA:
			if ((dac_cr0 & 0x80) && dac_waddr == 0x01) {
				dac_cr3 = v;
				cursor_addr = (uint16_t)((cursor_addr & 0xFF) | ((v & 3u) << 8));
				if (vga.native.active) UpdateDisplay(); /* clock doubler */
			}
			break;
		case 0xB:
			cursor_ram[CursorRamIndex()] = v;
			NextCursorAddr();
			break;
		case 0xC: cursor_x_pending = (uint16_t)((cursor_x_pending & 0xF00) | v); break;
		case 0xD: cursor_x_pending = (uint16_t)((cursor_x_pending & 0x0FF) | ((v & 0xFu) << 8)); break;
		case 0xE: cursor_y_pending = (uint16_t)((cursor_y_pending & 0xF00) | v); break;
		default:
			/* the position takes effect when the Y high byte is written */
			cursor_y_pending = (uint16_t)((cursor_y_pending & 0x0FF) | ((v & 0xFu) << 8));
			cursor_x = cursor_x_pending;
			cursor_y = cursor_y_pending;
			break;
	}
}

uint32_t VeriteBoard::ExtRegRead(uint32_t off, unsigned len) {
	off &= 0xFF;
	if (off >= 0xB0 && off < 0xC0) {
		uint32_t r = 0;
		for (unsigned i = 0; i < len && off + i < 0xC0; i++) r |= (uint32_t)DacRead(off + i - 0xB0) << (8 * i);
		return r;
	}
	if (off >= 0xC0) {
		if (off == 0xC0) {
			/* reading the PLL device latches the bits shifted in. Words with bit 18 or 19 set
			 * are not frequencies: Rendition's BIOS latches 40000h after every clock change. */
			if (!(pll_shift & 0xC0000u)) pll_value = pll_shift & 0x3FFFFu;
			if (debug && pll_bits) LOG_MSG("Verite: PLL latched %05x after %u bits, pixel clock %.3f MHz", pll_shift, pll_bits, PixelClock() / 1e6);
			pll_bits = 0;
			if (vga.native.active) UpdateDisplay();
		}
		uint32_t r = 0;
		for (unsigned i = 0; i < len && off + i < 0x100; i++) r |= (uint32_t)devregs[off + i - 0xC0] << (8 * i);
		return r;
	}
	const uint32_t base = off & ~3u;
	return Reg32Read(base) >> (8 * (off & 3));
}

void VeriteBoard::ExtRegWrite(uint32_t off, uint32_t val, unsigned len) {
	off &= 0xFF;

	if (off >= 0xB0 && off < 0xC0) {
		/* palette word write: index auto-increments to the next RGB triple [SPEC] 7 */
		if (off == 0xB1 && len == 4) {
			write_p3c9(0x3C9, (val >> 16) & 0xFF, 1);
			write_p3c9(0x3C9, (val >> 8) & 0xFF, 1);
			write_p3c9(0x3C9, val & 0xFF, 1);
			return;
		}
		for (unsigned i = 0; i < len && off + i < 0xC0; i++) DacWrite(off + i - 0xB0, (uint8_t)(val >> (8 * i)));
		return;
	}
	if (off >= 0xC0) {
		if (off == 0xC0) { pll_shift = ((pll_shift << 1) | (val & 1u)) & 0xFFFFFu; pll_bits++; }
		for (unsigned i = 0; i < len && off + i < 0x100; i++) devregs[off + i - 0xC0] = (uint8_t)(val >> (8 * i));
		return;
	}
	const uint32_t base = off & ~3u;
	if (len == 4 && (off & 3) == 0) {
		Reg32Write(base, val);
		return;
	}
	/* partial write: merge into the current value */
	const unsigned sh = 8 * (off & 3);
	const uint32_t mask = (len >= 4 ? 0xFFFFFFFFu : ((1u << (8 * len)) - 1u)) << sh;
	uint32_t cur;
	switch (base) {
		case 0x88: cur = crtc_horz_pending; break;
		case 0x90: cur = framebase_b_pending; break;
		default: cur = Reg32Read(base); break;
	}
	Reg32Write(base, (cur & ~mask) | ((val << sh) & mask));
}

void VeriteBoard::IrqUpdate(bool asserted) {
	if (irq <= 0 || asserted == irq_asserted) return;
	irq_asserted = asserted;
	if (asserted) irq_raises++;
	if (debug && irq_log_left) {
		irq_log_left--;
		LOG_MSG("Verite: IRQ %d %s, intr %04x intren %04x, RISC pc %08x", irq, asserted ? "raised" : "dropped",
			(unsigned)core->IORead(Verite::IO_INTR, 2), (unsigned)core->IORead(Verite::IO_INTREN, 2), core->pc);
	}
	if (asserted) PIC_ActivateIRQ((unsigned int)irq);
	else PIC_DeActivateIRQ((unsigned int)irq);
}

uint32_t VeriteBoard::CatchUpBudget() {
	const double owed = (PIC_FullIndex() - risc_time) * risc_hz / 1000.0;
	if (owed < 1.0) return 0;
	const uint32_t n = owed > 20000.0 ? 20000u : (uint32_t)owed;
	risc_time += n * 1000.0 / risc_hz;
	return n;
}

void VeriteBoard::ModeChanged() {
	UpdateDisplay();
	VGA_SetupHandlers();
}

/* ---------------------------------------------------------------------------------- */
/* I/O BAR                                                                            */

/* host access trace; identical consecutive accesses are collapsed into one line */
struct TraceRepeat { const char *what; uint32_t off, val; unsigned len, count; } trace_last = { NULL, 0, 0, 0, 0 };

void TraceFlush() {
	if (trace_last.count > 1)
		LOG_MSG("Verite trace: ... %s %02x/%u = %08x repeated %u times", trace_last.what, trace_last.off,
			trace_last.len, trace_last.val, trace_last.count);
	trace_last.count = 0;
}

void Trace(const char *what, uint32_t off, unsigned len, uint32_t val) {
	if (board->trace_left <= 0) return;
	/* polled status registers (CRTCStatus, CRTCTest) change on every read: collapse by register */
	const bool status_poll = (off == 0x9C || off == 0x80) && what[3] == 'r' && what[4] == 'd';
	if (trace_last.count && trace_last.what == what && trace_last.off == off && trace_last.len == len &&
		(trace_last.val == val || status_poll)) {
		trace_last.count++;
		return;
	}
	TraceFlush();
	board->trace_left--;
	trace_last.what = what; trace_last.off = off; trace_last.len = len; trace_last.val = val; trace_last.count = 1;
	LOG_MSG("Verite trace: %s %02x/%u = %08x (pc %08x)", what, off, len, val, board->core->pc);
}

Bitu read_io(Bitu port, Bitu iolen) {
	if (!board) return ~0u;
	const uint32_t off = (uint32_t)(port - board->io_bar);
	const uint32_t v = board->core->IORead(off, (unsigned)iolen);
	Trace("io rd", off, (unsigned)iolen, v);
	return v;
}

void write_io(Bitu port, Bitu val, Bitu iolen) {
	if (!board) return;
	const uint32_t off = (uint32_t)(port - board->io_bar);
	Trace("io wr", off, (unsigned)iolen, (uint32_t)val);
	board->core->IOWrite(off, (uint32_t)val, (unsigned)iolen);
}

void MapIO(uint32_t base) {
	base &= ~(IO_BAR_SIZE - 1) & 0xFFFFu;
	if (board->io_bar == base) return;
	if (board->io_bar) {
		IO_FreeReadHandler(board->io_bar, IO_MA, IO_BAR_SIZE);
		IO_FreeWriteHandler(board->io_bar, IO_MA, IO_BAR_SIZE);
	}
	board->io_bar = base;
	if (base) {
		IO_RegisterReadHandler(base, read_io, IO_MA, IO_BAR_SIZE);
		IO_RegisterWriteHandler(base, write_io, IO_MA, IO_BAR_SIZE);
	}
}

/* VGA extended registers 3DEh/3DFh: native memory and I/O base [SPEC] table 11g */
Bitu read_p3de(Bitu port, Bitu /*iolen*/) {
	if (!board) return 0xFF;
	return (port == 0x3DE) ? (board->mem_bar >> 24) & 0xFF : (board->io_bar >> 8) & 0xFF;
}

/* ---------------------------------------------------------------------------------- */
/* memory BAR: local memory through MemEndian                                         */

class Verite_SwapHandler : public PageHandler {
public:
	Verite_SwapHandler() : PageHandler(PFLAG_NOCODE) {}
	static uint32_t Off(PhysPt addr) { return (PAGING_GetPhysicalAddress(addr) - board->mem_bar) & (MEM_BAR_SIZE - 1); }
	uint8_t readb(PhysPt addr) override { return (uint8_t)board->core->MemRead(Off(addr), 1); }
	uint16_t readw(PhysPt addr) override { return (uint16_t)board->core->MemRead(Off(addr), 2); }
	uint32_t readd(PhysPt addr) override { return board->core->MemRead(Off(addr), 4); }
	void writeb(PhysPt addr, uint8_t v) override { board->core->MemWrite(Off(addr), v, 1); }
	void writew(PhysPt addr, uint16_t v) override { board->core->MemWrite(Off(addr), v, 2); }
	void writed(PhysPt addr, uint32_t v) override { board->core->MemWrite(Off(addr), v, 4); }
};

/* MemEndian = 0: the host byte layout is the storage layout, so map host pointers */
class Verite_DirectHandler : public PageHandler {
public:
	Verite_DirectHandler() : PageHandler(PFLAG_READABLE | PFLAG_WRITEABLE | PFLAG_NOCODE) {}
	HostPt GetHostReadPt(PageNum phys_page) override {
		const uint32_t off = (uint32_t)((phys_page << 12) - board->mem_bar) & (board->core->MemSize() - 1);
		return board->core->Memory() + off;
	}
	HostPt GetHostWritePt(PageNum phys_page) override { return GetHostReadPt(phys_page); }
};

/* A0000-AFFFF in native mode with the VESA bit: 64KB of local memory at BankSelect */
class Verite_WindowHandler : public PageHandler {
public:
	Verite_WindowHandler() : PageHandler(PFLAG_NOCODE) {}
	static uint32_t Off(PhysPt addr) {
		return (board->core->BankSelect() & 0x00FFE000u) + (PAGING_GetPhysicalAddress(addr) & 0xFFFFu);
	}
	static bool Rom() { return (board->core->BankSelect() & 0x80000000u) != 0; }
	uint8_t readb(PhysPt addr) override { return Rom() ? 0xFF : (uint8_t)board->core->MemRead(Off(addr), 1); }
	uint16_t readw(PhysPt addr) override { return Rom() ? 0xFFFF : (uint16_t)board->core->MemRead(Off(addr), 2); }
	uint32_t readd(PhysPt addr) override { return Rom() ? 0xFFFFFFFFu : board->core->MemRead(Off(addr), 4); }
	void writeb(PhysPt addr, uint8_t v) override { if (!Rom()) board->core->MemWrite(Off(addr), v, 1); }
	void writew(PhysPt addr, uint16_t v) override { if (!Rom()) board->core->MemWrite(Off(addr), v, 2); }
	void writed(PhysPt addr, uint32_t v) override { if (!Rom()) board->core->MemWrite(Off(addr), v, 4); }
};

Verite_SwapHandler swap_handler;
Verite_DirectHandler direct_handler;
Verite_WindowHandler window_handler;
MEM_Callout_t mem_cb = MEM_Callout_t_none;

PageHandler *mem_cb_func(MEM_CalloutObject &co, Bitu phys_page) {
	(void)co; (void)phys_page;
	if (!board) return NULL;
	if (board->core->MemEndian() == 0) return &direct_handler;
	return &swap_handler;
}

void MapMemory(uint32_t base) {
	base &= ~(MEM_BAR_SIZE - 1);
	if (base == board->mem_bar && mem_cb != MEM_Callout_t_none) return;
	board->mem_bar = base;
	board->core->SetPciMemBase(base);
	if (mem_cb == MEM_Callout_t_none) {
		mem_cb = MEM_AllocateCallout(MEM_TYPE_PCI);
		if (mem_cb == MEM_Callout_t_none) E_Exit("Verite: unable to allocate memory callout");
	}
	MEM_CalloutObject *obj = MEM_GetCallout(mem_cb);
	assert(obj != NULL);
	obj->Uninstall();
	if (base != 0) obj->Install(base >> 12, MEMMASK_Combine(MEMMASK_FULL, MEMMASK_Range(MEM_BAR_SIZE >> 12)), mem_cb_func);
	MEM_PutCallout(obj);
	PAGING_ClearTLB();
}

/* ---------------------------------------------------------------------------------- */
/* PCI                                                                                */

void WriteVpd();

class PCI_VeriteDevice : public PCI_Device {
public:
	uint32_t config_read(uint8_t regnum, Bitu iolen) override {
		const uint32_t v = PCI_Device::config_read(regnum, iolen);
		if (board && iolen != 1) Trace("pci rd", regnum, (unsigned)iolen, v);
		return v;
	}

	PCI_VeriteDevice(int irq, uint32_t mem, uint32_t io) : PCI_Device(RENDITION_VENDOR_ID, V1000_DEVICE_ID) {
		config[0x08] = 0x00;               /* revision */
		config[0x09] = 0x00;
		config[0x0a] = 0x00;               /* VGA compatible */
		config[0x0b] = 0x03;               /* display controller */
		config[0x0e] = 0x00;
		config[0x04] = 0x07;               /* I/O, memory, bus master enabled */
		config[0x06] = 0x80;
		config[0x3c] = (irq > 0) ? (unsigned char)irq : 0xFF;
		config[0x3d] = (irq > 0) ? 0x01 : 0x00; /* INTA# */
		host_writew(config_writemask + 0x04, 0x0027);
		host_writed(config_writemask + 0x10, ~(MEM_BAR_SIZE - 1));
		host_writed(config_writemask + 0x14, ~(IO_BAR_SIZE - 1) & 0xFFFFu);
		config_writemask[0x3c] = 0xFF;
		host_writed(config + 0x10, mem | 0x8);   /* prefetchable */
		host_writed(config + 0x14, io | 0x1);    /* I/O space */
	}

	void config_write(uint8_t regnum, Bitu iolen, uint32_t value) override {
		if (iolen == 1) {
			const unsigned char mask = config_writemask[regnum];
			config[regnum] = (unsigned char)((config[regnum] & ~mask) | (value & mask));
			if (board && regnum >= 0x10 && regnum <= 0x17) {
				MapMemory(host_readd(config + 0x10) & 0xFFFFFFF0u);
				MapIO(host_readd(config + 0x14) & 0xFFFCu);
				WriteVpd();
			}
		}
		else {
			if (board && iolen != 1) Trace("pci wr", regnum, (unsigned)iolen, value);
			PCI_Device::config_write(regnum, iolen, value);
		}
	}
};

PCI_VeriteDevice *pci_dev = NULL;
uint32_t assigned_mem_base = 0;

/* PCI data structure and vital product data in the VGA BIOS ROM (C000:vpd_off) */
uint32_t rom_phys = 0;
uint16_t pcir_off = 0, vpd_off = 0;

/* Board resources as the BIOS reports them, before or after the board exists */
uint32_t BoardMemBase() {
	if (board) return board->mem_bar;
	const uint32_t top = (uint32_t)(MEM_TotalPages() * 4096u);
	const uint32_t mem = assigned_mem_base > top ? assigned_mem_base : top;
	return (mem + MEM_BAR_SIZE - 1) & ~(MEM_BAR_SIZE - 1);
}

/* Vital product data in the format the Rendition BIOS presents it (PCI VPD resource
 * tags). Speedy3D reads the "ZC" keyword: I/O base >> 8, memory base >> 24, IRQ, the
 * local memory size in 64KB units (Speedy3D assumes 2MB when it is 0, and Tomb Raider
 * and Scorched Planet then fail to allocate texture space), then a word 0020h and 0. */
void WriteVpd() {
	if (!rom_phys || !vpd_off) return;
	const uint32_t io = board ? board->io_bar : DEFAULT_IO_BASE;
	int irq = board ? board->irq : 11;
	if (!board) {
		Section_prop *section = static_cast<Section_prop *>(control->GetSection("verite"));
		if (section) irq = section->Get_int("verite_irq");
	}
	static const char ident[] = "Rendition Reference Board";
	uint8_t v[0x60];
	unsigned n = 0;
	v[n++] = 0x82; v[n++] = (uint8_t)(sizeof(ident) - 1); v[n++] = 0;
	for (unsigned i = 0; i + 1 < sizeof(ident); i++) v[n++] = (uint8_t)ident[i];
	v[n++] = 0x90; v[n++] = 0x26; v[n++] = 0x00;           /* read-only VPD, 38 bytes */
	v[n++] = 'P'; v[n++] = 'N'; v[n++] = 5;
	v[n++] = '1'; v[n++] = '0'; v[n++] = '0'; v[n++] = '0'; v[n++] = 'A';
	v[n++] = 'L'; v[n++] = 'L'; v[n++] = 1; v[n++] = 0;
	v[n++] = 'Z'; v[n++] = 'C'; v[n++] = 16;
	v[n++] = (uint8_t)(io >> 8);
	v[n++] = (uint8_t)(BoardMemBase() >> 24);
	v[n++] = (uint8_t)(irq > 0 ? irq : 0);
	v[n++] = (uint8_t)(LOCAL_MEMORY >> 16); v[n++] = (uint8_t)(LOCAL_MEMORY >> 24);
	v[n++] = 0x20; v[n++] = 0;
	for (unsigned i = 0; i < 9; i++) v[n++] = 0;
	v[n++] = 'Z'; v[n++] = 'R'; v[n++] = 4; v[n++] = 0; v[n++] = 0; v[n++] = 0x14; v[n++] = 0;
	v[n++] = 0x79; v[n++] = 0;                             /* end tag */
	for (unsigned i = 0; i < n; i++) phys_writeb(rom_phys + vpd_off + i, v[i]);
	INT10_SetupRomMemoryChecksum();
}

/* ---------------------------------------------------------------------------------- */
/* scheduling                                                                         */

/* The RISC runs risc_hz instructions per second of emulated time: in this 1ms tick, and
 * early when the host polls for it (CatchUpBudget), but never ahead of the clock. */
void VERITE_Tick(Bitu /*val*/) {
	if (!board) return;
	VeriteBoard &b = *board;
	const double now = PIC_FullIndex();
	double owed = (now - b.risc_time) * b.risc_hz / 1000.0;
	if (owed > 4.0 * b.risc_hz / 1000.0) owed = 4.0 * b.risc_hz / 1000.0; /* after a pause */
	const uint32_t budget = owed > 0 ? (uint32_t)owed : 0;
	const uint32_t used = b.core->Runnable() ? b.core->Run(budget) : 0;
	if (used < budget) b.core->Idle(budget - used);
	b.risc_time = now;
	PIC_AddEvent(VERITE_Tick, 1.0);
}

void VERITE_Destroy(Section * /*sec*/) {
	PIC_RemoveEvents(VERITE_Tick);
	if (pci_dev) {
		UnregisterPCIDevice(pci_dev);
		delete pci_dev;
		pci_dev = NULL;
	}
	if (mem_cb != MEM_Callout_t_none) {
		MEM_FreeCallout(mem_cb);
		mem_cb = MEM_Callout_t_none;
	}
	if (board) {
		if (board->io_bar) {
			IO_FreeReadHandler(board->io_bar, IO_MA, IO_BAR_SIZE);
			IO_FreeWriteHandler(board->io_bar, IO_MA, IO_BAR_SIZE);
		}
		delete board->core;
		delete board;
		board = NULL;
	}
	vga.native.active = false;
	PAGING_ClearTLB();
}

/* The board's ROM as the RISC sees it (0xfffe0000 up): a dump of the user's own board EPROM,
 * up to 128K bytes, read by software that expects a particular board's ROM */
void LoadBoardRom(Section_prop *section) {
	Prop_path *pp = section ? section->Get_path("verite_bios") : NULL;
	if (!pp || pp->realpath.empty()) {
		/* no dump: DOSBox-X's own loader stands in for the board's when CGL modules run */
		if (section && !strcasecmp(section->Get_string("verite_cgl"), "loader")) {
			std::vector<uint8_t> rom(Verite::ROM_SIZE);
			Verite::Core::BuildLoaderRom(rom.data(), (uint32_t)rom.size());
			board->core->SetRom(rom.data(), (uint32_t)rom.size());
			LOG_MSG("Verite: board ROM with DOSBox-X's loader for CGL modules");
		}
		return;
	}
	FILE *f = fopen(pp->realpath.c_str(), "rb");
	if (!f) {
		LOG_MSG("Verite: cannot open the BIOS file %s", pp->realpath.c_str());
		return;
	}
	std::vector<uint8_t> rom(Verite::ROM_SIZE + 1);
	const size_t n = fread(rom.data(), 1, rom.size(), f);
	fclose(f);
	if (n == 0 || n > Verite::ROM_SIZE) {
		LOG_MSG("Verite: %s is not a BIOS image of up to 128K bytes; ignored", pp->realpath.c_str());
		return;
	}
	board->core->SetRom(rom.data(), (uint32_t)n);
	LOG_MSG("Verite: board ROM loaded from %s (%u bytes)", pp->realpath.c_str(), (unsigned)n);
}

void InstallBoard() {
	if (svgaCard != SVGA_Verite || board) return;
	if (!pcibus_enable) {
		LOG_MSG("Verite: the PCI bus is not enabled, Rendition Verite not installed");
		return;
	}
	Section_prop *section = static_cast<Section_prop *>(control->GetSection("verite"));
	const uint32_t mem = BoardMemBase();
	board = new VeriteBoard();
	board->core = new Verite::Core(board, Verite::CHIP_V1000, LOCAL_MEMORY);
	board->irq = section ? section->Get_int("verite_irq") : 11;
	board->debug = section ? section->Get_bool("verite_debug") : false;
	board->trace_left = section ? section->Get_int("verite_trace") : 0;
	if (board->trace_left > 0) board->core->ctl_log_left = (unsigned)board->trace_left;
	const int mhz = section ? section->Get_int("verite_risc_mhz") : 25;
	board->risc_hz = (mhz > 0 ? mhz : 25) * 1e6;
	LoadBoardRom(section);

	pci_dev = new PCI_VeriteDevice(board->irq, mem, DEFAULT_IO_BASE);
	Bits slot = 1;
	while (slot < 32 && !PCI_IsSlotFree(0, slot)) slot++;
	RegisterPCIDevice(pci_dev, 0, slot < 32 ? slot : -1);
	MapMemory(mem);
	MapIO(DEFAULT_IO_BASE);
	IO_RegisterReadHandler(0x3DE, read_p3de, IO_MB, 2);
	board->risc_time = PIC_FullIndex();
	PIC_AddEvent(VERITE_Tick, 1.0);
	LOG_MSG("Rendition Verite V1000 installed: 4MB local memory at %08x, I/O at %04x, IRQ %d, RISC %d MHz",
		mem, DEFAULT_IO_BASE, board->irq, mhz);
}

void VERITE_OnPowerOn(Section * /*sec*/) {
	InstallBoard();
	WriteVpd(); /* the ROM's VPD carries the board resources, also before the board exists */
}

} /* anonymous namespace */

/* ---------------------------------------------------------------------------------- */
/* hooks for the VGA and BIOS code                                                    */

void VERITE_VSync(void) {
	if (!board) return;
	VeriteBoard &b = *board;
	/* latch the frame base; FrameDisplayed and FrameSwitched report the flip */
	vga.native.start = b.framebase_a;
	b.crtc_ctl |= CRTCCTL_FRAMEDISPLAYED | CRTCCTL_FRAMESWITCHED;
	if (b.Native()) b.core->VerticalRetrace();
	b.stat_frames++;
	if (b.debug && (b.stat_frames % 600) == 0) {
		LOG_MSG("Verite: %llu RISC instructions, %llu pixels, pc %08x, %llu IRQs",
			(unsigned long long)b.core->instructions, (unsigned long long)b.core->pixels, b.core->pc,
			(unsigned long long)b.irq_raises);
		LOG_MSG("Verite: crtcctl %08x horz %08x vert %08x offset %04x mode %02x memendian %u, DAC cr0 %02x cr1 %02x, frame %06x",
			b.crtc_ctl, b.crtc_horz, b.crtc_vert, b.crtc_offset, b.core->ModeReg(), b.core->MemEndian(),
			b.dac_cr0, b.dac_cr1, b.framebase_a);
		LOG_MSG("Verite: cursor cr2 %02x cr3 %02x at %u,%u, colours %02x%02x%02x %02x%02x%02x %02x%02x%02x",
			b.dac_cr2, b.dac_cr3, b.cursor_x, b.cursor_y,
			b.curcol[1][0], b.curcol[1][1], b.curcol[1][2], b.curcol[2][0], b.curcol[2][1], b.curcol[2][2],
			b.curcol[3][0], b.curcol[3][1], b.curcol[3][2]);
	}
}

PageHandler *VERITE_WindowHandler(void) {
	if (board && board->Native() && (board->core->ModeReg() & Verite::MODE_VESA)) return &window_handler;
	return NULL;
}

void VERITE_BeginSetMode(void) {
	if (!board) return;
	board->dma_en_saved = board->core->ModeReg() & Verite::MODE_DMA_EN;
	/* back to VGA mode, as the BIOS does before any mode set (DMA enable is kept) */
	board->core->IOWrite(Verite::IO_MODE, Verite::MODE_VGA | (board->core->ModeReg() & Verite::MODE_DMA_EN), 1);
	board->core->IOWrite(Verite::IO_MEMENDIAN, 0, 1);
}

bool VERITE_FinishSetMode(const VideoModeBlock *m, bool clearmem) {
	if (!board || m == NULL) return false;
	if (m->mode < 0x100) return false;
	unsigned bpp, fmt;
	uint8_t memend, cr1;
	switch (m->type) {
		case M_LIN8:  bpp = 1; fmt = 2;  memend = 1; cr1 = 0x40; break; /* indexed */
		case M_LIN15: bpp = 2; fmt = 6;  memend = 3; cr1 = 0x30; break; /* 1555, CLUT bypass */
		case M_LIN16: bpp = 2; fmt = 4;  memend = 3; cr1 = 0x38; break; /* 565, CLUT bypass */
		case M_LIN32: bpp = 4; fmt = 12; memend = 0; cr1 = 0x10; break; /* 8888 */
		default: return false;
	}
	VeriteBoard &b = *board;
	Verite::Core &c = *b.core;
	const uint8_t dma_en = b.dma_en_saved;
	unsigned w = (unsigned)m->swidth, h = (unsigned)m->sheight;
	/* Low resolution modes (320 or 400 wide) are shown pixel and line doubled, as the
	 * Rendition BIOS does: 320x200 uses 640x480 timing with 400 active lines. */
	const bool ldbl = w <= 400;
	if (w > 2048) w = 2048;
	const uint32_t pitch = w * bpp;

	/* native mode, A0000 window; the Rendition BIOS keeps DMA enable across mode sets */
	c.IOWrite(Verite::IO_MODE, Verite::MODE_VESA | dma_en, 1);
	c.IOWrite(Verite::IO_MEMENDIAN, memend, 1);
	c.IOWrite(Verite::IO_BANKSELECT, 0, 4);

	/* VESA standard timings for the output size: front porch, sync, back porch */
	struct StdTiming { unsigned w, h, hfp, hsy, hbp, vfp, vsy, vbp; double clock; };
	static const StdTiming std_timings[] = {
		{  640,  480, 16,  96,  48, 10, 2, 33,  25.175e6 },
		{  800,  600, 40, 128,  88,  1, 4, 23,  40.000e6 },
		{ 1024,  768, 24, 136, 160,  3, 6, 29,  65.000e6 },
		{ 1280, 1024, 48, 112, 248,  1, 3, 38, 108.000e6 },
		{ 1600, 1200, 64, 192, 304,  1, 3, 46, 162.000e6 }
	};
	const unsigned ow = ldbl ? w * 2 : w, oh = ldbl ? h * 2 : h;
	const StdTiming *st = &std_timings[0];
	for (const StdTiming &t : std_timings) { st = &t; if (ow <= t.w && oh <= t.h) break; }
	auto r8 = [](unsigned v) { const unsigned r = ((v + 4u) / 8u) * 8u; return r < 8u ? 8u : r; };
	const unsigned hdiv = ldbl ? 2u : 1u;
	const unsigned hfp = r8(st->hfp / hdiv), hsy = r8(st->hsy / hdiv), hbp = r8(st->hbp / hdiv);
	/* unused lines go into the porches (400 of 480 lines: front porch 59, back porch 64) */
	const unsigned extra = st->h > oh ? st->h - oh : 0, shift = extra ? 9u : 0u;
	const unsigned vfp = st->vfp + extra / 2 + shift, vsy = st->vsy, vbp = st->vbp + (extra - extra / 2) - shift;
	const uint32_t horz = ((((hfp >> 3) - 1) & 7u) << 21) | ((((hsy >> 3) - 1) & 0x1Fu) << 16) |
		((((hbp >> 3) - 1) & 0x3Fu) << 9) | (((w >> 3) - 1) & 0xFFu);
	const uint32_t vert = (((vfp - 1) & 0x3Fu) << 20) | (((vsy - 1) & 7u) << 17) |
		(((vbp - 1) & 0x3Fu) << 11) | ((oh - 1) & 0x7FFu);
	/* pixel clock through the PLL formula */
	const double target = st->clock / hdiv;
	unsigned best = b.pll_value;
	double bestdiff = 1e30;
	for (unsigned p = 0; p < 4; p++)
		for (unsigned n = 2; n <= 129; n++) {
			const unsigned mm = (unsigned)floor(target * n * (1u << p) / (2.0 * 14.31818e6) + 0.5);
			if (mm < 2 || mm > 129) continue;
			const double f = 14.31818e6 * 2.0 * mm / n / (1u << p);
			if (fabs(f - target) < bestdiff) {
				bestdiff = fabs(f - target);
				best = ((mm - 2) << 10) | (p << 8) | (n - 2);
			}
		}
	b.pll_value = best;
	/* As the Rendition BIOS: 6-bit palette for 8bpp, 8-bit DAC for direct colour */
	b.DacWrite(0x6, (m->type == M_LIN8) ? (uint8_t)(0x80 | (vga_8bit_dac ? 0x02 : 0x00)) : (uint8_t)0x82);
	b.dac_cr1 = cr1;
	b.dac_cr2 = 0x20;
	b.dac_cr3 = 0;
	b.Reg32Write(0x88, horz);
	b.Reg32Write(0x8C, vert);
	b.framebase_a = 0;
	vga.native.start = 0;
	/* CRTCOffset for the 64-byte video FIFO the Rendition BIOS selects; syncs active low */
	const uint32_t fifo = 64;
	const uint32_t last = (pitch % fifo) ? pitch - pitch % fifo : pitch - fifo;
	b.crtc_offset = (pitch - last) & 0xFFFFu;
	b.crtc_ctl = (b.crtc_ctl & (CRTCCTL_FRAMEDISPLAYED | CRTCCTL_FRAMESWITCHED)) | fmt |
		0xC00u | CRTCCTL_VIDEOENABLE | (ldbl ? CRTCCTL_LINEDOUBLE : 0u);
	if (clearmem) memset(c.Memory(), 0, (std::min<uint32_t>)(c.MemSize(), pitch * h));
	b.UpdateDisplay();
	VGA_SetupHandlers();
	return true;
}

/* Rendition BIOS INT 10h AH=15h extensions used by Speedy3D. Behaviour reproduced from
 * observing the Rendition V1000 BIOS (2.105 TSR from the reference driver package) from
 * the outside: the registers it returns and the chip registers it programs. */
bool VERITE_BiosExtension(void) {
	if (!board) return false;
	VeriteBoard &b = *board;
	Verite::Core &c = *b.core;
	auto force = [&](uint32_t ir) {
		c.IOWrite(Verite::IO_STATEINDEX, 128, 1);
		c.IOWrite(Verite::IO_STATEDATA, ir, 4);
		c.IOWrite(Verite::IO_DEBUG, Verite::DEBUG_HOLDRISC | Verite::DEBUG_STEPRISC, 1);
	};
	if (b.debug) LOG_MSG("Verite: BIOS extension AX=%04x BX=%04x CX=%04x", reg_ax, reg_bx, reg_cx);
	switch (reg_al) {
		case 0x8D: /* vital product data: far pointer in DX:CX (Speedy3D reads DX*16+CX) */
			WriteVpd();
			reg_ax = 0x0015;
			reg_dx = 0xC000;
			reg_cx = vpd_off;
			return true;
		case 0x80:
			reg_ax = 0x0015;
			reg_bx = 0x0084;
			return true;
		case 0x82: { /* prepare for a microcode load: native 640x480x8 (VESA mode 101h), RISC
		              * reset and held, display off, DMA enabled, no byte swapping, and the BIOS
		              * palette loader at local address 0 (Speedy3D's microcode calls it through
		              * the pointer at F360h, which a cleared memory leaves at 0) */
			INT10_SetVideoMode(0x101);
			c.IOWrite(Verite::IO_MODE, Verite::MODE_VESA, 1);
			c.IOWrite(Verite::IO_DEBUG, Verite::DEBUG_SOFTRESET | Verite::DEBUG_HOLDRISC, 1);
			c.IOWrite(Verite::IO_DEBUG, Verite::DEBUG_HOLDRISC, 1);
			c.IOWrite(0xA0, 0x140000, 4);                  /* DRAM control */
			c.IOWrite(Verite::IO_INTR, 0xFFFF, 4);
			c.IOWrite(0x84, c.IORead(0x84, 4) & ~(CRTCCTL_VIDEOENABLE | 0xC00u), 4);
			c.IOWrite(Verite::IO_MODE, Verite::MODE_VESA | Verite::MODE_DMA_EN, 1);
			c.IOWrite(Verite::IO_LOWWATERMARK, 3, 1);
			c.IOWrite(Verite::IO_MEMENDIAN, 0, 1);
			c.IOWrite(Verite::IO_BANKSELECT, 0, 4);
			/* palette loader, called with the return address in ra. FIFO: one word
			 * (first index << 16) | count, then count words 00RRGGBB. Uses %160-%165. */
			static const uint32_t palette_loader[] = {
				0x10a42100, /*      add   %164, fifo, zero     header        */
				0x47a0a410, /*      sr    %160, %164, #16      first index   */
				0x45a1a410, /*      sl    %161, %164, #16                    */
				0x47a1a110, /*      sr    %161, %161, #16      count         */
				0x77a3ffe0, /*      lui   %163, #0xffe0                      */
				0x76a50580, /*      li    %165, #0x0580                      */
				0x15a3a3a5, /*      or    %163, %163, %165     Bt485 at B0h  */
				0x7800a0a3, /*      sb    0(%163), %160        write address */
				0x10a42100, /* 1:   add   %164, fifo, zero     00RRGGBB      */
				0x47a2a410, /*      sr    %162, %164, #16                    */
				0x7808a2a3, /*      sb    8(%163), %162        red           */
				0x47a2a408, /*      sr    %162, %164, #8                     */
				0x7808a2a3, /*      sb    8(%163), %162        green         */
				0x01a1a101, /*      sub   %161, %161, #1                     */
				0x64fff9a1, /*      bgz   %161, 1b                           */
				0x7808a4a3, /*      sb    8(%163), %164        blue (delay)  */
				0x6f0000fe, /*      jmprl zero, (ra)                         */
				0x00000000  /*      nop                                      */
			};
			for (unsigned i = 0; i < sizeof(palette_loader) / sizeof(palette_loader[0]); i++)
				c.Write32(i * 4, palette_loader[i]);
			reg_ax = 0x0015;
			return true;
		}
		case 0x83: /* flush the instruction cache, start the RISC at CX, clear interrupts */
			c.IOWrite(Verite::IO_DEBUG, Verite::DEBUG_HOLDRISC, 1);
			force(0); force(0); force(0);
			force(0x6C000000u | ((uint32_t)reg_cx >> 2)); /* jmp CX */
			force(0);
			c.IOWrite(Verite::IO_INTR, 0xFF, 1);
			c.IOWrite(Verite::IO_DEBUG, 0, 1);
			reg_ax = 0x0015;
			reg_bx = 0x0064;
			reg_di = 0x0200;
			return true;
		case 0x84: /* release the RISC */
			c.IOWrite(Verite::IO_DEBUG, c.IORead(Verite::IO_DEBUG, 1) & ~Verite::DEBUG_HOLDRISC, 1);
			reg_ax = 0x0015;
			return true;
		case 0x87: /* RAMDAC direct colour setup: clears the Bt485 CR1 palette bypass bit for BX=2.
		            * vQuake requests this but never loads a ramp, and its output is correct on
		            * real boards, so direct colour modes are always displayed without the
		            * palette (see docs/Rendition-Verite.md) */
			if (reg_bx & 2) b.dac_cr1 &= (uint8_t)~0x10;
			else b.dac_cr1 |= 0x10;
			reg_ax = 0x0015;
			reg_dx = reg_bx;
			return true;
		default:
			LOG_MSG("Verite: unknown Rendition BIOS function AX=%04x BX=%04x CX=%04x DX=%04x", reg_ax, reg_bx, reg_cx, reg_dx);
			return false;
	}
}

void VERITE_SetupRom(uint32_t rom_base, uint16_t &used) {
	/* PCI data structure: the pointer at 18h is how option ROMs are located. Rendition's
	 * ROM has the class code with the base class first (Speedy3D checks PCIR+0Dh == 3). */
	used = (uint16_t)((used + 3u) & ~3u);
	rom_phys = rom_base;
	pcir_off = used;
	used += 0x18;
	vpd_off = used;
	used += 0x60;
	phys_writew(rom_base + 0x18, pcir_off);
	const uint8_t pcir[0x18] = {
		'P', 'C', 'I', 'R',
		(uint8_t)RENDITION_VENDOR_ID, (uint8_t)(RENDITION_VENDOR_ID >> 8),
		(uint8_t)V1000_DEVICE_ID, (uint8_t)(V1000_DEVICE_ID >> 8),
		(uint8_t)(vpd_off), (uint8_t)(vpd_off >> 8),   /* pointer to vital product data */
		0x18, 0x00,                                     /* structure length */
		0x00,                                           /* structure revision */
		0x03, 0x00, 0x00,                               /* class code (display controller) */
		0x40, 0x00,                                     /* image length: 32KB */
		0x00, 0x00,                                     /* code revision */
		0x00,                                           /* x86 code */
		0x80,                                           /* last image */
		0x00, 0x00 };
	for (unsigned i = 0; i < sizeof(pcir); i++) phys_writeb(rom_base + pcir_off + i, pcir[i]);
	WriteVpd();
}

uint32_t VERITE_LfbBase(void) {
	return board ? board->mem_bar : 0;
}

void VERITE_SetBank(uint32_t offset) {
	if (!board) return;
	board->core->IOWrite(Verite::IO_BANKSELECT, offset & 0x00FFE000u, 4);
}

uint32_t VERITE_GetBank(void) {
	return board ? (board->core->BankSelect() & 0x00FFE000u) : 0;
}

Verite::Core *VERITE_GetCore(void) {
	return board ? board->core : NULL;
}

uint64_t VERITE_FrameCount(void) {
	return board ? board->stat_frames : 0;
}

bool VERITE_CglSetMode(unsigned width, unsigned height, unsigned type) {
	if (!board) return false;
	VideoModeBlock m = {};
	m.mode = 0x1FF;
	m.type = (VGAModes)type;
	m.swidth = width;
	m.sheight = height;
	VERITE_BeginSetMode();
	return VERITE_FinishSetMode(&m, true);
}

void VERITE_CglEndMode(void) {
	if (!board) return;
	VERITE_BeginSetMode();
	board->UpdateDisplay();
}

void VERITE_SetDisplayStart(uint32_t offset) {
	if (!board) return;
	board->Reg32Write(0x94, offset);
}

uint32_t VERITE_GetDisplayStart(void) {
	return board ? board->framebase_a : 0;
}

void VERITE_SetPitch(uint32_t bytes) {
	if (!board || !vga.native.active) return;
	const uint32_t fifo = (board->crtc_ctl & CRTCCTL_FIFO256) ? 256u : ((board->crtc_ctl & CRTCCTL_FIFO128) ? 128u : 64u);
	const unsigned bpp = (std::max)(1u, Verite::FormatBytes(board->crtc_ctl & CRTCCTL_SCRNFMT));
	const uint32_t w = vga.native.width * bpp;
	const uint32_t last = (w % fifo) ? w - w % fifo : ((board->framebase_a & 7) ? w : w - fifo);
	board->Reg32Write(0x98, (bytes - last) & 0xFFFFu);
}

uint32_t VERITE_GetPitch(void) {
	return (board && vga.native.active) ? vga.native.pitch : 0;
}

static bool VERITE_AcceptsMode(Bitu mode) {
	if (mode < 0x100) return mode <= 0x13 || mode == 0x6A;
	VideoModeBlock *m = NULL;
	for (VideoModeBlock *p = ModeList_VGA; p->mode != 0xFFFF; p++)
		if (p->mode == mode) { m = p; break; }
	if (m == NULL) return false;
	switch (m->type) {
		case M_LIN8: case M_LIN15: case M_LIN16: case M_LIN32: break;
		default: return false;
	}
	return m->swidth <= 1600 && m->sheight <= 1200 && VideoModeMemSize(m, mode) <= LOCAL_MEMORY;
}

void SVGA_Setup_Verite(void) {
	/* memory of the VGA core used for standard VGA modes; the VESA mode list is
	 * checked against the 4MB of local memory */
	vga.mem.memsize = LOCAL_MEMORY;
	vga.max_svga_width = 1600;
	vga.max_svga_height = 1200;
	svga.accepts_mode = &VERITE_AcceptsMode;
}

void VERITE_Init(void) {
	if (svgaCard == SVGA_Verite)
		assigned_mem_base = MEM_HardwareAllocate("Verite", MEM_BAR_SIZE);
	AddExitFunction(AddExitFunctionFuncPair(VERITE_Destroy), true);
	AddVMEventFunction(VM_EVENT_POWERON, AddVMEventFunctionFuncPair(VERITE_OnPowerOn));
}
