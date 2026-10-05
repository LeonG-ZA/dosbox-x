/*
 * run_pix: standalone PIX 1000 board test harness.
 *
 * Replays a captured host session (tools/make_replay.py) into one emulated processor card:
 * CPU A (and B) run the real MAINA.OUT/MAINB.OUT code in the M88110 interpreter. B's draw routine
 * is either interpreted (--lle-b, reference) or replaced by the HLE rasteriser (default).
 * Frames are written as PPM files whenever the firmware flips the display buffer.
 *
 * Build: see su2000/tests/Makefile
 * Usage: run_pix MAINA.OUT MAINB.OUT REPLAY.bin OUTDIR [--lle-b] [--max-frames N] [--trace-unknown]
 */
#include "../../src/hardware/su2000/m88110.h"
#include "../../src/hardware/su2000/pixboard.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

static int g_every = 1, g_flip = 0, g_compare = 0;

static std::vector<uint8_t> read_file(const char *p) {
    FILE *f = fopen(p, "rb");
    if (!f) { perror(p); exit(1); }
    std::vector<uint8_t> d;
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) d.insert(d.end(), buf, buf + n);
    fclose(f);
    return d;
}

static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

/* Load the loadable sections of an m88k COFF file into board memory, like PIX_ReadCoff does. */
static void load_coff(PixBoard &b, const std::vector<uint8_t> &d) {
    const unsigned nscns = be16(&d[2]), opthdr = be16(&d[16]);
    size_t off = 20 + opthdr;
    for (unsigned i = 0; i < nscns; i++, off += 44) {
        const uint32_t vaddr = be32(&d[off + 12]), size = be32(&d[off + 16]), ptr = be32(&d[off + 20]), flags = be32(&d[off + 40]);
        if (!ptr || !size || (flags & 0x80)) continue;
        for (uint32_t k = 0; k < size; k++) b.host_write8(vaddr + k, d[ptr + k]);
    }
}

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: run_pix MAINA.OUT MAINB.OUT REPLAY.bin OUTDIR [--lle-b] [--max-frames N] [--max-insns N]\n"); return 1; }
    const std::vector<uint8_t> fa = read_file(argv[1]), fb = read_file(argv[2]), rep = read_file(argv[3]);
    const std::string outdir = argv[4];
    bool lle_b = false;
    int max_frames = 50;
    uint64_t max_insns = 4000000000ull;
    for (int i = 5; i < argc; i++) {
        if (!strcmp(argv[i], "--lle-b")) lle_b = true;
        else if (!strcmp(argv[i], "--max-frames")) max_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--max-insns")) max_insns = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--every")) g_every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--compare")) g_compare = atoi(argv[++i]);
    }

    PixBoard board;
    board.hle_b = !lle_b;
    board.frame_dir = outdir;
    board.max_frames = max_frames;
    board.compare_every = (unsigned)g_compare;

    /* Replay: host writes and FIFO words are applied in their original order; a FIFO word becomes
     * visible to CPU A only after every host write that preceded it. Writes after the last FIFO
     * word read so far are held back. */
    struct Rec { uint8_t t; uint32_t a, v; };
    std::vector<Rec> recs(rep.size() / 9);
    for (size_t i = 0; i < recs.size(); i++) {
        const uint8_t *p = &rep[i * 9];
        recs[i].t = p[0];
        memcpy(&recs[i].a, p + 1, 4);
        memcpy(&recs[i].v, p + 5, 4);
    }
    size_t ri = 0;
    bool run_a = false, run_b = false;
    /* Boot: apply everything up to RUN_A (uploads of MAINB/MAINA and data) */
    while (ri < recs.size() && !(recs[ri].t == 3)) {
        const Rec &r = recs[ri++];
        if (r.t == 1) board.host_write16(r.a, (uint16_t)r.v);
        else if (r.t == 2) board.fifo_push((uint16_t)r.v);
        else if (r.t == 4) { run_b = true; board.run_cpu(false); }
    }
    if (ri < recs.size()) { run_a = true; ri++; }
    (void)fa; (void)fb;
    fprintf(stderr, "boot replay applied (%zu records), runB=%d runA=%d\n", ri, run_b, run_a);
    if (run_a) board.run_cpu(true);
    board.frame_cb = [](PixBoard *b, void *u) {
        const std::string &dir = *(const std::string *)u;
        if (b->frames_written >= b->max_frames) return;
        if (g_flip++ % g_every) return;
        /* geometry from the shared globals (phys 0x7000): +0x40 = line stride in bytes [inferred] */
        const uint32_t stride = ((uint32_t)b->dram[0x7040] << 24 | b->dram[0x7041] << 16 | b->dram[0x7042] << 8 | b->dram[0x7043]);
        const bool px16 = (b->dram[0x2104 + 2] >> 1) & 1;   /* board 0x2104 bit 9: 16-bit pixels */
        const unsigned bpp = px16 ? 2 : 4;
        const unsigned w = stride / bpp ? (stride / bpp > 1024 ? 1024 : stride / bpp) : 512, h = 288;
        char name[512];
        snprintf(name, sizeof(name), "%s/frame%05d.ppm", dir.c_str(), b->frames_written);
        FILE *f = fopen(name, "wb");
        if (!f) return;
        fprintf(f, "P6\n%u %u\n255\n", w, h);
        for (unsigned y = 0; y < h; y++)
            for (unsigned x = 0; x < w; x++) {
                const uint32_t a = (b->display_base + y * stride + x * bpp) & (PixBoard::VRAM_SIZE - 1);
                if (px16) {
                    const unsigned p = (b->vram[a] << 8) | b->vram[a + 1];   /* 4:4:4:4, R in bits 11..8 [inferred] */
                    fputc(((p >> 8) & 15) * 17, f); fputc(((p >> 4) & 15) * 17, f); fputc((p & 15) * 17, f);
                } else { fputc(b->vram[a + 1], f); fputc(b->vram[a + 2], f); fputc(b->vram[a + 3], f); }
            }
        fclose(f);
        snprintf(name, sizeof(name), "%s/frame%05d.raw", dir.c_str(), b->frames_written);
        if ((f = fopen(name, "wb")) != NULL) {
            for (unsigned y = 0; y < h; y++) fwrite(&b->vram[(b->display_base + y * stride) & (PixBoard::VRAM_SIZE - 1)], 1, stride, f);
            fclose(f);
        }
        if (b->frames_written < 5 || b->frames_written % 50 == 0)
            fprintf(stderr, "frame %d: display %08x stride %u\n", b->frames_written, b->display_base, stride);
        b->frames_written++;
    };
    board.frame_user = (void *)&outdir;

    uint64_t total = 0;
    while (total < max_insns && board.frames_written < max_frames) {
        /* feed the FIFO ahead of the CPU, keeping host writes ordered before later words */
        while (ri < recs.size() && board.fifo_level() < 4096) {
            const Rec &r = recs[ri++];
            if (r.t == 1) board.host_write16(r.a, (uint16_t)r.v);
            else if (r.t == 2) board.fifo_push((uint16_t)r.v);
        }
        const uint64_t n = board.run(20000);
        total += n;
        if (ri >= recs.size() && board.fifo_level() == 0 && board.idle) break;
    }
    board.report(stderr);
    fprintf(stderr, "executed %llu instructions, replay %zu/%zu, frames %d\n",
            (unsigned long long)total, ri, recs.size(), board.frames_written);
    return 0;
}
