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

/* PowerVR Series 1 (NEC/VideoLogic PCX1 and PCX2) 3D accelerator.
 *
 * The PCX1/PCX2 is a 3D-only PCI card: it has no display output of its own. The guest's
 * SGL library builds a display list in host memory (ISP plane parameters reached through
 * an on-chip page table, plus a list of "object pointers" grouped by screen tile), puts the
 * TSP (texturing/shading) parameters and the textures in the card's texture memory, and
 * then writes START_RENDER. The chip renders the frame tile by tile (tile based deferred
 * rendering) and bus-masters the finished pixels straight into the VGA card's linear frame
 * buffer at the physical address in SOF_ADDR, then raises an end-of-render interrupt.
 *
 * This is a synchronous software implementation of that pipeline: the whole frame is
 * rendered at the moment START_RENDER is written, and end-of-render is reported at once.
 *
 * References (all MIT licensed, Imagination Technologies 1995-2022,
 * github.com/powervr-graphics/PowerVR-Series1):
 *   [HWREGS]  Source/pcx/hwregs.h, hwregs.c: register map and how the driver programs it.
 *   [TEXAS]   Source/pcx/hwtexas.h, texas.c: TSP control word bits, fog colour/camera regs.
 *   [PVRLIMS] Source/pvrlims.h: tile header and object pointer formats for PCX1 and PCX2.
 *   [PTO20B]  Source/pto20b.h: the PCX1 "20 bit float" format of the ISP A and B params.
 *   [SIM1]    Source/simulat/ (PCX1-era vendor simulator): TSP texture sampling without
 *             bilinear filtering (point sampling, linear blend between two mipmap levels).
 *   [SIM3]    Source/simulat3/ (PCX2-era vendor simulator): hwsabsim.c and hwsabren.c are
 *             the ISP ("SABRE") cell and tile traversal model, texas.c the TSP model with
 *             bilinear filtering, the PCX2 floating point planes and linked object lists.
 *   [W32]     Source/win32/system.c, brdsetup.c: PCX1 object pointers are a contiguous
 *             buffer whose physical address goes in OBJECT_OFFSET; ISP planes are reached
 *             through the TLB; texture memory is PCI BAR1 (win32/texif.c).
 *   [DOS32]   Source/dos32/pvrosapi.c, irq.c, isr.asm: BAR0 is the register file, reading
 *             INT_STATUS acknowledges the interrupt, end-of-render is INT_STATUS bit 1.
 *
 * The ISP and TSP below are ports of [SIM3] hwsabsim.c/hwsabren.c and [SIM1]/[SIM3]
 * texas.c: cell state machine, instruction expansion, depth evaluation, translucent pass
 * handling, fog, flat/smooth shading, shadows, highlights and texture addressing follow the
 * vendor's own software models of the hardware. Where this file departs from them it says so.
 *
 * Things the released source does not document, and what is done here instead:
 *   - The TLB entry and PAGE_CTRL formats are written only by the (unreleased) VxD. The
 *     driver's own tables hold one physical address per slot and a page size of 4/8/16KB
 *     ([W32] DMASBToPCXTLB), so a TLB entry is taken to be the slot's physical address and
 *     PAGE_CTRL bits 1:0 the page size code. An entry that is not page aligned, or that is
 *     below 1MB, is taken to be a page frame number instead.
 *   - Where the driver copies the TSP parameters is also in the VxD. They are read from
 *     texture memory at PREC_BASE (a 32-bit word offset: the driver's ISP_BASE value
 *     0x80000 is commented as "the bottom of the second bank", i.e. 2MB).
 *   - PCX_ID/PCX_REVISION read-back values are unknown; the PCI IDs are returned.
 *   - X_CLIP is taken to discard pixels left of / right of the programmed columns.
 *   - The dither bit of PACKMODE is ignored (output is truncated).
 *   - Depth values that overflow the ISP's 32-bit comparator are saturated rather than
 *     wrapped, and 8-bit (332) texels are always expanded to 8 bits per channel ([SIM1]
 *     skips that expansion on its point sampled path, which would darken such textures).
 */

#include <algorithm>
#include <assert.h>
#include <math.h>
#include <string.h>
#include <stdint.h>
#include <vector>

#include "dosbox.h"
#include "logging.h"
#include "setup.h"
#include "control.h"
#include "mem.h"
#include "paging.h"
#include "pic.h"
#include "pci_bus.h"

