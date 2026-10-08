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
#include "mem.h"
#include "pic.h"

#include <deque>
#include <string>
#include <string.h>
#include <stdlib.h>
#include <math.h>

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

/* Raw InsideTrak record words per sensor: x, y, z, azimuth, elevation, roll (TRKI_IT_GetRawTrackerRecord:
   position = word * 0.0916 cm, angle = word * pi/32767 rad; azimuth +pi/2 and roll -word+pi are added by the
   library). Set from [su2000] tracker pose, adjusted at run time by TRACKER_MouseDelta. */
int16_t pose[4][6];
bool calibrate = false;
double pose_f[4][6];

void command_line(InsideTrak &t, const std::string &line) {
    if (!line.empty() && line[0] == 'l') {
        /* active station query: bytes 3 and 4 = '1' for sensors 1 and 2 present (TRKI_IT_Open 0x97386) */
        static const char rec[8] = { '2', '1', 'l', '1', '1', ' ', '\r', '\n' };
        reply(t, rec, sizeof(rec));
    }
    static unsigned logged = 0;
    if (logged++ < 32) LOG_MSG("SU2000: InsideTrak command '%s'", line.c_str());
}

void command(InsideTrak &t, uint8_t c) {
    if (t.cmd.empty() && (c == 'S' || c == 'W' || c == 'D' || c == 'P')) {
        if (c == 'S') {
            /* 28 words = 56 bytes. Layout only as far as TRKI_IT_Open checks it. [FAKED] */
            char rec[56];
            memset(rec, ' ', sizeof(rec));
            memcpy(rec, "21S", 3);
            memcpy(rec + 0x0F, "3.00", 4);           /* version (anything but 151) */
            memcpy(rec + 0x19, "InsideTRAK", 10);
            rec[54] = '\r'; rec[55] = '\n';
            reply(t, rec, sizeof(rec));
        } else if (c == 'W') {
            t.out.clear();
        }
        return;
    }
    if (c == '\r' || c == '\n') {
        if (!t.cmd.empty()) command_line(t, t.cmd);
        t.cmd.clear();
        return;
    }
    if (t.cmd.size() < 128) t.cmd.push_back((char)c);
}

bool autoplace = true;
double target[3] = { 150.0, 450.0, 1750.0 };   /* x right, y forward, z up; head is at (0, 0, 2100) */
int32_t data_delta = 0x7fffffff;

/* Closed-loop placement of the hand sensor for DAC: the raw -> game mapping of the hand position goes through the
   library alignment and the game's own scaling, so solve it numerically. Broyden update of a 3x3 Jacobian
   (game units per raw unit), started from a measured column set. Target = DAC's FIXED_TRACKER hand. */
struct Placer {
    double J[3][3];
    double lastg[3], lastr[3];
    bool have;
};
Placer hand_pl = { { { 0.0, -0.37, 0.27 }, { 0.37, 0.27, 0.0 }, { 0.09, 0.0, 0.09 } }, { 0 }, { 0 }, false };
Placer head_pl = { { { 0.0915, 0.0, 0.0 }, { 0.0, 0.0915, 0.0 }, { 0.0, 0.0, 0.0915 } }, { 0 }, { 0 }, false };

