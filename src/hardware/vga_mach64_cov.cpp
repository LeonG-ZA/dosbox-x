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
	bool pe = false;             /* 32-bit PE module: data is the image by RVA */
	uint32_t exe_base = 0;       /* an EXE (not a DLL): always at its ImageBase */
};

std::vector<CovSeg> segs;
std::unordered_map<uint64_t, int> by_base;   /* (base, limit) -> segment or -1 */
bool parsed = false;
int watch16_done = 0;   /* reset by PE watch hits */
struct RingEnt { int seg; uint32_t ip; uint64_t n; };
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

/* PE modules: the image laid out by RVA; code pages are recognised by content */
void LoadPE(const std::string &fn) {
	FILE *f = fopen(fn.c_str(), "rb");
	if (!f) { LOG_MSG("COV: cannot open %s", fn.c_str()); return; }
	std::vector<uint8_t> d; uint8_t buf[65536]; size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) d.insert(d.end(), buf, buf + n);
	fclose(f);
	const uint32_t pe = rd32(d, 0x3c);
	if (pe + 0xF8 > d.size() || d[pe] != 'P' || d[pe + 1] != 'E') { LOG_MSG("COV: %s is not PE", fn.c_str()); return; }
	const unsigned int nsec = rd16(d, pe + 6), optsz = rd16(d, pe + 0x14);
	const uint32_t imgsize = rd32(d, pe + 0x18 + 0x38);
	CovSeg s;
	const size_t slash = fn.find_last_of("/\\");
	s.file = (slash == std::string::npos) ? fn : fn.substr(slash + 1);
	s.segnum = 0; s.pe = true;
	if (!(rd16(d, pe + 0x16) & 0x2000)) s.exe_base = rd32(d, pe + 0x18 + 0x1C);
	s.data.assign(imgsize, 0); s.hit.assign(imgsize, 0);
	const uint32_t hdr = rd32(d, pe + 0x18 + 0x3C);
	for (uint32_t i = 0; i < hdr && i < d.size() && i < imgsize; i++) s.data[i] = d[i];
	for (unsigned int i = 0; i < nsec; i++) {
		const uint32_t sh = pe + 0x18 + optsz + i * 40;
		const uint32_t va = rd32(d, sh + 12), rawsz = rd32(d, sh + 16), raw = rd32(d, sh + 20);
		for (uint32_t k = 0; k < rawsz && raw + k < d.size() && va + k < imgsize; k++) s.data[va + k] = d[raw + k];
	}
	segs.push_back(std::move(s));
	LOG_MSG("COV: %s loaded (PE, %x bytes)", fn.c_str(), (unsigned)imgsize);
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
		if (s.pe) continue;
		if ((size_t)limit + 1 < s.data.size() || (size_t)limit + 1 > s.data.size() + 0x1000) continue;
		bool ok = false;
		for (uint32_t w0 : { 0u, 0x200u, 0x800u }) {
			if (w0 + 64 > s.data.size()) continue;
			unsigned int same = 0, total = 0;
			for (uint32_t o = w0; o < w0 + 64; o++) {
				uint32_t p;
				if (!LinToPhys(base + o, p)) return -2;  /* not present yet: try again later */
				total++;
				if (phys_readb(p) == s.data[o]) same++;
			}
			if (total && same * 8 >= total * 7) { ok = true; break; }
		}
		if (ok) return (int)i;
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
		if (q > p) {
			const std::string fn = list.substr(p, q - p);
			FILE *t = fopen(fn.c_str(), "rb"); uint8_t hd[0x40] = {0}; uint32_t pe = 0; uint8_t sig[2] = {0};
			if (t) { if (fread(hd, 1, 0x40, t) == 0x40) { pe = hd[0x3c] | (hd[0x3d] << 8) | (hd[0x3e] << 16) | ((uint32_t)hd[0x3f] << 24); fseek(t, (long)pe, SEEK_SET); if (fread(sig, 1, 2, t) != 2) sig[0] = 0; } fclose(t); }
			if (sig[0] == 'P' && sig[1] == 'E') LoadPE(fn); else LoadNE(fn);
		}
		p = q + 1;
	}
	mach64_cov_on = !segs.empty();
}

static std::unordered_map<uint32_t, int64_t> pe_pages;   /* linear page -> (seg << 32) | rva page, or -1 */

static int64_t IdentifyPEPage(uint32_t lin_page) {
	for (size_t si = 0; si < segs.size(); si++) {
		const CovSeg &s = segs[si];
		if (s.pe && s.exe_base && lin_page >= s.exe_base && lin_page < s.exe_base + s.data.size())
			return ((int64_t)si << 32) | (int64_t)(lin_page - s.exe_base);
	}
	uint8_t cur[64];
	for (unsigned int i = 0; i < 64; i++) {
		uint32_t p;
		if (!LinToPhys(lin_page + i, p)) return -2;
		cur[i] = phys_readb(p);
	}
	for (size_t si = 0; si < segs.size(); si++) {
		const CovSeg &s = segs[si];
		if (!s.pe) continue;
		for (size_t rva = 0x1000; rva + 64 <= s.data.size(); rva += 0x1000) {
			unsigned int same = 0, nz = 0;
			for (unsigned int i = 0; i < 64; i++) { if (s.data[rva + i]) nz++; if (s.data[rva + i] == cur[i]) same++; }
			if (nz >= 16 && same >= 60) return ((int64_t)si << 32) | (int64_t)rva;
		}
	}
	return -1;
}

