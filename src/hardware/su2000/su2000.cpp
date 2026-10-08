/*
 *  Virtuality SU2000 hardware emulation (research build).
 *
 *  This file owns the [su2000] config section, the binary bus-trace logger, and logging-only
 *  stubs for the SU2000 cards that are not modelled yet (format/control card, InsideTrak
 *  tracker cards, network card). The PIX 1000 lives in pix1000.cpp.
 *
 *  Addresses default to the values in the SU2000 C:\CONFIG.VPC (2 player, 21-07-95):
 *    [PIX]  broadcastFIFO 0x320, procMem 0xD0000, video1 0x340, channel1 proc 0x300, channel2 proc 0x360
 *    [CTRL] FORMAT1 io 0x210 mem 0xE0000, FORMAT2 io 0x218 mem 0xE0800
 *    [TRK]  INSIDETRAK io 0x270 and 0x278
 *    [NET]  io 0x280 mem 0xC8000 irq 5
 */

#include "dosbox.h"
#include "setup.h"
#include "control.h"
#include "inout.h"
#include "mem.h"
#include "paging.h"
#include "pic.h"
#include "regs.h"
#include "logging.h"
#include "su2000.h"
#include "pixboard.h"

#if C_SDL2
#include "SDL.h"
#endif

#include <stdio.h>
#include <string.h>
#include <vector>
#include <sstream>

/* ------------------------------------------------------------------------------------------ */
/* Binary trace logger                                                                         */

#pragma pack(push,1)
struct SU2K_Record {            /* 24 bytes, little-endian */
    uint8_t  kind;
    uint8_t  width;             /* bytes: 1, 2 or 4 */
    uint16_t cs;
    uint32_t eip;
    uint32_t addr;
    uint32_t value;
    uint32_t aux;
    uint32_t time_us;           /* emulated time since power-on, microseconds */
};
#pragma pack(pop)

static FILE *su2k_log = NULL;
static bool  su2k_log_mem_reads = true;

bool SU2K_LogEnabled(void) {
    return su2k_log != NULL;
}

void SU2K_Log(SU2K_Kind kind, uint8_t width, uint32_t addr, uint32_t value, uint32_t aux) {
    if (su2k_log == NULL) return;
    if (kind == SU2K_MEM_READ && !su2k_log_mem_reads) return;
    SU2K_Record r;
    r.kind = kind;
    r.width = width;
    r.cs = SegValue(cs);
    r.eip = (uint32_t)reg_eip;
    r.addr = addr;
    r.value = value;
    r.aux = aux;
    r.time_us = (uint32_t)(PIC_FullIndex() * 1000.0);
    fwrite(&r, sizeof(r), 1, su2k_log);
}

/* ------------------------------------------------------------------------------------------ */
/* Logging-only stubs for the cards that are not emulated yet                                 */

/* Unmodelled I/O reads return the floating ISA bus value 0xFF. Writes are only logged. */
static Bitu su2k_stub_io_read(Bitu port, Bitu iolen) {
    const Bitu v = (iolen == 1) ? 0xFFu : (iolen == 2 ? 0xFFFFu : 0xFFFFFFFFu);
    SU2K_Log(SU2K_IO_READ, (uint8_t)iolen, (uint32_t)port, (uint32_t)v);
    return v;
}

static void su2k_stub_io_write(Bitu port, Bitu val, Bitu iolen) {
    SU2K_Log(SU2K_IO_WRITE, (uint8_t)iolen, (uint32_t)port, (uint32_t)val);
}

/* RAM-backed logging window for a memory-mapped card that is not emulated (format card at
 * E0000, network card at C8000). Behaves as plain RAM so the host sees what it wrote. */
class SU2K_LogRAMHandler : public PageHandler {
public:
    SU2K_LogRAMHandler() : PageHandler(PFLAG_NOCODE) {}
    uint32_t base = 0;
    std::vector<uint8_t> ram;

