/* Host-side tests of the Rendition Verite V1000 core (src/hardware/verite_core.cpp).
 *
 * Part 1 runs small hand-assembled RISC programs that check instruction semantics.
 * Part 2 drives the V1000 2D microcode from the X.org rendition driver (v10002d.uc, given
 * on the command line) through the core's host interface exactly as that driver does:
 * soft reset, forced single steps, the context switch monitor at 0x800, ELF loading with
 * MemEndian byte swapping, then FIFO commands. The frame buffer is checked pixel by pixel
 * and written to verite_2d.ppm.
 *
 * Usage: verite_test [path/to/v10002d.uc] [output directory]
 */

#include "../../src/hardware/verite_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <string>
#include <vector>

using namespace Verite;

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

class TestBoard : public Board {
public:
	uint32_t ext[256 / 4] = {};
	std::vector<uint8_t> host_ram;
	bool irq = false;
	bool quiet = false;
	TestBoard() : host_ram(1 << 20, 0) {}
	uint32_t ExtRegRead(uint32_t off, unsigned len) override {
		(void)len;
		return ext[(off & 0xFF) >> 2] >> (8 * (off & 3));
	}
	void ExtRegWrite(uint32_t off, uint32_t val, unsigned len) override {
		const unsigned i = (off & 0xFF) >> 2, sh = 8 * (off & 3);
		const uint32_t mask = len >= 4 ? 0xFFFFFFFFu : (((1u << (8 * len)) - 1u) << sh);
		ext[i] = (ext[i] & ~mask) | ((val << sh) & mask);
	}
	void IrqUpdate(bool asserted) override { irq = asserted; }
	uint32_t DmaRead32(uint32_t phys) override {
		if (phys + 4 > host_ram.size()) return 0;
		return host_ram[phys] | (host_ram[phys + 1] << 8) | (host_ram[phys + 2] << 16) | ((uint32_t)host_ram[phys + 3] << 24);
	}
	void ModeChanged() override {}
	void Message(const char *msg) override { if (!quiet) printf("  [core] %s\n", msg); }
};

/* ---------------------------------------------------------------------------------- */
/* Part 1: instruction semantics                                                      */

static uint32_t INT(unsigned op, unsigned d, unsigned s2, unsigned s1) { return (op << 24) | (d << 16) | (s2 << 8) | (s1 & 0xFF); }
static uint32_t LI(unsigned op, unsigned d, unsigned imm) { return (op << 24) | (d << 16) | (imm & 0xFFFF); }
static uint32_t BR(unsigned op, int off, unsigned s1) { return (op << 24) | (((unsigned)off & 0xFFFF) << 8) | s1; }
static uint32_t ST(unsigned op, unsigned off8, unsigned s2, unsigned s1) { return (op << 24) | (off8 << 16) | (s2 << 8) | s1; }
static uint32_t LD(unsigned op, unsigned d, unsigned off8, unsigned s1) { return (op << 24) | (d << 16) | (off8 << 8) | s1; }
static const uint32_t NOP = 0, HALT = 0x6D000000u;

/* Load a program at address 0x1000 through the host interface and run it from there */
static void RunProgram(Core &c, const std::vector<uint32_t> &prog, uint32_t max_cycles = 100000) {
	c.Reset(true);
	c.IOWrite(IO_MODE, 0, 1);
	for (size_t i = 0; i < prog.size(); i++) c.MemWrite(0x1000 + 4 * (uint32_t)i, prog[i], 4);
	/* jump there with forced steps, as v1k_start does */
	auto force = [&](uint32_t ir) {
		c.IOWrite(IO_STATEINDEX, 128, 1);
		c.IOWrite(IO_STATEDATA, ir, 4);
		c.IOWrite(IO_DEBUG, DEBUG_HOLDRISC | DEBUG_STEPRISC, 1);
	};
	force(NOP); force(NOP); force(NOP);
	force(0x6C000000u | (0x1000 >> 2));
	force(NOP);
	c.IOWrite(IO_DEBUG, 0, 1);
	c.Run(max_cycles);
}

