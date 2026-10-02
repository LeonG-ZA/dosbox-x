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

/* Creative Graphics Library (CGL) for the Rendition Verite (machine=svga_verite).
 *
 * Games for Creative's 3D Blaster PCI load a CGL driver module (VRIT?1xx.CGL) from their own
 * directory at run time. When a game opens one, DOSBox-X hands it a copy with every code and
 * data page cleared and every export turned into a stub that calls into this file. The header
 * (which identifies the variant: fixed or floating point, register or stack arguments), the
 * tables and the sizes stay, so the game's loader behaves as with the original. Nothing has to
 * be added to the game's directory, and none of the module's code runs.
 *
 * Container ("HotLink CGL Rev 2"): a text banner, an identification line ending in 1Ah, "HOT2",
 * the module size (file size - 83h) and the variant type, then a compact linear executable
 * header whose table offsets count from the byte after the type: page count, page size, object
 * table, page table (offset, size, flags per page), entry table and resident name table in the
 * LE formats, fixup page and record tables, import tables, data pages.
 */

#include <algorithm>
#include <cmath>
#include <string.h>
#include <set>
#include <string>
#include <vector>

#include "dosbox.h"
#include "callback.h"
#include "control.h"
#include "cpu.h"
#include "dos_inc.h"
#include "dos_system.h"
#include "logging.h"
#include "mem.h"
#include "paging.h"
#include "pic.h"
#include "regs.h"
#include "setup.h"
#include "vga.h"
#include "verite.h"
#include "verite_core.h"