    uint8_t readb(PhysPt addr) override {
        const uint8_t v = ram[(addr - base) % ram.size()];
        SU2K_Log(SU2K_MEM_READ, 1, (uint32_t)addr, v);
        return v;
    }
    uint16_t readw(PhysPt addr) override {
        const uint16_t v = (uint16_t)(raw(addr) | (raw(addr + 1) << 8u));
        SU2K_Log(SU2K_MEM_READ, 2, (uint32_t)addr, v);
        return v;
    }
    uint32_t readd(PhysPt addr) override {
        const uint32_t v = (uint32_t)raw(addr) | ((uint32_t)raw(addr + 1) << 8u) |
                           ((uint32_t)raw(addr + 2) << 16u) | ((uint32_t)raw(addr + 3) << 24u);
        SU2K_Log(SU2K_MEM_READ, 4, (uint32_t)addr, v);
        return v;
    }
    void writeb(PhysPt addr, uint8_t val) override {
        SU2K_Log(SU2K_MEM_WRITE, 1, (uint32_t)addr, val);
        raw(addr) = val;
    }
    void writew(PhysPt addr, uint16_t val) override {
        SU2K_Log(SU2K_MEM_WRITE, 2, (uint32_t)addr, val);
        raw(addr) = (uint8_t)val; raw(addr + 1) = (uint8_t)(val >> 8u);
    }
    void writed(PhysPt addr, uint32_t val) override {
        SU2K_Log(SU2K_MEM_WRITE, 4, (uint32_t)addr, val);
        for (unsigned int i = 0; i < 4; i++) raw(addr + i) = (uint8_t)(val >> (8u * i));
    }
private:
    uint8_t &raw(PhysPt addr) { return ram[(addr - base) % ram.size()]; }
};

struct SU2K_StubWindow {
    SU2K_LogRAMHandler handler;
    uint32_t pages = 0;
};

static std::vector<IO_ReadHandleObject*>  su2k_rd;
static std::vector<IO_WriteHandleObject*> su2k_wr;
static SU2K_StubWindow su2k_win[4];
static unsigned int su2k_nwin = 0;
static bool su2k_active = false;

static void su2k_stub_ports(uint32_t base, unsigned int count) {
    if (base == 0) return;
    IO_ReadHandleObject *r = new IO_ReadHandleObject;
    IO_WriteHandleObject *w = new IO_WriteHandleObject;
    r->Install(base, su2k_stub_io_read, IO_MA, count);
    w->Install(base, su2k_stub_io_write, IO_MA, count);
    su2k_rd.push_back(r);
    su2k_wr.push_back(w);
}

static void su2k_stub_window(uint32_t base, uint32_t bytes) {
    if (base == 0 || su2k_nwin >= 4) return;
    SU2K_StubWindow &w = su2k_win[su2k_nwin++];
    w.handler.base = base & ~0xFFFu;
    w.pages = (bytes + 0xFFFu) >> 12u;
    w.handler.ram.assign(w.pages << 12u, 0);
    MEM_SetPageHandler(w.handler.base >> 12u, w.pages, &w.handler);
}

/* ------------------------------------------------------------------------------------------ */

void Null_Init(Section *sec);

/* Headless research runs: periodically dump the VGA text screen (B800:0000) to a file. */
static std::string su2k_screen_path;

static void su2k_screen_dump(Bitu val) {
    (void)val;
    if (su2k_screen_path.empty()) return;
    FILE *f = fopen(su2k_screen_path.c_str(), "w");
    if (f) {
        const unsigned int cols = mem_readw(0x44a) ? mem_readw(0x44a) : 80;
        const unsigned int rows = mem_readb(0x484) ? mem_readb(0x484) + 1u : 25;
        for (unsigned int y = 0; y < rows && y < 60; y++) {
            char line[256];
            unsigned int n = 0;
            for (unsigned int x = 0; x < cols && x < 255; x++) {
                const uint8_t c = mem_readb(0xB8000 + (y * cols + x) * 2);
                line[n++] = (c >= 32 && c < 127) ? (char)c : (c ? '.' : ' ');
            }
            while (n && line[n - 1] == ' ') n--;
            line[n] = 0;
            fprintf(f, "%s\n", line);
        }
        fclose(f);
    }
    PIC_AddEvent(su2k_screen_dump, 1000.0);
}

static uint32_t parse_hex(const std::string &s) {
    return (uint32_t)strtoul(s.c_str(), NULL, 0);
}