static void TestInstructions() {
	TestBoard b;
	Core c(&b, CHIP_V1000, 4 << 20);

	/* ALU basics, li/lui, delay slot */
	RunProgram(c, {
		LI(0x76, 128, 0x1234),          /* li %128, 0x1234 */
		LI(0x77, 129, 0xABCD),          /* lui %129, 0xabcd */
		INT(0x15, 130, 129, 128),       /* or %130, %129, %128 */
		INT(0x01, 131, 130, 0x34),      /* sub %131, %130, #0x34 */
		INT(0x45, 132, 128, 4),         /* sl %132, %128, #4 */
		INT(0x46, 133, 129, 16),        /* asr %133, %129, #16 */
		INT(0x47, 134, 129, 16),        /* sr %134, %129, #16 */
		BR(0x62, 2, 0),                 /* bgez zero, +2 (to the li %136) */
		LI(0x76, 135, 7),               /* delay slot: executed */
		LI(0x76, 136, 9),               /* skipped */
		LI(0x76, 137, 11),              /* branch target */
		HALT
	});
	CHECK(c.reg[130] == 0xABCD1234u, "or: %08x", c.reg[130]);
	CHECK(c.reg[131] == 0xABCD1200u, "sub imm: %08x", c.reg[131]);
	CHECK(c.reg[132] == 0x12340u, "sl: %08x", c.reg[132]);
	CHECK(c.reg[133] == 0xFFFFABCDu, "asr: %08x", c.reg[133]);
	CHECK(c.reg[134] == 0x0000ABCDu, "sr: %08x", c.reg[134]);
	CHECK(c.reg[135] == 7, "delay slot not executed");
	CHECK(c.reg[136] == 0, "branch not taken");
	CHECK(c.reg[137] == 11, "branch target not reached");

	/* vector ops on constant pairs, as the 2D microcode uses them */
	RunProgram(c, {
		LI(0x76, 140, 5), LI(0x76, 141, 3),
		INT(0x20, 140, 140, 12),        /* addv %140, %140, zer2 -> (5+0, 3-1) */
		INT(0x20, 142, 14, 0),          /* addv %142, zer3, zero -> (0, 0x10000) */
		LI(0x77, 144, 0x1234), INT(0x05, 144, 144, 0x56),
		INT(0x34, 146, 8, 144),         /* andvs %146, h0msk, %144 */
		HALT
	});
	CHECK(c.reg[140] == 5 && c.reg[141] == 2, "addv constant pair: %u %u", c.reg[140], c.reg[141]);
	CHECK(c.reg[142] == 0 && c.reg[143] == 0x10000, "addv zer3: %08x %08x", c.reg[142], c.reg[143]);
	CHECK(c.reg[146] == 0x12340000u && c.reg[147] == 0x56, "andvs: %08x %08x", c.reg[146], c.reg[147]);

	/* multiply variants */
	RunProgram(c, {
		LI(0x77, 150, 0x0003), /* 3.0 in 16.16 */
		LI(0x77, 151, 0x0002), INT(0x40, 151, 151, 0), /* 2.0 */
		INT(0x1A, 152, 150, 151),       /* mulsr16 -> 6.0 */
		LI(0x76, 153, 0xFFFF), INT(0x40, 153, 153, 0xFF), /* 0x00ffffff */
		INT(0x01, 154, 0, 5),           /* -5 */
		INT(0x18, 155, 154, 154),       /* mul -> 25 */
		HALT
	});
	CHECK(c.reg[152] == 0x60000, "mulsr16: %08x", c.reg[152]);
	CHECK(c.reg[155] == 25, "mul: %d", (int)c.reg[155]);

	/* getpc / jmpl link past the delay slot, jmprl return */
	RunProgram(c, {
		INT(0x6B, 160, 0, 0),           /* 1000: getpc %160 -> 0x1008 */
		0x6E000000u | (0x1020 >> 2),    /* 1004: jmpl 0x1020 */
		LI(0x76, 161, 1),               /* 1008: delay slot */
		LI(0x76, 162, 2),               /* 100c: return lands here */
		HALT,                           /* 1010 */
		NOP, NOP, NOP,
		LI(0x76, 163, 3),               /* 1020 */
		INT(0x6F, 0, 0, 254),           /* jmprl zero, (ra) */
		NOP,
		HALT
	});
	CHECK(c.reg[160] == 0x1008, "getpc link: %08x", c.reg[160]);
	CHECK(c.reg[R_RA] == 0x100C, "jmpl link: %08x", c.reg[R_RA]);
	CHECK(c.reg[161] == 1 && c.reg[162] == 2 && c.reg[163] == 3, "jmpl/jmprl flow");

	/* loads and stores, big-endian byte order, host view through MemEndian */
	RunProgram(c, {
		LI(0x77, 170, 0x1122), INT(0x4B, 170, 170, 0x33), INT(0x00, 170, 170, 0x44), /* 0x11223344 */
		LI(0x76, 171, 0x8000),
		ST(0x7A, 0, 170, 171),          /* sw 0(%171), %170 */
		LD(0x70, 172, 0, 171),          /* lb %172, 0(%171) -> 0x11 */
		LD(0x70, 173, 3, 171),          /* lb %173, 3(%171) -> 0x44 */
		LD(0x71, 174, 1, 171),          /* lh %174, 2(%171) -> 0x3344 */
		LI(0x76, 175, 0xAB), ST(0x78, 1, 175, 171), /* sb 1(%171) */
		LD(0x72, 176, 0, 171),
		HALT
	});
	CHECK(c.reg[172] == 0x11 && c.reg[173] == 0x44 && c.reg[174] == 0x3344, "loads: %x %x %x", c.reg[172], c.reg[173], c.reg[174]);
	CHECK(c.reg[176] == 0x11AB3344u, "sb: %08x", c.reg[176]);
	c.IOWrite(IO_MEMENDIAN, 0, 1);
	CHECK(c.MemRead(0x8000, 4) == 0x11AB3344u, "host word read, no swap: %08x", c.MemRead(0x8000, 4));
	CHECK(c.MemRead(0x8000, 1) == 0x44, "host byte read, no swap: %02x", c.MemRead(0x8000, 1));
	c.IOWrite(IO_MEMENDIAN, 1, 1);
	CHECK(c.MemRead(0x8000, 1) == 0x11, "host byte read, byte swap: %02x", c.MemRead(0x8000, 1));
	CHECK(c.MemRead(0x8000, 4) == 0x4433AB11u, "host word read, byte swap: %08x", c.MemRead(0x8000, 4));
	c.IOWrite(IO_MEMENDIAN, 3, 1);
	CHECK(c.MemRead(0x8000, 2) == 0x11AB, "host half read, half swap: %04x", c.MemRead(0x8000, 2));

	/* FIFO: the RISC stalls on an empty input FIFO and resumes when fed */
	RunProgram(c, {
		INT(0x10, 180, 33, 0),          /* add %180, fifo, zero */
		INT(0x34, 182, 8, 33),          /* andvs %182, h0msk, fifo */
		INT(0x10, 33, 180, 0),          /* add fifo, %180, zero -> output */
		HALT
	});
	CHECK(c.WaitingForInput(), "RISC should stall on the empty FIFO");
	c.IOWrite(IO_FIFO0, 0xCAFEBABEu, 4);
	c.IOWrite(IO_FIFO0, 0x00050007u, 4);
	c.Run(1000);
	CHECK(c.reg[180] == 0xCAFEBABEu, "fifo read: %08x", c.reg[180]);
	CHECK(c.reg[182] == 0x00050000u && c.reg[183] == 7, "andvs fifo: %08x %08x", c.reg[182], c.reg[183]);
	CHECK(c.IORead(IO_FIFOOUTVALID, 1) == 1, "output fifo count");
	CHECK(c.IORead(IO_FIFO0, 4) == 0xCAFEBABEu, "output fifo read");

	/* seed: the reciprocal used by the 2D microcode's line setup (one Newton step) */
	RunProgram(c, {
		LI(0x77, 190, 0x000A),          /* 10.0 in 16.16 */
		INT(0xD1, 0, 190, 1),           /* step q, zero, %190, zer1 */
		INT(0x01, 191, 36, 10),         /* sub %191, scale, #10 */
		INT(0x57, 192, 34, 191),        /* sr %192, seed, %191 */
		INT(0x1A, 193, 190, 192),       /* mulsr16 %193, %190, %192 */
		INT(0x11, 194, 19, 193),        /* sub %194, bit25, %193 */
		INT(0x1B, 195, 194, 192),       /* mulsr24 %195, %194, %192 */
		HALT
	});
	CHECK(c.reg[195] >= 0x199990 && c.reg[195] <= 0x1999A0, "1/10 in 0.24: %08x", c.reg[195]);

	/* f2i */
	RunProgram(c, {
		LI(0x77, 200, 0x4049), INT(0x4B, 200, 200, 0x0F), INT(0x00, 200, 200, 0xDB), /* 3.14159 */
		INT(0x0F, 201, 200, 16),        /* f2i %201, %200, #16 */
		HALT
	});
	CHECK(c.reg[201] == 0x3243F, "f2i: %08x", c.reg[201]);
}

