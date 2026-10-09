/*
 *  SU2000 VR link: a small HTTP + WebSocket server that serves a WebXR page (xrclient.h) to a headset browser,
 *  streams the PIX video channels to it and takes head / controller poses and buttons back.
 *
 *  GET /    the WebXR page
 *  GET /ws  WebSocket. Server -> client binary frames (little-endian):
 *             "SUF1", u16 width, u16 height, u16 eyes (1 or 2), u16 flags (bit0 = emulator stereo),
 *             f32 head pose echo [px py pz qx qy qz qw] (the pose that was current when the frame was taken),
 *             then eyes * width * height RGB565 pixels.
 *           Client -> server text messages:
 *             "P hx hy hz hqx hqy hqz hqw c cx cy cz cqx cqy cqz cqw buttons"  (c = 1 if a controller pose follows)
 *             "S player"   select player 1 / 2 (video channel)
 *             "R"          recenter
 *  Only the input of a client on player 1 drives the tracker (sensor 1 = head, sensor 2 = hand) and format card.
 */

#include "dosbox.h"
#include "logging.h"
#include "su2000.h"
#include "pixboard.h"
#include "xrclient.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define SOCK_BAD INVALID_SOCKET
#define sock_close closesocket
#else
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
typedef int sock_t;
#define SOCK_BAD (-1)
#define sock_close close
#endif

namespace {

/* ---- SHA-1 and base64 for the WebSocket handshake ---- */
std::string sha1(const std::string &msg) {
    uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    std::string m = msg;
    const uint64_t bits = (uint64_t)msg.size() * 8;
    m.push_back((char)0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    for (int i = 7; i >= 0; i--) m.push_back((char)(bits >> (i * 8)));
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = ((uint32_t)(uint8_t)m[off + 4 * i] << 24) | ((uint32_t)(uint8_t)m[off + 4 * i + 1] << 16) |
                   ((uint32_t)(uint8_t)m[off + 4 * i + 2] << 8) | (uint32_t)(uint8_t)m[off + 4 * i + 3];
        for (int i = 16; i < 80; i++) { const uint32_t x = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16]; w[i] = (x << 1) | (x >> 31); }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            const uint32_t t = ((a << 5) | (a >> 27)) + f + e + k + w[i];
            e = d; d = c; c = (b << 30) | (b >> 2); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    std::string out;
    for (int i = 0; i < 5; i++) for (int j = 3; j >= 0; j--) out.push_back((char)(h[i] >> (j * 8)));
    return out;
}

std::string base64(const std::string &in) {
    static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const uint32_t v = ((uint32_t)(uint8_t)in[i] << 16) | ((uint32_t)(uint8_t)in[i + 1] << 8) | (uint8_t)in[i + 2];
        out += tab[v >> 18]; out += tab[(v >> 12) & 63]; out += tab[(v >> 6) & 63]; out += tab[v & 63];
    }
    if (i + 1 == in.size()) {
        const uint32_t v = (uint32_t)(uint8_t)in[i] << 16;
        out += tab[v >> 18]; out += tab[(v >> 12) & 63]; out += "==";
    } else if (i + 2 == in.size()) {
        const uint32_t v = ((uint32_t)(uint8_t)in[i] << 16) | ((uint32_t)(uint8_t)in[i + 1] << 8);
        out += tab[v >> 18]; out += tab[(v >> 12) & 63]; out += tab[(v >> 6) & 63]; out += '=';
    }
    return out;
}

/* ---- shared state ---- */
std::mutex mtx;
std::condition_variable frame_cv;
std::vector<PixFrame> channels;    /* latest video channels (left eyes first when stereo) */
bool chan_stereo = false;
uint32_t frame_seq = 0;

