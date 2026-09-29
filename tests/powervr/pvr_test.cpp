/* Host-side test for the PowerVR PCX1/PCX2 emulation (src/hardware/pvr_pcx.cpp).
 *
 * Builds display lists the way the vendor SGL packs them (pkisp.c, dtri.c, pvrlims.h),
 * renders them through the emulated registers and writes the result as a PPM image.
 * Also feeds random garbage display lists to check that rendering always terminates.
 *
 * Build and run: see tests/powervr/run.sh
 */

#include <stdarg.h>
#include <math.h>
#include <chrono>
#include "../../src/hardware/pvr_pcx.cpp"

/* ---- shim implementations ---- */
std::vector<uint8_t> shim_ram(32u << 20);
ShimControl *control = NULL;
bool pcibus_enable = true;
static bool quiet = false;
void shim_log(const char *fmt, ...) {
	if (quiet) return;
	va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); printf("\n");
}
void E_Exit(const char *fmt, ...) {
	va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); printf("\n"); exit(1);
}
Bitu MEM_TotalPages(void) { return shim_ram.size() / 4096; }
uint32_t MEM_HardwareAllocate(const char *, uint32_t) { return 0xE0000000u; }
void PAGING_ClearTLB(void) {}
static MEM_CalloutObject callouts[4];
static int ncallouts = 0;
MEM_Callout_t MEM_AllocateCallout(int) { return ncallouts++; }
void MEM_FreeCallout(MEM_Callout_t) {}
MEM_CalloutObject *MEM_GetCallout(MEM_Callout_t c) { return &callouts[c]; }
void MEM_PutCallout(MEM_CalloutObject *) {}
static int irq_state = 0;
void PIC_ActivateIRQ(Bitu) { irq_state = 1; }
void PIC_DeActivateIRQ(Bitu) { irq_state = 0; }
void AddExitFunction(SectionFunc, bool) {}
void AddVMEventFunction(int, SectionFunc) {}
Bits RegisterPCIDevice(PCI_Device *, Bits, Bits) { return 0; }
bool UnregisterPCIDevice(PCI_Device *) { return true; }
bool PCI_IsSlotFree(Bits, Bits) { return true; }

/* ---- display list construction ---- */

static const uint32_t OBJ_BASE = 0x100000;   /* object pointers (physical, PCX1) */
static const uint32_t PLANE_BASE = 0x20B000; /* plane data, reached through the TLB (not 16KB aligned, as in Tomb Raider) */
static const uint32_t FB_BASE = 0x800000;
static const int W = 640, H = 480;

static std::vector<uint32_t> plane_words;
static std::vector<uint32_t> obj_words;

static uint32_t pack_plane(float A, float B, float C, int instr, uint32_t tag, bool pcx2) {
	const uint32_t addr = (uint32_t)plane_words.size();
	if (pcx2) {
		uint32_t a, b, c; memcpy(&a, &A, 4); memcpy(&b, &B, 4); memcpy(&c, &C, 4);
		plane_words.push_back(a); plane_words.push_back(b); plane_words.push_back(c);
		plane_words.push_back((uint32_t)instr | (tag << 4));
	}
	else {
		if (instr == forw_perp || instr == test_shad_perp) { /* dlines.c perpendicular scaling */
			const float inv = 1.0f / (2 * 1024.0f * (fabsf(A) + fabsf(B)) + fabsf(C));
			A *= inv; B *= inv; C *= inv;
		}
		plane_words.push_back(((uint32_t)instr << 26) | ((tag & 0x3F000) << 8) | PackTo20Bit(A));
		plane_words.push_back((tag << 20) | PackTo20Bit(B));
		plane_words.push_back((uint32_t)(int32_t)(C * 2147483648.0f));
	}
	return addr;
}

struct Poly { std::vector<uint32_t> first; uint32_t n; };

/* convex polygon: face plane with depth (1/w) a*x+b*y+c, plus one perpendicular plane per
 * edge, positive inside */