namespace {

/* register indices, 32-bit word offsets into BAR0 [HWREGS] */
enum {
	PCX_ID            = 0x000,
	PCX_REVISION      = 0x001,
	PCX_SOFTRESET     = 0x002,
	PCX_INTSTATUS     = 0x003,
	PCX_INTMASK       = 0x004,
	PCX_STARTRENDER   = 0x005,
	PCX_FOGAMOUNT     = 0x006,
	PCX_OBJECT_OFFSET = 0x007,
	PCX_PAGE_CTRL     = 0x008,
	PCX_ISP_BASE      = 0x00A,
	PCX_PREC_BASE     = 0x00B,
	PCX_TMEM_SETUP    = 0x00C,
	PCX_TMEM_REFRESH  = 0x00D,
	PCX_FOGCOL        = 0x00E,
	PCX_CAMERA        = 0x00F,
	PCX_PACKMODE      = 0x010,
	PCX_ARBMODE       = 0x011,
	PCX_LSTRIDE       = 0x012,
	PCX_SOFADDR       = 0x013,
	PCX_XCLIP         = 0x014,
	PCX_ABORTADDR     = 0x015,
	PCX_GPPORT        = 0x016,
	PCX_IEEEFP        = 0x018, /* PCX2 */
	PCX_BILINEAR      = 0x019, /* PCX2 */
	PCX_FOG_TABLE     = 0x080,
	PCX_TLB           = 0x100,
	PCX_DIVIDER_TABLE = 0x200,
	PCX_NUM_REGS      = 0x400
};

enum {
	INT_END_OF_RENDER = 0x00000002u /* [HWREGS] INTMASK default, [DOS32] isr.asm */
};

const uint16_t NEC_VENDOR_ID  = 0x1033;
const uint16_t PCX1_DEVICE_ID = 0x002A; /* [PCX] brdsetup.h */
const uint16_t PCX2_DEVICE_ID = 0x0046;

const uint32_t REG_BAR_SIZE  = 4096;       /* 0x400 registers */
const uint32_t TMEM_SIZE     = 4u << 20;   /* 4MB texture memory (TMEM_4M_SETTING) */
const uint32_t TMEM_WORDS    = TMEM_SIZE / 4;
const uint32_t BIG_BANK      = 0x100000;   /* in 16-bit pixel units [TEXAS] */

const int NUM_SABRE_CELLS = 32;

/* region header / object pointer bits [PVRLIMS] */
const uint32_t REG_COMPULSARY_BITS = 0x40000000u;
const uint32_t LINK_LIST_BIT       = 0x20000000u; /* PCX2 only */
const uint32_t OBJ_VERY_LAST_PTR   = 0x80000000u;

/* ISP plane instructions ([PVRLIMS]/hwinterf.h INSTR_CODE_ENUMS, unshifted) */
enum {
	forw_visib = 0x0, forw_invis, forw_perp, test_shad_forw, test_shad_perp,
	rev_visib, rev_invis, rev_replace_if, forw_visib_fp, forw_invis_fp, forw_perp_fp,
	test_shad_forw_fp, test_shad_perp_fp, test_shadow_rev, test_light_rev, begin_trans
};

/* expanded ("wide") cell commands [SIM3] sabre.h */
enum { load_i_nop = 0, load_i_invis_forw = 1, load_i_closer = 2, load_i_further = 4, load_i = 8 };
enum { load_u_nop = 0, load_u_invis_forw = 1, load_u = 2, load_u_closer = 3 };
enum { test_shad_nop = 0, test_shadow_further = 1, test_shad_closer = 2, test_light_further = 3 };
enum { prev_none = 0, prev_shad_forw = 1, prev_light = 2, prev_shadow = 3, prev_trans = 6, prev_def_rep = 7 };

/* TSP control word bits [TEXAS] hwtexas.h */
const uint32_t MASK_TEXTURE        = 0x80000000u;
const uint32_t MASK_SMOOTH_SHADE   = 0x40000000u;
const uint32_t MASK_DISABLE_FOG    = 0x20000000u;
const uint32_t MASK_SHADOW_FLAG    = 0x10000000u;
const uint32_t MASK_FLAT_HIGHLIGHT = 0x04000000u;
const uint32_t MASK_EXPONENT       = 0x003C0000u;
const uint32_t MASK_GLOBAL_TRANS   = 0x0001E000u;
const uint32_t MASK_FLIP_UV        = 0x00001800u;
const uint32_t MASK_TRANS          = 0x00000400u;
const uint32_t MASK_MIP_MAPPED     = 0x80000000u;
const uint32_t MASK_8_16_MAPS      = 0x40000000u;
const uint32_t MASK_MAP_SIZE       = 0x30000000u;
const uint32_t MASK_4444_555       = 0x08000000u;
const uint32_t MASK_PMIP_M         = 0xFF000000u;
const uint32_t MASK_PMIP_E         = 0x00FC0000u;

/* safety limits against malformed display lists */
const uint32_t MAX_REGIONS_PER_RENDER = 65536;
const uint32_t MAX_OBJECTS_PER_REGION = 65536;
const uint32_t MAX_PLANES_PER_REGION  = 262144;
const int      MAX_LINK_HOPS          = 10; /* as [SIM3] */
/* Work budget per render, in plane-span evaluations (one plane against 32 pixels) and
 * decoded planes. A busy 640x480 frame needs a few million of the former; a garbage list
 * could otherwise keep the emulator busy for minutes. */
const uint64_t MAX_SPAN_PLANES_PER_RENDER = 256u << 20;
const uint32_t MAX_PLANES_PER_RENDER      = 4u << 20;

struct CellState {
	int32_t  I_depth, U_depth;
	uint32_t I_id, U_id;
	bool     I_visib, I_forward, shad_temp, U_visib, U_shadow;
};

struct CellControl {
	uint8_t i_load, u_load, test_shad;
	bool    plane_visib, delayed_clear_u_id, plane_perp, mux_sel;
};

struct Plane {
	int32_t  a30, b30;   /* A and B converted from 20-bit float to integer */
	int32_t  c;
	uint8_t  instr;
	uint32_t index;      /* 18-bit TSP tag */
};

struct Object {
	uint32_t first_plane, num_planes;
};

struct RGBi { int r, g, b; };

struct PFloat { int32_t m; int e; };

uint16_t twiddle_table[256];

void InitTwiddle() {
	for (int i = 0; i < 256; i++) {
		uint16_t t = 0;
		for (int b = 0; b < 8; b++) t |= ((i >> b) & 1) << (2 * b);
		twiddle_table[i] = t;
	}
}

inline uint32_t Twid(int x, int y) {
	return ((uint32_t)twiddle_table[x & 255] << 1) | twiddle_table[y & 255];
}

/* PCX1 20-bit float: 15-bit magnitude, sign in bit 15, exponent in bits 19:16 [PTO20B] */
inline int32_t Conv20To30(uint32_t n) {
	int32_t v = (int32_t)(n & 0x7FFF);
	if (n & 0x8000) v = -v;
	return v << ((n >> 16) & 0xF);
}

/* [PTO20B] PackTo20Bit, IEEE version, used for PCX2 floating point planes as in [SIM3] */
uint32_t PackTo20Bit(float value) {
	uint32_t iv;
	memcpy(&iv, &value, 4);
	int exp = (int)((iv >> 23) & 0xFF) - 102;
	uint32_t mantissa = (iv & 0x7FFFFF) | 0x800000;
	const uint32_t sign = (iv >> 16) & 0x8000;
	if (exp >= 0) {
		if (exp > 14) return (14u << 16) | 0x7FFF | sign; /* out of range: saturate */
		return ((uint32_t)exp << 16) | (mantissa >> 9) | sign;
	}
	if (exp < -15) return 0;
	mantissa >>= -exp;
	mantissa += 1u << 8;
	mantissa >>= 9;
	return mantissa | sign;
}

inline int ToInt(uint32_t x) {
	return (int)(int16_t)(uint16_t)(x & 0xFFFF);
}

inline int32_t SatInt32(int64_t v) {
	if (v > INT32_MAX) return INT32_MAX;
	if (v < INT32_MIN) return INT32_MIN;
	return (int32_t)v;
}

/* [SIM3] texas.c ToPfloat, without its loop running off the end for large values */
PFloat ToPfloat(int32_t x) {
	PFloat t;
	const int64_t ax = x < 0 ? -(int64_t)x : (int64_t)x;
	t.e = 0;
	while (t.e < 40 && ((int64_t)1 << t.e) <= ax) t.e++;
	int32_t m;
	if (t.e <= 31) m = (int32_t)(uint32_t)((uint64_t)(int64_t)x << (31 - t.e));
	else m = x >> (t.e - 31);
	t.m = m >> 16;
	return t;
}

inline int32_t AShift(int32_t x, int shift) {
	if (shift < 0) {
		if (shift < -31) return x < 0 ? -1 : 0;
		return x >> -shift;
	}
	if (shift > 31) return 0;
	return (int32_t)((uint32_t)x << shift);
}

inline RGBi ConvertFrom16to24(uint32_t raw) {
	RGBi p;
	p.r = (int)(((raw >> 10) & 0x1F) << 3) + 4;
	p.g = (int)(((raw >> 5) & 0x1F) << 3) + 4;
	p.b = (int)((raw & 0x1F) << 3) + 4;
	return p;
}

struct RGBA { int r, g, b, a; };

class PowerVR;
PowerVR *pvr = NULL;

class PowerVR {
public:
	bool     pcx2;
	int      irq;
	bool     debug_log;
	uint32_t regs[PCX_NUM_REGS];
	uint8_t *tmem;
	uint32_t bar0, bar1;         /* current physical BAR addresses (0 = unmapped) */
	unsigned renders;

	PowerVR(bool is_pcx2, int irq_line, bool dbg) : pcx2(is_pcx2), irq(irq_line), debug_log(dbg),
		bar0(0), bar1(0), renders(0), prev_intern_out(prev_none), sec_obj(false), first_obj(false) {
		tmem = new uint8_t[TMEM_SIZE];
		memset(tmem, 0, TMEM_SIZE);
		Reset();
	}
	~PowerVR() {
		if (irq > 0) PIC_DeActivateIRQ((unsigned int)irq);
		delete[] tmem;
	}

	void Reset() {
		memset(regs, 0, sizeof(regs));
		regs[PCX_ID] = ((uint32_t)(pcx2 ? PCX2_DEVICE_ID : PCX1_DEVICE_ID) << 16) | NEC_VENDOR_ID;
		regs[PCX_REVISION] = pcx2 ? 0x02 : 0x01;
		prev_intern_out = prev_none;
		UpdateIRQ();
	}

	void UpdateIRQ() {
		if (irq <= 0) return;
		if (regs[PCX_INTSTATUS] & regs[PCX_INTMASK]) PIC_ActivateIRQ((unsigned int)irq);
		else PIC_DeActivateIRQ((unsigned int)irq);
	}

	unsigned trace_count = 0;
	bool Trace() {
		if (!debug_log || trace_count >= 4000) return false;
		if (++trace_count == 4000) LOG_MSG("PowerVR: trace limit reached, further register accesses not logged");
		return true;
	}

	uint32_t ReadReg(uint32_t idx) {
		idx &= PCX_NUM_REGS - 1;
		if (idx < PCX_FOG_TABLE && idx != PCX_INTSTATUS && Trace())
			LOG_MSG("PowerVR: read  reg %03x = %08x", idx * 4, regs[idx]);
		if (idx == PCX_INTSTATUS) {
			/* reading acknowledges the interrupt [DOS32] isr.asm */
			const uint32_t v = regs[PCX_INTSTATUS];
			regs[PCX_INTSTATUS] = 0;
			UpdateIRQ();
			return v;
		}
		return regs[idx];
	}

	void WriteReg(uint32_t idx, uint32_t val) {
		idx &= PCX_NUM_REGS - 1;
		if ((idx < PCX_FOG_TABLE || (idx >= PCX_TLB && idx < PCX_TLB + 4)) && Trace())
			LOG_MSG("PowerVR: write reg %03x = %08x", idx * 4, val);
		switch (idx) {
			case PCX_ID:
			case PCX_REVISION:
				break;
			case PCX_INTSTATUS:
				regs[idx] &= ~val;
				UpdateIRQ();
				break;
			case PCX_INTMASK:
				regs[idx] = val;
				UpdateIRQ();
				break;
			case PCX_SOFTRESET:
				regs[idx] = val;
				break;
			case PCX_STARTRENDER:
				regs[idx] = val;
				Render();
				regs[PCX_INTSTATUS] |= INT_END_OF_RENDER;
				UpdateIRQ();
				break;
			default:
				regs[idx] = val;
				break;
		}
	}

	/* ---- memory the chip reads while rendering ---- */