namespace {

/* variant type in the module header */
enum : uint32_t { CGL_INT_REG = 0, CGL_INT_STACK = 1, CGL_FLOAT_REG = 2, CGL_FLOAT_STACK = 3 };

/* argument kinds: p pointer, w 16-bit, d 32-bit, b 8-bit, t coordinate (16.16 or float) */
struct CglFunc {
	const char *name;
	const char *args; /* NULL: not known yet */
};

const CglFunc cgl_funcs[] = {
	{ "cglVersionControl", "dp" },
	{ "cglSendStream", "pw" },
	{ "cglGetPixel", "ttp" },
	{ "cglPutPixel", "ttp" },
	{ "cglInitScreen", "p" },
	{ "cglQueryScreen", "p" },
	{ "cglClearScreen", "" },
	{ "cglCloseScreen", "" },
	{ "cglSetWindowClip", "p" },
	{ "cglInitPan", "p" },
	{ "cglPanPos", "ww" },
	{ "cglSetLineStyle", "pd" },
	{ "cglSetFillPattern", "pd" },
	{ "cglSetStipplePattern", "p" },
	{ "cglSetDepthMode", "w" },
	{ "cglSetStencilMode", "wwww" },
	{ "cglSetLogicalOpsMode", "w" },
	{ "cglSetDepthCueColor", "p" },
	{ "cglSetDitherMode", "w" },
	{ "cglLoadTextureMap", "p" },
	{ "cglSelectRenderBuffer", "w" },
	{ "cglCopyDisplayBuffer", "w" },
	{ "cglLockBuffer", "wp" },
	{ "cglClearBuffer", "dp" },
	{ "cglSetConstant", "pdd" },
	{ "cglSwapBuffer", "" },
	{ "cglUpLoad", "p" },
	{ "cglDownLoad", "p" },
	{ "cglSetPalette", "pww" },
	{ "cglGetPalette", "pww" },
	{ "cglSetPickRegion", "p" },
	{ "cglGetPickResult", "p" },
	{ "cglSetExtent", "p" },
	{ "cglGetExtentResult", "p" },
	{ "cglGetConfig", "p" },
	{ "cglSetCursor", "bpp" },
	{ "cglSetCursorPos", "ww" },
	{ "cglSetupInterrupt", "dpp" },
	{ "cglReleaseInterrupt", "d" },
	{ "cglBoardPresent", "d" },
	{ "cglQueryFeature", NULL },
	{ "cglEventRegister", NULL },
	{ "cglEventDeregister", NULL },
	{ "cglBoardConfigure", NULL },
};
const unsigned CGL_FUNCS = sizeof(cgl_funcs) / sizeof(cgl_funcs[0]);
const uint8_t CGL_UNKNOWN_EXPORT = 0xFF;

struct CglState {
	bool log = false;
	Bitu callback = 0;
	bool retry = false;               /* the current call runs again */
	bool waiting = false;             /* cglSwapBuffer waiting for the vertical sync */
	uint64_t wait_frame = 0;
	double wait_until = 0.0;
	uint32_t type = CGL_INT_REG; /* of the module making the current call */
	unsigned log_left[CGL_FUNCS] = {};
};
CglState cgl;

uint32_t Get32(const std::vector<uint8_t> &d, size_t o) {
	return o + 4 <= d.size() ? (uint32_t)d[o] | ((uint32_t)d[o + 1] << 8) | ((uint32_t)d[o + 2] << 16) | ((uint32_t)d[o + 3] << 24) : 0;
}
uint16_t Get16(const std::vector<uint8_t> &d, size_t o) {
	return o + 2 <= d.size() ? (uint16_t)(d[o] | (d[o + 1] << 8)) : 0;
}
void Put32(std::vector<uint8_t> &d, size_t o, uint32_t v) {
	for (unsigned i = 0; i < 4; i++) d[o + i] = (uint8_t)(v >> (8 * i));
}
void Put16(std::vector<uint8_t> &d, size_t o, uint16_t v) {
	d[o] = (uint8_t)v;
	d[o + 1] = (uint8_t)(v >> 8);
}

/* the game's module, read through the file it opened */
bool ReadWhole(DOS_File *f, std::vector<uint8_t> &out) {
	uint32_t size = 0;
	if (!f->Seek(&size, DOS_SEEK_END)) return false;
	if (size < 0x200 || size > 0x100000) return false;
	out.resize(size);
	uint32_t pos = 0;
	if (!f->Seek(&pos, DOS_SEEK_SET)) return false;
	uint32_t done = 0;
	while (done < size) {
		uint16_t n = (uint16_t)((size - done) > 0x8000 ? 0x8000 : (size - done));
		if (!f->Read(&out[done], &n) || n == 0) return false;
		done += n;
	}
	pos = 0;
	f->Seek(&pos, DOS_SEEK_SET);
	return true;
}

/* Builds our module from the game's one. Header, tables and sizes stay as they are, so the
 * game's loader reads and allocates exactly what it would for the original (some games are
 * sensitive to the heap layout this leaves). Every code and data page is cleared, the fixups
 * are emptied, and each export points at a stub that calls into this file. */
bool BuildModule(const std::vector<uint8_t> &orig, std::vector<uint8_t> &img, uint32_t &type) {
	size_t hot2 = 1;
	while (hot2 + 4 < 0x200 && memcmp(&orig[hot2], "HOT2", 4)) hot2++;
	if (hot2 + 4 >= 0x200 || orig[hot2 - 1] != 0x1A) return false;
	const size_t base = hot2 + 0x11;
	type = Get32(orig, hot2 + 0x0D);
	if (type > CGL_FLOAT_STACK) return false;
	const uint32_t pages = Get32(orig, base), objects = Get32(orig, base + 0x10);
	const size_t obj_tab = base + Get32(orig, base + 0x0C), page_tab = base + Get32(orig, base + 0x14);
	const size_t entry_tab = base + Get32(orig, base + 0x18), fixpage_tab = base + Get32(orig, base + 0x1C);
	const size_t fixrec_tab = base + Get32(orig, base + 0x20), data = base + Get32(orig, base + 0x24);
	const size_t res_tab = base + Get32(orig, base + 0x28);
	if (!pages || pages > 4096 || !objects || objects > 16 || data >= orig.size() ||
		page_tab + 8 * pages > orig.size() || fixpage_tab + 4 * (pages + 1) > orig.size()) return false;

	/* our function for each exported name */
	std::vector<uint8_t> func_of(0x10000, CGL_UNKNOWN_EXPORT);
	size_t o = res_tab;
	while (o < orig.size() && orig[o]) {
		const unsigned n = orig[o];
		if (o + 3 + n > orig.size()) return false;
		std::string name((const char *)&orig[o + 1], n);
		const uint16_t ordinal = Get16(orig, o + 1 + n);
		if (!name.empty() && name.back() == '_') name.pop_back(); /* register calls */
		for (unsigned i = 0; i < CGL_FUNCS; i++)
			if (name == cgl_funcs[i].name) func_of[ordinal] = (uint8_t)i;
		o += 3 + n;
	}

	img = orig;
	/* our banner in place of the original's first two lines */
	const char banner[] = "HotLink CGL Rev 2\r\nDOSBox-X CGL driver for the emulated Rendition Verite";
	size_t id = 0, crlf = 0;
	for (size_t i = 0; i + 1 < hot2 && crlf < 2; i++)
		if (orig[i] == '\r' && orig[i + 1] == '\n') { crlf++; id = i + 2; }
	if (crlf < 2 || sizeof(banner) + 1 > id) return false;
	memset(&img[0], ' ', id - 2);
	memcpy(&img[0], banner, sizeof(banner) - 1);

	/* the exports' object, and where its first page lies in the file */
	if (entry_tab + 4 > img.size() || img[entry_tab + 1] != 3) return false;
	const uint16_t code_obj = Get16(orig, entry_tab + 2);
	if (code_obj == 0 || code_obj > objects || obj_tab + 24 * code_obj > orig.size()) return false;
	const uint32_t first_page = Get32(orig, obj_tab + 24 * (code_obj - 1) + 12);
	if (first_page == 0 || first_page > pages) return false;
	const size_t stubs = data + Get32(orig, page_tab + 8 * (first_page - 1));
	const uint32_t room = Get16(orig, page_tab + 8 * (first_page - 1) + 4);
	const uint32_t stub_size = 8;

	/* clear the pages; only plain pages are known */
	for (uint32_t i = 0; i < pages; i++) {
		const size_t off = data + Get32(orig, page_tab + 8 * i);
		const size_t len = Get16(orig, page_tab + 8 * i + 4);
		if (Get16(orig, page_tab + 8 * i + 6) != 0 || off + len > img.size()) return false;
		memset(&img[off], 0, len);
	}
	/* no fixups: each page's range in the (cleared) record table is empty */
	const uint32_t fix_end = Get32(orig, fixpage_tab + 4 * pages);
	if (fixrec_tab + fix_end > img.size()) return false;
	memset(&img[fixrec_tab], 0, fix_end);
	for (uint32_t i = 0; i <= pages; i++) Put32(img, fixpage_tab + 4 * i, fix_end);
	/* every 32-bit entry points at its stub at the start of the exports' object */
	o = entry_tab;
	uint32_t ordinal = 1;
	while (o + 2 <= img.size() && img[o]) {
		const unsigned count = img[o], kind = img[o + 1];
		if (kind == 0) { ordinal += count; o += 2; continue; }
		if (kind != 3 || o + 4 + 5 * count > img.size()) return false;
		Put16(img, o + 2, code_obj);
		o += 4;
		for (unsigned k = 0; k < count; k++, ordinal++, o += 5) {
			if (ordinal * stub_size > room) return false;
			img[o] = 1; /* exported */
			Put32(img, o + 1, (ordinal - 1) * stub_size);
			/* stub: callback instruction, then our function number and the variant */
			uint8_t *st = &img[stubs + (ordinal - 1) * stub_size];
			st[0] = 0xFE; st[1] = 0x38;
			st[2] = (uint8_t)cgl.callback; st[3] = (uint8_t)(cgl.callback >> 8);
			st[4] = ordinal < func_of.size() ? func_of[ordinal] : CGL_UNKNOWN_EXPORT;
			st[5] = (uint8_t)type;
			st[6] = st[7] = 0xCC;
		}
	}
	return true;
}

/* serves the module from memory */
class CglModuleFile : public DOS_File {
public:
	CglModuleFile(std::vector<uint8_t> &&d, const DOS_File &orig) : img(std::move(d)) {
		open = true;
		attr = orig.attr;
		date = orig.date;
		time = orig.time;
		flags = orig.flags;
		if (orig.name) SetName(orig.name);
	}
	bool Read(uint8_t *data, uint16_t *size) override {
		const uint32_t left = pos < img.size() ? (uint32_t)img.size() - pos : 0;
		if (*size > left) *size = (uint16_t)left;
		if (*size) memcpy(data, &img[pos], *size);
		pos += *size;
		return true;
	}
	bool Write(const uint8_t * /*data*/, uint16_t * /*size*/) override {
		DOS_SetError(DOSERR_ACCESS_DENIED);
		return false;
	}
	bool Seek(uint32_t *new_pos, uint32_t type) override {
		int64_t p = (int32_t)*new_pos;
		if (type == DOS_SEEK_SET) p = *new_pos;
		else if (type == DOS_SEEK_CUR) p += pos;
		else if (type == DOS_SEEK_END) p += (int64_t)img.size();
		else return false;
		if (p < 0) { DOS_SetError(DOSERR_ACCESS_DENIED); return false; }
		pos = (uint32_t)p;
		*new_pos = pos;
		return true;
	}
	bool Close() override { return true; }
	uint16_t GetInformation(void) override { return 0; }
private:
	std::vector<uint8_t> img;
	uint32_t pos = 0;
};

/* ---- calls ---- */

bool StackArgs() { return cgl.type == CGL_INT_STACK || cgl.type == CGL_FLOAT_STACK; }
bool FloatModel() { return cgl.type == CGL_FLOAT_REG || cgl.type == CGL_FLOAT_STACK; }

uint32_t Arg(unsigned k) {
	if (!StackArgs()) {
		switch (k) {
			case 0: return reg_eax;
			case 1: return reg_edx;
			case 2: return reg_ebx;
			case 3: return reg_ecx;
			default: return mem_readd(SegPhys(ss) + reg_esp + 4 + 4 * (k - 4));
		}
	}
	return mem_readd(SegPhys(ss) + reg_esp + 4 + 4 * k);
}

PhysPt Ptr(uint32_t p) { return SegPhys(ds) + p; }
uint32_t Rd32(uint32_t p, uint32_t o) { return mem_readd(Ptr(p) + o); }
uint16_t Rd16(uint32_t p, uint32_t o) { return mem_readw(Ptr(p) + o); }
uint8_t Rd8(uint32_t p, uint32_t o) { return mem_readb(Ptr(p) + o); }
void Wr32(uint32_t p, uint32_t o, uint32_t v) { mem_writed(Ptr(p) + o, v); }
void Wr16(uint32_t p, uint32_t o, uint16_t v) { mem_writew(Ptr(p) + o, v); }

/* a coordinate: 16.16 fixed point or a float, by the module's variant */
float Coord(uint32_t raw) {
	if (FloatModel()) {
		float f;
		memcpy(&f, &raw, sizeof(f));
		return f;
	}
	return (float)(int32_t)raw / 65536.0f;
}

void LogCall(unsigned f) {
	if (!cgl.log || !cgl.log_left[f]) return;
	cgl.log_left[f]--;
	const CglFunc &fn = cgl_funcs[f];
	std::string s = std::string("CGL: ") + fn.name + "(";
	char buf[64];
	const unsigned n = fn.args ? (unsigned)strlen(fn.args) : 4;
	for (unsigned k = 0; k < n; k++) {
		snprintf(buf, sizeof(buf), "%s%08x", k ? ", " : "", Arg(k));
		s += buf;
	}
	s += fn.args ? ")" : " ...?)";
	LOG_MSG("%s", s.c_str());
	for (unsigned k = 0; fn.args && fn.args[k]; k++) {
		if (fn.args[k] != 'p' || !Arg(k)) continue;
		std::string d = "CGL:   ";
		for (unsigned i = 0; i < 32; i++) {
			snprintf(buf, sizeof(buf), "%02x ", mem_readb(Ptr(Arg(k)) + i));
			d += buf;
		}
		LOG_MSG("%s", d.c_str());
	}
}

/* ---- interface values ---- */

enum : int16_t {
	ERR_NONE = 0,
	ERR_OPERATION = 1013,
	ERR_NOT_ENOUGH_VRAM = 1202, ERR_SCREEN_MODE = 1203, ERR_DEPTH_STENCIL_OPTION = 1204, ERR_COLOR_FORMAT = 1205,
	ERR_TEXTURE_HOSTBUFFER = 1800, ERR_TEXTURE_SIZE = 1801, ERR_TEXTURE_EXCEED = 1803,
	ERR_INVALID_BUFFER_TYPE = 2100,
	ERR_MUST_INIT_SCREEN = 32000
};
enum { OP_RENDER = 1, OP_COPY = 2 };
enum {
	PRIM_POINT = 0x000, PRIM_LINE = 0x100, PRIM_LINE_STRIP = 0x101, PRIM_LINE_LOOP = 0x102,
	PRIM_TRIANGLE = 0x200, PRIM_TRIANGLE_STRIP = 0x201, PRIM_TRIANGLE_FAN = 0x202,
	PRIM_QUAD = 0x300, PRIM_QUAD_STRIP = 0x301, PRIM_RECTANGLE = 0x400
};
enum {
	PROP_SHADE = 0x001, PROP_DEPTH = 0x002, PROP_STENCIL = 0x004, PROP_BLEND = 0x008, PROP_TEXTURE = 0x010,
	PROP_DEPTHCUE = 0x020, PROP_LOGICOPS = 0x040, PROP_BITMASK = 0x080, PROP_STIPPLE = 0x100, PROP_PERSPECTIVE = 0x200
};
enum { FMT_8888 = 0, FMT_5551 = 1, FMT_4444 = 2, FMT_332 = 5, FMT_2321 = 9, FMT_232 = 11, FMT_CI8 = 14 };
enum { BUF_FRAME = 0x10, BUF_DEPTH = 0x08, BUF_STENCIL = 0x04 };
enum { SEL_FRONT = 1, SEL_BACK = 2, BUF_LOCAL = 3, BUF_TEXTURE = 4 };
enum { TEX_MODULATE = 0, TEX_DECAL = 1, TEX_COPY = 3 };
enum { WRAP_CLAMP = 0, WRAP_REPEAT = 1, WRAP_MIRROR = 2 };
enum { CMP_NEVER, CMP_LESS, CMP_EQUAL, CMP_LEQUAL, CMP_GREATER, CMP_NOTEQUAL, CMP_GEQUAL, CMP_ALWAYS };

/* screen modes by number */
const uint16_t mode_size[16][2] = {
	{ 320, 200 }, { 320, 240 }, { 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1280, 1024 }, { 640, 480 }, { 800, 600 },
	{ 800, 600 }, { 800, 600 }, { 1024, 768 }, { 1280, 1024 }, { 0, 0 }, { 640, 480 }, { 640, 400 }, { 640, 350 }
};

/* ---- screen and render state ---- */

struct Screen {
	bool open = false;
	unsigned mode = 0, width = 0, height = 0, format = FMT_5551, bytes = 2, buffers = 1, option = 0;
	bool bottom_left = false;
	uint32_t pitch = 0, buf[2] = { 0, 0 };
	unsigned front = 0, target = 0;   /* buf[] indices: displayed, drawn into */
	std::vector<uint16_t> depth;
	uint32_t tex_base = 0;            /* card memory after the frame buffers */
};

struct Render {
	Screen scr;
	int clip_x0 = 0, clip_y0 = 0, clip_x1 = 0, clip_y1 = 0;
	unsigned depth_func = CMP_LEQUAL;
	uint8_t clear_bgra[4] = { 0, 0, 0, 0 };
	uint32_t clear_depth = 0xFFFF;
	bool dither = false;
	uint8_t fog_bgra[4] = { 0, 0, 0, 0 };
	uint32_t palette[256] = {};       /* 00RRGGBB */
	unsigned unknown_log = 0;
};
Render rs;

Verite::Core *card = NULL; /* the board, for the call in progress */
inline Verite::Core *Card() { return card; }

unsigned FormatBytes(unsigned fmt) {
	switch (fmt) {
		case FMT_8888: return 4;
		case FMT_5551: case FMT_4444: return 2;
		default: return 1;
	}
}

uint32_t BufferAddr(unsigned i) { return rs.scr.buf[i & 1]; }

/* ---- pixels ---- */

const uint8_t bayer4[4][4] = { { 0, 4, 1, 5 }, { 6, 2, 7, 3 }, { 1, 5, 0, 4 }, { 7, 3, 6, 2 } };

void PutScreenPixel(uint32_t base, int x, int y, int r, int g, int b, int a) {
	Verite::Core *c = Card();
	const Screen &s = rs.scr;
	const uint32_t addr = base + (uint32_t)y * s.pitch + (uint32_t)x * s.bytes;
	switch (s.format) {
		case FMT_8888:
			c->Write32(addr, ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b);
			break;
		case FMT_CI8:
			c->Write8(addr, (uint8_t)r); /* colour index mode: the red component is the index */
			break;
		default: {
			if (rs.dither) {
				const int d = bayer4[y & 3][x & 3];
				r = (std::min)(255, r + d); g = (std::min)(255, g + d); b = (std::min)(255, b + d);
			}
			c->Write16(addr, (uint16_t)((a >= 128 ? 0x8000 : 0) | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)));
			break;
		}
	}
}

