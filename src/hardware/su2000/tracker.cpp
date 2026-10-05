/*
 *  Polhemus InsideTrak ISA card (Virtuality SU2000 head/hand tracking) - minimal responder.
 *
 *  Interface as used by TRKI_IT_SendData / TRKI_IT_GetData in SFL.EXE (see su2000/docs/PROTOCOL.md §8):
 *    base+1  read : status. bit0 = a response word is waiting, bit1 = ready to accept a command byte
 *    base+0  write: one ASCII command byte (Polhemus command set)
 *    base+0  read : 16-bit response word, two ASCII/binary bytes, first byte in the low half
 *
 *  Milestone 1 only needs the open sequence to succeed: 'W' (drain), then 'S' must return a 28-word
 *  status record with a version number at byte 0x0F and "InsideTRAK" at byte 0x19 (byte 0x17 for
 *  firmware version 151; TRKI_IT_Open @873c0). Every other command is logged and gets no reply until
 *  Milestone 3 decodes the record format. All replies here are FAKED.
 */

#include "dosbox.h"
#include "inout.h"
#include "logging.h"
#include "su2000.h"

#include <deque>
#include <string>
#include <string.h>

namespace {

struct InsideTrak {
    uint32_t io = 0;
    std::deque<uint8_t> out;
    std::string cmd;
    IO_ReadHandleObject rd;
    IO_WriteHandleObject wr;
};

InsideTrak trk[2];
unsigned int ntrk = 0;

InsideTrak *find(Bitu port) {
    for (unsigned int i = 0; i < ntrk; i++)
        if (port >= trk[i].io && port < trk[i].io + 8) return &trk[i];
    return NULL;
}

void reply(InsideTrak &t, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) t.out.push_back((uint8_t)s[i]);
    if (n & 1) t.out.push_back(0);
}

void command(InsideTrak &t, uint8_t c) {
    switch (c) {
        case 'S': {
            /* 28 words = 56 bytes. Layout only as far as TRKI_IT_Open checks it. [FAKED] */
            char rec[56];
            memset(rec, ' ', sizeof(rec));
            memcpy(rec, "21S", 3);
            memcpy(rec + 0x0F, "3.00", 4);           /* version (anything but 151) */
            memcpy(rec + 0x19, "InsideTRAK", 10);
            rec[54] = '\r'; rec[55] = '\n';
            reply(t, rec, sizeof(rec));
            break;
        }
        default:
            break;
    }
}

Bitu trk_read(Bitu port, Bitu iolen) {
    InsideTrak *t = find(port);
    Bitu v = 0xFF;
    if (t) {
        if (port - t->io == 1) {
            v = 0x02u | (t->out.empty() ? 0u : 0x01u);
        } else if (port - t->io == 0) {
            uint8_t lo = 0, hi = 0;
            if (!t->out.empty()) { lo = t->out.front(); t->out.pop_front(); }
            if (iolen >= 2 && !t->out.empty()) { hi = t->out.front(); t->out.pop_front(); }
            v = (Bitu)lo | ((Bitu)hi << 8u);
        } else {
            v = 0;
        }
    }
    SU2K_Log(SU2K_IO_READ, (uint8_t)iolen, (uint32_t)port, (uint32_t)v);
    return v;
}

void trk_write(Bitu port, Bitu val, Bitu iolen) {
    SU2K_Log(SU2K_IO_WRITE, (uint8_t)iolen, (uint32_t)port, (uint32_t)val);
    InsideTrak *t = find(port);
    if (t && port == t->io) command(*t, (uint8_t)val);
}

} // namespace

void TRACKER_Setup(const uint32_t *ports, unsigned int n) {
    TRACKER_Shutdown();
    ntrk = n > 2 ? 2 : n;
    for (unsigned int i = 0; i < ntrk; i++) {
        trk[i].io = ports[i];
        trk[i].out.clear();
        trk[i].rd.Install(ports[i], trk_read, IO_MA, 8);
        trk[i].wr.Install(ports[i], trk_write, IO_MA, 8);
    }
    if (ntrk) LOG_MSG("SU2000: InsideTrak responder at %03xh%s", ports[0], ntrk > 1 ? " (+1)" : "");
}

void TRACKER_Shutdown(void) {
    for (unsigned int i = 0; i < ntrk; i++) {
        trk[i].rd.Uninstall();
        trk[i].wr.Uninstall();
    }
    ntrk = 0;
}
