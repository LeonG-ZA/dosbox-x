/*
 *  Expality PIX 1000 (Virtuality SU2000 graphics) - Milestone 1 logging stub.
 *
 *  Model derived from the SFL.EXE host library (symbols from SFL.SYM) - see
 *  su2000/docs/PROTOCOL.md for the evidence behind every register below.
 *
 *  Processor card (one per video channel, 2x 88110 "A" and "B" + DRAM), I/O base P:
 *    P+0  write: control. bit0 = run CPU A, bit1 = run CPU B, bit4 = map board memory into
 *                the host window, bit5 = (cleared by PIXI_DisableProcCards; meaning unknown).
 *         read : status. bit7 = FIFO not full, bit6 = FIFO not half full, bit5 = FIFO not empty
 *                (all active low as used by PIXI_FIFOStatus). Low bits: unknown.
 *    P+1  write: window page, board address bits 31..24
 *    P+2  write: window page, board address bits 23..16
 *    P+3  write: 1 = reset (CPU A?)  [inferred, PIXI_ResetProcCards]
 *    P+4  write: 1 = reset (CPU B?)  [inferred]
 *  Host memory window: procMem (0xD0000), 64KB, maps board address page<<16 | offset.
 *    The board is big-endian; the host accesses it as 16-bit halfwords, high half first.
 *  Broadcast FIFO: 16-bit OUT to the FIFO port reaches every selected card's CPU A.
 *
 *  This stub keeps board RAM so the host can read back what it wrote, fakes the firmware's
 *  boot handshake (optional), reports an always-empty FIFO and logs everything.
 */

#include "dosbox.h"
#include "inout.h"
#include "mem.h"
#include "paging.h"
#include "logging.h"
#include "regs.h"
#include "cpu.h"
#include "su2000.h"

#include <vector>
#include <unordered_map>

namespace {

const uint32_t BOARD_RAM = 16u * 1024u * 1024u;   /* covers 8MB DRAM + 4MB VRAM decode */

struct ProcCard {
    uint32_t io = 0;
    uint8_t  ctrl = 0;
    uint8_t  page_hi = 0, page_lo = 0;
    uint8_t  reg[8] = {};
    std::vector<uint8_t> ram;
    std::unordered_map<uint32_t, uint8_t> high;    /* board addresses >= 16MB (registers etc.) */

    uint32_t page() const { return ((uint32_t)page_hi << 24u) | ((uint32_t)page_lo << 16u); }
    uint8_t &at(uint32_t a) {
        if (a < BOARD_RAM) return ram[a];
        return high[a];
    }
    void put16(uint32_t a, uint16_t v) { at(a) = (uint8_t)(v >> 8u); at(a + 1) = (uint8_t)v; }
    uint16_t get16(uint32_t a) { return (uint16_t)((at(a) << 8u) | at(a + 1)); }
};

ProcCard cards[4];
unsigned int ncards = 0;
uint32_t fifo_port = 0, video_port = 0, procmem = 0;
bool fake_boot = true;
unsigned int fake_rev = 9;
bool installed = false;
IO_ReadHandleObject  rd_proc[4], rd_fifo, rd_video;
IO_WriteHandleObject wr_proc[4], wr_fifo, wr_video;
uint8_t video_reg[16];

/* the card whose memory the window currently shows (first with bit4 set) */
ProcCard *window_card() {
    for (unsigned int i = 0; i < ncards; i++)
        if (cards[i].ctrl & 0x10) return &cards[i];
    return NULL;
}

/* ---- memory window ------------------------------------------------------------------------ */

class PixWindow : public PageHandler {
public:
    PixWindow() : PageHandler(PFLAG_NOCODE) {}

    uint32_t board(PhysPt addr, ProcCard *c) const { return c->page() | ((uint32_t)addr - procmem); }

    uint16_t rd16(PhysPt addr) {
        ProcCard *c = window_card();
        uint16_t v = 0xFFFF;
        if (c) v = c->get16(board(addr, c) & ~1u);
        SU2K_Log(SU2K_MEM_READ, 2, (uint32_t)addr, v, c ? board(addr, c) : 0xFFFFFFFFu);
        return v;
    }
    void wr16(PhysPt addr, uint16_t v) {
        bool any = false;
        for (unsigned int i = 0; i < ncards; i++) {
            if (!(cards[i].ctrl & 0x10)) continue;
            cards[i].put16(board(addr, &cards[i]) & ~1u, v);
            if (!any) SU2K_Log(SU2K_MEM_WRITE, 2, (uint32_t)addr, v, board(addr, &cards[i]));
            any = true;
        }
        if (!any) SU2K_Log(SU2K_MEM_WRITE, 2, (uint32_t)addr, v, 0xFFFFFFFFu);
    }