static void Mach64_CovHookPE(void) {
	const uint32_t lin = (uint32_t)SegPhys(cs) + reg_eip, page = lin & ~0xFFFu;
	static uint32_t last_page = 1; static int64_t last_v = -1;
	int64_t v;
	if (page == last_page) v = last_v;
	else {
		auto it = pe_pages.find(page);
		if (it == pe_pages.end()) {
			v = IdentifyPEPage(page);
			if (v == -2) return;
			pe_pages[page] = v;
			if (v >= 0) LOG_MSG("COV: page %08x = %s rva %x", (unsigned)page, segs[(size_t)(v >> 32)].file.c_str(), (unsigned)(v & 0xFFFFFFFF));
		}
		else v = it->second;
		last_page = page; last_v = v;
	}
	if (v < 0) return;
	const int idx = (int)(v >> 32);
	const uint32_t rva = (uint32_t)(v & 0xFFFFFFFF) + (lin & 0xFFF);
	if (rva < segs[idx].hit.size()) segs[idx].hit[rva] = 1;
	{   /* DOSBOX_X_COVWATCHPE="FILE:rva[,rva...]" logs registers and the stack at those instructions (first 8 times each) */
		static bool parsed = false; static std::string wfile; static std::vector<uint32_t> wr; static std::unordered_map<uint32_t, int> cnt;
		if (!parsed) {
			parsed = true;
			const char *w = getenv("DOSBOX_X_COVWATCHPE");
			if (w) { std::string t(w); size_t c = t.rfind(':'); if (c != std::string::npos) { wfile = t.substr(0, c); std::string r = t.substr(c + 1); size_t p0 = 0; while (p0 < r.size()) { size_t q = r.find(',', p0); if (q == std::string::npos) q = r.size(); wr.push_back((uint32_t)strtoul(r.substr(p0, q - p0).c_str(), NULL, 16)); p0 = q + 1; } } }
			LOG_MSG("COV: WATCHPE armed file=%s count=%u", wfile.c_str(), (unsigned)wr.size());
		}
		if (!wr.empty() && wfile.size() >= segs[idx].file.size() && wfile.compare(wfile.size() - segs[idx].file.size(), std::string::npos, segs[idx].file) == 0) for (uint32_t x : wr) if (x == rva && cnt[x]++ < 8) {
			watch16_done = 0;
			const uint32_t sp = (uint32_t)SegPhys(ss) + reg_esp; uint32_t st[8];
			for (int k = 0; k < 8; k++) { st[k] = 0; for (int b = 0; b < 4; b++) { uint32_t ph; if (LinToPhys(sp + (uint32_t)k * 4 + (uint32_t)b, ph)) st[k] |= (uint32_t)phys_readb(ph) << (8 * b); } }
			LOG_MSG("COV WATCHPE %s+%x eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x stack %08x %08x %08x %08x %08x %08x %08x %08x",
				wfile.c_str(), (unsigned)rva, (unsigned)reg_eax, (unsigned)reg_ebx, (unsigned)reg_ecx, (unsigned)reg_edx, (unsigned)reg_esi, (unsigned)reg_edi,
				st[0], st[1], st[2], st[3], st[4], st[5], st[6], st[7]);
			{   /* DOSBOX_X_COVPEEK="addr:off" also logs [[addr]+off] (e.g. MACXDD32's frame buffer base) */
				const char *pk = getenv("DOSBOX_X_COVPEEK");
				if (pk) {
					const uint32_t a = (uint32_t)strtoul(pk, NULL, 16); const char *c = strchr(pk, ':'); const uint32_t o = c ? (uint32_t)strtoul(c + 1, NULL, 16) : 0;
					auto rd = [](uint32_t l, uint32_t &v) { v = 0; for (int b = 0; b < 4; b++) { uint32_t ph; if (!LinToPhys(l + (uint32_t)b, ph)) return false; v |= (uint32_t)phys_readb(ph) << (8 * b); } return true; };
					uint32_t p1 = 0, p2 = 0; const bool ok = rd(a, p1) && rd(p1 + o, p2);
					LOG_MSG("COV WATCHPE   peek [%08x]=%08x [+%x]=%08x%s", (unsigned)a, (unsigned)p1, (unsigned)o, (unsigned)p2, ok ? "" : " (unmapped)");
				}
			}
			for (int k = 1; k <= 3; k++) {
				char str[160]; int n = 0;
				for (; n < 150; n++) { uint32_t ph; if (!LinToPhys(st[k] + (uint32_t)n, ph)) break; const uint8_t c = phys_readb(ph); if (!c) break; str[n] = (c >= 32 && c < 127) ? (char)c : '.'; }
				str[n] = 0;
				LOG_MSG("COV WATCHPE   string arg %d: \"%s\"", k, str);
			}
		}
	}
	ring_count++;
	RingEnt &last = ring[(ring_pos + 255) & 255];
	if (last.seg == idx && last.ip == rva) return;
	ring[ring_pos] = { idx, rva, ring_count };
	ring_pos = (ring_pos + 1) & 255;
}