/* ---------------------------------------------------------------------------------- */
/* Part 2: the X.org V1000 2D microcode                                                */

class Host {
public:
	Core &c;
	explicit Host(Core &core) : c(core) {}
	/* every host access gives the RISC a little time, as on real hardware */
	void slice() { c.Run(200); }
	uint8_t in8(unsigned o) { uint8_t v = (uint8_t)c.IORead(o, 1); slice(); return v; }
	uint32_t in32(unsigned o) { uint32_t v = c.IORead(o, 4); slice(); return v; }
	void out8(unsigned o, uint8_t v) { c.IOWrite(o, v, 1); slice(); }
	void out32(unsigned o, uint32_t v) { c.IOWrite(o, v, 4); slice(); }

	/* v1krisc.c */
	void iopoll(unsigned port, uint32_t data, uint32_t mask) {
		for (int i = 0; i <= 100; i++) if ((in32(port) & mask) == (data & mask)) break;
	}
	void forcestep(uint32_t ir) {
		uint8_t debugreg = in8(IO_DEBUG), si = in8(IO_STATEINDEX);
		out8(IO_STATEINDEX, 128);
		out32(IO_STATEDATA, ir);
		iopoll(IO_STATEDATA, ir, 0xFFFFFFFFu);
		out8(IO_DEBUG, debugreg | DEBUG_HOLDRISC | DEBUG_STEPRISC);
		iopoll(IO_STATEDATA, 0, 0);
		for (int i = 0; i < 100; i++) if ((in8(IO_DEBUG) & (DEBUG_HOLDRISC | DEBUG_STEPRISC)) == DEBUG_HOLDRISC) break;
		out8(IO_STATEINDEX, si);
	}
	uint32_t readRF(unsigned index) {
		uint8_t debug = in8(IO_DEBUG), si = in8(IO_STATEINDEX);
		out8(IO_DEBUG, debug | DEBUG_HOLDRISC);
		const uint32_t ir = (0x10u << 24) | index;
		out32(IO_STATEDATA, ir);
		out8(IO_STATEINDEX, 128);
		iopoll(IO_STATEDATA, ir, 0xFFFFFFFFu);
		out8(IO_STATEINDEX, 130);
		const uint32_t v = in32(IO_STATEDATA);
		out8(IO_STATEINDEX, si);
		out8(IO_DEBUG, debug);
		return v;
	}
	void writeRF(unsigned index, uint32_t data) {
		unsigned special = 0;
		if (index < 64) { special = index; index = R_SP; }
		if (!(data & 0xFF000000u)) {
			forcestep(LI(0x76, index, data & 0xFFFF));
			if (data & 0x00FF0000u) forcestep(INT(0x40, index, index, data >> 16));
		}
		else {
			forcestep(LI(0x77, index, data >> 16));
			forcestep(INT(0x4B, index, index, (data >> 8) & 0xFF));
			forcestep(INT(0x00, index, index, data & 0xFF));
		}
		if (special) {
			forcestep(INT(0x10, special, 0, R_SP));
			forcestep(NOP); forcestep(NOP); forcestep(NOP);
		}
	}
	uint32_t readmem(uint32_t addr) {
		writeRF(R_RA, addr);
		forcestep(LD(0x72, R_SP, 0, R_RA));
		forcestep(NOP); forcestep(NOP);
		return readRF(R_SP);
	}
	void writemem(uint32_t addr, uint32_t data) {
		writeRF(R_RA, addr);
		writeRF(R_FP, data);
		forcestep(ST(0x7A, 0, R_FP, R_RA));
	}
	void stop() {
		const uint8_t d = in8(IO_DEBUG);
		out8(IO_DEBUG, d | DEBUG_HOLDRISC);
		iopoll(IO_STATEDATA, 0, 0); iopoll(IO_STATEDATA, 0, 0); iopoll(IO_STATEDATA, 0, 0);
	}
	void cont() {
		const uint8_t d = in8(IO_DEBUG);
		out8(IO_DEBUG, d & ~DEBUG_HOLDRISC);
		iopoll(IO_STATEDATA, 0, 0);
	}
	void start(uint32_t pc) {
		stop();
		forcestep(NOP); forcestep(NOP); forcestep(NOP);
		forcestep(0x6C000000u | (pc >> 2));
		forcestep(NOP);
		cont();
	}
	void softreset() {
		out8(IO_DEBUG, DEBUG_SOFTRESET | DEBUG_HOLDRISC);
		out8(IO_STATEINDEX, 129);
		iopoll(IO_STATEDATA, 0, 0xFFFFFFFFu); iopoll(IO_STATEDATA, 0, 0xFFFFFFFFu); iopoll(IO_STATEDATA, 0, 0xFFFFFFFFu);
		out8(IO_DEBUG, DEBUG_HOLDRISC);
		iopoll(IO_STATEDATA, 0, 0); iopoll(IO_STATEDATA, 0, 0); iopoll(IO_STATEDATA, 0, 0);
		forcestep(LI(0x76, R_RA, 0x0008));
		forcestep(INT(0x40, R_FLAG, R_RA, 0x0002));
		out8(IO_INTR, 0xFF);
		out8(IO_MEMENDIAN, 0);
	}
	void flushicache() {
		const uint32_t p1 = readmem(0), p2 = readmem(8);
		writemem(0, p1); writemem(8, p2);
		(void)readmem(0); (void)readmem(8);
		forcestep(INT(0x4F, 0, 0, 31)); /* spri Sync, zero */
		forcestep(NOP); forcestep(NOP); forcestep(NOP);
		writeRF(R_RA, 0x20008);
		forcestep(INT(0x15, R_FLAG, R_FLAG, R_RA));
		forcestep(NOP); forcestep(NOP); forcestep(NOP);
		for (uint32_t a = 0; a < 2048 * 2; a += 32) forcestep(0x6C000000u | (a >> 2));
		writeRF(R_RA, 0x20008);
		forcestep(INT(0x12, R_FLAG, R_FLAG, R_RA));
		forcestep(NOP);
		forcestep(0x6C000000u);
		forcestep(NOP);
	}
	bool waitfifo(unsigned n) {
		for (int i = 0; i < 0xFFFFF; i++) if ((in8(IO_FIFOINFREE) & 0x1F) >= n) return true;
		return false;
	}
};