void GetScreenPixel(uint32_t base, int x, int y, int &r, int &g, int &b, int &a) {
	Verite::Core *c = Card();
	const Screen &s = rs.scr;
	const uint32_t addr = base + (uint32_t)y * s.pitch + (uint32_t)x * s.bytes;
	switch (s.format) {
		case FMT_8888: {
			const uint32_t v = c->Read32(addr);
			a = (int)(v >> 24); r = (int)((v >> 16) & 0xFF); g = (int)((v >> 8) & 0xFF); b = (int)(v & 0xFF);
			break;
		}
		case FMT_CI8:
			r = c->Read8(addr); g = b = 0; a = 255;
			break;
		default: {
			const uint16_t v = c->Read16(addr);
			r = ((v >> 10) & 31) * 255 / 31; g = ((v >> 5) & 31) * 255 / 31; b = (v & 31) * 255 / 31;
			a = (v & 0x8000) ? 255 : 0;
			break;
		}
	}
}

/* a pixel in a CGL colour format as 8-bit B, G, R, A */
void DecodePixel(uint32_t v, unsigned fmt, bool bgr, uint8_t out[4]) {
	unsigned r, g, b, a = 255;
	switch (fmt) {
		case FMT_8888:
			b = v & 0xFF; g = (v >> 8) & 0xFF; r = (v >> 16) & 0xFF; a = (v >> 24) & 0xFF;
			break;
		case FMT_4444:
			a = ((v >> 12) & 15) * 17; r = ((v >> 8) & 15) * 17; g = ((v >> 4) & 15) * 17; b = (v & 15) * 17;
			break;
		case FMT_332:
			r = ((v >> 5) & 7) * 255 / 7; g = ((v >> 2) & 7) * 255 / 7; b = (v & 3) * 85;
			break;
		case FMT_CI8: {
			const uint32_t c = rs.palette[v & 0xFF];
			r = (c >> 16) & 0xFF; g = (c >> 8) & 0xFF; b = c & 0xFF;
			break;
		}
		default: /* 5551 */
			r = (v >> 10) & 31; g = (v >> 5) & 31; b = v & 31;
			r = (r << 3) | (r >> 2); g = (g << 3) | (g >> 2); b = (b << 3) | (b >> 2);
			a = (v & 0x8000) ? 255 : 0;
			break;
	}
	if (bgr) std::swap(r, b);
	out[0] = (uint8_t)b; out[1] = (uint8_t)g; out[2] = (uint8_t)r; out[3] = (uint8_t)a;
}

/* a texel in card memory */
void Texel(uint32_t addr, unsigned fmt, bool bgr, uint8_t out[4]) {
	Verite::Core *c = Card();
	const unsigned n = FormatBytes(fmt);
	DecodePixel(n == 4 ? c->Read32(addr) : (n == 2 ? c->Read16(addr) : c->Read8(addr)), fmt, bgr, out);
}

/* ---- primitives ---- */

/* interpolated attributes */
enum { A_Z, A_R, A_G, A_B, A_A, A_S, A_T, A_Q, A_F, A_COUNT };
struct Vtx {
	float x, y;
	float v[A_COUNT];
};

struct TexEnv {
	uint32_t addr;
	unsigned stride, width, height, fmt, mode, swrap, twrap, filter;
	bool bgr, overlay, chroma;
	uint8_t chroma_bgra[4];
};

struct Prim {
	unsigned mask;
	bool depth_write;
	uint8_t blend_alpha;
	bool has_tex;
	TexEnv tex;
	uint32_t target;
	float flat[A_COUNT];
};