/* ------------------------------------------------------------------------------------------ */
/* PIX display: show each channel's displayed buffer side by side (left = channel 1)            */

static bool su2k_pix_window = false;
static std::string su2k_frame_dir;
static uint32_t su2k_seen_seq[4];
static unsigned su2k_dumped = 0;
#if C_SDL2
static SDL_Window *su2k_sdlwin = NULL;
static SDL_Renderer *su2k_ren = NULL;
static SDL_Texture *su2k_tex = NULL;
static unsigned su2k_tex_w = 0, su2k_tex_h = 0;
static std::vector<uint32_t> su2k_canvas;
#endif

static void su2k_display_close(void) {
#if C_SDL2
    if (su2k_tex) SDL_DestroyTexture(su2k_tex);
    if (su2k_ren) SDL_DestroyRenderer(su2k_ren);
    if (su2k_sdlwin) SDL_DestroyWindow(su2k_sdlwin);
    su2k_tex = NULL; su2k_ren = NULL; su2k_sdlwin = NULL; su2k_tex_w = su2k_tex_h = 0;
#endif
}

static void su2k_display_tick(Bitu val) {
    (void)val;
    PIX1000_Tick();
    const unsigned ncards = PIX1000_NumCards();
    /* Build video channels from the processor cards: a card whose band starts at line 0 begins a channel, a card whose
     * band starts further down contributes its lines to the previous channel (SU2000 Solo: two cards, one picture). */
    PixFrame f[4];
    unsigned n = 0;
    bool any_new = false, have[4] = {false, false, false, false};
    unsigned w = 0, h = 0;
    for (unsigned i = 0; i < ncards && i < 4; i++) {
        uint32_t seq = 0;
        PixFrame cf;
        if (!PIX1000_GetFrame(i, cf, seq)) continue;
        if (seq != su2k_seen_seq[i]) { any_new = true; su2k_seen_seq[i] = seq; }
        if (cf.band_lo == 0 || n == 0 || f[n - 1].width != cf.width) {
            f[n] = cf;
            have[n] = true;
            n++;
        } else {
            PixFrame &c = f[n - 1];
            if (cf.height > c.height) { c.pixels.resize((size_t)c.width * cf.height, 0); c.height = cf.height; }
            for (unsigned y = cf.band_lo; y <= cf.band_hi && y < cf.height; y++)
                memcpy(&c.pixels[(size_t)y * c.width], &cf.pixels[(size_t)y * cf.width], cf.width * 4);
        }
    }
    for (unsigned i = 0; i < n; i++) {
        w += f[i].width;
        if (f[i].height > h) h = f[i].height;
    }
    if (any_new && w && h) {
        if (!su2k_frame_dir.empty()) {
            char name[1024];
            snprintf(name, sizeof(name), "%s/pix%06u.ppm", su2k_frame_dir.c_str(), su2k_dumped++);
            FILE *fp = fopen(name, "wb");
            if (fp) {
                fprintf(fp, "P6\n%u %u\n255\n", w, h);
                for (unsigned y = 0; y < h; y++)
                    for (unsigned i = 0; i < n && i < 4; i++) {
                        if (!have[i]) continue;
                        for (unsigned x = 0; x < f[i].width; x++) {
                            const uint32_t p = y < f[i].height ? f[i].pixels[(size_t)y * f[i].width + x] : 0;
                            fputc((int)((p >> 16) & 255), fp); fputc((int)((p >> 8) & 255), fp); fputc((int)(p & 255), fp);
                        }
                    }
                fclose(fp);
            }
        }
#if C_SDL2
        if (su2k_pix_window) {
            if (!su2k_sdlwin) {
                su2k_sdlwin = SDL_CreateWindow("SU2000 PIX 1000 (channel 1 | channel 2)", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                            (int)w, (int)h * 2, SDL_WINDOW_RESIZABLE);
                if (su2k_sdlwin) su2k_ren = SDL_CreateRenderer(su2k_sdlwin, -1, 0);
                if (!su2k_ren) { LOG_MSG("SU2000: cannot open the PIX window: %s", SDL_GetError()); su2k_pix_window = false; su2k_display_close(); }
            }
            if (su2k_ren && (w != su2k_tex_w || h != su2k_tex_h)) {
                if (su2k_tex) SDL_DestroyTexture(su2k_tex);
                su2k_tex = SDL_CreateTexture(su2k_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, (int)w, (int)h);
                su2k_tex_w = w; su2k_tex_h = h;
            }
            if (su2k_tex) {
                su2k_canvas.assign((size_t)w * h, 0);
                unsigned x0 = 0;
                for (unsigned i = 0; i < n && i < 4; i++) {
                    if (!have[i]) continue;
                    for (unsigned y = 0; y < f[i].height && y < h; y++)
                        memcpy(&su2k_canvas[(size_t)y * w + x0], &f[i].pixels[(size_t)y * f[i].width], f[i].width * 4);
                    x0 += f[i].width;
                }
                SDL_UpdateTexture(su2k_tex, NULL, su2k_canvas.data(), (int)(w * 4));
                SDL_RenderClear(su2k_ren);
                SDL_RenderCopy(su2k_ren, su2k_tex, NULL, NULL);   /* PAL lines are shown doubled by the window aspect */
                SDL_RenderPresent(su2k_ren);
            }
        }
#endif
    }
    PIC_AddEvent(su2k_display_tick, 20.0);
}