struct Input {
    double head[7] = { 0, 0, 0, 0, 0, 0, 1 };
    double hand[7] = { 0, 0, 0, 0, 0, 0, 1 };
    bool have_hand = false;
    unsigned buttons = 0;
    unsigned recenter = 0;
    uint32_t seq = 0;
};
Input input;            /* player 1 */
uint32_t input_seen = 0;
unsigned recenter_seen = 0;
std::atomic<int> p1_clients(0);
unsigned applied_buttons = 0;

std::atomic<bool> running(false);
sock_t listener = SOCK_BAD;
std::thread accept_thread;
std::atomic<int> live_threads(0);

bool send_all(sock_t s, const char *p, size_t n) {
    while (n) {
        const int r = send(s, p, (int)(n > 1u << 20 ? 1u << 20 : n), 0);
        if (r <= 0) return false;
        p += r; n -= (size_t)r;
    }
    return true;
}

bool ws_send(sock_t s, uint8_t opcode, const char *data, size_t n) {
    char hdr[10];
    size_t h = 0;
    hdr[h++] = (char)(0x80 | opcode);
    if (n < 126) hdr[h++] = (char)n;
    else if (n < 65536) { hdr[h++] = 126; hdr[h++] = (char)(n >> 8); hdr[h++] = (char)n; }
    else { hdr[h++] = 127; for (int i = 7; i >= 0; i--) hdr[h++] = (char)((uint64_t)n >> (i * 8)); }
    return send_all(s, hdr, h) && send_all(s, data, n);
}

void handle_text(const std::string &msg, int &player) {
    if (msg.empty()) return;
    if (msg[0] == 'S') {
        const int p = atoi(msg.c_str() + 1) == 2 ? 2 : 1;
        if (p != player) {
            if (player == 1) p1_clients--;
            if (p == 1) p1_clients++;
            player = p;
            LOG_MSG("SU2000: VR client now on player %d", player);
        }
        return;
    }
    if (player != 1) return;
    std::lock_guard<std::mutex> lk(mtx);
    if (msg[0] == 'R') { input.recenter++; return; }
    if (msg[0] != 'P') return;
    double v[15];
    int c = 0;
    unsigned b = 0;
    const int n = sscanf(msg.c_str() + 1, "%lf %lf %lf %lf %lf %lf %lf %d %lf %lf %lf %lf %lf %lf %lf %u",
                         &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &c, &v[7], &v[8], &v[9], &v[10], &v[11], &v[12], &v[13], &b);
    if (n != 16) return;     /* the client always sends all fields (zeros when there is no controller) */
    for (int i = 0; i < 7; i++) input.head[i] = v[i];
    input.have_hand = c != 0;
    if (input.have_hand) for (int i = 0; i < 7; i++) input.hand[i] = v[7 + i];
    input.buttons = b;
    input.seq++;
}

/* Builds the frame message for one player from the latest channels. */
bool build_frame(int player, std::string &out) {
    std::lock_guard<std::mutex> lk(mtx);
    if (channels.empty()) return false;
    const unsigned per_eye = chan_stereo ? (unsigned)channels.size() / 2 : (unsigned)channels.size();
    if (!per_eye) return false;
    const unsigned ci = ((unsigned)player - 1) < per_eye ? (unsigned)player - 1 : 0;
    const PixFrame *eye[2] = { &channels[ci], chan_stereo ? &channels[per_eye + ci] : NULL };
    const unsigned w = eye[0]->width, h = eye[0]->height, eyes = eye[1] && eye[1]->width == w && eye[1]->height == h ? 2 : 1;
    out.resize(40 + (size_t)eyes * w * h * 2);
    char *p = &out[0];
    memcpy(p, "SUF1", 4);
    const uint16_t hw[4] = { (uint16_t)w, (uint16_t)h, (uint16_t)eyes, (uint16_t)(chan_stereo ? 1 : 0) };
    memcpy(p + 4, hw, 8);
    for (int i = 0; i < 7; i++) { const float f = (float)input.head[i]; memcpy(p + 12 + 4 * i, &f, 4); }
    uint16_t *px = (uint16_t *)(p + 40);
    for (unsigned e = 0; e < eyes; e++) {
        const uint32_t *src = eye[e]->pixels.data();
        const size_t cnt = (size_t)w * h < eye[e]->pixels.size() ? (size_t)w * h : eye[e]->pixels.size();
        for (size_t i = 0; i < cnt; i++) {
            const uint32_t c = src[i];
            *px++ = (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F));
        }
        for (size_t i = cnt; i < (size_t)w * h; i++) *px++ = 0;
    }
    return true;
}