static void add_object(const float *xy, int nv, float da, float db, float dc, uint32_t tag, bool pcx2,
		int face_instr = forw_visib_fp) {
	const uint32_t words = pcx2 ? 4 : 3;
	const uint32_t first = pack_plane(da, db, dc, face_instr, tag, pcx2);
	for (int i = 0; i < nv; i++) {
		const float x0 = xy[i * 2], y0 = xy[i * 2 + 1];
		const float x1 = xy[((i + 1) % nv) * 2], y1 = xy[((i + 1) % nv) * 2 + 1];
		/* inside (clockwise on screen) gives a positive value */
		const float A = -(y1 - y0), B = (x1 - x0), C = -(A * x0 + B * y0);
		pack_plane(A, B, C, forw_perp, 0, pcx2);
	}
	obj_words.push_back(((uint32_t)(nv + 1) << 19) | (first / words * words));
	(void)words;
}

static void add_dummy(int instr, bool pcx2) {
	const uint32_t first = pack_plane(0, 0, 0, instr, 0, pcx2);
	obj_words.push_back((1u << 19) | first);
}

static void write_tsp(uint32_t word, uint32_t v) {
	uint8_t *p = pvr->tmem + word * 4;
	p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void write_texel(uint32_t pixaddr, uint16_t v) {
	uint8_t *p = pvr->tmem + (pixaddr >> 1) * 4;
	if (pixaddr & 1) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
	else { p[2] = (uint8_t)v; p[3] = (uint8_t)(v >> 8); }
}

static void flat_tsp(uint32_t tag, int r, int g, int b, uint32_t extra = 0) {
	write_tsp(tag * 2, MASK_DISABLE_FOG | extra | (uint32_t)r);
	write_tsp(tag * 2 + 1, ((uint32_t)g << 24) | ((uint32_t)b << 16));
}

/* textured, flat white, affine mapping u = x/ku, v = y/kv (p = q = 0) */
static void tex_tsp(uint32_t tag, uint32_t texaddr, int raw_mapsize, bool trans, bool trans4444, int globaltrans) {
	const uint32_t base = tag * 2;
	const int exp = 8;
	write_tsp(base + 0, MASK_TEXTURE | MASK_DISABLE_FOG | (trans ? MASK_TRANS : 0) | ((uint32_t)exp << 18) |
		((uint32_t)globaltrans << 13) | 0xFF);
	write_tsp(base + 1, (0xFFu << 24) | (0xFFu << 16));
	write_tsp(base + 2, 0x4000);          /* r (pmip 0) */
	write_tsp(base + 3, 0);               /* q, p */
	write_tsp(base + 4, ((texaddr & 0xFFFF) << 16) | 0); /* texture address low, c */
	write_tsp(base + 5, (0u << 16) | 0x100); /* b, a */
	write_tsp(base + 6, MASK_8_16_MAPS | (trans4444 ? MASK_4444_555 : 0) | ((uint32_t)raw_mapsize << 28) |
		(texaddr & 0x00FF0000) | 0);      /* f */
	write_tsp(base + 7, (0x100u << 16) | 0); /* e, d */
}

static void setup_regs(bool pcx2, uint32_t packmode) {
	pvr->WriteReg(PCX_INTMASK, INT_END_OF_RENDER);
	if (!pcx2) {
		/* as Tomb Raider (SGL4DOS 1.27): page frame numbers, 16KB slots, PAGE_CTRL = 0x300 */
		pvr->WriteReg(PCX_PAGE_CTRL, 0x300);
		for (int i = 0; i < 256; i++) pvr->WriteReg(PCX_TLB + i, (PLANE_BASE >> 12) + (uint32_t)i * 4);
	}
	else {
		pvr->WriteReg(PCX_PAGE_CTRL, 0);
		for (int i = 0; i < 256; i++) pvr->WriteReg(PCX_TLB + i, PLANE_BASE + (uint32_t)i * 4096);
	}
	pvr->WriteReg(PCX_OBJECT_OFFSET, OBJ_BASE);
	pvr->WriteReg(PCX_SOFADDR, FB_BASE);
	pvr->WriteReg(PCX_LSTRIDE, W * (packmode == 0 ? 4 : 2));
	pvr->WriteReg(PCX_PACKMODE, packmode);
	pvr->WriteReg(PCX_CAMERA, 1);
	pvr->WriteReg(PCX_IEEEFP, pcx2 ? 1 : 0);
	pvr->WriteReg(PCX_BILINEAR, 3);
}

static void upload() {
	for (size_t i = 0; i < plane_words.size(); i++) phys_writed(PLANE_BASE + (uint32_t)i * 4, plane_words[i]);
	for (size_t i = 0; i < obj_words.size(); i++) phys_writed(OBJ_BASE + (uint32_t)i * 4, obj_words[i]);
}

static void write_ppm(const char *name, uint32_t packmode) {
	FILE *f = fopen(name, "wb");
	fprintf(f, "P6\n%d %d\n255\n", W, H);
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++) {
			uint8_t rgb[3];
			if (packmode == 0) {
				const uint32_t v = phys_readd(FB_BASE + (uint32_t)(y * W + x) * 4);
				rgb[0] = (uint8_t)(v >> 16); rgb[1] = (uint8_t)(v >> 8); rgb[2] = (uint8_t)v;
			}
			else {
				const uint32_t v = phys_readw(FB_BASE + (uint32_t)(y * W + x) * 2);
				rgb[0] = (uint8_t)(((v >> 11) & 31) << 3); rgb[1] = (uint8_t)(((v >> 5) & 63) << 2); rgb[2] = (uint8_t)((v & 31) << 3);
			}
			fwrite(rgb, 1, 3, f);
		}
	fclose(f);
}