void place(Placer &P, const double *g, double *r, const double *target) {
    double (&J)[3][3] = P.J;
    double *lastg = P.lastg, *lastr = P.lastr;
    bool &have = P.have;
    if (have) {
        double dr[3], dg[3], n2 = 0;
        for (int i = 0; i < 3; i++) { dr[i] = r[i] - lastr[i]; dg[i] = g[i] - lastg[i]; n2 += dr[i] * dr[i]; }
        if (n2 > 1.0) {
            for (int i = 0; i < 3; i++) {
                double jd = 0;
                for (int k = 0; k < 3; k++) jd += J[i][k] * dr[k];
                for (int k = 0; k < 3; k++) J[i][k] += (dg[i] - jd) * dr[k] / n2;
            }
        }
    }
    double e[3], err = 0;
    for (int i = 0; i < 3; i++) { e[i] = target[i] - g[i]; err += e[i] * e[i]; }
    for (int i = 0; i < 3; i++) { lastg[i] = g[i]; lastr[i] = r[i]; }
    have = true;
    if (err < 25.0) return;
    /* solve J * dr = e (Cramer) */
    const double a = J[0][0], b = J[0][1], c = J[0][2], d = J[1][0], ee = J[1][1], f = J[1][2], gg = J[2][0], h = J[2][1], i9 = J[2][2];
    const double det = a * (ee * i9 - f * h) - b * (d * i9 - f * gg) + c * (d * h - ee * gg);
    if (fabs(det) < 1e-9) return;
    double dr[3];
    dr[0] = (e[0] * (ee * i9 - f * h) - b * (e[1] * i9 - f * e[2]) + c * (e[1] * h - ee * e[2])) / det;
    dr[1] = (a * (e[1] * i9 - f * e[2]) - e[0] * (d * i9 - f * gg) + c * (d * e[2] - e[1] * gg)) / det;
    dr[2] = (a * (ee * e[2] - e[1] * h) - b * (d * e[2] - e[1] * gg) + e[0] * (d * h - ee * gg)) / det;
    for (int k = 0; k < 3; k++) {
        double s = dr[k] * 0.5;
        if (s > 3000) s = 3000;
        if (s < -3000) s = -3000;
        r[k] += s;
    }
    TRACKER_MouseDelta(0, 0, false);
}

void debug_game_pose(void) {
    /* DAC.EXE (Solo): head[] at link 0x17d9b0, hand[] at 0x17da10, runtime = link + 0x18c000 [inferred] */
    static double last = -1e9, last_log = -1e9;
    const double now = PIC_FullIndex();
    if (now - last < 100) return;
    const bool do_log = (now - last_log) >= 2000;
    if (do_log) last_log = now;
    last = now;
    /* find the data object once: the IT filter string "v0.2,0.2,0.8,0.8" is at link address 0xde870 */
    int32_t &delta = data_delta;
    if (delta == 0x7fffffff) {
        static const char key[] = "v0.2,0.2,0.8,0.8\rx0.2";
        for (uint32_t a = 0x100000; a < 0x1000000 && delta == 0x7fffffff; a += 1) {
            unsigned k = 0;
            while (key[k] && mem_readb(a + k) == (uint8_t)key[k]) k++;
            if (!key[k]) delta = (int32_t)(a - 0xde870);
        }
        LOG_MSG("SU2000: DAC data delta %08x", (unsigned)delta);
        if (delta == 0x7fffffff) delta = 0;
    }
    float v[12];
    for (unsigned i = 0; i < 6; i++) {
        uint32_t a = mem_readd(0x17d9b0 + delta + 4 * i), b = mem_readd(0x17da10 + delta + 4 * i);
        memcpy(&v[i], &a, 4); memcpy(&v[6 + i], &b, 4);
    }
    /* calibration sweep (su2000 tracker calibrate): perturb hand raw x, y, z by +1000 in turn */
    static unsigned phase = 0;
    if (calibrate && v[0] != 0.0f) {
        static double base[3]; static bool have = false;
        if (!have) { for (int i = 0; i < 3; i++) base[i] = pose_f[1][i]; have = true; }
        const unsigned ph = (phase++ / 2) % 4;
        for (int i = 0; i < 3; i++) pose_f[1][i] = base[i] + ((ph == (unsigned)i + 1) ? 1000.0 : 0.0);
        TRACKER_MouseDelta(0, 0, false);
        LOG_MSG("SU2000: calibrate phase %u", ph);
    }
    if (autoplace && delta && v[2] > 100.0f) {
        /* head stays at standing height; the hand target turns with the head (game frame: x right, y forward, z up) */
        static const double head_target[3] = { 0.0, 0.0, 2100.0 };
        const double gh[3] = { v[0], v[1], v[2] }, gd[3] = { v[6], v[7], v[8] };
        const double az = v[3], c = cos(az), s = sin(az);
        const double ht[3] = { target[0] * c - target[1] * s, target[0] * s + target[1] * c, target[2] };
        place(head_pl, gh, pose_f[0], head_target);
        place(hand_pl, gd, pose_f[1], ht);
    }
    if (do_log) LOG_MSG("SU2000: game head %.1f %.1f %.1f  %.3f %.3f %.3f   hand %.1f %.1f %.1f  %.3f %.3f %.3f",
            v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11]);
}