	inline uint32_t TMemWord(uint32_t word) const {
		const uint8_t *p = tmem + (word & (TMEM_WORDS - 1)) * 4;
		return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
	}

	/* 16-bit texel at a pixel address: two per 32-bit word, even address in the upper half
	 * ([SIM3] FetchPixel, texapi.c WriteTexture) */
	inline uint32_t FetchPixel(uint32_t address) const {
		const uint32_t w = TMemWord(address >> 1);
		return (address & 1) ? (w & 0xFFFF) : (w >> 16);
	}

	inline uint32_t FetchParameter(uint32_t address) const {
		return TMemWord(regs[PCX_PREC_BASE] + address);
	}

	/* TLB entry to physical address. Tomb Raider (SGL4DOS 1.27) writes page frame numbers;
	 * an entry that is page aligned and at or above 1MB is taken as an address. */
	static uint32_t TLBEntryPhys(uint32_t entry) {
		if ((entry & 0xFFF) != 0 || entry < 0x100000) return entry << 12;
		return entry;
	}

	unsigned tlb_shift = 12;

	/* Page size of the TLB slots. PAGE_CTRL's encoding is not documented (Tomb Raider writes
	 * 0x300 with 16KB slots), but the driver fills the slots with physically consecutive
	 * blocks ([W32] DMASBToPCXTLB), so the spacing of the first two entries gives it. */
	void DetermineTLBPageSize() {
		const uint32_t p0 = TLBEntryPhys(regs[PCX_TLB]), p1 = TLBEntryPhys(regs[PCX_TLB + 1]);
		const uint32_t d = p1 - p0;
		if (regs[PCX_TLB + 1] != 0 && (d == 0x1000 || d == 0x2000 || d == 0x4000))
			tlb_shift = d == 0x1000 ? 12 : (d == 0x2000 ? 13 : 14);
		else
			tlb_shift = 12 + ((regs[PCX_PAGE_CTRL] & 3) > 2 ? 2 : (regs[PCX_PAGE_CTRL] & 3));
	}

	uint32_t TLBPhys(uint32_t byte_offset) const {
		const uint32_t slot = (byte_offset >> tlb_shift) & 0xFF;
		return TLBEntryPhys(regs[PCX_TLB + slot]) + (byte_offset & ((1u << tlb_shift) - 1));
	}

	inline uint32_t PlaneWord(uint32_t word_index) const {
		return phys_readd(TLBPhys(word_index * 4));
	}

	/* object pointer / region data */
	uint32_t ObjWord(uint32_t word_index) const {
		const uint32_t off = regs[PCX_OBJECT_OFFSET];
		if (pcx2 && (off & 1)) return phys_readd(TLBPhys((off & ~3u) + word_index * 4));
		return phys_readd(off + word_index * 4);
	}

	uint32_t ObjWordStripped(uint32_t idx) const {
		uint32_t w = ObjWord(idx);
		/* PCX2 start-of-translucent-pass bit [SIM3] */
		if (pcx2 && (w & 0x80000000u) && (w & LINK_LIST_BIT)) w &= 0x5FFFFFFFu;
		return w;
	}

	/* follow PCX2 link list pointers; returns false on too many hops */
	bool ResolveLinks(uint32_t &idx, uint32_t &w) const {
		if (!pcx2) return true;
		int hops = 0;
		while (w & LINK_LIST_BIT) {
			if (++hops > MAX_LINK_HOPS) return false;
			idx = w & 0x00FFFFFFu;
			w = ObjWordStripped(idx);
		}
		return true;
	}

	/* ---- ISP ([SIM3] hwsabsim.c) ---- */

	int prev_intern_out;
	bool sec_obj, first_obj;

	static CellControl Wide(uint8_t i, uint8_t u, uint8_t shad, bool pvis, bool clear_uid, bool pper, bool muxsel) {
		CellControl c;
		c.i_load = i; c.u_load = u; c.test_shad = shad; c.plane_visib = pvis;
		c.delayed_clear_u_id = clear_uid; c.plane_perp = pper; c.mux_sel = muxsel;
		return c;
	}

	CellControl ExpandInstruction(int instr, bool do_flush) {
		CellControl w;
		if (do_flush) return Wide(load_i_nop, load_u_closer, test_shad_nop, true, false, false, true);

		const bool sec = sec_obj;
		int prev_intern = prev_none;
		switch (instr) {
			default:
			case forw_visib:    w = Wide(load_i_further, load_u_nop, test_shad_nop, true, false, false, false); break;
			case forw_visib_fp: w = Wide(load_i, sec ? load_u : load_u_closer, test_shad_nop, true, false, false, true); break;
			case forw_invis:    w = Wide(load_i_further, load_u_nop, test_shad_nop, false, false, false, false); break;
			case forw_invis_fp: w = Wide(load_i, sec ? load_u : load_u_closer, test_shad_nop, false, false, false, true); break;
			case forw_perp:     w = Wide(load_i_further, load_u_nop, test_shad_nop, false, false, true, false); break;
			case forw_perp_fp:  w = Wide(load_i, sec ? load_u : load_u_closer, test_shad_nop, true, false, true, true); break;
			case rev_visib:     w = Wide(load_i_closer, load_u_nop, test_shad_nop, true, false, false, false); break;
			case rev_invis:     w = Wide(load_i_closer, load_u_nop, test_shad_nop, false, false, false, false); break;
			case rev_replace_if:
				w = Wide(load_i_closer, load_u_nop, test_shad_nop, false, false, false, false);
				prev_intern = prev_def_rep;
				break;
			case test_shad_forw:
				w = Wide(load_i_further, load_u_nop, test_shad_nop, true, false, false, false);
				prev_intern = prev_shad_forw;
				break;
			case test_shad_forw_fp:
				w = Wide(load_i, sec ? load_u : load_u_closer, test_shad_nop, true, false, false, true);
				prev_intern = prev_shad_forw;
				break;
			case test_shad_perp:
				w = Wide(load_i_further, load_u_nop, test_shad_nop, false, false, true, false);
				prev_intern = prev_shad_forw;
				break;
			case test_shad_perp_fp:
				w = Wide(load_i, sec ? load_u : load_u_closer, test_shad_nop, false, false, true, true);
				prev_intern = prev_shad_forw;
				break;
			case test_shadow_rev:
				w = Wide(load_i_closer, load_u_nop, test_shad_nop, false, false, false, false);
				prev_intern = prev_shadow;
				break;
			case test_light_rev:
				w = Wide(load_i_closer, load_u_nop, test_shad_nop, false, false, false, false);
				prev_intern = prev_light;
				break;
			case begin_trans:
				w = Wide(load_i_nop, load_u_closer, test_shad_nop, true, false, false, true);
				prev_intern = prev_trans;
				break;
		}
		switch (prev_intern_out) {
			case prev_shad_forw:
				w.test_shad = test_shad_closer;
				w.i_load = load_i;
				w.mux_sel = true;
				break;
			case prev_shadow:
				w.test_shad = test_shadow_further;
				w.u_load = load_u_nop;
				break;
			case prev_light:
				w.test_shad = test_light_further;
				w.u_load = load_u_nop;
				break;
			case prev_trans:
				w.delayed_clear_u_id = true;
				w.u_load = load_u_nop;
				break;
			case prev_def_rep:
				w.i_load = load_i_invis_forw;
				break;
			default:
				break;
		}
		prev_intern_out = prev_intern;
		return w;
	}

