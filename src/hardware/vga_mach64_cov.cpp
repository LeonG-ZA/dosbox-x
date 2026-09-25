/*
 *  TEST HOOK (not for upstream): code coverage of 16-bit protected mode
 *  segments of given NE modules (Windows drivers), for debugging guest
 *  driver behaviour on the emulated ATI mach64 GT.
 *
 *  DOSBOX_X_COV="file1.drv;file2.dll"  NE files whose segments are tracked
 *  DOSBOX_X_COVOUT="path"              coverage written to path.N on each dump
 *
 *  Needs core=normal. A code segment is recognised by its limit (length - 1)
 *  and its first bytes compared with the file.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <unordered_map>

#include "dosbox.h"
#include "logging.h"
#include "mem.h"
#include "paging.h"
#include "regs.h"
#include "cpu.h"

bool mach64_cov_on = false;

namespace {

struct CovSeg {
	std::string file;
	unsigned int segnum;
	std::vector<uint8_t> data;   /* file bytes of the segment */
	std::vector<uint8_t> hit;
};

std::vector<CovSeg> segs;
std::unordered_map<uint64_t, int> by_base;   /* (base, limit) -> segment or -1 */
bool parsed = false;
struct RingEnt { int seg; uint16_t ip; uint64_t n; };
RingEnt ring[256];
unsigned int ring_pos = 0;
uint64_t ring_count = 0;

uint16_t rd16(const std::vector<uint8_t> &d, size_t o) { return (uint16_t)(d[o] | (d[o + 1] << 8)); }
uint32_t rd32(const std::vector<uint8_t> &d, size_t o) { return (uint32_t)rd16(d, o) | ((uint32_t)rd16(d, o + 2) << 16); }

void LoadNE(const std::string &fn) {
	FILE *f = fopen(fn.c_str(), "rb");
	if (!f) { LOG_MSG("COV: cannot open %s", fn.c_str()); return; }
	std::vector<uint8_t> d;
	uint8_t buf[65536]; size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) d.insert(d.end(), buf, buf + n);
	fclose(f);
	if (d.size() < 0x40) return;
	const uint32_t ne = rd32(d, 0x3c);
	if (ne + 0x40 > d.size() || d[ne] != 'N' || d[ne + 1] != 'E') { LOG_MSG("COV: %s is not NE", fn.c_str()); return; }
	const unsigned int nseg = rd16(d, ne + 0x1c);
	const uint32_t segtab = ne + rd16(d, ne + 0x22);
	const unsigned int shift = rd16(d, ne + 0x32);
	for (unsigned int i = 0; i < nseg; i++) {
		const uint32_t off = (uint32_t)rd16(d, segtab + i * 8) << shift;
		const uint32_t len = rd16(d, segtab + i * 8 + 2) ? rd16(d, segtab + i * 8 + 2) : 0x10000;
		const uint16_t flags = rd16(d, segtab + i * 8 + 4);
		if (flags & 1) continue;                 /* data segment */
		if (off == 0 || off + len > d.size() || len < 32) continue;
		CovSeg s;
		const size_t slash = fn.find_last_of("/\\");
		s.file = (slash == std::string::npos) ? fn : fn.substr(slash + 1);
		s.segnum = i + 1;
		s.data.assign(d.begin() + off, d.begin() + off + len);
		s.hit.assign(len, 0);
		segs.push_back(std::move(s));
	}
	LOG_MSG("COV: %s loaded", fn.c_str());
}

bool LinToPhys(uint32_t lin, uint32_t &phys) {
	if (!paging.enabled) { phys = lin; return true; }
	const uint32_t pde = phys_readd((paging.cr3 & 0xFFFFF000u) + ((lin >> 22) << 2));
	if (!(pde & 1)) return false;
	if (pde & 0x80) { phys = (pde & 0xFFC00000u) | (lin & 0x3FFFFFu); return true; }
	const uint32_t pte = phys_readd((pde & 0xFFFFF000u) + (((lin >> 12) & 0x3FF) << 2));
	if (!(pte & 1)) return false;
	phys = (pte & 0xFFFFF000u) | (lin & 0xFFFu);
	return true;
}

int Identify(uint32_t base, uint32_t limit) {
	for (size_t i = 0; i < segs.size(); i++) {
		const CovSeg &s = segs[i];
		if ((size_t)limit + 1 < s.data.size() || (size_t)limit + 1 > s.data.size() + 0x1000) continue;
		unsigned int same = 0, total = 0;
		for (uint32_t o = 0; o < 64 && o < s.data.size(); o++) {
			uint32_t p;
			if (!LinToPhys(base + o, p)) return -2;  /* not present yet: try again later */
			total++;
			if (phys_readb(p) == s.data[o]) same++;
		}
		if (total && same * 8 >= total * 7) return (int)i;
	}
	return -1;
}

} // namespace