static uint32_t pixel(int x, int y) {
	return phys_readd(FB_BASE + (uint32_t)(y * W + x) * 4) & 0xFFFFFF;
}

static int failures = 0;
static void expect(const char *what, int x, int y, uint32_t want, int tol = 8) {
	const uint32_t got = pixel(x, y);
	bool ok = true;
	for (int s = 0; s < 24; s += 8) {
		if (abs((int)((got >> s) & 0xFF) - (int)((want >> s) & 0xFF)) > tol) ok = false;
	}
	printf("%s %-40s (%3d,%3d) got %06x want %06x\n", ok ? "PASS" : "FAIL", what, x, y, got, want);
	if (!ok) failures++;
}

static void build_scene(bool pcx2) {
	plane_words.clear();
	obj_words.clear();
	/* tags: PCX1 TSP parameters start at word 12, i.e. tag 6 */
	const uint32_t T_BG = 8, T_RED = 10, T_TEX = 12, T_GLASS = 20;
	flat_tsp(T_BG, 0x20, 0x40, 0x80);
	flat_tsp(T_RED, 0xF0, 0x10, 0x10);
	tex_tsp(T_TEX, 0x10000, 1 /* 64x64 */, false, false, 0);
	tex_tsp(T_GLASS, 0x20000, 1, true, true, 0);
	for (int v = 0; v < 64; v++)
		for (int u = 0; u < 64; u++) {
			const bool on = ((u >> 3) ^ (v >> 3)) & 1;
			write_texel(0x10000 + Twid(u, v), on ? 0x7FFF : 0x001F);   /* 555 white / blue */
			write_texel(0x20000 + Twid(u, v), 0x80F0);                 /* 4444: alpha 8, green */
		}

	/* region list: 20x15 tiles of 32x32 */
	for (int ty = 0; ty < H / 32; ty++)
		for (int tx = 0; tx < W / 32; tx++) {
			obj_words.push_back(REG_COMPULSARY_BITS | ((uint32_t)(ty * 32) << 20) | ((uint32_t)tx << 15) | (31u << 5) | 0u);
			const float bg[] = { 0, 0, 640, 0, 640, 480, 0, 480 };
			/* background: flat plane, far away */
			const uint32_t words = pcx2 ? 4 : 3;
			(void)bg;
			const uint32_t first = pack_plane(0, 0, 0.0001f, forw_visib_fp, T_BG, pcx2);
			obj_words.push_back((1u << 19) | first);
			(void)words;
			const float tri[] = { 100, 50, 400, 300, 60, 400 };
			add_object(tri, 3, 0, 0, 0.25f, T_RED, pcx2);
			/* textured quad in front of the triangle's right part, tilted in depth */
			const float quad[] = { 250, 100, 500, 100, 500, 350, 250, 350 };
			add_object(quad, 4, 0.0002f, 0, 0.2f, T_TEX, pcx2);
			/* translucent pass */
			add_dummy(begin_trans, pcx2);
			const float glass[] = { 150, 200, 550, 200, 550, 450, 150, 450 };
			add_object(glass, 4, 0, 0, 0.5f, T_GLASS, pcx2);
		}
	obj_words.back() |= OBJ_VERY_LAST_PTR;
	upload();
}