void Mach64_CovHook(void) {
	if (!cpu.pmode || (reg_flags & FLAG_VM)) return;
	if (cpu.code.big) { Mach64_CovHookPE(); return; }
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
		static bool wparsed = false; static std::vector<std::pair<int, unsigned int>> wl; int &done = watch16_done;
		if (!wparsed) { wparsed = true; const char *w = getenv("DOSBOX_X_COVWATCH"); std::string t = w ? w : ""; size_t p0 = 0;
			while (p0 < t.size()) { size_t q = t.find(',', p0); if (q == std::string::npos) q = t.size(); std::string it = t.substr(p0, q - p0); int hal = 0; if (!it.empty() && it[0] == 'H') { hal = 1000; it = it.substr(1); }
				unsigned int a, b; if (sscanf(it.c_str(), "%u:%x", &a, &b) == 2) wl.push_back({ (int)a + hal, b }); p0 = q + 1; } }
		bool wmatch = false;
		for (auto &x : wl) {
			const bool want_hal = x.first >= 1000;
			if ((x.first % 1000) == (int)segs[idx].segnum && x.second == ip && segs[idx].file.find(want_hal ? "HAL" : "DRV") != std::string::npos) wmatch = true;
		}
		if (done < 400 && wmatch) {
			done++;
			{
				char mm[80]; size_t mn = 0;
				for (int k = 0; k < 24; k++) { uint32_t ph; uint8_t c = 0; if (LinToPhys((uint32_t)SegPhys(es) + (uint16_t)(reg_bx + k), ph)) c = phys_readb(ph); mn += (size_t)snprintf(mm + mn, sizeof(mm) - mn, "%02x", c); }
				char bpw[80]; size_t bn2 = 0;
				for (int k = 0; k < 8; k++) { uint16_t w = 0; for (int q = 0; q < 2; q++) { uint32_t ph; if (LinToPhys((uint32_t)SegPhys(ss) + (uint16_t)(reg_bp + 4 + k * 2 + q), ph)) w |= (uint16_t)(phys_readb(ph) << (8 * q)); } bn2 += (size_t)snprintf(bpw + bn2, sizeof(bpw) - bn2, " %04x", w); }
				char spw[80]; size_t sn2 = 0;
				for (int k = 0; k < 8; k++) { uint16_t w = 0; for (int q = 0; q < 2; q++) { uint32_t ph; if (LinToPhys((uint32_t)SegPhys(ss) + (uint16_t)(reg_sp + k * 2 + q), ph)) w |= (uint16_t)(phys_readb(ph) << (8 * q)); } sn2 += (size_t)snprintf(spw + sn2, sizeof(spw) - sn2, " %04x", w); }
				LOG_MSG("COV WATCH   dx=%04x [sp..]:%s", (unsigned)reg_dx, spw);
				LOG_MSG("COV WATCH %s %u:%04x ax=%04x es:bx=%04x:%04x bytes %s [bp+4..]:%s", segs[idx].file.c_str(), segs[idx].segnum, ip, (unsigned)reg_ax, (unsigned)SegValue(es), (unsigned)reg_bx, mm, bpw);
			}
			uint32_t pds = 0; const bool okp = LinToPhys((uint32_t)SegPhys(ds) + 0xF10, pds);
			LOG_MSG("COV WATCH %s seg %u:%04x ds=%04x base %08x lim %x (ds:F10 -> phys %08x %s) es=%04x base %08x fs=%04x base %08x eax=%08x ebx=%08x esi=%08x",
				segs[idx].file.c_str(), segs[idx].segnum, ip, (unsigned)SegValue(ds), (unsigned)SegPhys(ds), (unsigned)Segs.limit[ds], (unsigned)pds, okp ? "" : "not present",
				(unsigned)SegValue(es), (unsigned)SegPhys(es), (unsigned)SegValue(fs), (unsigned)SegPhys(fs), (unsigned)reg_eax, (unsigned)reg_ebx, (unsigned)reg_esi);
		}
	}
	ring_count++;
	/* keep only the latest visit of a (seg, ip) run so loops show up once per turn */
	RingEnt &last = ring[(ring_pos + 255) & 255];
	if (last.seg == idx && last.ip == ip) return;
	ring[ring_pos] = { idx, ip, ring_count };
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