    /* Byte lanes: assume the bus bridge swaps bytes within a halfword [inferred]. */
    uint8_t readb(PhysPt addr) override {
        ProcCard *c = window_card();
        uint8_t v = 0xFF;
        if (c) v = c->at(board(addr, c) ^ 1u);
        SU2K_Log(SU2K_MEM_READ, 1, (uint32_t)addr, v, c ? board(addr, c) : 0xFFFFFFFFu);
        return v;
    }
    void writeb(PhysPt addr, uint8_t val) override {
        for (unsigned int i = 0; i < ncards; i++)
            if (cards[i].ctrl & 0x10) cards[i].at(board(addr, &cards[i]) ^ 1u) = val;
        ProcCard *c = window_card();
        SU2K_Log(SU2K_MEM_WRITE, 1, (uint32_t)addr, val, c ? board(addr, c) : 0xFFFFFFFFu);
    }
    uint16_t readw(PhysPt addr) override {
        if (addr & 1) return (uint16_t)(readb(addr) | (readb(addr + 1) << 8u));
        return rd16(addr);
    }
    void writew(PhysPt addr, uint16_t val) override {
        if (addr & 1) { writeb(addr, (uint8_t)val); writeb(addr + 1, (uint8_t)(val >> 8u)); return; }
        wr16(addr, val);
    }
    /* A 32-bit host access is two halfword cycles: low word at addr, high word at addr+2 */
    uint32_t readd(PhysPt addr) override {
        return (uint32_t)readw(addr) | ((uint32_t)readw(addr + 2) << 16u);
    }
    void writed(PhysPt addr, uint32_t val) override {
        writew(addr, (uint16_t)val);
        writew(addr + 2, (uint16_t)(val >> 16u));
    }
};

PixWindow window;

/* ---- processor card I/O ------------------------------------------------------------------ */

ProcCard *card_for_port(Bitu port) {
    for (unsigned int i = 0; i < ncards; i++)
        if (port >= cards[i].io && port < cards[i].io + 8) return &cards[i];
    return NULL;
}

void boot_handshake(ProcCard &c, unsigned int idx, bool cpu_a) {
    SU2K_Log(SU2K_EVENT, 4, cpu_a ? SU2K_EV_RUN_A : SU2K_EV_RUN_B, c.page(), idx);
    if (!fake_boot) return;
    /* MAINB writes its PID revision to board 0x2102; MAINA waits for that, then writes its own
     * to 0x2100 (MAINA.OUT .text 0x4a764-0x4a780). PIXI_TestProcessorMasks needs both >= 9. */
    const uint32_t a = cpu_a ? 0x2100u : 0x2102u;
    c.put16(a, (uint16_t)fake_rev);
    SU2K_Log(SU2K_EVENT, 2, SU2K_EV_FAKE_WRITE, fake_rev, (idx << 24u) | a);
}

Bitu proc_read(Bitu port, Bitu iolen) {
    ProcCard *c = card_for_port(port);
    Bitu v = 0xFF;
    if (c) {
        const unsigned int r = (unsigned int)(port - c->io);
        if (r == 0) v = 0xC0u | (c->ctrl & 0x1Fu);   /* FIFO: not full, not half full, empty */
        else v = c->reg[r];
    }
    SU2K_Log(SU2K_IO_READ, (uint8_t)iolen, (uint32_t)port, (uint32_t)v);
    return v;
}

void proc_write(Bitu port, Bitu val, Bitu iolen) {
    SU2K_Log(SU2K_IO_WRITE, (uint8_t)iolen, (uint32_t)port, (uint32_t)val);
    ProcCard *c = card_for_port(port);
    if (!c) return;
    const unsigned int idx = (unsigned int)(c - cards);
    const unsigned int r = (unsigned int)(port - c->io);
    const uint8_t v = (uint8_t)val;
    c->reg[r] = v;
    switch (r) {
        case 0: {
            const uint8_t old = c->ctrl;
            c->ctrl = v;
            if ((v & 0x02) && !(old & 0x02)) boot_handshake(*c, idx, false);
            if ((v & 0x01) && !(old & 0x01)) boot_handshake(*c, idx, true);
            break;
        }
        case 1: c->page_hi = v; break;
        case 2: c->page_lo = v; break;
        case 3: case 4:
            SU2K_Log(SU2K_EVENT, 1, SU2K_EV_RESET, v, (idx << 8u) | r);
            break;
        default: break;
    }
}

/* ---- broadcast FIFO ---------------------------------------------------------------------- */

Bitu fifo_read(Bitu port, Bitu iolen) {
    SU2K_Log(SU2K_IO_READ, (uint8_t)iolen, (uint32_t)port, 0xFFFF);
    return (iolen == 1) ? 0xFF : 0xFFFF;
}

/* For FIFO records, addr/aux carry the stack dwords at [esp+12]/[esp+8] instead of the port:
 * inside PIXI_OutWordBCFIFO (2 pushes) and PIXI_OutLong/FloatBCFIFO (3 pushes) one of them is the
 * return address into the PIX_Send* function, which lets tools/su2klog.py split the stream into
 * commands by host function. */
void fifo_log(Bitu val, Bitu iolen) {
    const PhysPt sp = SegPhys(ss) + (cpu.stack.big ? (PhysPt)reg_esp : (PhysPt)reg_sp);
    SU2K_Log(SU2K_FIFO, (uint8_t)iolen, mem_readd(sp + 12), (uint32_t)val, mem_readd(sp + 8));
}

void fifo_write(Bitu port, Bitu val, Bitu iolen) {
    (void)port;
    if (iolen == 4) {   /* split like the 16-bit ISA bus would: low word first */
        fifo_log(val & 0xFFFFu, 2);
        fifo_log(val >> 16u, 2);
        return;
    }
    fifo_log(val, iolen);
}

/* ---- video card (logging only) ----------------------------------------------------------- */

Bitu video_read(Bitu port, Bitu iolen) {
    const Bitu v = video_reg[(port - video_port) & 15u];
    SU2K_Log(SU2K_IO_READ, (uint8_t)iolen, (uint32_t)port, (uint32_t)v);
    return v;
}

void video_write(Bitu port, Bitu val, Bitu iolen) {
    SU2K_Log(SU2K_IO_WRITE, (uint8_t)iolen, (uint32_t)port, (uint32_t)val);
    video_reg[(port - video_port) & 15u] = (uint8_t)val;
}

} // namespace

