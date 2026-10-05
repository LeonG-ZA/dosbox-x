/*
 *  PIX 1000 processor card model - see pixboard.h.
 */
#include "pixboard.h"

#include <string.h>

/* ---- bus glue -------------------------------------------------------------------------------- */

static inline uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static inline void wbe32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

/* returns a host pointer for plain memory, or NULL for devices */
static inline uint8_t *mem_ptr(PixBoard *b, uint32_t pa) {
    const uint32_t top = pa >> 28;
    if (top == 0x0 || top == 0x1) return &b->dram[pa & (PixBoard::DRAM_SIZE - 1)];
    if ((pa & 0xFFC00000u) == 0x40000000u) return &b->vram[pa & (PixBoard::VRAM_SIZE - 1)];
    return NULL;
}

uint8_t PixBus::rd8(uint32_t pa) { uint8_t *p = mem_ptr(board, pa); return p ? *p : (uint8_t)board->dev_read(which, pa, 1); }
uint16_t PixBus::rd16(uint32_t pa) { uint8_t *p = mem_ptr(board, pa); return p ? (uint16_t)((p[0] << 8) | p[1]) : (uint16_t)board->dev_read(which, pa, 2); }
uint32_t PixBus::rd32(uint32_t pa) { uint8_t *p = mem_ptr(board, pa); return p ? be32(p) : board->dev_read(which, pa, 4); }
void PixBus::wr8(uint32_t pa, uint8_t v) { uint8_t *p = mem_ptr(board, pa); if (p) *p = v; else board->dev_write(which, pa, v, 1); }
void PixBus::wr16(uint32_t pa, uint16_t v) { uint8_t *p = mem_ptr(board, pa); if (p) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; } else board->dev_write(which, pa, v, 2); }
void PixBus::wr32(uint32_t pa, uint32_t v) {
    uint8_t *p = mem_ptr(board, pa);
    if (p) {
        wbe32(p, v);

    } else board->dev_write(which, pa, v, 4);
}
uint32_t PixBus::fetch(uint32_t pa) { return rd32(pa); }

/* ---- board ----------------------------------------------------------------------------------- */

PixBoard::PixBoard() : bus_a(this, 0), bus_b(this, 1) {
    memset(sam, 0, sizeof(sam));
    dram.assign(DRAM_SIZE, 0);
    vram.assign(VRAM_SIZE, 0);
    for (PixBus *bus : {&bus_a, &bus_b}) {
        for (unsigned i = 0; i < DRAM_SIZE >> 22; i++) {
            bus->fast[i] = &dram[(size_t)i << 22];
            bus->fast[(0x10000000u >> 22) + i] = &dram[(size_t)i << 22];   /* uncached alias */
        }
        bus->fast[0x40000000u >> 22] = &vram[0];
    }
    cpu_a = new M88110(&bus_a);
    cpu_b = new M88110(&bus_b);
}

PixBoard::~PixBoard() {
    delete cpu_a;
    delete cpu_b;
}

void PixBoard::host_write8(uint32_t a, uint8_t v) { uint8_t *p = mem_ptr(this, a); if (p) *p = v; }
void PixBoard::host_write16(uint32_t a, uint16_t v) { uint8_t *p = mem_ptr(this, a & ~1u); if (p) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; } }
uint8_t PixBoard::host_read8(uint32_t a) { uint8_t *p = mem_ptr(this, a); return p ? *p : 0xFF; }
uint16_t PixBoard::host_read16(uint32_t a) { uint8_t *p = mem_ptr(this, a & ~1u); return p ? (uint16_t)((p[0] << 8) | p[1]) : 0xFFFF; }

void PixBoard::run_cpu(bool a) {
    /* The CPU fetches its reset branch from address 0 as it is at this moment. */
    M88110 *c = a ? cpu_a : cpu_b;
    const uint32_t w0 = be32(&dram[0]);
    c->reset((w0 >> 26) == 0x30 ? (uint32_t)((int32_t)((w0 & 0x03FFFFFFu) << 6) >> 4) : 0);
    if (a) a_on = true;
    else { b_on = true; find_b_entry_points(); }
}

void PixBoard::start(bool a, bool b) {
    if (b && !b_on) run_cpu(false);
    if (a && !a_on) run_cpu(true);
}

void PixBoard::stop() { a_on = b_on = false; }