	static inline void SurfProcess(CellState &cell, const CellControl &ins, int32_t C, uint32_t index) {
		if (ins.delayed_clear_u_id) cell.U_id = 0;

		const bool sign_bit = C < 0;
		const int32_t mux_depth = ins.mux_sel ? cell.U_depth : C;
		const bool I_gte_mux = cell.I_depth >= mux_depth;
		const bool I_lte_mux = cell.I_depth <= mux_depth;

		bool I_RE = false, I_visib = cell.I_visib, I_forward = cell.I_forward;
		switch (ins.i_load) {
			case load_i:
				I_RE = true; I_forward = true; I_visib = ins.plane_visib;
				break;
			case load_i_further:
				if ((!ins.plane_perp && I_gte_mux) || (ins.plane_perp && sign_bit)) {
					I_RE = true; I_forward = true; I_visib = ins.plane_visib;
				}
				break;
			case load_i_closer:
				if (!I_gte_mux) {
					I_RE = true; I_forward = false; I_visib = ins.plane_visib;
				}
				break;
			case load_i_invis_forw:
				if (!cell.I_visib && cell.I_forward) {
					I_RE = true; I_forward = false; I_visib = ins.plane_visib;
				}
				break;
			default:
				break;
		}

		bool U_RE = false, U_ID_RE = false, U_visib = cell.U_visib;
		switch (ins.u_load) {
			case load_u:
				U_RE = true; U_ID_RE = true; U_visib = cell.I_visib;
				break;
			case load_u_closer:
				if ((I_gte_mux && cell.I_visib) || !cell.U_visib) {
					U_RE = true; U_ID_RE = true; U_visib = cell.I_visib;
				}
				break;
			default:
				break;
		}

		bool U_shadow = cell.U_shadow, shad_temp = cell.shad_temp;
		if (U_RE) {
			U_shadow = false;
			shad_temp = false; /* [SIM3] leaves shad_temp_intern at its FALSE initial value */
		}
		else {
			switch (ins.test_shad) {
				case test_shad_closer:
					shad_temp = !I_lte_mux;
					break;
				case test_shadow_further:
					U_shadow = cell.U_shadow || (I_lte_mux && cell.shad_temp);
					break;
				case test_light_further:
					U_shadow = !((I_lte_mux && cell.shad_temp) || !cell.U_shadow);
					break;
				default:
					break;
			}
		}

		cell.U_visib = U_visib;
		cell.U_shadow = U_shadow;
		if (U_RE) cell.U_depth = cell.I_depth;
		if (U_ID_RE) cell.U_id = cell.I_id;
		cell.I_visib = I_visib;
		cell.I_forward = I_forward;
		cell.shad_temp = shad_temp;
		if (I_RE) {
			cell.I_depth = C;
			cell.I_id = index;
		}
	}

	/* ---- fog ([SIM3] hwsabren.c Fog, table from the FOG_TABLE registers) ---- */

	int fog_table[128];

	void LoadFogTable() {
		bool any = false;
		for (int i = 0; i < 128; i++) {
			fog_table[i] = (int)(regs[PCX_FOG_TABLE + i] & 0xFF);
			if (fog_table[i]) any = true;
		}
		if (!any) { /* not loaded: use the simulator's power table */
			for (int i = 0; i < 128; i++) fog_table[i] = (int)(pow(2.0, -i / 128.0) * 256.0);
			fog_table[0] = 255;
		}
	}

	int Fog(int32_t depth) const {
		if (depth < 0) depth = 0;
		const unsigned shift = regs[PCX_FOGAMOUNT] & 31;
		const int32_t idx = depth >> shift;
		int32_t fac = idx >> 7;
		if (fac > 8) fac = 9;
		return fog_table[idx & 0x7F] >> fac;
	}

	/* ---- TSP ([SIM1]/[SIM3] texas.c) ---- */

	uint32_t AddressCalc(int u, int v, uint32_t address, int mapSize, int whichMap,
			bool colourDepth, bool mipMapped, int flipUV) const {
		static const uint32_t mmc[] = { /* PCX1: maps aligned to 2 pixels */
			0x5556, 0x1556, 0x0556, 0x0156, 0x0056, 0x0016, 0x0006, 0x0002, 0x0001 };
		static const int mapMask[] = { 0xFF, 0x7F, 0x3F, 0x1F };

		if (mapSize && flipUV) {
			if (((mapMask[mapSize] + 1) & u) && (flipUV & 2)) u = ~u;
			if (((mapMask[mapSize] + 1) & v) && (flipUV & 1)) v = ~v;
		}
		if (mapSize == 0 && flipUV) {
			if ((128 & u) && (flipUV & 2)) u = ~u;
			if ((128 & v) && (flipUV & 1)) v = ~v;
		}
		u &= mapMask[mapSize];
		v &= mapMask[mapSize];

		if (!mipMapped) {
			if (colourDepth) return address + Twid(u, v);
			return address + Twid(u >> 1, v);
		}
		if ((mapSize + whichMap) & 1) address ^= BIG_BANK; /* odd maps are in the other bank */
		int level = mapSize + whichMap;
		if (level > 8) level = 8;
		return address + mmc[level] + Twid(u >> whichMap, v >> whichMap);
	}

	/* texel to 5 bits per channel (4444 to 5 bits too), alpha 0..15 */
	static RGBA ColourConvert(uint32_t raw, bool colourDepth, int whichPixel, bool trans4444) {
		RGBA p;
		if (trans4444) {
			p.b = (int)(raw & 0xF) << 1;
			p.g = (int)((raw >> 4) & 0xF) << 1;
			p.r = (int)((raw >> 8) & 0xF) << 1;
			p.a = (int)((raw >> 12) & 0xF);
			p.r |= p.r >> 4; p.g |= p.g >> 4; p.b |= p.b >> 4;
		}
		else if (colourDepth) {
			p.r = (int)((raw >> 10) & 0x1F);
			p.g = (int)((raw >> 5) & 0x1F);
			p.b = (int)(raw & 0x1F);
			p.a = 0;
		}
		else {
			if (whichPixel == 0) raw >>= 8;
			int r = (int)(raw & 0xE0), g = (int)((raw << 3) & 0xE0), b = (int)((raw << 6) & 0xC0);
			if (b & 0x80) b |= 0x3F;
			if (g & 0x80) g |= 0x1F;
			if (r & 0x80) r |= 0x1F;
			p.r = r >> 3; p.g = g >> 3; p.b = b >> 3; p.a = 0;
		}
		return p;
	}

	bool BilinearEnabled() const {
		/* PCX2 filter mode 00 = bilinear, 01 = adaptive bilinear, 11 = linear mipmapping
		 * [HWREGS]; the PCX1 has no bilinear filtering */
		return pcx2 && (regs[PCX_BILINEAR] & 3) != 3;
	}

	RGBA Bilerp(int u, int v, int uFrac, int vFrac, uint32_t address, int mapSize, int whichMap,
			bool colourDepth, bool mipMapped, bool col4444, int flipUV) const {
		const int binc = 1 << whichMap;
		const RGBA t  = ColourConvert(FetchPixel(AddressCalc(u, v, address, mapSize, whichMap, colourDepth, mipMapped, flipUV)), colourDepth, u & 1, col4444);
		const RGBA u1 = ColourConvert(FetchPixel(AddressCalc((u + binc) & 255, v, address, mapSize, whichMap, colourDepth, mipMapped, flipUV)), colourDepth, (u + binc) & 1, col4444);
		const RGBA v1 = ColourConvert(FetchPixel(AddressCalc(u, (v + binc) & 255, address, mapSize, whichMap, colourDepth, mipMapped, flipUV)), colourDepth, u & 1, col4444);
		const RGBA uv = ColourConvert(FetchPixel(AddressCalc((u + binc) & 255, (v + binc) & 255, address, mapSize, whichMap, colourDepth, mipMapped, flipUV)), colourDepth, (u + binc) & 1, col4444);
		RGBA ab, cd, o;
		ab.r = (t.r << 3) + 4 + (((u1.r - t.r) * uFrac) >> 2);
		ab.g = (t.g << 3) + 4 + (((u1.g - t.g) * uFrac) >> 2);
		ab.b = (t.b << 3) + 4 + (((u1.b - t.b) * uFrac) >> 2);
		ab.a = t.a + (((u1.a - t.a) * uFrac) >> 5);
		cd.r = (v1.r << 3) + 4 + (((uv.r - v1.r) * uFrac) >> 2);
		cd.g = (v1.g << 3) + 4 + (((uv.g - v1.g) * uFrac) >> 2);
		cd.b = (v1.b << 3) + 4 + (((uv.b - v1.b) * uFrac) >> 2);
		cd.a = v1.a + (((uv.a - v1.a) * uFrac) >> 5);
		o.r = (((cd.r - ab.r) * vFrac) >> 5) + ab.r;
		o.g = (((cd.g - ab.g) * vFrac) >> 5) + ab.g;
		o.b = (((cd.b - ab.b) * vFrac) >> 5) + ab.b;
		o.a = (((cd.a - ab.a) * vFrac) >> 5) + ab.a;
		return o;
	}