void serve_ws(sock_t s) {
    int player = 1;
    p1_clients++;
    LOG_MSG("SU2000: VR client connected");
    std::string buf, frame;
    uint32_t sent_seq = 0;
    bool ok = true;
    while (ok && running) {
        /* new frame? */
        uint32_t seq;
        {
            std::unique_lock<std::mutex> lk(mtx);
            seq = frame_seq;
        }
        if (seq != sent_seq && build_frame(player, frame)) {
            sent_seq = seq;
            if (!ws_send(s, 2, frame.data(), frame.size())) break;
        }
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(s, &rs);
        timeval tv = { 0, 2000 };
        const int r = select((int)s + 1, &rs, NULL, NULL, &tv);
        if (r < 0) break;
        if (r == 0) continue;
        char tmp[4096];
        const int n = recv(s, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        buf.append(tmp, (size_t)n);
        /* parse complete client frames (always masked) */
        while (buf.size() >= 2) {
            const uint8_t b0 = (uint8_t)buf[0], b1 = (uint8_t)buf[1];
            size_t len = b1 & 0x7F, hl = 2;
            if (len == 126) { if (buf.size() < 4) break; len = ((size_t)(uint8_t)buf[2] << 8) | (uint8_t)buf[3]; hl = 4; }
            else if (len == 127) { if (buf.size() < 10) break; len = 0; for (int i = 0; i < 8; i++) len = (len << 8) | (uint8_t)buf[2 + i]; hl = 10; }
            const size_t ml = (b1 & 0x80) ? 4 : 0;
            if (len > (1u << 20)) { ok = false; break; }
            if (buf.size() < hl + ml + len) break;
            std::string payload = buf.substr(hl + ml, len);
            if (ml) for (size_t i = 0; i < len; i++) payload[i] ^= buf[hl + (i & 3)];
            buf.erase(0, hl + ml + len);
            const uint8_t op = b0 & 0x0F;
            if (op == 1) handle_text(payload, player);
            else if (op == 8) { ws_send(s, 8, payload.data(), payload.size() < 2 ? payload.size() : 2); ok = false; break; }
            else if (op == 9) ws_send(s, 10, payload.data(), payload.size());
        }
    }
    if (player == 1) p1_clients--;
    LOG_MSG("SU2000: VR client disconnected");
}

void serve(sock_t s) {
    std::string req;
    char tmp[2048];
    while (req.find("\r\n\r\n") == std::string::npos && req.size() < 16384) {
        const int n = recv(s, tmp, sizeof(tmp), 0);
        if (n <= 0) { sock_close(s); live_threads--; return; }
        req.append(tmp, (size_t)n);
    }
    const size_t sp = req.find(' '), sp2 = req.find(' ', sp + 1);
    const std::string path = sp == std::string::npos ? "" : req.substr(sp + 1, sp2 - sp - 1);
    std::string key;
    {
        std::string low = req;
        for (auto &ch : low) ch = (char)tolower((unsigned char)ch);
        const size_t k = low.find("sec-websocket-key:");
        if (k != std::string::npos) {
            size_t a = k + 18, e = req.find("\r\n", a);
            while (a < e && req[a] == ' ') a++;
            key = req.substr(a, e - a);
            while (!key.empty() && key.back() == ' ') key.pop_back();
        }
    }
    if (path.compare(0, 3, "/ws") == 0 && !key.empty()) {
        const std::string acc = base64(sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
        const std::string resp = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + acc + "\r\n\r\n";
        if (send_all(s, resp.data(), resp.size())) {
            int one = 1;
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
            serve_ws(s);
        }
    } else if (path == "/" || path.compare(0, 2, "/?") == 0 || path == "/index.html") {
        const std::string body = XR_ClientPage();
        char hdr[256];
        snprintf(hdr, sizeof(hdr), "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %u\r\n"
                 "Cache-Control: no-cache\r\nConnection: close\r\n\r\n", (unsigned)body.size());
        send_all(s, hdr, strlen(hdr));
        send_all(s, body.data(), body.size());
    } else {
        static const char nf[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send_all(s, nf, sizeof(nf) - 1);
    }
    sock_close(s);
    live_threads--;
}

void accept_loop(void) {
    while (running) {
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(listener, &rs);
        timeval tv = { 0, 100000 };
        if (select((int)listener + 1, &rs, NULL, NULL, &tv) <= 0) continue;
        sock_t c = accept(listener, NULL, NULL);
        if (c == SOCK_BAD) continue;
        live_threads++;
        std::thread(serve, c).detach();
    }
}

} // namespace

void XR_Setup(int port) {
    XR_Shutdown();
    if (port <= 0) return;
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == SOCK_BAD) return;
    int one = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
    sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(listener, (sockaddr *)&a, sizeof(a)) != 0 || listen(listener, 8) != 0) {
        LOG_MSG("SU2000: VR server cannot listen on port %d", port);
        sock_close(listener);
        listener = SOCK_BAD;
        return;
    }
    running = true;
    accept_thread = std::thread(accept_loop);
    LOG_MSG("SU2000: VR server on http://localhost:%d/", port);
}