int Wrap(int i, unsigned size, unsigned mode) {
	if (size == 0) return 0;
	const int n = (int)size;
	switch (mode) {
		case WRAP_REPEAT: i %= n; if (i < 0) i += n; return i;
		case WRAP_MIRROR: {
			int m = i % (2 * n);
			if (m < 0) m += 2 * n;
			return m < n ? m : 2 * n - 1 - m;
		}
		default: return i; /* no wrapping: the address runs on linearly, as on the card */
	}
}

/* bilinear filtering between the four texels around (u, v); texel centres at +0.5. Textures
 * with transparent texels are sampled without filtering, so the key colour can't bleed. */
void SampleBilinear(const TexEnv &t, float u, float v, uint8_t out[4]) {
	u -= 0.5f;
	v -= 0.5f;
	const float fu = floorf(u), fv = floorf(v);
	const int iu = (int)fu, iv = (int)fv;
	const int wu = (int)((u - fu) * 256.0f), wv = (int)((v - fv) * 256.0f);
	const unsigned tb = FormatBytes(t.fmt);
	const int x0 = Wrap(iu, t.width, t.swrap), x1 = Wrap(iu + 1, t.width, t.swrap);
	const int y0 = Wrap(iv, t.height, t.twrap), y1 = Wrap(iv + 1, t.height, t.twrap);
	uint8_t c[4][4];
	Texel(t.addr + ((uint32_t)y0 * t.stride + (uint32_t)x0) * tb, t.fmt, t.bgr, c[0]);
	Texel(t.addr + ((uint32_t)y0 * t.stride + (uint32_t)x1) * tb, t.fmt, t.bgr, c[1]);
	Texel(t.addr + ((uint32_t)y1 * t.stride + (uint32_t)x0) * tb, t.fmt, t.bgr, c[2]);
	Texel(t.addr + ((uint32_t)y1 * t.stride + (uint32_t)x1) * tb, t.fmt, t.bgr, c[3]);
	for (unsigned i = 0; i < 4; i++) {
		const int top = c[0][i] * (256 - wu) + c[1][i] * wu, bottom = c[2][i] * (256 - wu) + c[3][i] * wu;
		out[i] = (uint8_t)((top * (256 - wv) + bottom * wv) >> 16);
	}
}

bool DepthPass(unsigned func, uint32_t z, uint32_t stored) {
	switch (func) {
		case CMP_NEVER: return false;
		case CMP_LESS: return z < stored;
		case CMP_EQUAL: return z == stored;
		case CMP_LEQUAL: return z <= stored;
		case CMP_GREATER: return z > stored;
		case CMP_NOTEQUAL: return z != stored;
		case CMP_GEQUAL: return z >= stored;
		default: return true;
	}
}

void Fragment(const Prim &p, int x, int y, const float *a) {
	Screen &s = rs.scr;
	uint16_t *dz = NULL;
	uint32_t z = 0;
	if ((p.mask & PROP_DEPTH) && !s.depth.empty()) {
		dz = &s.depth[(size_t)y * s.width + (size_t)x];
		const float zf = a[A_Z];
		z = zf <= 0.0f ? 0u : (zf >= 65535.0f ? 65535u : (uint32_t)zf);
		if (!DepthPass(rs.depth_func, z, *dz)) return;
	}
	const float *col = (p.mask & PROP_SHADE) ? a : p.flat;
	int r = (int)col[A_R], g = (int)col[A_G], b = (int)col[A_B], al = (int)col[A_A];
	if (p.has_tex) {
		float u = a[A_S], v = a[A_T];
		if (p.mask & PROP_PERSPECTIVE) {
			const float q = a[A_Q];
			if (q != 0.0f) { u /= q; v /= q; }
		}
		uint8_t t[4];
		if (p.tex.filter && !p.tex.overlay && !p.tex.chroma) SampleBilinear(p.tex, u, v, t);
		else {
			const int tu = Wrap((int)floorf(u), p.tex.width, p.tex.swrap);
			const int tv = Wrap((int)floorf(v), p.tex.height, p.tex.twrap);
			Texel(p.tex.addr + ((uint32_t)tv * p.tex.stride + (uint32_t)tu) * FormatBytes(p.tex.fmt), p.tex.fmt, p.tex.bgr, t);
		}
		/* overlay: texels with a clear alpha bit are holes; chroma: so are those of the key colour */
		if (p.tex.overlay && t[3] < 128) return;
		if (p.tex.chroma && t[0] == p.tex.chroma_bgra[0] && t[1] == p.tex.chroma_bgra[1] && t[2] == p.tex.chroma_bgra[2])
			return;
		switch (p.tex.mode) {
			case TEX_COPY:
				r = t[2]; g = t[1]; b = t[0]; al = t[3];
				break;
			case TEX_DECAL:
				r = (t[2] * t[3] + r * (255 - t[3])) / 255;
				g = (t[1] * t[3] + g * (255 - t[3])) / 255;
				b = (t[0] * t[3] + b * (255 - t[3])) / 255;
				break;
			default: /* modulate */
				r = t[2] * r / 255; g = t[1] * g / 255; b = t[0] * b / 255; al = t[3] * al / 255;
				break;
		}
	}
	if (p.mask & PROP_DEPTHCUE) {
		/* d = 1: the primitive's colour, d = 0: the depth cue colour */
		const float d = (std::max)(0.0f, (std::min)(1.0f, a[A_F]));
		r = (int)(r * d + rs.fog_bgra[2] * (1.0f - d));
		g = (int)(g * d + rs.fog_bgra[1] * (1.0f - d));
		b = (int)(b * d + rs.fog_bgra[0] * (1.0f - d));
	}
	r = (std::max)(0, (std::min)(255, r)); g = (std::max)(0, (std::min)(255, g));
	b = (std::max)(0, (std::min)(255, b)); al = (std::max)(0, (std::min)(255, al));
	if (p.mask & PROP_BLEND) {
		int dr, dg, db, da;
		GetScreenPixel(p.target, x, y, dr, dg, db, da);
		const int k = p.blend_alpha;
		r = (r * k + dr * (255 - k)) / 255;
		g = (g * k + dg * (255 - k)) / 255;
		b = (b * k + db * (255 - k)) / 255;
	}
	PutScreenPixel(p.target, x, y, r, g, b, al);
	if (dz && p.depth_write) *dz = (uint16_t)z;
}