	RGBA TexturePixel(int x, int y, uint32_t base) const {
		const uint32_t w0 = FetchParameter(base);
		const int a = ToInt(FetchParameter(base + 5)), b = ToInt(FetchParameter(base + 5) >> 16);
		const int c = ToInt(FetchParameter(base + 4));
		const int d = ToInt(FetchParameter(base + 7)), e = ToInt(FetchParameter(base + 7) >> 16);
		const int f = ToInt(FetchParameter(base + 6));
		const int p = ToInt(FetchParameter(base + 3)), q = ToInt(FetchParameter(base + 3) >> 16);
		const int r = ToInt(FetchParameter(base + 2));
		const int exp = (int)((w0 & MASK_EXPONENT) >> 18);
		const int globalTrans = (int)((w0 & MASK_GLOBAL_TRANS) >> 13);
		const int flipUV = (int)((w0 & MASK_FLIP_UV) >> 11);
		const uint32_t w6 = FetchParameter(base + 6);
		const uint32_t address = (FetchParameter(base + 4) >> 16) | (w6 & 0x00FF0000u);
		int mapSize = (int)((w6 & MASK_MAP_SIZE) >> 28);
		PFloat pmip;
		pmip.m = (int32_t)((FetchParameter(base + 2) & MASK_PMIP_M) >> 24);
		pmip.e = (int)((FetchParameter(base + 2) & MASK_PMIP_E) >> 18);
		const bool colourDepth = (w6 & MASK_8_16_MAPS) != 0;
		const bool mipMapped = (w6 & MASK_MIP_MAPPED) != 0;
		const bool col4444 = (w6 & MASK_4444_555) != 0;
		const int32_t cfr = (int32_t)(regs[PCX_CAMERA] & 0xFFFF);

		/* 32-bit arithmetic as on the simulators' hosts */
		const int32_t abc = (int32_t)((int64_t)a * x + (int64_t)b * y + (int64_t)c * cfr);
		const int32_t def = (int32_t)((int64_t)d * x + (int64_t)e * y + (int64_t)f * cfr);
		const int32_t pqr = (int32_t)((int64_t)p * x + (int64_t)q * y + (int64_t)r * cfr);

		PFloat bot = ToPfloat(pqr);
		bot.m >>= 1;
		if (bot.m > 0) bot.m = 0x8000000 / bot.m;
		else bot.m = 0x4000;
		const bool powerTwo = (bot.m == 0x4000);
		if (powerTwo) bot.m = 0x2000;

		PFloat top = ToPfloat(abc);
		top.e += exp;
		int32_t u = (int32_t)(((int64_t)top.m * bot.m) >> 14);
		int shift = powerTwo ? top.e - (bot.e + 13) : top.e - (bot.e + 14);
		int32_t uFrac = AShift(u, shift + 5) & 8191;
		u = AShift(u, shift) & 255;

		top = ToPfloat(def);
		top.e += exp;
		int32_t v = (int32_t)(((int64_t)top.m * bot.m) >> 14);
		shift = powerTwo ? top.e - (bot.e + 13) : top.e - (bot.e + 14);
		int32_t vFrac = AShift(v, shift + 5) & 8191;
		v = AShift(v, shift) & 255;

		/* mipmap level ("d") */
		bot.m >>= 6;
		bot.m = bot.m * bot.m;
		if (powerTwo) bot.e--;
		if (bot.m & 0x8000) { bot.e = bot.e * 2; bot.m >>= 8; }
		else { bot.e = bot.e * 2 + 1; bot.m >>= 7; }
		bot.m = bot.m * pmip.m;
		if (bot.m & 0x8000) { bot.e = pmip.e - (bot.e - 2); bot.m >>= 8; }
		else { bot.e = pmip.e - (bot.e - 1); bot.m >>= 7; }
		if (bot.e < 0) bot.e = 0;
		if (bot.e > 15) bot.e = 15;

		mapSize = 3 - mapSize; /* 0 = 256x256 */
		const bool bilinear = BilinearEnabled();
		RGBA t;
		if (bot.e < 1 || !mipMapped || !colourDepth) {
			if (bilinear) {
				t = Bilerp(u, v, uFrac & 31, vFrac & 31, address, mapSize, 0, colourDepth, mipMapped, col4444, flipUV);
			}
			else {
				t = ColourConvert(FetchPixel(AddressCalc(u, v, address, mapSize, 0, colourDepth, mipMapped, flipUV)), colourDepth, u & 1, col4444);
				t.r = (t.r << 3) + 4; t.g = (t.g << 3) + 4; t.b = (t.b << 3) + 4;
			}
		}
		else if (mapSize + bot.e > 8) {
			t = ColourConvert(FetchPixel(AddressCalc(u, v, address, 0, 8, colourDepth, mipMapped, flipUV)), colourDepth, 0, col4444);
			t.r = (t.r << 3) + 4; t.g = (t.g << 3) + 4; t.b = (t.b << 3) + 4;
		}
		else if (bilinear) {
			const int comp = bot.e - 1;
			t = Bilerp(u, v, (uFrac >> comp) & 31, (vFrac >> comp) & 31, address, mapSize, comp, colourDepth, mipMapped, col4444, flipUV);
		}
		else {
			/* point sample two mipmap levels and blend [SIM1] */
			const RGBA hi = ColourConvert(FetchPixel(AddressCalc(u, v, address, mapSize, bot.e - 1, colourDepth, mipMapped, flipUV)), colourDepth, 0, col4444);
			const RGBA lo = ColourConvert(FetchPixel(AddressCalc(u, v, address, mapSize, bot.e, colourDepth, mipMapped, flipUV)), colourDepth, 0, col4444);
			const int m = (bot.m & 0x7F) >> 2;
			t.r = (hi.r << 3) + ((m * (lo.r - hi.r)) >> 2) + 4;
			t.g = (hi.g << 3) + ((m * (lo.g - hi.g)) >> 2) + 4;
			t.b = (hi.b << 3) + ((m * (lo.b - hi.b)) >> 2) + 4;
			t.a = lo.a;
		}
		t.a += globalTrans;
		if (t.a > 15) t.a = 15;
		return t;
	}

	static inline int Clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

	/* shade one pixel; 'address' is the TSP parameter word address (tag << 1) */
	void Texas(int x, int y, uint32_t address, bool shadow, int fog, RGBi &fb) const {
		const uint32_t w0 = FetchParameter(address);
		const bool lit_shadow = (w0 & MASK_SHADOW_FLAG) && !shadow;
		RGBi base = { 0, 0, 0 };
		int x_offset = 0, y_offset = 0;
		uint32_t inc = address;
		RGBA col;

		if (w0 & MASK_SMOOTH_SHADE) {
			x_offset = ToInt(FetchParameter(address + 1) >> 16);
			y_offset = ToInt(FetchParameter(address + 1));
		}
		else {
			const uint32_t w1 = FetchParameter(address + 1);
			base.r = (int)(w0 & 0xFF);
			base.g = (int)((w1 >> 24) & 0xFF);
			base.b = (int)((w1 >> 16) & 0xFF);
			if (lit_shadow) {
				const RGBi s = ConvertFrom16to24(w1 & 0xFFFF);
				base.r = (std::min)(base.r + s.r, 255);
				base.g = (std::min)(base.g + s.g, 255);
				base.b = (std::min)(base.b + s.b, 255);
			}
		}

		if (w0 & MASK_TEXTURE) {
			col = TexturePixel(x, y, address);
			if (!(w0 & MASK_SMOOTH_SHADE)) {
				col.r = (col.r * base.r) >> 8;
				col.g = (col.g * base.g) >> 8;
				col.b = (col.b * base.b) >> 8;
			}
			inc += 8;
		}
		else {
			col.r = base.r; col.g = base.g; col.b = base.b; col.a = 0;
			inc += 2;
		}

		if (w0 & MASK_SMOOTH_SHADE) {
			RGBi hold = { 0, 0, 0 };
			for (int light = 0; light < 2; light++) {
				if (light == 1 && !lit_shadow) break;
				const uint32_t s0 = FetchParameter(inc), s1 = FetchParameter(inc + 1);
				int32_t frac = (ToInt(s0) << 2) + ToInt(s1 >> 16) * (y - y_offset) + ToInt(s1) * (x - x_offset);
				RGBi hc = ConvertFrom16to24(s0 >> 16);
				hc.r >>= 3; hc.g >>= 3; hc.b >>= 3;
				if (frac < 0) frac = 0;
				if (frac > 0x10000) frac = 0x10000;
				frac >>= 8;
				hc.r = (hc.r * frac) >> 5; hc.g = (hc.g * frac) >> 5; hc.b = (hc.b * frac) >> 5;
				if (frac == 0x100) { hc.r += 4; hc.g += 4; hc.b += 4; }
				if (light == 0) hold = hc;
				else {
					hold.r = (std::min)(hold.r + hc.r, 255);
					hold.g = (std::min)(hold.g + hc.g, 255);
					hold.b = (std::min)(hold.b + hc.b, 255);
				}
				inc += 2;
			}
			if (!(w0 & MASK_TEXTURE)) {
				col.r = hold.r; col.g = hold.g; col.b = hold.b; col.a = 0;
			}
			else {
				col.r = (col.r * hold.r) >> 8;
				col.g = (col.g * hold.g) >> 8;
				col.b = (col.b * hold.b) >> 8;
			}
		}

		if (w0 & MASK_FLAT_HIGHLIGHT) {
			const uint32_t h = FetchParameter(inc);
			RGBi hl = ConvertFrom16to24(h >> 16), sh = ConvertFrom16to24(h);
			hl.r >>= 3; hl.g >>= 3; hl.b >>= 3;
			sh.r >>= 3; sh.g >>= 3; sh.b >>= 3;
			if (lit_shadow) {
				hl.r = (std::min)(hl.r + sh.r, 31);
				hl.g = (std::min)(hl.g + sh.g, 31);
				hl.b = (std::min)(hl.b + sh.b, 31);
			}
			col.r = (std::min)(col.r + (hl.r << 3), 255);
			col.g = (std::min)(col.g + (hl.g << 3), 255);
			col.b = (std::min)(col.b + (hl.b << 3), 255);
		}

		if (!(w0 & MASK_DISABLE_FOG)) {
			const uint32_t fc = regs[PCX_FOGCOL];
			const int fr = (int)((fc >> 16) & 0xFF), fg = (int)((fc >> 8) & 0xFF), fbl = (int)(fc & 0xFF);
			col.r += ((fr - col.r) * fog) >> 8;
			col.g += ((fg - col.g) * fog) >> 8;
			col.b += ((fbl - col.b) * fog) >> 8;
		}

		if (w0 & MASK_TRANS) {
			const int alpha = col.a == 15 ? 16 : col.a;
			col.r = ((fb.r * alpha) >> 4) + (((16 - alpha) * col.r) >> 4);
			col.g = ((fb.g * alpha) >> 4) + (((16 - alpha) * col.g) >> 4);
			col.b = ((fb.b * alpha) >> 4) + (((16 - alpha) * col.b) >> 4);
		}

		fb.r = Clamp255(col.r);
		fb.g = Clamp255(col.g);
		fb.b = Clamp255(col.b);
	}