/* Locate draw32B/draw16B and the idle poll loop in MAINB without symbols:
 *   mainB loads r27 = draw32B (or.u r27,r0,HI / or r27,r27,LO), conditionally r27 = draw16B, then jsr r27;
 *   the idle loop is  ld r28,r30,$100 / cmp r29,r28,r0 / bb1 2,r29,-1.  (SFL MAINB 0xaa78-0xaad0) */
void PixBoard::find_b_entry_points() {
    const uint32_t w0 = be32(&dram[0]);
    uint32_t mainb = 0;
    if ((w0 >> 26) == 0x30) mainb = (uint32_t)((int32_t)((w0 & 0x03FFFFFFu) << 6) >> 4);
    draw32_addr = draw16_addr = mainb_poll_pc = 0;
    uint32_t hi[32] = {0};
    for (uint32_t pc = mainb; pc < mainb + 0x1000 && pc + 8 < DRAM_SIZE; pc += 4) {
        const uint32_t w = be32(&dram[pc]);
        const unsigned op = w >> 26, d = (w >> 21) & 31, s1 = (w >> 16) & 31;
        if (op == 0x17 && s1 == 0) hi[d] = (w & 0xFFFFu) << 16;
        else if (op == 0x16 && d == 27 && s1 == 27) {
            const uint32_t t = hi[27] | (w & 0xFFFFu);
            if (!draw32_addr) draw32_addr = t; else if (!draw16_addr) draw16_addr = t;
        }
        if (w == 0x179E0100u && be32(&dram[pc + 4]) == 0xF7BC7C00u && be32(&dram[pc + 8]) == 0xD85DFFFEu && !mainb_poll_pc)
            mainb_poll_pc = pc;
    }
    disp32 = draw32_addr ? draw32_addr + 0x2C : 0;   /* ld.d r4,r28,$0: record dispatch head */
    disp16 = draw16_addr ? draw16_addr + 0x2C : 0;
    if (disp32 && be32(&dram[disp32]) != 0x109C0000u) disp32 = 0;
    if (disp16 && be32(&dram[disp16]) != 0x109C0000u) disp16 = 0;
    cpu_b->bp[0] = disp32 ? disp32 : 0xFFFFFFFFu;
    cpu_b->bp[1] = disp16 ? disp16 : 0xFFFFFFFFu;
    cpu_b->bp[2] = mainb_poll_pc ? mainb_poll_pc : 0xFFFFFFFFu;
    fprintf(stderr, "PIX: dispatch heads %#x %#x\n", disp32, disp16);
    fprintf(stderr, "PIX: mainB %#x draw32B %#x draw16B %#x poll %#x\n", mainb, draw32_addr, draw16_addr, mainb_poll_pc);
}

/* VRAM row transfers (MAINA swapBuffers/clear code, e.g. DN2 0x4d2d0-0x4d380):
 *   read  0x48xxxxxx / 0x49xxxxxx : read transfer - load the 8 KB VRAM row containing xxxxxx into the SAM
 *   write 0x4Fxxxxxx              : write transfer - store the SAM into that row, bits enabled by the value
 * [inferred from usage: clears load one blank row and write it to every row of a buffer] */
