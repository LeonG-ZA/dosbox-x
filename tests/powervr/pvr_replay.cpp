/* Replays a frame saved by the PowerVR emulation with powervr_debug = true
 * (pvrdump_NNNN.bin) and writes the result as a PPM image.
 *
 * Build: g++ -std=gnu++14 -O2 -Itests/powervr/shim tests/powervr/pvr_replay.cpp -o pvr_replay
 * Run:   ./pvr_replay pvrdump_0060.bin out.ppm
 */

#include <stdarg.h>
#include "../../src/hardware/pvr_pcx.cpp"

std::vector<uint8_t> shim_ram(64u << 20);
ShimControl *control = NULL;
bool pcibus_enable = true;
void shim_log(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); printf("\n"); }
void E_Exit(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); printf("\n"); exit(1); }
Bitu MEM_TotalPages(void) { return shim_ram.size() / 4096; }
uint32_t MEM_HardwareAllocate(const char *, uint32_t) { return 0xE0000000u; }
void PAGING_ClearTLB(void) {}
static MEM_CalloutObject callouts[4];
static int ncallouts = 0;
MEM_Callout_t MEM_AllocateCallout(int) { return ncallouts++; }
void MEM_FreeCallout(MEM_Callout_t) {}
MEM_CalloutObject *MEM_GetCallout(MEM_Callout_t c) { return &callouts[c]; }
void MEM_PutCallout(MEM_CalloutObject *) {}
void PIC_ActivateIRQ(Bitu) {}
void PIC_DeActivateIRQ(Bitu) {}
void AddExitFunction(SectionFunc, bool) {}
void AddVMEventFunction(int, SectionFunc) {}
Bits RegisterPCIDevice(PCI_Device *, Bits, Bits) { return 0; }
bool UnregisterPCIDevice(PCI_Device *) { return true; }
bool PCI_IsSlotFree(Bits, Bits) { return true; }

static const uint32_t PLANE_BASE = 0x0400000, OBJ_BASE = 0x1000000, FB_BASE = 0x2000000;

static uint32_t get32(FILE *f) { uint8_t b[4]; if (fread(b, 1, 4, f) != 4) E_Exit("short dump"); return b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24); }

int main(int argc, char **argv) {
	if (argc < 3) { printf("usage: pvr_replay dump.bin out.ppm\n"); return 1; }
	FILE *f = fopen(argv[1], "rb");
	if (!f) E_Exit("cannot open %s", argv[1]);
	char magic[8];
	if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "PVRDUMP1", 8)) E_Exit("not a PowerVR dump");
	const bool pcx2 = get32(f) != 0;
	const uint32_t shift = get32(f);
	InitTwiddle();
	pvr = new PowerVR(pcx2, 0, true);
	uint32_t regs[PCX_NUM_REGS];
	for (int i = 0; i < PCX_NUM_REGS; i++) regs[i] = get32(f);
	if (fread(pvr->tmem, 1, TMEM_SIZE, f) != TMEM_SIZE) E_Exit("short dump");
	const uint32_t plane_words = (256u << shift) / 4;
	for (uint32_t i = 0; i < plane_words; i++) phys_writed(PLANE_BASE + i * 4, get32(f));
	for (uint32_t i = 0; i < (1u << 20); i++) phys_writed(OBJ_BASE + i * 4, get32(f));
	fclose(f);

	/* relocate: planes contiguous at PLANE_BASE, object list at OBJ_BASE, frame buffer at FB_BASE */
	for (int i = 0; i < PCX_NUM_REGS; i++) pvr->regs[i] = regs[i];
	for (int i = 0; i < 256; i++) pvr->regs[PCX_TLB + i] = PLANE_BASE + ((uint32_t)i << shift);
	if (!(pcx2 && (regs[PCX_OBJECT_OFFSET] & 1))) pvr->regs[PCX_OBJECT_OFFSET] = OBJ_BASE;
	pvr->regs[PCX_SOFADDR] = FB_BASE;
	pvr->regs[PCX_PAGE_CTRL] = 0;
	pvr->WriteReg(PCX_STARTRENDER, 0);

	const unsigned bpp = pvr->OutBytesPerPixel();
	const int W = 640, H = 480;
	FILE *o = fopen(argv[2], "wb");
	fprintf(o, "P6\n%d %d\n255\n", W, H);
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++) {
			(void)bpp;
			const RGBi c = pvr->ReadFB(x, y);
			const uint8_t rgb[3] = { (uint8_t)c.r, (uint8_t)c.g, (uint8_t)c.b };
			fwrite(rgb, 1, 3, o);
		}
	fclose(o);
	printf("wrote %s\n", argv[2]);
	return 0;
}