static void fuzz(bool pcx2) {
	quiet = true;
	srand(1234);
	for (int iter = 0; iter < 300; iter++) {
		for (uint32_t i = 0; i < 4096; i++) phys_writed(OBJ_BASE + i * 4, (uint32_t)rand() * 2654435761u ^ (uint32_t)rand());
		for (uint32_t i = 0; i < 65536; i++) phys_writed(PLANE_BASE + i * 4, (uint32_t)rand() * 2246822519u ^ (uint32_t)rand());
		/* make the first word a plausible header so the traversal actually starts */
		phys_writed(OBJ_BASE, REG_COMPULSARY_BITS | (uint32_t)(rand() & 0x3FFFFFFF));
		for (int i = 0; i < 16; i++) pvr->WriteReg(PCX_TLB + i, (uint32_t)rand() * 2654435761u);
		pvr->WriteReg(PCX_TLB, PLANE_BASE);
		pvr->WriteReg(PCX_PREC_BASE, (uint32_t)rand() * 2654435761u);
		pvr->WriteReg(PCX_CAMERA, (uint32_t)rand());
		pvr->WriteReg(PCX_FOGAMOUNT, (uint32_t)rand());
		pvr->WriteReg(PCX_STARTRENDER, 1);
	}
	quiet = false;
	printf("PASS fuzz (%s): 300 random display lists terminated\n", pcx2 ? "PCX2" : "PCX1");
}

int main(int argc, char **argv) {
	const char *outdir = argc > 1 ? argv[1] : ".";
	InitTwiddle();
	for (int chip = 0; chip < 2; chip++) {
		const bool pcx2 = chip == 1;
		pvr = new PowerVR(pcx2, 11, false);
		setup_regs(pcx2, 0);
		build_scene(pcx2);
		const auto t0 = std::chrono::steady_clock::now();
		pvr->WriteReg(PCX_STARTRENDER, 1);
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		printf("--- %s --- (640x480 frame rendered in %.1f ms)\n", pcx2 ? "PCX2 (floating point planes)" : "PCX1", ms);
		if (!irq_state) { printf("FAIL end-of-render interrupt not raised\n"); failures++; }
		if (!(pvr->ReadReg(PCX_INTSTATUS) & INT_END_OF_RENDER)) { printf("FAIL INT_STATUS\n"); failures++; }
		if (irq_state || pvr->ReadReg(PCX_INTSTATUS)) { printf("FAIL INT_STATUS read did not acknowledge\n"); failures++; }
		expect("background", 10, 10, 0x204080);
		expect("flat red triangle", 110, 200, 0xF01010);
		expect("textured quad (checker white)", 252, 102, 0xFFFFFF, 12);
		/* 4444 green 0x8 alpha 8 over background: half and half */
		expect("translucent over background", 520, 420, 0x109E40, 8);
		expect("translucent over red triangle", 200, 300, 0x788608, 8);
		expect("textured quad (checker blue)", 262, 102, 0x0303FB, 8);
		char name[256];
		snprintf(name, sizeof(name), "%s/pvr_%s.ppm", outdir, pcx2 ? "pcx2" : "pcx1");
		write_ppm(name, 0);
		printf("wrote %s\n", name);
		fuzz(pcx2);
		delete pvr;
		pvr = NULL;
	}
	printf(failures ? "%d FAILURES\n" : "ALL PASSED\n", failures);
	return failures ? 1 : 0;
}