void DrawTriangle(const Prim &p, const Vtx &v0, const Vtx &v1, const Vtx &v2) {
	const float area = (v1.x - v0.x) * (v2.y - v0.y) - (v2.x - v0.x) * (v1.y - v0.y);
	if (area == 0.0f || !std::isfinite(area)) return;
	/* attribute planes: A(x,y) = A0 + dx * (x - x0) + dy * (y - y0) */
	float ddx[A_COUNT], ddy[A_COUNT];
	for (unsigned i = 0; i < A_COUNT; i++) {
		const float d1 = v1.v[i] - v0.v[i], d2 = v2.v[i] - v0.v[i];
		ddx[i] = (d1 * (v2.y - v0.y) - d2 * (v1.y - v0.y)) / area;
		ddy[i] = (d2 * (v1.x - v0.x) - d1 * (v2.x - v0.x)) / area;
	}
	const float fminx = (std::min)(v0.x, (std::min)(v1.x, v2.x)), fmaxx = (std::max)(v0.x, (std::max)(v1.x, v2.x));
	const float fminy = (std::min)(v0.y, (std::min)(v1.y, v2.y)), fmaxy = (std::max)(v0.y, (std::max)(v1.y, v2.y));
	const int x0 = (std::max)(rs.clip_x0, (int)ceilf(fminx - 0.5f)), x1 = (std::min)(rs.clip_x1, (int)ceilf(fmaxx - 0.5f));
	const int y0 = (std::max)(rs.clip_y0, (int)ceilf(fminy - 0.5f)), y1 = (std::min)(rs.clip_y1, (int)ceilf(fmaxy - 0.5f));
	if (x0 >= x1 || y0 >= y1) return;
	/* edges oriented so that inside is positive; top-left rule for pixels on an edge */
	const Vtx *v[3] = { &v0, &v1, &v2 };
	const float sign = area > 0.0f ? 1.0f : -1.0f;
	float ea[3], eb[3], ec[3];
	bool tl[3];
	for (unsigned i = 0; i < 3; i++) {
		const Vtx &p0 = *v[(i + 1) % 3], &p1 = *v[(i + 2) % 3];
		ea[i] = (p0.y - p1.y) * sign;
		eb[i] = (p1.x - p0.x) * sign;
		ec[i] = -(ea[i] * p0.x + eb[i] * p0.y);
		tl[i] = ea[i] > 0.0f || (ea[i] == 0.0f && eb[i] < 0.0f);
	}
	auto inside = [&](int x, float py) {
		const float px = (float)x + 0.5f;
		for (unsigned i = 0; i < 3; i++) {
			const float e = ea[i] * px + eb[i] * py + ec[i];
			if (!(e > 0.0f || (e == 0.0f && tl[i]))) return false;
		}
		return true;
	};
	float attr[A_COUNT];
	for (int y = y0; y < y1; y++) {
		const float py = (float)y + 0.5f;
		/* the row's span from the edges crossing it, then exact tests at its ends */
		float lo = (float)x0, hi = (float)x1;
		bool empty = false;
		for (unsigned i = 0; i < 3; i++) {
			const float c = eb[i] * py + ec[i];
			if (ea[i] > 0.0f) lo = (std::max)(lo, -c / ea[i] - 0.5f);
			else if (ea[i] < 0.0f) hi = (std::min)(hi, -c / ea[i] - 0.5f + 1.0f);
			else if (c < 0.0f) empty = true;
		}
		if (empty) continue;
		int xs = (std::max)(x0, (int)floorf(lo) - 1), xe = (std::min)(x1, (int)ceilf(hi) + 1);
		while (xs < xe && !inside(xs, py)) xs++;
		while (xe > xs && !inside(xe - 1, py)) xe--;
		if (xs >= xe) continue;
		const float px = (float)xs + 0.5f;
		for (unsigned i = 0; i < A_COUNT; i++)
			attr[i] = v0.v[i] + ddx[i] * (px - v0.x) + ddy[i] * (py - v0.y);
		for (int x = xs; x < xe; x++) {
			Fragment(p, x, y, attr);
			for (unsigned i = 0; i < A_COUNT; i++) attr[i] += ddx[i];
		}
	}
}

void DrawLine(const Prim &p, const Vtx &a, const Vtx &b) {
	const float dx = b.x - a.x, dy = b.y - a.y;
	const int steps = (int)ceilf((std::max)(fabsf(dx), fabsf(dy)));
	float attr[A_COUNT];
	for (int i = 0; i <= steps; i++) {
		const float t = steps ? (float)i / (float)steps : 0.0f;
		const int x = (int)floorf(a.x + dx * t), y = (int)floorf(a.y + dy * t);
		if (x < rs.clip_x0 || x >= rs.clip_x1 || y < rs.clip_y0 || y >= rs.clip_y1) continue;
		for (unsigned k = 0; k < A_COUNT; k++) attr[k] = a.v[k] + (b.v[k] - a.v[k]) * t;
		Fragment(p, x, y, attr);
	}
}

void DrawPoint(const Prim &p, const Vtx &a) {
	const int x = (int)floorf(a.x), y = (int)floorf(a.y);
	if (x < rs.clip_x0 || x >= rs.clip_x1 || y < rs.clip_y0 || y >= rs.clip_y1) return;
	Fragment(p, x, y, a.v);
}

int16_t RenderStream(uint32_t h) {
	Screen &s = rs.scr;
	const uint32_t n = Rd32(h, 2);
	const unsigned prim = Rd16(h, 6), mask = Rd16(h, 8);
	const uint32_t p_depth = Rd32(h, 0x0C), p_blend = Rd32(h, 0x14), p_texenv = Rd32(h, 0x18);
	const uint32_t p_vertex = Rd32(h, 0x1C), p_texture = Rd32(h, 0x20), p_color = Rd32(h, 0x24), p_cue = Rd32(h, 0x28);
	if (!p_vertex || n == 0 || n > 65536) return n ? ERR_OPERATION : ERR_NONE;

	Prim p;
	p.mask = mask;
	p.target = BufferAddr(s.target);
	p.depth_write = !(mask & PROP_DEPTH) || !p_depth || Rd16(p_depth, 0) != 0;
	p.blend_alpha = (mask & PROP_BLEND) && p_blend ? Rd8(p_blend, 0) : 255;
	p.has_tex = (mask & PROP_TEXTURE) && p_texenv && p_texture;
	if (p.has_tex) {
		TexEnv &t = p.tex;
		t.addr = s.tex_base + Rd32(p_texenv, 0);
		t.stride = Rd16(p_texenv, 4);
		t.width = Rd16(p_texenv, 6);
		t.height = Rd16(p_texenv, 8);
		t.bgr = Rd16(p_texenv, 10) == 0;
		t.fmt = Rd16(p_texenv, 12);
		t.mode = Rd16(p_texenv, 14);
		t.swrap = Rd16(p_texenv, 16);
		t.twrap = Rd16(p_texenv, 18);
		t.overlay = Rd16(p_texenv, 20) != 0;
		t.chroma = Rd16(p_texenv, 22) != 0;
		for (unsigned i = 0; i < 4; i++) t.chroma_bgra[i] = Rd8(p_texenv, 24 + i);
		t.filter = Rd16(p_texenv, 28);
		if (!t.stride) t.stride = t.width;
	}
	if (cgl.log) { /* each distinct setup once */
		static std::set<std::string> seen;
		std::string d;
		char buf[64];
		snprintf(buf, sizeof(buf), "%04x mask %03x blend %02x texenv", prim, mask, p_blend ? Rd8(p_blend, 0) : 0);
		d = buf;
		for (unsigned i = 4; p_texenv && i < 30; i++) { snprintf(buf, sizeof(buf), " %02x", Rd8(p_texenv, i)); d += buf; }
		if (seen.size() < 100 && seen.insert(d).second) LOG_MSG("CGL: render %s (x%u, first vertex %.1f,%.1f)", d.c_str(), n,
			Coord(Rd32(p_vertex, 0)), Coord(Rd32(p_vertex, 4)));
	}

	/* the arrays hold only what the property mask asks for */
	const uint32_t vstride = (mask & PROP_DEPTH) ? 12 : 8, tstride = (mask & PROP_PERSPECTIVE) ? 12 : 8;
	std::vector<Vtx> vt(n);
	for (uint32_t i = 0; i < n; i++) {
		Vtx &v = vt[i];
		v.x = Coord(Rd32(p_vertex, i * vstride));
		v.y = Coord(Rd32(p_vertex, i * vstride + 4));
		if (s.bottom_left) v.y = (float)s.height - v.y;
		v.v[A_Z] = (mask & PROP_DEPTH) ? Coord(Rd32(p_vertex, i * vstride + 8)) : 0.0f;
		if (p_color) {
			v.v[A_B] = Rd8(p_color, i * 4 + 0);
			v.v[A_G] = Rd8(p_color, i * 4 + 1);
			v.v[A_R] = Rd8(p_color, i * 4 + 2);
			v.v[A_A] = Rd8(p_color, i * 4 + 3);
		}
		else v.v[A_R] = v.v[A_G] = v.v[A_B] = v.v[A_A] = 255.0f;
		if (p.has_tex) {
			v.v[A_S] = Coord(Rd32(p_texture, i * tstride));
			v.v[A_T] = Coord(Rd32(p_texture, i * tstride + 4));
			v.v[A_Q] = (mask & PROP_PERSPECTIVE) ? Coord(Rd32(p_texture, i * tstride + 8)) : 1.0f;
		}
		else v.v[A_S] = v.v[A_T] = 0.0f, v.v[A_Q] = 1.0f;
		v.v[A_F] = (mask & PROP_DEPTHCUE) && p_cue ? Coord(Rd32(p_cue, i * 4)) : 1.0f;
	}
	memcpy(p.flat, vt[0].v, sizeof(p.flat));

	switch (prim) {
		case PRIM_POINT:
			for (uint32_t i = 0; i < n; i++) DrawPoint(p, vt[i]);
			break;
		case PRIM_LINE:
			for (uint32_t i = 0; i + 1 < n; i += 2) DrawLine(p, vt[i], vt[i + 1]);
			break;
		case PRIM_LINE_STRIP:
		case PRIM_LINE_LOOP:
			for (uint32_t i = 0; i + 1 < n; i++) DrawLine(p, vt[i], vt[i + 1]);
			if (prim == PRIM_LINE_LOOP && n > 2) DrawLine(p, vt[n - 1], vt[0]);
			break;
		case PRIM_TRIANGLE:
			for (uint32_t i = 0; i + 2 < n; i += 3) DrawTriangle(p, vt[i], vt[i + 1], vt[i + 2]);
			break;
		case PRIM_TRIANGLE_STRIP:
			for (uint32_t i = 0; i + 2 < n; i++) DrawTriangle(p, vt[i], vt[i + 1], vt[i + 2]);
			break;
		case PRIM_TRIANGLE_FAN:
			for (uint32_t i = 1; i + 1 < n; i++) DrawTriangle(p, vt[0], vt[i], vt[i + 1]);
			break;
		case PRIM_QUAD:
			for (uint32_t i = 0; i + 3 < n; i += 4) {
				DrawTriangle(p, vt[i], vt[i + 1], vt[i + 2]);
				DrawTriangle(p, vt[i], vt[i + 2], vt[i + 3]);
			}
			break;
		case PRIM_QUAD_STRIP:
			for (uint32_t i = 0; i + 3 < n; i += 2) {
				DrawTriangle(p, vt[i], vt[i + 1], vt[i + 3]);
				DrawTriangle(p, vt[i], vt[i + 3], vt[i + 2]);
			}
			break;
		case PRIM_RECTANGLE:
			/* opposite corners; the other two take their coordinates from them */
			for (uint32_t i = 0; i + 1 < n; i += 2) {
				Vtx c1 = vt[i], c3 = vt[i];
				c1.x = vt[i + 1].x; c1.v[A_S] = vt[i + 1].v[A_S];
				c3.y = vt[i + 1].y; c3.v[A_T] = vt[i + 1].v[A_T];
				DrawTriangle(p, vt[i], c1, vt[i + 1]);
				DrawTriangle(p, vt[i], vt[i + 1], c3);
			}
			break;
		default:
			if (rs.unknown_log < 20) { rs.unknown_log++; LOG_MSG("CGL: primitive type %04x is not supported yet", prim); }
			break;
	}
	return ERR_NONE;
}