static void SU2000_Teardown(void) {
    if (!su2k_active) return;
    PIC_RemoveEvents(su2k_display_tick);
    PIX1000_Shutdown();
    TRACKER_Shutdown();
    FCARD_Shutdown();
    SSCAPE_Shutdown();
    su2k_display_close();
    for (auto *p : su2k_rd) delete p;
    for (auto *p : su2k_wr) delete p;
    su2k_rd.clear();
    su2k_wr.clear();
    for (unsigned int i = 0; i < su2k_nwin; i++)
        MEM_ResetPageHandler_Unmapped(su2k_win[i].handler.base >> 12u, su2k_win[i].pages);
    su2k_nwin = 0;
    PAGING_ClearTLB();
    if (su2k_log) { fclose(su2k_log); su2k_log = NULL; }
    su2k_active = false;
}

static void SU2000_ShutDown(Section *sec) {
    (void)sec;
    SU2000_Teardown();
}

static void SU2000_OnReset(Section *sec) {
    (void)sec;
    SU2000_Teardown();

    Section_prop *s = static_cast<Section_prop *>(control->GetSection("su2000"));
    if (s == NULL || !s->Get_bool("enable") || IS_PC98_ARCH) return;

    const std::string logpath = s->Get_string("logfile");
    if (!logpath.empty()) {
        su2k_log = fopen(logpath.c_str(), "wb");
        if (su2k_log == NULL) LOG_MSG("SU2000: cannot open trace file %s", logpath.c_str());
        else LOG_MSG("SU2000: writing bus trace to %s", logpath.c_str());
    }
    su2k_log_mem_reads = s->Get_bool("log memory reads");

    uint32_t procs[4];
    unsigned int nproc = 0;
    {
        std::istringstream in(s->Get_string("pix proc ports"));
        std::string tok;
        while (nproc < 4 && in >> tok) procs[nproc++] = parse_hex(tok);
    }
    PIX1000_SetMode(s->Get_bool("pix emulation"), s->Get_bool("pix hle"));
    su2k_pix_window = s->Get_bool("pix window");
    su2k_frame_dir = s->Get_string("pix frame dump");
    memset(su2k_seen_seq, 0, sizeof(su2k_seen_seq));
    PIX1000_Setup(parse_hex(s->Get_string("pix fifo port")), procs, nproc,
                  parse_hex(s->Get_string("pix video port")), parse_hex(s->Get_string("pix procmem")),
                  s->Get_bool("pix fake boot"), (unsigned int)s->Get_int("pix cpu revision"));

    {
        uint32_t tp[2];
        unsigned int nt = 0;
        std::istringstream in(s->Get_string("tracker ports"));
        std::string t;
        while (nt < 2 && in >> t) tp[nt++] = parse_hex(t);
        TRACKER_Setup(tp, nt);
        TRACKER_SetPose(s->Get_string("tracker pose"));
        TRACKER_SetCalibrate(s->Get_bool("tracker calibrate"));
        TRACKER_SetHandTarget(s->Get_string("tracker hand target"));
        TRACKER_SetMouse(s->Get_bool("tracker mouse"));
    }
    {
        /* format/control cards: "ctrl ports" and "ctrl mem" pair up in order */
        uint32_t cio[2], cmem[2];
        unsigned int ni = 0, nm = 0;
        std::istringstream in1(s->Get_string("ctrl ports")), in2(s->Get_string("ctrl mem"));
        std::string t;
        while (ni < 2 && in1 >> t) cio[ni++] = parse_hex(t);
        while (nm < 2 && in2 >> t) cmem[nm++] = parse_hex(t);
        FCARD_Setup(cio, cmem, ni < nm ? ni : nm);
        SSCAPE_Setup(parse_hex(s->Get_string("sound port")));
    }

    /* Logging-only stubs (Milestone 3 replaces these) */
    std::istringstream ports(s->Get_string("stub ports"));
    std::string tok;
    while (ports >> tok) {
        /* "base:count" */
        const size_t c = tok.find(':');
        su2k_stub_ports(parse_hex(tok.substr(0, c)), c == std::string::npos ? 8u : (unsigned int)parse_hex(tok.substr(c + 1)));
    }
    std::istringstream wins(s->Get_string("stub windows"));
    while (wins >> tok) {
        const size_t c = tok.find(':');
        su2k_stub_window(parse_hex(tok.substr(0, c)), c == std::string::npos ? 0x1000u : parse_hex(tok.substr(c + 1)));
    }
    PAGING_ClearTLB();
    PIC_RemoveEvents(su2k_display_tick);
    PIC_AddEvent(su2k_display_tick, 20.0);
    su2k_screen_path = s->Get_string("screen dump");
    PIC_RemoveEvents(su2k_screen_dump);
    if (!su2k_screen_path.empty()) PIC_AddEvent(su2k_screen_dump, 1000.0);
    su2k_active = true;
    LOG_MSG("SU2000: hardware stubs installed");
}