void PIX1000_Setup(uint32_t fifo, const uint32_t *proc_ports, unsigned int nproc,
                   uint32_t video, uint32_t mem, bool fake, unsigned int rev) {
    PIX1000_Shutdown();
    fifo_port = fifo; video_port = video; procmem = mem & ~0xFFFFu;
    fake_boot = fake; fake_rev = rev;
    ncards = nproc > 4 ? 4 : nproc;
    for (unsigned int i = 0; i < ncards; i++) {
        cards[i] = ProcCard();
        cards[i].io = proc_ports[i];
        cards[i].ram.assign(BOARD_RAM, 0);
        rd_proc[i].Install(cards[i].io, proc_read, IO_MA, 8);
        wr_proc[i].Install(cards[i].io, proc_write, IO_MA, 8);
    }
    if (fifo_port) {
        rd_fifo.Install(fifo_port, fifo_read, IO_MA, 2);
        wr_fifo.Install(fifo_port, fifo_write, IO_MA, 2);
    }
    if (video_port) {
        for (auto &r : video_reg) r = 0;
        rd_video.Install(video_port, video_read, IO_MA, 16);
        wr_video.Install(video_port, video_write, IO_MA, 16);
    }
    if (procmem) {
        MEM_SetPageHandler(procmem >> 12u, 16, &window);
        PAGING_ClearTLB();
    }
    installed = true;
    LOG_MSG("SU2000: PIX 1000 stub, %u processor card(s), FIFO %03xh, window %05xh", ncards, fifo_port, procmem);
}

void PIX1000_Shutdown(void) {
    if (!installed) return;
    for (unsigned int i = 0; i < ncards; i++) {
        rd_proc[i].Uninstall();
        wr_proc[i].Uninstall();
        cards[i].ram.clear();
        cards[i].ram.shrink_to_fit();
        cards[i].high.clear();
    }
    rd_fifo.Uninstall(); wr_fifo.Uninstall();
    rd_video.Uninstall(); wr_video.Uninstall();
    if (procmem) {
        MEM_ResetPageHandler_Unmapped(procmem >> 12u, 16);
        PAGING_ClearTLB();
    }
    installed = false;
}