	/* ---- frame buffer output ---- */

	unsigned OutBytesPerPixel() const {
		switch (regs[PCX_PACKMODE] & 3) {
			case 0: return 4;
			case 1: return 3;
			default: return 2;
		}
	}

	PhysPt PixelAddr(int x, int y) const {
		return (PhysPt)(regs[PCX_SOFADDR] + (uint32_t)y * regs[PCX_LSTRIDE] + (uint32_t)x * OutBytesPerPixel());
	}

	RGBi ReadFB(int x, int y) const {
		const PhysPt a = PixelAddr(x, y);
		RGBi c;
		switch (regs[PCX_PACKMODE] & 3) {
			case 0: {
				const uint32_t v = phys_readd(a);
				c.r = (int)((v >> 16) & 0xFF); c.g = (int)((v >> 8) & 0xFF); c.b = (int)(v & 0xFF);
				break;
			}
			case 1:
				c.b = phys_readb(a); c.g = phys_readb(a + 1); c.r = phys_readb(a + 2);
				break;
			case 2: {
				const uint32_t v = phys_readw(a);
				c.r = (int)((v >> 11) & 0x1F) << 3; c.g = (int)((v >> 5) & 0x3F) << 2; c.b = (int)(v & 0x1F) << 3;
				break;
			}
			default: {
				const uint32_t v = phys_readw(a);
				c.r = (int)((v >> 10) & 0x1F) << 3; c.g = (int)((v >> 5) & 0x1F) << 3; c.b = (int)(v & 0x1F) << 3;
				break;
			}
		}
		return c;
	}

	void WriteFB(int x, int y, const RGBi &c) const {
		const PhysPt a = PixelAddr(x, y);
		switch (regs[PCX_PACKMODE] & 3) {
			case 0: phys_writed(a, ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | (uint32_t)c.b); break;
			case 1: phys_writeb(a, (uint8_t)c.b); phys_writeb(a + 1, (uint8_t)c.g); phys_writeb(a + 2, (uint8_t)c.r); break;
			case 2: phys_writew(a, (uint16_t)(((c.r >> 3) << 11) | ((c.g >> 2) << 5) | (c.b >> 3))); break;
			default: phys_writew(a, (uint16_t)(((c.r >> 3) << 10) | ((c.g >> 3) << 5) | (c.b >> 3))); break;
		}
	}

	bool XClipped(int x) const {
		const uint32_t xc = regs[PCX_XCLIP];
		if ((xc & (1u << 12)) && x < (int)(xc & 0x3FF)) return true;
		if ((xc & (1u << 28)) && x > (int)((xc >> 16) & 0x3FF)) return true;
		return false;
	}

	/* ---- tile traversal ([SIM3] hwsabren.c HWISPRenderer) ---- */

	std::vector<Plane>  planes;
	std::vector<Object> objects;
	uint64_t span_plane_budget;
	uint32_t plane_budget;
	/* per-render statistics for the diagnostic log */
	uint32_t stat_regions, stat_objects, stat_planes, stat_pixels, stat_first_tag;
	unsigned warnings = 0;

	void Warn(const char *msg, uint32_t a, uint32_t b) {
		if (warnings < 20 || debug_log) {
			warnings++;
			LOG_MSG("PowerVR: %s (%08x %08x), render #%u stopped", msg, a, b, renders);
		}
	}

	bool DecodePlane(uint32_t addr, Plane &pl) const {
		if (pcx2 && (regs[PCX_IEEEFP] & 1)) {
			/* PCX2: IEEE floats A, B, C and a word of instruction and tag */
			const uint32_t w0 = PlaneWord(addr), w1 = PlaneWord(addr + 1), w2 = PlaneWord(addr + 2), w3 = PlaneWord(addr + 3);
			float fA, fB, fC;
			memcpy(&fA, &w0, 4); memcpy(&fB, &w1, 4); memcpy(&fC, &w2, 4);
			if (!(fA == fA)) fA = 0; /* NaN from a malformed list */
			if (!(fB == fB)) fB = 0;
			if (!(fC == fC)) fC = 0;
			pl.instr = (uint8_t)(w3 & 0xF);
			pl.index = (w3 >> 4) & 0x3FFFF;
			float scale = 1.0f;
			if (pl.instr == forw_perp || pl.instr == test_shad_perp || pl.instr == forw_perp_fp || pl.instr == test_shad_perp_fp) {
				const float maxval = fabsf(fC) + 2.0f * 1024.0f * (fabsf(fA) + fabsf(fB));
				scale = maxval > 0 ? 1.0f / maxval : 1.0f;
			}
			pl.a30 = Conv20To30(PackTo20Bit(fA * scale));
			pl.b30 = Conv20To30(PackTo20Bit(fB * scale));
			const double cfix = (double)fC * scale * 2147483648.0;
			pl.c = SatInt32((int64_t)(cfix > 9.0e18 ? 9.0e18 : (cfix < -9.0e18 ? -9.0e18 : cfix)));
		}
		else {
			/* PCX1: 3 words; A with instruction and top 6 tag bits, B with the low 12 */
			const uint32_t w0 = PlaneWord(addr), w1 = PlaneWord(addr + 1), w2 = PlaneWord(addr + 2);
			pl.a30 = Conv20To30(w0 & 0xFFFFF);
			pl.instr = (uint8_t)((w0 >> 26) & 0xF);
			pl.index = ((w0 >> 8) & 0x3F000) | (w1 >> 20);
			pl.b30 = Conv20To30(w1 & 0xFFFFF);
			pl.c = (int32_t)w2;
		}
		return true;
	}

	/* Decode the object pointers of the region whose header is at 'pos' into objects/planes.
	 * Returns the index of the next region header, or ~0 if this was the last region. */
	uint32_t DecodeRegion(uint32_t q, bool &error) {
		objects.clear();
		planes.clear();
		const uint32_t words_per_plane = (pcx2 && (regs[PCX_IEEEFP] & 1)) ? 4 : 3;
		for (uint32_t n = 0; n < MAX_OBJECTS_PER_REGION; n++) {
			q++;
			uint32_t od = ObjWordStripped(q);
			if (!ResolveLinks(q, od)) { error = true; return ~0u; }
			if (od & REG_COMPULSARY_BITS) {
				/* a region with no objects at all (not produced by the driver) */
				return q;
			}
			Object o;
			o.first_plane = (uint32_t)planes.size();
			o.num_planes = (od >> 19) & 0x3FF;
			const uint32_t paddr = od & 0x7FFFF;
			if (planes.size() + o.num_planes > MAX_PLANES_PER_REGION || o.num_planes > plane_budget) { error = true; return ~0u; }
			plane_budget -= o.num_planes;
			for (uint32_t p = 0; p < o.num_planes; p++) {
				Plane pl;
				DecodePlane(paddr + p * words_per_plane, pl);
				planes.push_back(pl);
			}
			objects.push_back(o);

			if (od & OBJ_VERY_LAST_PTR) return ~0u;
			uint32_t nq = q + 1;
			uint32_t next = ObjWordStripped(nq);
			if (!ResolveLinks(nq, next)) { error = true; return ~0u; }
			if (next & REG_COMPULSARY_BITS) return q + 1;
		}
		error = true;
		return ~0u;
	}