/* ---- functions ---- */

void ResetClip() {
	rs.clip_x0 = rs.clip_y0 = 0;
	rs.clip_x1 = (int)rs.scr.width;
	rs.clip_y1 = (int)rs.scr.height;
}

void ClearRect(unsigned mask, int x0, int y0, int x1, int y1) {
	Screen &s = rs.scr;
	x0 = (std::max)(x0, 0); y0 = (std::max)(y0, 0);
	x1 = (std::min)(x1, (int)s.width); y1 = (std::min)(y1, (int)s.height);
	if (mask & BUF_FRAME) {
		const bool dither = rs.dither;
		rs.dither = false;
		for (int y = y0; y < y1; y++)
			for (int x = x0; x < x1; x++)
				PutScreenPixel(BufferAddr(s.target), x, y, rs.clear_bgra[2], rs.clear_bgra[1], rs.clear_bgra[0], rs.clear_bgra[3]);
		rs.dither = dither;
	}
	if ((mask & BUF_DEPTH) && !s.depth.empty())
		for (int y = y0; y < y1; y++)
			for (int x = x0; x < x1; x++) s.depth[(size_t)y * s.width + (size_t)x] = (uint16_t)rs.clear_depth;
}

int16_t CheckScreen(uint32_t p, unsigned &w, unsigned &h, unsigned &fmt, unsigned &buffers, unsigned &option) {
	const unsigned mode = Rd16(p, 2);
	fmt = Rd16(p, 4);
	buffers = Rd16(p, 6);
	option = Rd16(p, 8);
	if (mode >= 16 || !mode_size[mode][0]) return ERR_SCREEN_MODE;
	w = mode_size[mode][0];
	h = mode_size[mode][1];
	if (fmt != FMT_5551 && fmt != FMT_8888 && fmt != FMT_CI8) return ERR_COLOR_FORMAT;
	if (buffers != 1 && buffers != 2) return ERR_SCREEN_MODE;
	if (option > 2) return ERR_DEPTH_STENCIL_OPTION;
	Verite::Core *c = Card();
	const uint32_t frame = ((w * FormatBytes(fmt) * h) + 0xFFFu) & ~0xFFFu;
	if (!c || frame * buffers > c->MemSize()) return ERR_NOT_ENOUGH_VRAM;
	return ERR_NONE;
}

int16_t InitScreen(uint32_t p) {
	unsigned w, h, fmt, buffers, option;
	const int16_t e = CheckScreen(p, w, h, fmt, buffers, option);
	if (e) return e;
	Screen &s = rs.scr;
	s.mode = Rd16(p, 2);
	s.bottom_left = Rd16(p, 0) == 1;
	s.width = w;
	s.height = h;
	s.format = fmt;
	s.bytes = FormatBytes(fmt);
	s.buffers = buffers;
	s.option = option;
	s.pitch = w * s.bytes;
	const uint32_t frame = (s.pitch * h + 0xFFFu) & ~0xFFFu;
	s.buf[0] = 0;
	s.buf[1] = buffers == 2 ? frame : 0;
	s.front = 0;
	s.target = buffers == 2 ? 1 : 0;
	s.tex_base = frame * buffers;
	s.depth.assign(option ? (size_t)w * h : 0, 0xFFFF);
	const unsigned type = fmt == FMT_8888 ? M_LIN32 : (fmt == FMT_CI8 ? M_LIN8 : M_LIN15);
	if (!VERITE_CglSetMode(w, h, type)) return ERR_SCREEN_MODE;
	Verite::Core *c = Card();
	memset(c->Memory(), 0, s.tex_base);
	VERITE_SetDisplayStart(s.buf[s.front]);
	ResetClip();
	s.open = true;
	if (cgl.log) LOG_MSG("CGL: screen %ux%u format %u, %u buffer(s), depth/stencil option %u", w, h, fmt, buffers, option);
	return ERR_NONE;
}

/* Texture addresses count from the start of texture memory, after the frame buffers. The
 * texture goes to dwFreeByteAddress, which then points past it: a game reusing the structure
 * gets its textures one after another, one filling it in each time places them itself. The host
 * image is packed; on the card the rows are wBufferWidth texels apart (a game may keep its
 * textures in one wide surface). */
int16_t LoadTextureMap(uint32_t p) {
	Screen &s = rs.scr;
	Verite::Core *c = Card();
	if (!c) return ERR_MUST_INIT_SCREEN;
	uint32_t addr = Rd32(p, 0);
	const unsigned hstride = Rd16(p, 8), w = Rd16(p, 10), h = Rd16(p, 12), fmt = Rd16(p, 14);
	const uint32_t host = Rd32(p, 20);
	if (!host) return ERR_TEXTURE_HOSTBUFFER;
	if (!w || !h) return ERR_TEXTURE_SIZE;
	const unsigned tb = FormatBytes(fmt);
	const unsigned stride = hstride ? hstride : w;
	const uint32_t size = ((h - 1) * stride + w) * tb;
	const uint32_t total = c->MemSize() - s.tex_base;
	if (addr + size > total) return ERR_TEXTURE_EXCEED;
	if (cgl.log) {
		static std::set<uint64_t> seen;
		if (seen.size() < 50 && seen.insert(((uint64_t)hstride << 48) | ((uint64_t)w << 32) | ((uint64_t)h << 16) | fmt).second)
			LOG_MSG("CGL: texture %ux%u format %u, stride %u, at %06x", w, h, fmt, stride, addr);
	}
	for (unsigned y = 0; y < h; y++) {
		const PhysPt src = Ptr(host) + (PhysPt)(y * w * tb);
		const uint32_t dst = s.tex_base + addr + y * stride * tb;
		for (unsigned x = 0; x < w * tb; x += tb) {
			if (tb == 4) c->Write32(dst + x, mem_readd(src + x));
			else if (tb == 2) c->Write16(dst + x, mem_readw(src + x));
			else c->Write8(dst + x, mem_readb(src + x));
		}
	}
	Wr32(p, 4, addr);
	Wr32(p, 0, addr + size);
	Wr32(p, 16, total - (addr + size));
	return ERR_NONE;
}

/* cglLockBuffer: a far pointer (offset, selector) to a buffer in the card's memory window, for
 * the game to reach it directly. It is valid while linear addresses equal physical ones, as with
 * DOS/4GW without paging. */