void SU2000_Init() {
    AddExitFunction(AddExitFunctionFuncPair(SU2000_ShutDown), true);
    AddVMEventFunction(VM_EVENT_RESET, AddVMEventFunctionFuncPair(SU2000_OnReset));
    FCARD_AddMapperKeys();   /* Ctrl+F5 coin, Ctrl+F6..F9 buttons 0..3 (rebindable in the mapper) */
}

void SU2000_AddConfigSection(Config *conf) {
    Section_prop *secprop = conf->AddSection_prop("su2000", &Null_Init, true);
    Prop_bool *Pbool;
    Prop_string *Pstring;
    Prop_int *Pint;

    Pbool = secprop->Add_bool("enable", Property::Changeable::WhenIdle, false);
    Pbool->Set_help("Enable the Virtuality SU2000 hardware (PIX 1000 graphics, format card, InsideTrak) research stubs.\n"
                    "Disable DOSBox-X EMS/UMB ([dos] ems=false, umb=false) so C8000-E0FFF is free, as on the real machine.");
    Pstring = secprop->Add_string("logfile", Property::Changeable::WhenIdle, "su2000.su2k");
    Pstring->Set_help("Binary bus trace (24-byte records). Decode with su2000/tools/su2klog.py. Empty to disable.");
    Pstring = secprop->Add_string("screen dump", Property::Changeable::WhenIdle, "");
    Pstring->Set_help("If set, write the text-mode screen to this file once per emulated second (headless runs).");
    Pbool = secprop->Add_bool("log memory reads", Property::Changeable::WhenIdle, true);
    Pbool->Set_help("Also log reads of the memory windows (large traces).");
    Pstring = secprop->Add_string("pix fifo port", Property::Changeable::WhenIdle, "0x320");
    Pstring->Set_help("PIX broadcast FIFO port (CONFIG.VPC broadcastFIFO).");
    Pstring = secprop->Add_string("pix proc ports", Property::Changeable::WhenIdle, "0x300 0x360");
    Pstring->Set_help("I/O base of each PIX processor card (CONFIG.VPC channelN proc), space separated.");
    Pstring = secprop->Add_string("pix video port", Property::Changeable::WhenIdle, "0x340");
    Pstring->Set_help("I/O base of the PIX video card (CONFIG.VPC video1).");
    Pstring = secprop->Add_string("pix procmem", Property::Changeable::WhenIdle, "0xD0000");
    Pstring->Set_help("Physical address of the 64KB processor-card memory window (CONFIG.VPC procMem).");
    Pbool = secprop->Add_bool("pix emulation", Property::Changeable::WhenIdle, true);
    Pbool->Set_help("Run the uploaded PIX firmware: CPU A (geometry) in an MC88110 interpreter, CPU B (rasteriser)\n"
                    "in HLE with interpreter fallback. false = logging stub only.");
    Pbool = secprop->Add_bool("pix hle", Property::Changeable::WhenIdle, false);
    Pbool->Set_help("Experimental: replace CPU B's draw-list handlers with C++ where implemented (only verified on 1995 firmware).\n"
                    "Default false = run all of CPU B's original code in the interpreter.");
    Pbool = secprop->Add_bool("pix window", Property::Changeable::WhenIdle, true);
    Pbool->Set_help("Show the PIX video channels in a separate window (SDL2 builds).");
    Pstring = secprop->Add_string("pix frame dump", Property::Changeable::WhenIdle, "");
    Pstring->Set_help("If set, write every new PIX frame (all channels side by side) as a PPM into this directory.");
    Pbool = secprop->Add_bool("pix fake boot", Property::Changeable::WhenIdle, true);
    Pbool->Set_help("When the host starts the 88110s, write the 'CPU ready' words the firmware would write\n"
                    "(board 0x2100 and 0x2102) so PIX_Open's processor test passes.");
    Pint = secprop->Add_int("pix cpu revision", Property::Changeable::WhenIdle, 9);
    Pint->Set_help("Value written to the ready words (88110 PID revision; the host requires >= 9).");
    Pstring = secprop->Add_string("ctrl ports", Property::Changeable::WhenIdle, "0x210 0x218");
    Pstring->Set_help("I/O base of each format/control card (CONFIG.VPC [CTRL] FORMATn IO_ADDRESS).");
    Pstring = secprop->Add_string("ctrl mem", Property::Changeable::WhenIdle, "0xE0000 0xE0800");
    Pstring->Set_help("Shared-memory address of each format/control card (CONFIG.VPC [CTRL] FORMATn MEM_ADDRESS).");
    Pstring = secprop->Add_string("sound port", Property::Changeable::WhenIdle, "0x330");
    Pstring->Set_help("Ensoniq Soundscape base port (CONFIG.VPC [SND] Port); 0 = off. Set [midi] mpu401=none, it uses the same port.");
    Pstring = secprop->Add_string("stub ports", Property::Changeable::WhenIdle, "0x280:32");
    Pstring->Set_help("Logging-only I/O ranges base:count (network card).");
    Pstring = secprop->Add_string("tracker ports", Property::Changeable::WhenIdle, "0x270 0x278");
    Pstring->Set_help("I/O base of each InsideTrak card (CONFIG.VPC [TRK] TRACKERn IO_ADDRESS).");
    Pbool = secprop->Add_bool("tracker calibrate", Property::Changeable::WhenIdle, false);
    Pbool->Set_help("Debug: sweep the hand sensor position and log where DAC puts the hand.");
    Pbool = secprop->Add_bool("tracker mouse", Property::Changeable::WhenIdle, true);
    Pbool->Set_help("Mouse aims the hand sensor; left button = trigger (Ctrl+5), right button = walk (Ctrl+6). Click the window to capture the mouse.");
    Pstring = secprop->Add_string("tracker hand target", Property::Changeable::WhenIdle, "150 450 1750");
    Pstring->Set_help("DAC only: where the emulated hand sensor is steered to in game coordinates (x y z).");
    Pstring = secprop->Add_string("tracker pose", Property::Changeable::WhenIdle, "273 -2770 22636 -16384 0 0  273 -2770 22636 -16384 0 32767");
    Pstring->Set_help("Raw InsideTrak words (x y z azimuth elevation roll) for sensor 1, then sensor 2, ...");
    Pstring = secprop->Add_string("stub windows", Property::Changeable::WhenIdle, "0xC8000:0x4000");
    Pstring->Set_help("Logging-only RAM windows base:size (network card).");
}