/* X.org cscode.h: context switch monitor (generated RISC code, MIT licensed driver) */
static const uint32_t csrisc[] = {
	0x10802100, 0x5d808000, 0x4c808002, 0x6b820000, 0x00818002, 0x45818103, 0x10828281, 0x6f000082,
	0x00000000, 0x62000500, 0x00000000, 0x62000300, 0x00000000, 0x62000800, 0x00000000, 0x10812100,
	0x10822100, 0x10c02100, 0x6ffe00c0, 0x00000000, 0x62ffeb00, 0x00000000, 0x04812502, 0x61fffe81,
	0x00000000, 0x10218000, 0x00000000, 0x00000000, 0x62ffe300, 0x00000000,
};

static bool LoadUcode(Host &h, const std::vector<uint8_t> &f, uint32_t &entry) {
	if (f.size() < 52 || memcmp(f.data(), "\x7f" "ELF", 4) != 0) return false;
	auto be16 = [&](size_t o) { return (uint32_t)((f[o] << 8) | f[o + 1]); };
	auto be32 = [&](size_t o) { return (uint32_t)((f[o] << 24) | (f[o + 1] << 16) | (f[o + 2] << 8) | f[o + 3]); };
	entry = be32(24);
	const uint32_t phoff = be32(28), phentsize = be16(42), phnum = be16(44);
	h.stop();
	const uint8_t memend = h.in8(IO_MEMENDIAN);
	h.out8(IO_MEMENDIAN, 1); /* MEMENDIAN_END: x86 byte addresses = RISC byte addresses */
	for (uint32_t i = 0; i < phnum; i++) {
		const size_t p = phoff + i * phentsize;
		if (be32(p) != 1) continue; /* PT_LOAD */
		const uint32_t off = be32(p + 4), paddr = be32(p + 12), filesz = be32(p + 16);
		for (uint32_t j = 0; j + 4 <= filesz; j += 4) {
			/* vloaduc.c reads the file into memory and writes it as x86 dwords */
			const uint32_t w = f[off + j] | (f[off + j + 1] << 8) | (f[off + j + 2] << 16) | ((uint32_t)f[off + j + 3] << 24);
			h.c.MemWrite(paddr + j, w, 4);
		}
	}
	h.out8(IO_MEMENDIAN, memend);
	return true;
}