int16_t LockBuffer(unsigned which, uint32_t out) {
	Screen &s = rs.scr;
	uint32_t at;
	switch (which) {
		case SEL_FRONT: at = BufferAddr(s.front); break;
		case SEL_BACK: at = BufferAddr(s.buffers == 2 ? s.front ^ 1 : s.front); break;
		case BUF_TEXTURE: at = s.tex_base; break;
		default: return ERR_INVALID_BUFFER_TYPE;
	}
	const uint32_t lin = VERITE_LfbBase() + at;
	if (PAGING_Enabled() && PAGING_GetPhysicalAddress(lin) != lin && rs.unknown_log < 40) {
		rs.unknown_log++;
		LOG_MSG("CGL: cglLockBuffer: paging maps %08x elsewhere; the pointer will miss the card", lin);
	}
	if (!out) return ERR_NONE;
	Wr32(out, 0, lin - SegPhys(ds));
	Wr16(out, 4, (uint16_t)SegValue(ds));
	return ERR_NONE;
}

int16_t GetConfig(uint32_t p) {
	const char version[] = "CGL 1.70 for the Rendition Verite (DOSBox-X)";
	for (unsigned i = 0; i < 80; i++) mem_writeb(Ptr(p) + i, i < sizeof(version) ? (uint8_t)version[i] : 0);
	Verite::Core *c = Card();
	Wr16(p, 80, 0x0001);                         /* board: Verite V1000 */
	Wr32(p, 82, c ? c->MemSize() : 0x400000);    /* frame buffer memory */
	Wr32(p, 86, 0);                              /* no separate texture memory */
	Wr16(p, 90, FMT_8888);
	Wr16(p, 92, (uint16_t)rs.scr.mode);
	/* texture, depth cue, shading, chroma key, dither, alpha blending, depth test, clipping */
	Wr32(p, 94, 0x0001 | 0x0008 | 0x0010 | 0x0040 | 0x0080 | 0x0100 | 0x2000 | 0x4000);
	return ERR_NONE;
}

/* cglDownLoad (host to card) and cglUpLoad (card to host) of a rectangle */
enum { PIX_NO_BLEND = 90, PIX_BLEND_IMAGE = 91, PIX_BLEND_PIXEL = 92 };
int16_t Transfer(uint32_t p, bool down) {
	Screen &s = rs.scr;
	const unsigned type = Rd16(p, 0), fmt = Rd16(p, 2);
	const uint32_t host = Rd32(p, 4);
	const int bx = (int16_t)Rd16(p, 8), by = (int16_t)Rd16(p, 10), bw = Rd16(p, 12), bh = Rd16(p, 14);
	const int sx = Rd16(p, 16), sy = Rd16(p, 18);
	const unsigned stride = Rd16(p, 20) ? Rd16(p, 20) : (unsigned)bw, ops = Rd16(p, 22);
	const unsigned alpha = Rd8(p, 24);
	if (!host) return down ? 2001 : 1901;
	const unsigned hb = type == BUF_DEPTH ? 2 : FormatBytes(fmt);
	const uint32_t target = BufferAddr(s.target);
	for (int y = 0; y < bh; y++) {
		const int dy = by + y;
		if (dy < 0 || dy >= (int)s.height) continue;
		const PhysPt row = Ptr(host) + ((uint32_t)(sy + y) * stride + (uint32_t)sx) * hb;
		for (int x = 0; x < bw; x++) {
			const int dx = bx + x;
			if (dx < 0 || dx >= (int)s.width) continue;
			const PhysPt hp = row + (uint32_t)x * hb;
			if (type == BUF_DEPTH) {
				if (s.depth.empty()) return down ? 2003 : 1903;
				uint16_t &d = s.depth[(size_t)dy * s.width + (size_t)dx];
				if (down) d = mem_readw(hp); else mem_writew(hp, d);
				continue;
			}
			if (type != BUF_FRAME) return ERR_INVALID_BUFFER_TYPE;
			if (down) {
				uint8_t c[4];
				DecodePixel(hb == 4 ? mem_readd(hp) : (hb == 2 ? mem_readw(hp) : mem_readb(hp)), fmt, false, c);
				int r = c[2], g = c[1], b = c[0];
				if (ops == PIX_BLEND_PIXEL && c[3] == 0) continue;
				if (ops == PIX_BLEND_IMAGE || (ops == PIX_BLEND_PIXEL && c[3] < 255)) {
					const unsigned k = ops == PIX_BLEND_IMAGE ? alpha : c[3];
					int dr, dg, db, da;
					GetScreenPixel(target, dx, dy, dr, dg, db, da);
					r = (int)((r * k + dr * (255 - k)) / 255);
					g = (int)((g * k + dg * (255 - k)) / 255);
					b = (int)((b * k + db * (255 - k)) / 255);
				}
				PutScreenPixel(target, dx, dy, r, g, b, c[3]);
			}
			else {
				int r, g, b, a;
				GetScreenPixel(target, dx, dy, r, g, b, a);
				uint32_t v;
				switch (fmt) {
					case FMT_8888: v = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b; break;
					case FMT_CI8: case FMT_332: v = fmt == FMT_CI8 ? (uint32_t)r : (uint32_t)(((r >> 5) << 5) | ((g >> 5) << 2) | (b >> 6)); break;
					case FMT_4444: v = (uint32_t)(((a >> 4) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4)); break;
					default: v = (uint32_t)((a >= 128 ? 0x8000 : 0) | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)); break;
				}
				if (hb == 4) mem_writed(hp, v); else if (hb == 2) mem_writew(hp, (uint16_t)v); else mem_writeb(hp, (uint8_t)v);
			}
		}
	}
	return ERR_NONE;
}

