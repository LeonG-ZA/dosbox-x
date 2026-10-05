/*
 *  One Expality PIX 1000 processor card: shared DRAM, VRAM, broadcast-FIFO input, CPU A (and B)
 *  in the M88110 interpreter, and the HLE replacement for CPU B's rasteriser.
 *
 *  Standalone (no DOSBox-X dependencies); used by pix1000.cpp and by su2000/tests/run_pix.
 *  Board physical map (from MAINA/MAINB, see su2000/docs/PROTOCOL.md §9):
 *    0x00000000  DRAM (16 MB decode; firmware uses < 8 MB). 0x1000_0000 is an uncached alias (BATC).
 *    0x20000006  FIFO data (16-bit read pops one word)
 *    0x30000000  status: bit29 FIFO not empty, bits 28..25 card id [inferred], bits 10..1 video line
 *    0x30000006  control (16-bit)
 *    0x40000000  VRAM, 4 MB (frame buffers, palettes at 0x403FC000)
 */
#ifndef DOSBOX_PIXBOARD_H
#define DOSBOX_PIXBOARD_H

#include "m88110.h"

#include <stdint.h>
#include <stdio.h>
#include <deque>
#include <string>
#include <vector>

class PixBoard;

class PixBus : public M88110Bus {
public:
    PixBus(PixBoard *b, int cpu) : board(b), which(cpu) {}
    uint8_t  rd8(uint32_t pa) override;
    uint16_t rd16(uint32_t pa) override;
    uint32_t rd32(uint32_t pa) override;
    void     wr8(uint32_t pa, uint8_t v) override;
    void     wr16(uint32_t pa, uint16_t v) override;
    void     wr32(uint32_t pa, uint32_t v) override;
    uint32_t fetch(uint32_t pa) override;
private:
    PixBoard *board;
    int which;
};

struct PixFrame {
    /* A displayed frame: 32-bit pixels 0x00RRGGBB (after the firmware's pixel format conversion) */
    unsigned width = 0, height = 0;
    std::vector<uint32_t> pixels;
};

class PixBoard {
public:
    static const uint32_t DRAM_SIZE = 16u * 1024u * 1024u;
    static const uint32_t VRAM_SIZE = 4u * 1024u * 1024u;

    PixBoard();
    ~PixBoard();

    /* host side */
    void     host_write8(uint32_t a, uint8_t v);
    void     host_write16(uint32_t a, uint16_t v);   /* big-endian halfword */
    uint8_t  host_read8(uint32_t a);
    uint16_t host_read16(uint32_t a);
    void     fifo_push(uint16_t w) { fifo.push_back(w); }
    size_t   fifo_level() const { return fifo.size(); }
    void     run_cpu(bool cpu_a);      /* host set the run bit: CPU starts at address 0 */
    void     start(bool a_on, bool b_on);
    void     stop();

    /* execution: run up to n instructions per CPU; returns instructions executed by A */
    uint64_t run(uint64_t n);
    bool     a_idle() const { return a_waiting_fifo; }

    /* configuration */
    bool hle_b = true;
    unsigned card_id = 0;
    unsigned insns_per_line = 2000;   /* video line timing for the status register */
    unsigned lines_per_frame = 625;
    std::string frame_dir;            /* harness: write PPM frames here */
    int max_frames = 1000000;
    int frames_written = 0;
    void (*frame_cb)(PixBoard *b, void *user) = NULL;
    void *frame_user = NULL;

    /* displayed-frame state */
    uint32_t display_base = 0;        /* VRAM address shown (board 0x200c) */
    PixFrame last_frame;
    void     grab_frame();

    /* stats */
    uint64_t fifo_pops = 0, draws = 0, unknown_io = 0;
    void report(FILE *f);

    std::vector<uint8_t> dram, vram;
    M88110 *cpu_a, *cpu_b;
    bool a_on = false, b_on = false;

    /* device access from the CPUs */
    uint32_t dev_read(int cpu, uint32_t pa, unsigned size);
    void     dev_write(int cpu, uint32_t pa, uint32_t v, unsigned size);

private:
    PixBus bus_a, bus_b;
    std::deque<uint16_t> fifo;
    uint16_t last_fifo = 0;
    bool a_waiting_fifo = false;
    uint64_t clock = 0;
    uint16_t ctrl = 0;
    uint32_t draw32_addr = 0, draw16_addr = 0, mainb_poll_pc = 0;
    void find_b_entry_points();
    void hle_draw(bool bpp32);
    uint64_t unknown_log = 0;
    uint8_t sam[0x2000];              /* VRAM serial access memory (one 8 KB row) */
    bool vram_transfer(uint32_t pa, bool write, uint32_t v);
};

#endif