	void ShadeSpan(const CellState *cell, int XSpan, int YLine, RGBi *fb, bool *fb_loaded, bool *fb_dirty) {
		for (int cl = 0; cl < NUM_SABRE_CELLS; cl++) {
			if (!cell[cl].U_id) continue;
			const int x = XSpan + cl;
			if (!fb_loaded[cl]) { fb[cl] = ReadFB(x, YLine); fb_loaded[cl] = true; }
			Texas(x, YLine, cell[cl].U_id << 1, cell[cl].U_shadow, Fog(cell[cl].U_depth), fb[cl]);
			fb_dirty[cl] = true;
			if (!stat_first_tag) stat_first_tag = cell[cl].U_id;
		}
	}

	void RenderSpan(int XSpan, int YLine) {
		CellState cell[NUM_SABRE_CELLS];
		memset(cell, 0, sizeof(cell));
		RGBi fb[NUM_SABRE_CELLS];
		bool fb_loaded[NUM_SABRE_CELLS], fb_dirty[NUM_SABRE_CELLS];
		memset(fb_loaded, 0, sizeof(fb_loaded));
		memset(fb_dirty, 0, sizeof(fb_dirty));
		bool no_object_yet = true;

		for (size_t oi = 0; oi < objects.size(); oi++) {
			const Object &o = objects[oi];
			bool translucent = false;
			for (uint32_t pi = 0; pi < o.num_planes; pi++) {
				const Plane &pl = planes[o.first_plane + pi];
				const int instr = pl.instr;
				if (instr == forw_visib_fp || instr == forw_invis_fp || instr == test_shad_forw_fp) {
					if (no_object_yet) { sec_obj = false; first_obj = true; no_object_yet = false; }
					else if (first_obj && !sec_obj) { first_obj = false; sec_obj = true; }
					else sec_obj = false;
				}
				if (!translucent) {
					const CellControl wide = ExpandInstruction(instr, false);
					/* depth = A*x + B*y + C, in the 40-bit adder of which C is the top 32 bits */
					int64_t c40 = (int64_t)pl.a30 * XSpan + (int64_t)pl.b30 * YLine + (int64_t)pl.c * 256;
					for (int cl = 0; cl < NUM_SABRE_CELLS; cl++) {
						SurfProcess(cell[cl], wide, SatInt32(c40 / 256), pl.index);
						c40 += pl.a30;
					}
				}
				if (instr == begin_trans) translucent = true;
			}

			/* a translucent pass start shades what the cells hold so far; the last object of
			 * the region is followed by a flush down the pipeline and the final shading */
			if (translucent) ShadeSpan(cell, XSpan, YLine, fb, fb_loaded, fb_dirty);
			if (oi + 1 == objects.size()) {
				const CellControl flush = ExpandInstruction(0, true);
				for (int cl = 0; cl < NUM_SABRE_CELLS; cl++) SurfProcess(cell[cl], flush, 0, 0);
				ShadeSpan(cell, XSpan, YLine, fb, fb_loaded, fb_dirty);
			}
		}

		for (int cl = 0; cl < NUM_SABRE_CELLS; cl++) {
			if (fb_dirty[cl] && !XClipped(XSpan + cl)) { WriteFB(XSpan + cl, YLine, fb[cl]); stat_pixels++; }
		}
	}

	void Render() {
		renders++;
		DetermineTLBPageSize();
		stat_regions = stat_objects = stat_planes = stat_pixels = stat_first_tag = 0;
		RenderFrame();
		if (debug_log || renders <= 3) {
			LOG_MSG("PowerVR: render #%u obj=%08x page=%08x tlb=%08x,%08x (%uKB) sof=%08x stride=%u pack=%x prec=%08x cam=%04x fog=%u fogcol=%08x xclip=%08x",
				renders, regs[PCX_OBJECT_OFFSET], regs[PCX_PAGE_CTRL], regs[PCX_TLB], regs[PCX_TLB + 1], (1u << tlb_shift) >> 10,
				regs[PCX_SOFADDR], regs[PCX_LSTRIDE], regs[PCX_PACKMODE], regs[PCX_PREC_BASE],
				regs[PCX_CAMERA] & 0xFFFF, regs[PCX_FOGAMOUNT], regs[PCX_FOGCOL], regs[PCX_XCLIP]);
			const uint32_t o1 = ObjWord(1);
			LOG_MSG("PowerVR:   regions=%u objects=%u planes=%u pixels=%u | hdr=%08x obj1=%08x plane=%08x %08x %08x | tag=%x tsp=%08x %08x %08x",
				stat_regions, stat_objects, stat_planes, stat_pixels, ObjWord(0), o1,
				PlaneWord(o1 & 0x7FFFF), PlaneWord((o1 & 0x7FFFF) + 1), PlaneWord((o1 & 0x7FFFF) + 2),
				stat_first_tag, FetchParameter(stat_first_tag << 1), FetchParameter((stat_first_tag << 1) + 1),
				FetchParameter((stat_first_tag << 1) + 2));
		}
	}