int16_t Call(unsigned f) {
	Screen &s = rs.scr;
	const char *name = cgl_funcs[f].name;
	const bool needs_screen = strcmp(name, "cglVersionControl") && strcmp(name, "cglBoardPresent") &&
		strcmp(name, "cglQueryScreen") && strcmp(name, "cglInitScreen") && strcmp(name, "cglGetConfig") &&
		strcmp(name, "cglQueryFeature") && strcmp(name, "cglBoardConfigure") && strcmp(name, "cglCloseScreen") &&
		strcmp(name, "cglLoadTextureMap") && strcmp(name, "cglSetPalette") && strcmp(name, "cglGetPalette");
	if (needs_screen && !s.open) return ERR_MUST_INIT_SCREEN;
	if (!strcmp(name, "cglQueryScreen")) {
		unsigned w, h, fmt, buffers, option;
		return CheckScreen(Arg(0), w, h, fmt, buffers, option);
	}
	if (!strcmp(name, "cglInitScreen")) return InitScreen(Arg(0));
	if (!strcmp(name, "cglCloseScreen")) {
		if (s.open) VERITE_CglEndMode();
		s.open = false;
		s.depth.clear();
		return ERR_NONE;
	}
	if (!strcmp(name, "cglSendStream")) {
		const uint32_t list = Arg(0);
		const unsigned count = Arg(1) & 0xFFFF;
		for (unsigned i = 0; i < count; i++) {
			const uint32_t h = Rd32(list, 4 * i);
			if (!h) return ERR_OPERATION;
			const unsigned op = Rd16(h, 0);
			int16_t e = ERR_NONE;
			if (op == OP_RENDER) e = RenderStream(h);
			else if (op == OP_COPY) {
				if (rs.unknown_log < 20) { rs.unknown_log++; LOG_MSG("CGL: copy streams are not supported yet"); }
			}
			else e = ERR_OPERATION;
			if (e) return e;
		}
		return ERR_NONE;
	}
	if (!strcmp(name, "cglSwapBuffer")) {
		/* the new frame is shown from the next vertical sync, and the call returns then, as on
		 * the card: a game draws no faster than the display refreshes */
		if (!cgl.waiting) {
			if (s.buffers == 2) {
				s.front ^= 1;
				s.target = s.front ^ 1;
				VERITE_SetDisplayStart(BufferAddr(s.front));
			}
			cgl.wait_frame = VERITE_FrameCount() + 1;
			cgl.wait_until = PIC_FullIndex() + 50.0;
			cgl.waiting = true;
		}
		if (VERITE_FrameCount() < cgl.wait_frame && PIC_FullIndex() < cgl.wait_until) {
			cgl.retry = true;
			return ERR_NONE;
		}
		cgl.waiting = false;
		return ERR_NONE;
	}
	if (!strcmp(name, "cglSelectRenderBuffer")) {
		const unsigned which = Arg(0) & 0xFFFF;
		if (which == SEL_FRONT) s.target = s.front;
		else if (which == SEL_BACK) s.target = s.buffers == 2 ? (s.front ^ 1) : s.front;
		else return ERR_INVALID_BUFFER_TYPE;
		return ERR_NONE;
	}
	if (!strcmp(name, "cglCopyDisplayBuffer")) {
		if (s.buffers == 2) {
			const unsigned dir = Arg(0) & 0xFFFF;
			const uint32_t from = BufferAddr(dir == 1 ? s.front : s.front ^ 1), to = BufferAddr(dir == 1 ? s.front ^ 1 : s.front);
			Verite::Core *c = Card();
			for (uint32_t i = 0; i < s.pitch * s.height; i += 4) c->Write32(to + i, c->Read32(from + i));
		}
		return ERR_NONE;
	}
	if (!strcmp(name, "cglClearScreen")) {
		ClearRect(BUF_FRAME | BUF_DEPTH, 0, 0, (int)s.width, (int)s.height);
		return ERR_NONE;
	}
	if (!strcmp(name, "cglSetConstant")) {
		const uint32_t col = Arg(0);
		if (col) for (unsigned i = 0; i < 4; i++) rs.clear_bgra[i] = Rd8(col, i);
		rs.clear_depth = Arg(1) & 0xFFFF;
		return ERR_NONE;
	}
	if (!strcmp(name, "cglClearBuffer")) {
		const uint32_t rect = Arg(1);
		if (!rect) ClearRect(Arg(0), 0, 0, (int)s.width, (int)s.height);
		else {
			const int x = Rd16(rect, 0), y = Rd16(rect, 2), w = Rd16(rect, 4), h = Rd16(rect, 6);
			ClearRect(Arg(0), x, y, x + w, y + h);
		}
		return ERR_NONE;
	}
	if (!strcmp(name, "cglSetWindowClip")) {
		const uint32_t rect = Arg(0);
		if (!rect) ResetClip();
		else {
			rs.clip_x0 = Rd16(rect, 0);
			rs.clip_y0 = Rd16(rect, 2);
			rs.clip_x1 = (std::min)((int)s.width, rs.clip_x0 + Rd16(rect, 4));
			rs.clip_y1 = (std::min)((int)s.height, rs.clip_y0 + Rd16(rect, 6));
		}
		return ERR_NONE;
	}
	if (!strcmp(name, "cglSetDepthMode")) { rs.depth_func = Arg(0) & 7; return ERR_NONE; }
	if (!strcmp(name, "cglSetDitherMode")) { rs.dither = (Arg(0) & 0xFFFF) != 0; return ERR_NONE; }
	if (!strcmp(name, "cglSetDepthCueColor")) {
		if (Arg(0)) for (unsigned i = 0; i < 4; i++) rs.fog_bgra[i] = Rd8(Arg(0), i);
		return ERR_NONE;
	}
	if (!strcmp(name, "cglLoadTextureMap")) return LoadTextureMap(Arg(0));
	if (!strcmp(name, "cglSetPalette") || !strcmp(name, "cglGetPalette")) {
		const uint32_t colors = Arg(0);
		const unsigned first = Arg(1) & 0xFFFF, count = Arg(2) & 0xFFFF;
		for (unsigned i = 0; i < count && first + i < 256; i++) {
			if (name[3] == 'S') {
				rs.palette[first + i] = ((uint32_t)Rd8(colors, 4 * i + 2) << 16) | ((uint32_t)Rd8(colors, 4 * i + 1) << 8) | Rd8(colors, 4 * i);
			}
			else {
				const uint32_t v = rs.palette[first + i];
				Wr32(colors, 4 * i, v);
			}
		}
		return ERR_NONE;
	}
	if (!strcmp(name, "cglPutPixel") || !strcmp(name, "cglGetPixel")) {
		const int x = (int)Coord(Arg(0)), y = (int)Coord(Arg(1));
		const uint32_t col = Arg(2);
		if (x < 0 || y < 0 || x >= (int)s.width || y >= (int)s.height || !col) return ERR_NONE;
		if (name[3] == 'P') PutScreenPixel(BufferAddr(s.target), x, y, Rd8(col, 2), Rd8(col, 1), Rd8(col, 0), Rd8(col, 3));
		else {
			int r, g, b, a;
			GetScreenPixel(BufferAddr(s.target), x, y, r, g, b, a);
			Wr32(col, 0, (uint32_t)b | ((uint32_t)g << 8) | ((uint32_t)r << 16) | ((uint32_t)a << 24));
		}
		return ERR_NONE;
	}
	if (!strcmp(name, "cglGetConfig")) return GetConfig(Arg(0));
	if (!strcmp(name, "cglLockBuffer")) return LockBuffer(Arg(0) & 0xFFFF, Arg(1));
	if (!strcmp(name, "cglDownLoad")) return Transfer(Arg(0), true);
	if (!strcmp(name, "cglUpLoad")) return Transfer(Arg(0), false);
	/* cglVersionControl, cglBoardPresent and anything not implemented: success */
	if (cgl.log && strcmp(name, "cglVersionControl") && strcmp(name, "cglBoardPresent") && rs.unknown_log < 40) {
		rs.unknown_log++;
		LOG_MSG("CGL: %s is not implemented, returning success", name);
	}
	return ERR_NONE;
}

Bitu CGL_Handler(void) {
	const uint8_t f = mem_readb(SegPhys(cs) + reg_eip);
	cgl.type = mem_readb(SegPhys(cs) + reg_eip + 1) & 3;
	card = VERITE_GetCore();
	int16_t result = ERR_NONE;
	cgl.retry = false;
	if (f < CGL_FUNCS) {
		if (!cgl.waiting) LogCall(f);
		result = Call(f);
		if (cgl.retry) {
			/* not done yet: run the stub's callback again, letting emulated time pass */
			reg_eip -= 4;
			return CBRET_NONE;
		}
		if (result && cgl.log) LOG_MSG("CGL: %s returns %d", cgl_funcs[f].name, result);
	}
	else LOG_MSG("CGL: call to an export this driver doesn't know");
	reg_eax = (uint32_t)(int32_t)result;
	/* return to the caller, which removes any stack arguments itself (the variants with stack
	 * arguments follow the C convention; register calls pass at most four) */
	reg_eip = mem_readd(SegPhys(ss) + reg_esp);
	reg_esp += 4;
	return CBRET_NONE;
}

bool IsDriverName(const char *fullname) {
	const char *b = strrchr(fullname, '\\');
	b = b ? b + 1 : fullname;
	if (strlen(b) != 12) return false;
	return !strncasecmp(b, "VRIT", 4) && !strcasecmp(b + 8, ".CGL");
}

} // namespace

bool VERITE_CglReplaceModule(DOS_File **file, const char *fullname) {
	if (svgaCard != SVGA_Verite || !IsDriverName(fullname)) return false;
	const Section_prop *sec = static_cast<Section_prop *>(control->GetSection("verite"));
	if (!sec || !sec->Get_bool("verite_cgl")) return false;
	if (!cgl.callback) {
		cgl.callback = CALLBACK_Allocate();
		CALLBACK_Setup(cgl.callback, &CGL_Handler, CB_RETF, "Verite CGL");
		for (unsigned i = 0; i < CGL_FUNCS; i++) cgl.log_left[i] = 20;
	}
	cgl.log = sec->Get_bool("verite_debug");
	std::vector<uint8_t> orig, img;
	uint32_t type = 0;
	if (!ReadWhole(*file, orig) || !BuildModule(orig, img, type)) {
		LOG_MSG("CGL: %s is not a CGL module this emulation understands; leaving it alone", fullname);
		return false;
	}
	static const char *const variants[] = { "fixed point, register", "fixed point, stack", "floating point, register", "floating point, stack" };
	LOG_MSG("CGL: serving the built-in driver for %s (%s arguments)", fullname, variants[type]);
	DOS_File *mod = new CglModuleFile(std::move(img), **file);
	mod->SetDrive((*file)->GetDrive());
	(*file)->Close();
	delete *file;
	*file = mod;
	return true;
}