void Mach64_CovInit(void) {
	if (parsed) return;
	parsed = true;
	const char *e = getenv("DOSBOX_X_COV");
	if (!e || !*e) return;
	std::string list(e);
	size_t p = 0;
	while (p <= list.size()) {
		size_t q = list.find(';', p);
		if (q == std::string::npos) q = list.size();
		if (q > p) LoadNE(list.substr(p, q - p));
		p = q + 1;
	}
	mach64_cov_on = !segs.empty();
}

void Mach64_CovHook(void) {
	if (!cpu.pmode || (reg_flags & FLAG_VM) || cpu.code.big) return;
	const uint32_t base = (uint32_t)SegPhys(cs), limit = (uint32_t)Segs.limit[cs];
	const uint64_t key = ((uint64_t)base << 32) | limit;
	auto it = by_base.find(key);
	int idx;
	if (it == by_base.end()) {
		idx = Identify(base, limit);
		if (idx == -2) return;
		by_base[key] = idx;
		if (idx >= 0) LOG_MSG("COV: cs=%04x base %08x = %s seg %u", (unsigned)SegValue(cs), (unsigned)base, segs[idx].file.c_str(), segs[idx].segnum);
	}
	else idx = it->second;
	if (idx < 0) return;
	const uint32_t ip = reg_eip & 0xFFFF;
	if (ip < segs[idx].hit.size()) segs[idx].hit[ip] = 1;
	{   /* one-shot register dump at DOSBOX_X_COVWATCH="segnum:ip" (hex ip) */
		static int wseg = -2; static unsigned int wip = 0; static bool done = false;
		if (wseg == -2) { const char *w = getenv("DOSBOX_X_COVWATCH"); wseg = -1; if (w) { unsigned int a, b; if (sscanf(w, "%u:%x", &a, &b) == 2) { wseg = (int)a; wip = b; } } }
		if (!done && (int)segs[idx].segnum == wseg && ip == wip) {
			done = true;
			uint32_t pds = 0; const bool okp = LinToPhys((uint32_t)SegPhys(ds) + 0xF10, pds);
			LOG_MSG("COV WATCH %s seg %u:%04x ds=%04x base %08x lim %x (ds:F10 -> phys %08x %s) es=%04x base %08x fs=%04x base %08x eax=%08x ebx=%08x esi=%08x",
				segs[idx].file.c_str(), segs[idx].segnum, ip, (unsigned)SegValue(ds), (unsigned)SegPhys(ds), (unsigned)Segs.limit[ds], (unsigned)pds, okp ? "" : "not present",
				(unsigned)SegValue(es), (unsigned)SegPhys(es), (unsigned)SegValue(fs), (unsigned)SegPhys(fs), (unsigned)reg_eax, (unsigned)reg_ebx, (unsigned)reg_esi);
		}
	}
	ring_count++;
	/* keep only the latest visit of a (seg, ip) run so loops show up once per turn */
	RingEnt &last = ring[(ring_pos + 255) & 255];
	if (last.seg == idx && last.ip == (uint16_t)ip) return;
	ring[ring_pos] = { idx, (uint16_t)ip, ring_count };
	ring_pos = (ring_pos + 1) & 255;
}

void Mach64_CovDump(const char *why) {
	if (!mach64_cov_on) return;
	const char *out = getenv("DOSBOX_X_COVOUT");
	if (!out) return;
	static unsigned int n = 0;
	char fn[1024];
	snprintf(fn, sizeof(fn), "%s.%u", out, n++);
	FILE *f = fopen(fn, "w");
	if (!f) return;
	fprintf(f, "# %s\n", why);
	for (const CovSeg &s : segs) {
		bool any = false;
		for (size_t o = 0; o < s.hit.size(); o++) {
			if (!s.hit[o]) continue;
			if (!any) { fprintf(f, "%s seg %u:", s.file.c_str(), s.segnum); any = true; }
			size_t e = o;
			while (e + 1 < s.hit.size() && s.hit[e + 1]) e++;
			fprintf(f, " %04x", (unsigned)o);
			if (e != o) fprintf(f, "-%04x", (unsigned)e);
			o = e;
		}
		if (any) fprintf(f, "\n");
	}
	fprintf(f, "# last instructions in tracked segments (oldest first), total %llu\n", (unsigned long long)ring_count);
	for (unsigned int i = 0; i < 256; i++) {
		const RingEnt &r = ring[(ring_pos + i) & 255];
		if (r.n == 0) continue;
		fprintf(f, "RING %s seg %u %04x @%llu\n", segs[r.seg].file.c_str(), segs[r.seg].segnum, (unsigned)r.ip, (unsigned long long)r.n);
	}
	fclose(f);
	LOG_MSG("COV: dumped %s (%s)", fn, why);
}