bool PixBoard::vram_transfer(uint32_t pa, bool write, uint32_t v) {
    const uint32_t hi = pa >> 24;
    const uint32_t row = pa & (VRAM_SIZE - 1) & ~0x1FFFu;
    if (!write && (hi == 0x48 || hi == 0x49)) { memcpy(sam, &vram[row], sizeof(sam)); return true; }
    if (write && hi == 0x4F) {
        if (v == 0xFFFFFFFFu) memcpy(&vram[row], sam, sizeof(sam));
        else {
            const uint8_t m[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
            for (unsigned i = 0; i < sizeof(sam); i++) vram[row + i] = (uint8_t)((vram[row + i] & ~m[i & 3]) | (sam[i] & m[i & 3]));
        }
        return true;
    }
    return false;
}

uint32_t PixBoard::dev_read(int cpu, uint32_t pa, unsigned size) {
    if (vram_transfer(pa, false, 0)) return 0;
    if ((pa & ~1u) == 0x20000006u || pa == 0x20000004u) {
        uint16_t w;
        if (fifo_pop(w)) { last_fifo = w; fifo_pops++; if (cpu == 0) a_waiting_fifo = false; }
        return size == 4 ? ((uint32_t)last_fifo << 16) : last_fifo;   /* only 16-bit reads seen */
    }
    if (pa == 0x30000000u) {
        /* The video timing generator runs independently of the host, so it is clocked from CPU A's own
         * instruction count; waitVBI needs to see every line value (it tests for equality). */
        const uint32_t line = (uint32_t)((clock / insns_per_line) % lines_per_frame);
        uint32_t v = ((uint32_t)(card_id & 15u) << 25) | ((line & 0x3FFu) << 1);
        if (fifo_level()) { v |= 1u << 29; if (cpu == 0) a_waiting_fifo = false; }
        else if (cpu == 0) {
            /* blocked only if this is the FIFO poll loop: next instruction is bb0 29,rX (waitWordFIFO) */
            const uint32_t next = be32(&dram[cpu_a->pc & (DRAM_SIZE - 1)]);
            a_waiting_fifo = (next & 0xFFE00000u) == 0xD3A00000u;
        }
        return v;
    }
    if ((pa & ~1u) == 0x30000006u) return ctrl;
    unknown_io++;
    if (unknown_log++ < 40) fprintf(stderr, "PIX: cpu %c read%u %08x\n", cpu ? 'B' : 'A', size * 8, pa);
    return 0;
}

void PixBoard::dev_write(int cpu, uint32_t pa, uint32_t v, unsigned size) {
    if ((pa & ~1u) == 0x30000006u) { ctrl = (uint16_t)v; return; }
    if (vram_transfer(pa, true, v)) return;
    unknown_io++;
    if (unknown_log++ < 40) fprintf(stderr, "PIX: cpu %c write%u %08x = %x\n", cpu ? 'B' : 'A', size * 8, pa, v);
}

uint64_t PixBoard::run(uint64_t n) {
    /* Interleave CPU A and CPU B in slices. A does not run while it is blocked on an empty FIFO;
     * B does not run while it polls an empty draw-list slot. Returns instructions executed by both;
     * sets idle when neither CPU has work. */
    uint64_t done = 0;
    const uint64_t slice = 2000;
    idle = false;
    while (done < n) {
        bool a_ran = false, b_ran = false;
        if (a_on && !(a_waiting_fifo && !fifo_level())) {
            a_waiting_fifo = false;
            const uint64_t k = cpu_a->run(slice);
            done += k;
            clock += k;
            a_ran = true;
            const uint32_t disp = be32(&dram[0x200C]);
            if (disp != display_base) { display_base = disp; grab_frame(); }
            if (cpu_a->unknown_count) {
                fprintf(stderr, "PIX: A unknown opcode %08x at %08x\n", cpu_a->last_unknown_inst, cpu_a->last_unknown_pc);
                cpu_a->unknown_count = 0;
            }
        }
        if (b_on) {
            const uint64_t b0 = cpu_b->icount;
            uint64_t left = slice;
            while (left) {
                const uint32_t bpc = cpu_b->pc;
                if (bpc == mainb_poll_pc && mainb_poll_pc && be32(&dram[0x7100]) == 0) break;   /* idle */
                if (bpc && (bpc == disp32 || bpc == disp16)) {
                    /* close the previous record's statistics */
                    if (cur_type >= 0) {
                        TypeStat &t = type_stats[cur_type & 255];
                        t.insns += cpu_b->icount - cur_insn0;
                        t.bytes += cpu_b->r[28] - cur_ptr;
                    }
                    const uint32_t ptr = cpu_b->r[28];
                    const int type = (int8_t)dram[ptr & (DRAM_SIZE - 1)];
                    cur_type = type; cur_insn0 = cpu_b->icount; cur_ptr = ptr;
                    type_stats[type & 255].count++;
                    if (type == 0) cur_type = -1;
                    else if (hle_b) {
                        extern bool PixRaster_Record(PixBoard *b, M88110 *cpu, int type, bool bpp32);
                        if (compare_every && (type_stats[type & 255].count % compare_every) == 1) {
                            /* run the HLE on a copy, then the original handler, and diff VRAM */
                            std::vector<uint8_t> before(vram);
                            uint32_t regs[32];
                            memcpy(regs, cpu_b->r, sizeof(regs));
                            if (PixRaster_Record(this, cpu_b, type, bpc == disp32)) {
                                std::vector<uint8_t> hle(vram);
                                const uint32_t hle_r28 = cpu_b->r[28];
                                vram.swap(before);
                                memcpy(cpu_b->r, regs, sizeof(regs));
                                cpu_b->pc = bpc;
                                const uint64_t i0 = cpu_b->icount;
                                do { cpu_b->run(1u << 30); } while (cpu_b->pc != disp32 && cpu_b->pc != disp16 && cpu_b->pc != mainb_poll_pc);
                                size_t diff = 0, first = 0;
                                for (size_t i = 0; i < vram.size(); i++) if (vram[i] != hle[i]) { if (!diff) first = i; diff++; }
                                compare_runs++;
                                if (diff || hle_r28 != cpu_b->r[28]) {
                                    compare_fail++;
                                    if (compare_fail < 20)
                                        fprintf(stderr, "PIX compare: type %#x rec %08x: %zu bytes differ (first %#zx hle %02x lle %02x), r28 hle %08x lle %08x, lle %llu insns\n",
                                                type & 255, regs[28], diff, first, hle[first], vram[first], hle_r28, cpu_b->r[28],
                                                (unsigned long long)(cpu_b->icount - i0));
                                }
                                cur_type = -1;
                                continue;
                            }
                        }
                        if (PixRaster_Record(this, cpu_b, type, bpc == disp32)) {
                            type_stats[type & 255].hle++;
                            cur_type = -1;
                            continue;          /* record consumed; B stays at the dispatch head */
                        }
                    }
                }
                const uint64_t k = cpu_b->run(left);
                left -= k < left ? k : left;
                if (!k) break;
            }
            if (cpu_b->unknown_count) {
                fprintf(stderr, "PIX: B unknown opcode %08x at %08x\n", cpu_b->last_unknown_inst, cpu_b->last_unknown_pc);
                cpu_b->unknown_count = 0;
            }
            done += cpu_b->icount - b0;
            b_ran = cpu_b->icount != b0;
        }
        if (!a_ran && !b_ran) { idle = true; break; }
    }
    return done;
}

void PixBoard::hle_draw(bool bpp32) { (void)bpp32; }

bool PixBoard::render_frame(PixFrame &out) {
    const uint32_t stride = be32(&dram[0x7040]);
    const uint32_t rows = be32(&dram[0x7050]);
    if ((display_base & 0xFFC00000u) != 0x40000000u || stride == 0 || stride > 8192 || rows == 0 || rows > 512) return false;
    const bool px16 = (dram[0x2106] >> 1) & 1;
    const unsigned bpp = px16 ? 2 : 4;
    out.width = stride / bpp;
    out.height = rows * 0x2000u / stride;
    if (out.height > 1024) out.height = 1024;
    out.pixels.resize((size_t)out.width * out.height);
    for (unsigned y = 0; y < out.height; y++) {
        const uint32_t row = (display_base + y * stride) & (VRAM_SIZE - 1);
        uint32_t *dst = &out.pixels[(size_t)y * out.width];
        for (unsigned x = 0; x < out.width; x++) {
            const uint8_t *p = &vram[(row + x * bpp) & (VRAM_SIZE - 1)];
            if (px16) {
                const unsigned v = ((unsigned)p[0] << 8) | p[1];
                dst[x] = (((v >> 8) & 15u) * 17u << 16) | (((v >> 4) & 15u) * 17u << 8) | ((v & 15u) * 17u);
            } else dst[x] = ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
        }
    }
    return true;
}

void PixBoard::grab_frame() {
    if (frame_cb) frame_cb(this, frame_user);
}

void PixBoard::report(FILE *f) {
    fprintf(f, "PIX board: A pc=%08x (%llu insns)  B pc=%08x (%llu insns)  fifo pops %llu  draws %llu  unknown io %llu  fifo left %zu\n",
            cpu_a->pc, (unsigned long long)cpu_a->icount, cpu_b->pc, (unsigned long long)cpu_b->icount,
            (unsigned long long)fifo_pops, (unsigned long long)draws, (unsigned long long)unknown_io, fifo_level());
    fprintf(f, "HLE compare: %llu checked, %llu mismatched\n", (unsigned long long)compare_runs, (unsigned long long)compare_fail);
    fprintf(f, "draw-list records: type count avg_bytes avg_B_insns hle\n");
    for (int t = -128; t < 128; t++) {
        const TypeStat &s = type_stats[t & 255];
        if (!s.count) continue;
        const uint64_t lle = s.count - s.hle;
        fprintf(f, "  %4d (%#04x) %10llu %8.1f %10.1f %llu\n", t, t & 255, (unsigned long long)s.count,
                lle ? (double)s.bytes / lle : 0.0, lle ? (double)s.insns / lle : 0.0, (unsigned long long)s.hle);
    }
}