static void WritePPM(Core &c, const char *path, uint32_t base, unsigned w, unsigned h, unsigned pitch) {
	FILE *fp = fopen(path, "wb");
	if (!fp) return;
	fprintf(fp, "P6\n%u %u\n255\n", w, h);
	for (unsigned y = 0; y < h; y++)
		for (unsigned x = 0; x < w; x++) {
			const uint32_t argb = ExpandToARGB(c.Read16(base + y * pitch + x * 2), 4, false, false);
			const uint8_t rgb[3] = { (uint8_t)(argb >> 16), (uint8_t)(argb >> 8), (uint8_t)argb };
			fwrite(rgb, 1, 3, fp);
		}
	fclose(fp);
}

static void Test2DMicrocode(const char *ucpath, const std::string &outdir) {
	FILE *fp = fopen(ucpath, "rb");
	if (!fp) {
		printf("SKIP 2D microcode tests: cannot open %s\n", ucpath);
		return;
	}
	std::vector<uint8_t> uc;
	{
		uint8_t buf[4096];
		size_t n;
		while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) uc.insert(uc.end(), buf, buf + n);
		fclose(fp);
	}

	TestBoard b;
	Core c(&b, CHIP_V1000, 4 << 20);
	Host h(c);
	c.IOWrite(IO_MODE, 0, 1); /* native mode */

	const unsigned W = 640, H = 480, BPP = 16, FMT = 4 /* 565 */;
	const uint32_t MC_SIZE = 0x10000, pitch = W * 2;
	const unsigned stride0 = 2, stride1 = 4; /* 640 * 2 bytes = 1280: 1 << 10 + 1 << 8? see below */

	/* verite_initboard */
	h.softreset();
	h.stop();
	h.out8(IO_MEMENDIAN, 0);
	for (unsigned i = 0; i < sizeof(csrisc) / 4; i++) c.MemWrite(0x800 + 4 * i, csrisc[i], 4);
	c.MemWrite(0x7F8, 0, 4);
	c.MemWrite(0x7FC, 0, 4);
	h.flushicache();
	h.start(0x800);
	h.out8(IO_STATEINDEX, 129);
	uint32_t pc = 0;
	for (int i = 0; i < 1000; i++) {
		h.stop();
		pc = h.in32(IO_STATEDATA);
		h.cont();
		if (pc == 0x800) break;
	}
	CHECK(pc == 0x800, "monitor did not reach its wait loop: pc %08x", pc);

	uint32_t entry = 0;
	CHECK(LoadUcode(h, uc, entry), "ucode load");
	CHECK(entry == 0x1000, "ucode entry %08x", entry);

	/* RENDITIONInitUcode */
	h.flushicache();
	h.start(0x800);
	h.out32(IO_FIFO0, 0); h.out32(IO_FIFO0, 0); h.out32(IO_FIFO0, 0); h.out32(IO_FIFO0, entry);
	CHECK(h.waitfifo(6), "fifo after init");
	/* 1280 bytes per line = 1 << 10 (DstStride1 code 5) + 1 << 8 (DstStride0 code 1) */
	(void)stride0; (void)stride1;
	const unsigned s0 = 1, s1 = 5;
	h.out32(IO_FIFO0, 32); /* CMD_SETUP */
	h.out32(IO_FIFO0, (W << 16) | H);
	h.out32(IO_FIFO0, (BPP << 16) | FMT);
	h.out32(IO_FIFO0, MC_SIZE);
	h.out32(IO_FIFO0, pitch);
	h.out32(IO_FIFO0, (s1 << 12) | (s0 << 8));
	c.Run(100000);

	auto px = [&](unsigned x, unsigned y) -> uint16_t { return c.Read16(MC_SIZE + y * pitch + x * 2); };
	auto sync = [&]() {
		/* RENDITIONSyncV1000, first part: CMD_GET_PIXEL returns a value */
		h.waitfifo(2);
		h.out32(IO_FIFO0, 9);
		h.out32(IO_FIFO0, 0);
		int n = 0;
		while (n < 1000 && (h.in8(IO_FIFOOUTVALID) & 7) == 0) n++;
		CHECK((h.in8(IO_FIFOOUTVALID) & 7) > 0, "GET_PIXEL produced no output");
		while (h.in8(IO_FIFOOUTVALID) & 7) (void)h.in32(IO_FIFO0);
	};

	/* solid fill: RENDITIONSubsequentSolidFillRect with GXcopy (ROP_S) */
	const uint32_t red = 0xF800;
	h.waitfifo(4);
	h.out32(IO_FIFO0, (0xCu << 16) | 41); /* CMD_RECT_SOLID_ROP */
	h.out32(IO_FIFO0, red | (red << 16));
	h.out32(IO_FIFO0, (10u << 16) | 20u);  /* x=10 y=20 */
	h.out32(IO_FIFO0, (100u << 16) | 50u); /* w=100 h=50 */
	sync();
	{
		unsigned bad = 0;
		for (unsigned y = 0; y < 100; y++)
			for (unsigned x = 0; x < 140; x++) {
				const bool inside = x >= 10 && x < 110 && y >= 20 && y < 70;
				if (px(x, y) != (inside ? red : 0)) bad++;
			}
		CHECK(bad == 0, "solid fill: %u wrong pixels (pixel 10,20 = %04x, 109,69 = %04x, 110,20 = %04x)",
			bad, px(10, 20), px(109, 69), px(110, 20));
	}

	/* a second fill with XOR (GXxor -> ROP_XOR_SD) over part of the first */
	const uint32_t blue = 0x001F;
	h.waitfifo(4);
	h.out32(IO_FIFO0, (0x6u << 16) | 41);
	h.out32(IO_FIFO0, blue | (blue << 16));
	h.out32(IO_FIFO0, (60u << 16) | 40u);
	h.out32(IO_FIFO0, (100u << 16) | 50u);
	sync();
	CHECK(px(70, 50) == (red ^ blue), "xor fill overlap: %04x", px(70, 50));
	CHECK(px(150, 80) == blue, "xor fill outside: %04x", px(150, 80));
	CHECK(px(20, 30) == red, "xor fill untouched: %04x", px(20, 30));

	/* screen to screen copy: CMD_SCREEN_BLT */
	h.waitfifo(5);
	h.out32(IO_FIFO0, 12);
	h.out32(IO_FIFO0, 0xC);
	h.out32(IO_FIFO0, (0u << 16) | 0u);     /* src 0,0 */
	h.out32(IO_FIFO0, (200u << 16) | 120u); /* 200 x 120 */
	h.out32(IO_FIFO0, (300u << 16) | 200u); /* dst 300,200 */
	sync();
	{
		unsigned bad = 0;
		for (unsigned y = 0; y < 120; y++)
			for (unsigned x = 0; x < 200; x++)
				if (px(300 + x, 200 + y) != px(x, y)) bad++;
		CHECK(bad == 0, "screen blt: %u wrong pixels", bad);
	}

	/* overlapping copy to the right and down (microcode walks right to left, bottom up) */
	h.waitfifo(5);
	h.out32(IO_FIFO0, 12);
	h.out32(IO_FIFO0, 0xC);
	h.out32(IO_FIFO0, (300u << 16) | 200u);
	h.out32(IO_FIFO0, (200u << 16) | 120u);
	h.out32(IO_FIFO0, (310u << 16) | 205u);
	sync();
	{
		unsigned bad = 0;
		for (unsigned y = 0; y < 120; y++)
			for (unsigned x = 0; x < 200; x++)
				if (px(310 + x, 205 + y) != px(x, y)) bad++;
		CHECK(bad == 0, "overlapping screen blt: %u wrong pixels", bad);
	}

	/* line: CMD_LINE_SOLID, from (20,300) to (220,380) */
	const uint32_t green = 0x07E0;
	h.waitfifo(5);
	h.out32(IO_FIFO0, (1u << 16) | 20);
	h.out32(IO_FIFO0, 0xC);
	h.out32(IO_FIFO0, green | (green << 16));
	h.out32(IO_FIFO0, (20u << 16) | 300u);
	h.out32(IO_FIFO0, (220u << 16) | 380u);
	sync();
	{
		unsigned on = 0, near_line = 0;
		for (unsigned y = 290; y < 390; y++)
			for (unsigned x = 10; x < 230; x++)
				if (px(x, y) == green) {
					on++;
					/* distance from the ideal line, in pixels along y */
					const double ly = 300.0 + (x - 20.0) * 80.0 / 200.0;
					if (y + 1.5 >= ly && y <= ly + 1.5) near_line++;
				}
		CHECK(on >= 195 && on <= 205, "line pixel count %u", on);
		CHECK(near_line == on, "line pixels off the line: %u of %u", on - near_line, on);
		CHECK(px(20, 300) == green, "line start pixel");
	}

	/* colour brush fill, as the Windows 98 driver draws tooltips: LoadColorBrush (5) with 8
	 * rows of 4 words (two 565 pixels each), then FillRectColorBrushRop (24): ROP, x/y,
	 * w/h, brush origin. The microcode draws from the brush in memory with SrcMask = 7,7. */
	auto brush = [](unsigned r, unsigned col) -> uint16_t { return (uint16_t)(0x1000u + r * 8u + col); };
	h.waitfifo(1);
	h.out32(IO_FIFO0, 5);
	for (unsigned r = 0; r < 8; r++)
		for (unsigned k = 0; k < 4; k++) {
			h.waitfifo(1);
			h.out32(IO_FIFO0, ((uint32_t)brush(r, 2 * k) << 16) | brush(r, 2 * k + 1));
		}
	const unsigned bx = 400, by = 40, bw = 37, bh = 21, ox = 3, oy = 5;
	h.waitfifo(5);
	h.out32(IO_FIFO0, 24);
	h.out32(IO_FIFO0, 0xC);
	h.out32(IO_FIFO0, (bx << 16) | by);
	h.out32(IO_FIFO0, (bw << 16) | bh);
	h.out32(IO_FIFO0, (ox << 16) | oy);
	sync();
	{
		unsigned bad = 0, outside = 0;
		for (unsigned y = by; y < by + bh; y++)
			for (unsigned x = bx; x < bx + bw; x++)
				if (px(x, y) != brush((y - oy) & 7, (x - ox) & 7)) bad++;
		for (unsigned x = bx; x < bx + bw; x++) if (px(x, by + bh) != 0) outside++;
		CHECK(bad == 0, "colour brush fill: %u wrong pixels (%u,%u = %04x, %u,%u = %04x)", bad,
			bx, by + 10, px(bx, by + 10), bx + 20, by + 10, px(bx + 20, by + 10));
		CHECK(outside == 0, "colour brush fill: %u pixels below the rectangle", outside);
	}

	WritePPM(c, (outdir + "/verite_2d.ppm").c_str(), MC_SIZE, W, H, pitch);
	printf("2D microcode: %llu RISC instructions, %llu pixels, pc %08x\n",
		(unsigned long long)c.instructions, (unsigned long long)c.pixels, c.pc);
}

int main(int argc, char **argv) {
	const char *uc = argc > 1 ? argv[1] : "v10002d.uc";
	const std::string outdir = argc > 2 ? argv[2] : ".";
	TestInstructions();
	Test2DMicrocode(uc, outdir);
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