void XR_Shutdown(void) {
    if (!running) return;
    running = false;
    if (accept_thread.joinable()) accept_thread.join();
    sock_close(listener);
    listener = SOCK_BAD;
    for (int i = 0; i < 200 && live_threads > 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::lock_guard<std::mutex> lk(mtx);
    channels.clear();
}

bool XR_Active(void) { return running && p1_clients > 0; }

void XR_PushFrame(const PixFrame *ch, unsigned n, bool stereo) {
    if (!running || live_threads <= 0) return;
    std::lock_guard<std::mutex> lk(mtx);
    channels.assign(ch, ch + n);
    chan_stereo = stereo && n >= 2;
    frame_seq++;
}

void XR_Poll(void) {
    if (!running) return;
    Input in;
    bool fresh;
    {
        std::lock_guard<std::mutex> lk(mtx);
        in = input;
        fresh = in.seq != input_seen;
        input_seen = in.seq;
    }
    const bool active = p1_clients > 0;
    static bool was_active = false;
    if (!active) {
        if (applied_buttons) { FCARD_SetButton(8, false); FCARD_SetButton(9, false); applied_buttons = 0; }
        if (was_active) TRACKER_XRPose(NULL, NULL, false);
        was_active = false;
        return;
    }
    was_active = true;
    if (!fresh && in.recenter == recenter_seen) return;
    const bool recenter = in.recenter != recenter_seen;
    recenter_seen = in.recenter;
    TRACKER_XRPose(in.head, in.have_hand ? in.hand : NULL, recenter);
    /* buttons: bit0 trigger -> fire (format card 8 = Ctrl+5), bit1 squeeze / bit2 A -> walk (9 = Ctrl+6) */
    const unsigned b = (in.buttons & 1u) | ((in.buttons & 6u) ? 2u : 0u);
    if ((b ^ applied_buttons) & 1u) FCARD_SetButton(8, (b & 1u) != 0);
    if ((b ^ applied_buttons) & 2u) FCARD_SetButton(9, (b & 2u) != 0);
    applied_buttons = b;
}