void steer(void) {
    /* Ctrl+arrows (the format card stick keys) turn the hand sensor: left/right = azimuth, up/down = elevation */
    static double last = -1;
    const double now = PIC_FullIndex();
    const double dt = last < 0 ? 0 : now - last;
    last = now;
    int x, y;
    FCARD_GetStick(x, y);
    if ((x || y) && dt > 0 && dt < 200) { TRACKER_MouseDelta(-x * dt * 12.0, 0, true); TRACKER_MouseDelta(-x * dt * 12.0, -y * dt * 8.0, false); }
}

void send_record(InsideTrak &t, unsigned sensor) {
    debug_game_pose();
    steer();
    if (sensor < 1 || sensor > 4) return;
    const int16_t *p = pose[sensor - 1];
    uint8_t rec[16];
    rec[0] = '0'; rec[1] = (uint8_t)('0' + sensor);       /* header word [inferred, not checked by DAC] */
    for (unsigned i = 0; i < 6; i++) { rec[2 + 2 * i] = (uint8_t)p[i]; rec[3 + 2 * i] = (uint8_t)((uint16_t)p[i] >> 8); }
    rec[14] = rec[15] = 0;
    reply(t, (const char *)rec, sizeof(rec));
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
    if (!t) return;
    if (port == t->io) command(*t, (uint8_t)val);
    else send_record(*t, (unsigned)(port - t->io));   /* base+n: request a record for hardware sensor n */
}
} // namespace

void TRACKER_SetCalibrate(bool on) { calibrate = on; }

static bool use_mouse = false;
void TRACKER_SetMouse(bool on) { use_mouse = on; }

/* Called from the DOS mouse emulation. While a tracker is emulated and [su2000] tracker mouse is on, the mouse aims the
   hand sensor and its buttons are the Solo player's trigger (left, format card button 8 = Ctrl+5) and walk button
   (right, button 9 = Ctrl+6); DOS programs then see no mouse. */
bool SU2000_MouseMove(float xrel, float yrel) {
    if (!use_mouse || !ntrk) return false;
    TRACKER_MouseDelta(-xrel * 40.0, 0, true);      /* turn: head and hand together */
    TRACKER_MouseDelta(-xrel * 40.0, -yrel * 40.0, false);
    return true;
}

bool SU2000_MouseButton(uint8_t button, bool pressed) {
    if (!use_mouse || !ntrk) return false;
    if (button == 0) FCARD_SetButton(8, pressed);
    else if (button == 1) FCARD_SetButton(9, pressed);
    return true;
}

void TRACKER_SetHandTarget(const char *s) {
    double v[3];
    if (s && sscanf(s, "%lf %lf %lf", &v[0], &v[1], &v[2]) == 3)
        for (int i = 0; i < 3; i++) target[i] = v[i];
}

void TRACKER_SetPose(const char *s) {
    double v[24] = { 0 };
    unsigned n = 0;
    while (s && *s && n < 24) {
        char *e;
        const double d = strtod(s, &e);
        if (e == s) { s++; continue; }
        v[n++] = d; s = e;
    }
    for (unsigned i = 0; i < 24; i++) pose_f[i / 6][i % 6] = v[i];
    TRACKER_MouseDelta(0, 0, false);
}

void TRACKER_MouseDelta(double dx, double dy, bool head) {
    /* mouse moves the hand (or, with head = true, the head) in azimuth / elevation, in raw angle units */
    double *p = pose_f[head ? 0 : 1];
    p[3] += dx; p[4] += dy;
    if (p[4] > 14000) p[4] = 14000;
    if (p[4] < -14000) p[4] = -14000;
    while (p[3] > 32767) p[3] -= 65534;
    while (p[3] < -32767) p[3] += 65534;
    for (unsigned s = 0; s < 4; s++)
        for (unsigned i = 0; i < 6; i++) {
            double d = pose_f[s][i];
            if (d > 32767) d = 32767;
            if (d < -32767) d = -32767;
            pose[s][i] = (int16_t)d;
        }
}

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