	void RenderFrame() {
		if (regs[PCX_SOFADDR] == 0 || regs[PCX_LSTRIDE] == 0) return;

		LoadFogTable();
		span_plane_budget = MAX_SPAN_PLANES_PER_RENDER;
		plane_budget = MAX_PLANES_PER_RENDER;
		prev_intern_out = prev_none;
		sec_obj = first_obj = false;

		uint32_t pos = 0;
		for (uint32_t region = 0; region < MAX_REGIONS_PER_RENDER; region++) {
			uint32_t hdr = ObjWordStripped(pos);
			if (!ResolveLinks(pos, hdr) || !(hdr & REG_COMPULSARY_BITS)) {
				Warn("malformed region list at word/value", pos, hdr);
				return;
			}
			const int xsize = (int)((hdr & 0x1F) + 1) * NUM_SABRE_CELLS;
			const int ysize = (int)((hdr >> 5) & 0x3FF) + 1;
			const int xpos = (int)((hdr >> 15) & 0x1F) * NUM_SABRE_CELLS;
			const int ypos = (int)((hdr >> 20) & 0x3FF);

			bool error = false;
			const uint32_t next = DecodeRegion(pos, error);
			if (error) {
				Warn("malformed object list in region at word", pos, 0);
				return;
			}
			stat_regions++;
			stat_objects += (uint32_t)objects.size();
			stat_planes += (uint32_t)planes.size();
			if (!objects.empty()) {
				for (int y = ypos; y < ypos + ysize && y < 1024; y++) {
					for (int x = xpos; x < xpos + xsize && x < 1024; x += NUM_SABRE_CELLS) {
						if (span_plane_budget < planes.size() + 1) {
							Warn("work limit exceeded at region/planes", stat_regions, (uint32_t)planes.size());
							return;
						}
						span_plane_budget -= planes.size() + 1;
						RenderSpan(x, y);
					}
				}
			}
			if (next == ~0u) return;
			pos = next;
		}
		Warn("too many regions", stat_regions, 0);
	}
};

/* ---- memory mapped BARs ---- */

class PVR_RegHandler : public PageHandler {
public:
	PVR_RegHandler() : PageHandler(PFLAG_NOCODE) {}
	static uint32_t Idx(PhysPt addr) {
		return (PAGING_GetPhysicalAddress(addr) & (REG_BAR_SIZE - 1)) >> 2;
	}
	static unsigned Sh(PhysPt addr) {
		return (PAGING_GetPhysicalAddress(addr) & 3) * 8;
	}
	uint8_t readb(PhysPt addr) override {
		return pvr ? (uint8_t)(pvr->ReadReg(Idx(addr)) >> Sh(addr)) : 0xFF;
	}
	uint16_t readw(PhysPt addr) override {
		return pvr ? (uint16_t)(pvr->ReadReg(Idx(addr)) >> Sh(addr)) : 0xFFFF;
	}
	uint32_t readd(PhysPt addr) override {
		return pvr ? pvr->ReadReg(Idx(addr)) : 0xFFFFFFFFu;
	}
	void writeb(PhysPt addr, uint8_t val) override {
		if (!pvr) return;
		const uint32_t i = Idx(addr); const unsigned s = Sh(addr);
		pvr->WriteReg(i, (pvr->regs[i] & ~(0xFFu << s)) | ((uint32_t)val << s));
	}
	void writew(PhysPt addr, uint16_t val) override {
		if (!pvr) return;
		const uint32_t i = Idx(addr); const unsigned s = Sh(addr);
		pvr->WriteReg(i, (pvr->regs[i] & ~(0xFFFFu << s)) | ((uint32_t)val << s));
	}
	void writed(PhysPt addr, uint32_t val) override {
		if (pvr) pvr->WriteReg(Idx(addr), val);
	}
};

class PVR_TMemHandler : public PageHandler {
public:
	PVR_TMemHandler() : PageHandler(PFLAG_READABLE | PFLAG_WRITEABLE | PFLAG_NOCODE) {}
	HostPt GetHostReadPt(PageNum phys_page) override {
		const uint32_t off = (uint32_t)((phys_page - (pvr->bar1 >> 12)) << 12) & (TMEM_SIZE - 1);
		return pvr->tmem + off;
	}
	HostPt GetHostWritePt(PageNum phys_page) override {
		return GetHostReadPt(phys_page);
	}
};

PVR_RegHandler  reg_handler;
PVR_TMemHandler tmem_handler;

MEM_Callout_t reg_cb = MEM_Callout_t_none, tmem_cb = MEM_Callout_t_none;

PageHandler *reg_cb_func(MEM_CalloutObject &co, Bitu phys_page) {
	(void)co; (void)phys_page;
	return &reg_handler;
}

PageHandler *tmem_cb_func(MEM_CalloutObject &co, Bitu phys_page) {
	(void)co; (void)phys_page;
	return &tmem_handler;
}

void InstallCallout(MEM_Callout_t &cb, uint32_t base, uint32_t size, MEM_CalloutHandler *func) {
	if (cb == MEM_Callout_t_none) {
		cb = MEM_AllocateCallout(MEM_TYPE_PCI);
		if (cb == MEM_Callout_t_none) E_Exit("PowerVR: unable to allocate memory callout");
	}
	MEM_CalloutObject *obj = MEM_GetCallout(cb);
	assert(obj != NULL);
	obj->Uninstall();
	if (base != 0) obj->Install(base >> 12, MEMMASK_Combine(MEMMASK_FULL, MEMMASK_Range(size >> 12)), func);
	MEM_PutCallout(obj);
}

void FreeCallout(MEM_Callout_t &cb) {
	if (cb != MEM_Callout_t_none) {
		MEM_FreeCallout(cb);
		cb = MEM_Callout_t_none;
	}
}

void MapBARs(uint32_t bar0, uint32_t bar1) {
	if (!pvr) return;
	bar0 &= ~(REG_BAR_SIZE - 1);
	bar1 &= ~(TMEM_SIZE - 1);
	if (bar0 != pvr->bar0) {
		pvr->bar0 = bar0;
		InstallCallout(reg_cb, bar0, REG_BAR_SIZE, reg_cb_func);
	}
	if (bar1 != pvr->bar1) {
		pvr->bar1 = bar1;
		InstallCallout(tmem_cb, bar1, TMEM_SIZE, tmem_cb_func);
	}
	PAGING_ClearTLB();
	LOG(LOG_MISC, LOG_DEBUG)("PowerVR: registers at %08x, texture memory at %08x", bar0, bar1);
}

class PCI_PowerVRDevice : public PCI_Device {
public:
	uint32_t config_read(uint8_t regnum, Bitu iolen) override {
		const uint32_t v = PCI_Device::config_read(regnum, iolen);
		if (pvr && pvr->Trace())
			LOG_MSG("PowerVR: PCI config read  %02x len %u = %08x", regnum, (unsigned)iolen, v);
		return v;
	}

	PCI_PowerVRDevice(bool pcx2, int irq, uint32_t bar0, uint32_t bar1)
		: PCI_Device(NEC_VENDOR_ID, pcx2 ? PCX2_DEVICE_ID : PCX1_DEVICE_ID) {
		config[0x08] = pcx2 ? 0x02 : 0x01; /* revision */
		config[0x09] = 0x00;
		config[0x0a] = 0x80;               /* subclass: other display controller */
		config[0x0b] = 0x03;               /* class: display controller */
		config[0x0e] = 0x00;               /* header type */
		config[0x04] = 0x06;               /* memory space + bus master enabled */
		config[0x06] = 0x80;
		config[0x3c] = (irq > 0) ? (unsigned char)irq : 0xFF;
		config[0x3d] = (irq > 0) ? 0x01 : 0x00; /* INTA# */

		host_writew(config_writemask + 0x04, 0x0147);
		host_writed(config_writemask + 0x10, ~(REG_BAR_SIZE - 1));
		host_writed(config_writemask + 0x14, ~(TMEM_SIZE - 1));
		config_writemask[0x3c] = 0xFF;
		host_writed(config + 0x10, bar0);
		host_writed(config + 0x14, bar1);
	}

	void config_write(uint8_t regnum, Bitu iolen, uint32_t value) override {
		if (pvr && pvr->Trace())
			LOG_MSG("PowerVR: PCI config write %02x len %u = %08x", regnum, (unsigned)iolen, value);
		if (iolen == 1) {
			const unsigned char mask = config_writemask[regnum];
			config[regnum] = (unsigned char)((config[regnum] & ~mask) | (value & mask));
			if (regnum >= 0x10 && regnum <= 0x17)
				MapBARs(host_readd(config + 0x10) & 0xFFFFFFF0u, host_readd(config + 0x14) & 0xFFFFFFF0u);
		}
		else {
			PCI_Device::config_write(regnum, iolen, value);
		}
	}
};

PCI_PowerVRDevice *pvr_pci = NULL;
uint32_t pvr_assigned_base = 0;

void PVR_Destroy(Section * /*sec*/) {
	if (pvr_pci) {
		UnregisterPCIDevice(pvr_pci);
		delete pvr_pci;
		pvr_pci = NULL;
	}
	FreeCallout(reg_cb);
	FreeCallout(tmem_cb);
	PAGING_ClearTLB();
	delete pvr;
	pvr = NULL;
}

void PVR_OnPowerOn(Section * /*sec*/) {
	if (pvr) return;
	Section_prop *section = static_cast<Section_prop *>(control->GetSection("powervr"));
	if (section == NULL) return;
	const std::string card = section->Get_string("powervr_card");
	if (card != "pcx1" && card != "pcx2") return;
	if (!pcibus_enable) {
		LOG_MSG("PowerVR: the PCI bus is not enabled, PowerVR card not installed");
		return;
	}
	if (pvr_assigned_base == 0) {
		LOG_MSG("PowerVR: no physical address space available, PowerVR card not installed");
		return;
	}

	InitTwiddle();
	const int irq = section->Get_int("powervr_irq");
	pvr = new PowerVR(card == "pcx2", irq, section->Get_bool("powervr_debug"));

	/* 8MB window: texture memory in the first 4MB, registers at the start of the second */
	uint32_t base = pvr_assigned_base;
	if (base < MEM_TotalPages() * 4096u) base = MEM_TotalPages() * 4096u;
	base = (base + TMEM_SIZE - 1) & ~(TMEM_SIZE - 1);
	const uint32_t bar1 = base, bar0 = base + TMEM_SIZE;

	pvr_pci = new PCI_PowerVRDevice(pvr->pcx2, irq, bar0, bar1);
	/* Never slot 0: the DOS SGL identifies the board by (bus << 8) | (device << 3) | function
	 * and treats 0 as "no PCX found" (Tomb Raider's tombpcx1.exe exits silently). On real
	 * hardware slot 0 is the host bridge. */
	Bits slot = 1;
	while (slot < 32 && !PCI_IsSlotFree(0, slot)) slot++;
	RegisterPCIDevice(pvr_pci, 0, slot < 32 ? slot : -1);
	MapBARs(bar0, bar1);
	LOG_MSG("PowerVR %s installed: registers at %08x, 4MB texture memory at %08x, IRQ %d",
		pvr->pcx2 ? "PCX2" : "PCX1", bar0, bar1, irq);
}

} // anonymous namespace

void PVR_Init() {
	Section_prop *section = static_cast<Section_prop *>(control->GetSection("powervr"));
	if (section != NULL && std::string(section->Get_string("powervr_card")) != "false")
		pvr_assigned_base = MEM_HardwareAllocate("PowerVR", 8u << 20);
	AddExitFunction(AddExitFunctionFuncPair(PVR_Destroy), true);
	AddVMEventFunction(VM_EVENT_POWERON, AddVMEventFunctionFuncPair(PVR_OnPowerOn));
}
