/*
 *  SU2000 VR link: an HTTP(S) + WebSocket server that serves a WebXR page (xrclient.h) to a headset browser, and a
 *  WebRTC data channel (UDP) per client for the real-time traffic.
 *
 *  GET /    the WebXR page
 *  GET /ws  WebSocket: control and WebRTC signalling, and the fallback transport while the data channel is not open.
 *           https / wss is detected on the same port (first byte 0x16) and uses a self-signed certificate.
 *
 *  Messages (little-endian; the same binary messages travel on the data channel or, as fallback, the WebSocket):
 *    server -> client
 *      "SUJ1" u32 frame, u8 eye, u8 eyes, u16 chunk, u16 chunks, u16 width, u16 height, u16 flags (bit0 = emulator
 *             stereo, bit1 = PNG instead of JPEG), f32 head pose [px py pz qx qy qz qw] the frame was taken with, then a piece of the eye's image
 *      "SUA1" u32 seq, u16 rate, u16 frames, then frames * 2 int16 (left, right): game audio, about 10 ms per packet
 *      "SUM1" u32 seq, u16 rate, u16 frames, then int16 mono: the other player's microphone (Visette intercom)
 *    client -> server
 *      "SUM1" ...       this player's microphone
 *      text "P hx hy hz hqx hqy hqz hqw c cx cy cz cqx cqy cqz cqw buttons"   (c = 1 if a controller pose is given)
 *    WebSocket text only
 *      "S player"  select player 1 / 2        "R"  recentre
 *      "O sdp" / "A sdp"  WebRTC offer (client) / answer (server)
 *      "C candidate\nmid"  ICE candidate (both directions)
 *  Only player 1's input drives the tracker (sensor 1 = head, sensor 2 = hand) and the format card buttons; each
 *  player's microphone level goes to its format card (CTRL_GetMic).
 *
 *  Optional libraries (C_SU2000_VRLINK): mbedTLS for https, libdatachannel for WebRTC. Without them the page is only
 *  served over plain http / ws.
 */

#include "dosbox.h"
#include "logging.h"
#include "su2000.h"
#include "pixboard.h"
#include "xrclient.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#if defined(_MSC_VER) && defined(_M_X64) && !defined(C_SU2000_VRLINK)
/* Windows x64 project: the libraries are built by vs/build-webrtc.cmd into obj/webrtc/lib */
#define C_SU2000_VRLINK 1
#define RTC_STATIC
#pragma comment(lib, "../obj/webrtc/lib/datachannel-static.lib")
#pragma comment(lib, "../obj/webrtc/lib/juice-static.lib")
#pragma comment(lib, "../obj/webrtc/lib/usrsctp.lib")
#pragma comment(lib, "../obj/webrtc/lib/mbedtls.lib")
#pragma comment(lib, "../obj/webrtc/lib/mbedx509.lib")
#pragma comment(lib, "../obj/webrtc/lib/mbedcrypto.lib")
#pragma comment(lib, "../obj/webrtc/lib/everest.lib")
#pragma comment(lib, "../obj/webrtc/lib/p256m.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#endif
#ifndef C_SU2000_VRLINK
#define C_SU2000_VRLINK 0
#endif

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
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <ifaddrs.h>
typedef int sock_t;
#define SOCK_BAD (-1)
#define sock_close close
#endif
#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL     /* a closed headset connection must not raise SIGPIPE */
#else
#define SEND_FLAGS 0
#endif

#if C_SU2000_VRLINK
#include <rtc/rtc.h>
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>
#include <mbedtls/ecp.h>
#include <mbedtls/oid.h>
#include <psa/crypto.h>
#endif

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

extern void (*MIXER_TapCallback)(unsigned rate, unsigned frames, const int16_t *lr);

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

void put16(std::string &s, uint16_t v) { s.push_back((char)v); s.push_back((char)(v >> 8)); }
void put32(std::string &s, uint32_t v) { put16(s, (uint16_t)v); put16(s, (uint16_t)(v >> 16)); }
uint16_t get16(const char *p) { return (uint16_t)((uint8_t)p[0] | ((uint8_t)p[1] << 8)); }

double now_ms(void) {
    return (double)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() / 1000.0;
}

/* ---- settings ---- */
int jpeg_quality = 95;
bool use_png = true;            /* lossless; smaller than JPEG on the flat-shaded PIX pictures */
bool audio_on = true;
bool rtc_on = true;
std::string cert_base = "su2000vr";

/* ---- video ---- */
std::mutex mtx;
std::vector<PixFrame> channels;    /* latest video channels (left eyes first when stereo) */
bool chan_stereo = false;
uint32_t frame_seq = 0;
float frame_pose[2][7];            /* each player's head pose when the frame was taken */
unsigned drawn_w[8], drawn_for[8]; /* widest drawn column per channel (the game may draw into part of the line) */

struct Encoded {
    uint32_t seq = 0;
    std::vector<std::string> jpg;  /* per eye */
    unsigned w = 0, h = 0;
    bool stereo = false;
    float pose[7];
};
std::mutex enc_mtx;
std::shared_ptr<const Encoded> enc_cache[2];

void jpg_write(void *ctx, void *data, int size) { ((std::string *)ctx)->append((const char *)data, (size_t)size); }

/* JPEG of the player's eyes for the latest frame; encoded once per frame and player. */
std::shared_ptr<const Encoded> encoded(int player) {
    PixFrame eye[2];
    unsigned crop = 0;
    auto e = std::make_shared<Encoded>();
    {
        std::lock_guard<std::mutex> lk(mtx);
        if (channels.empty()) return NULL;
        {
            std::lock_guard<std::mutex> lk2(enc_mtx);
            if (enc_cache[player - 1] && enc_cache[player - 1]->seq == frame_seq) return enc_cache[player - 1];
        }
        const unsigned per_eye = chan_stereo ? (unsigned)channels.size() / 2 : (unsigned)channels.size();
        if (!per_eye) return NULL;
        const unsigned ci = ((unsigned)player - 1) < per_eye ? (unsigned)player - 1 : 0;
        eye[0] = channels[ci];
        e->stereo = chan_stereo && channels[per_eye + ci].width == eye[0].width && channels[per_eye + ci].height == eye[0].height;
        if (e->stereo) eye[1] = channels[per_eye + ci];
        e->seq = frame_seq;
        memcpy(e->pose, frame_pose[player - 1], sizeof(e->pose));
        /* drawn width, kept per channel */
        const unsigned w = eye[0].width, h = eye[0].height;
        if (drawn_for[ci] != w) { drawn_for[ci] = w; drawn_w[ci] = 8; }
        for (unsigned y = 0; y < h; y += 4) {
            const uint32_t *row = &eye[0].pixels[(size_t)y * w];
            for (unsigned x = w; x-- > drawn_w[ci];)
                if (row[x] & 0xFFFFFFu) { drawn_w[ci] = (x + 8) & ~7u; if (drawn_w[ci] > w) drawn_w[ci] = w; break; }
        }
        crop = drawn_w[ci];
    }
    const unsigned w = eye[0].width, h = eye[0].height;
    if (!w || !h) return NULL;
    e->w = crop; e->h = h;
    std::vector<uint8_t> rgb((size_t)crop * h * 3);
    for (unsigned k = 0; k < (e->stereo ? 2u : 1u); k++) {
        for (unsigned y = 0; y < h; y++)
            for (unsigned x = 0; x < crop; x++) {
                const uint32_t c = (size_t)y * w + x < eye[k].pixels.size() ? eye[k].pixels[(size_t)y * w + x] : 0;
                uint8_t *p = &rgb[((size_t)y * crop + x) * 3];
                p[0] = (uint8_t)(c >> 16); p[1] = (uint8_t)(c >> 8); p[2] = (uint8_t)c;
            }
        std::string out;
        if (use_png) stbi_write_png_to_func(jpg_write, &out, (int)crop, (int)h, 3, rgb.data(), (int)crop * 3);
        else stbi_write_jpg_to_func(jpg_write, &out, (int)crop, (int)h, 3, rgb.data(), jpeg_quality);   /* > 90: no chroma subsampling */
        e->jpg.push_back(out);
    }
    std::lock_guard<std::mutex> lk2(enc_mtx);
    enc_cache[player - 1] = e;
    return e;
}

/* ---- audio and microphone packets ---- */
struct Packet {
    uint64_t seq;
    int from;                      /* microphone: sending player */
    std::string data;
};
std::mutex aud_mtx;
std::deque<Packet> audio_q, mic_q;
uint64_t audio_seq = 0, mic_seq = 0;
std::vector<int16_t> audio_acc;
unsigned audio_rate = 0;

std::atomic<int> nclients(0);

void audio_tap(unsigned rate, unsigned frames, const int16_t *lr) {
    if (!audio_on || nclients <= 0) return;
    std::lock_guard<std::mutex> lk(aud_mtx);
    if (rate != audio_rate) { audio_rate = rate; audio_acc.clear(); }
    audio_acc.insert(audio_acc.end(), lr, lr + frames * 2);
    const size_t per = (size_t)rate / 100 * 2;     /* 10 ms */
    while (per && audio_acc.size() >= per) {
        Packet p;
        p.seq = ++audio_seq;
        p.from = 0;
        p.data = "SUA1";
        put32(p.data, (uint32_t)p.seq);
        put16(p.data, (uint16_t)rate);
        put16(p.data, (uint16_t)(per / 2));
        p.data.append((const char *)audio_acc.data(), per * 2);
        audio_acc.erase(audio_acc.begin(), audio_acc.begin() + (ptrdiff_t)per);
        audio_q.push_back(std::move(p));
        while (audio_q.size() > 50) audio_q.pop_front();
    }
}

std::atomic<int> mic_level[2];
std::atomic<double> mic_time[2];

void handle_mic(const char *d, size_t n, int player) {
    if (n < 12 || memcmp(d, "SUM1", 4) != 0) return;
    const unsigned frames = get16(d + 10);
    if (12 + (size_t)frames * 2 > n) return;
    int peak = 0;
    for (unsigned i = 0; i < frames; i++) { const int v = abs((int)(int16_t)get16(d + 12 + 2 * i)); if (v > peak) peak = v; }
    mic_level[player - 1] = peak >> 7;
    mic_time[player - 1] = now_ms();
    std::lock_guard<std::mutex> lk(aud_mtx);
    Packet p;
    p.seq = ++mic_seq;
    p.from = player;
    p.data.assign(d, 12 + (size_t)frames * 2);
    mic_q.push_back(std::move(p));
    while (mic_q.size() > 50) mic_q.pop_front();
}

/* ---- input ---- */
struct Input {
    double head[7] = { 0, 0, 0, 0, 0, 0, 1 };
    double hand[7] = { 0, 0, 0, 0, 0, 0, 1 };
    bool have_hand = false;
    unsigned buttons = 0;
    unsigned recenter = 0;
    uint32_t seq = 0;
};
Input inputs[2];
uint32_t input_seen = 0;
unsigned recenter_seen = 0;
std::atomic<int> p1_clients(0);
unsigned applied_buttons = 0;

void handle_input(const std::string &msg, int player) {
    if (msg.empty()) return;
    std::lock_guard<std::mutex> lk(mtx);
    Input &in = inputs[player - 1];
    if (msg[0] == 'R') { in.recenter++; return; }
    if (msg[0] != 'P') return;
    double v[15];
    int c = 0;
    unsigned b = 0;
    const int n = sscanf(msg.c_str() + 1, "%lf %lf %lf %lf %lf %lf %lf %d %lf %lf %lf %lf %lf %lf %lf %u",
                         &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &c, &v[7], &v[8], &v[9], &v[10], &v[11], &v[12], &v[13], &b);
    if (n != 16) return;     /* the client always sends all fields (zeros when there is no controller) */
    for (int i = 0; i < 7; i++) in.head[i] = v[i];
    in.have_hand = c != 0;
    if (in.have_hand) for (int i = 0; i < 7; i++) in.hand[i] = v[7 + i];
    in.buttons = b;
    in.seq++;
}

/* ---- connections: plain or TLS ---- */
std::atomic<bool> running(false);
sock_t listener = SOCK_BAD;
std::thread accept_thread;
std::atomic<int> live_threads(0);

bool raw_send(sock_t s, const char *p, size_t n) {
    while (n) {
        const int r = send(s, p, (int)(n > 1u << 20 ? 1u << 20 : n), SEND_FLAGS);
        if (r <= 0) return false;
        p += r; n -= (size_t)r;
    }
    return true;
}

#if C_SU2000_VRLINK
bool tls_ready = false;
mbedtls_entropy_context tls_entropy;
mbedtls_ctr_drbg_context tls_drbg;
std::mutex drbg_mtx;
mbedtls_ssl_config tls_conf;
mbedtls_x509_crt tls_cert;
mbedtls_pk_context tls_key;

std::vector<std::string> lan_addresses(void);

int locked_random(void *p, unsigned char *out, size_t n) {
    std::lock_guard<std::mutex> lk(drbg_mtx);
    return mbedtls_ctr_drbg_random(p, out, n);
}

bool read_file(const std::string &path, std::string &out) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[4096];
    size_t n;
    out.clear();
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return !out.empty();
}

bool write_file(const std::string &path, const char *data) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return false;
    fputs(data, f);
    fclose(f);
    return true;
}

/* Self-signed "CN=SU2000 VR" certificate (EC P-256), kept as <cert_base>.crt / .key so the browser's security
   exception stays valid across runs. */
bool tls_make_certificate(const std::string &crt_path, const std::string &key_path) {
    mbedtls_pk_context key;
    mbedtls_pk_init(&key);
    bool ok = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) == 0 &&
              mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key), locked_random, &tls_drbg) == 0;
    mbedtls_x509write_cert crt;
    mbedtls_x509write_crt_init(&crt);
    if (ok) {
        unsigned char serial[8];
        locked_random(&tls_drbg, serial, sizeof(serial));
        serial[0] = (unsigned char)((serial[0] & 0x7F) | 0x40);     /* positive, minimal DER integer */
        time_t t = time(NULL);
        struct tm g;
#ifdef _WIN32
        gmtime_s(&g, &t);
#else
        gmtime_r(&t, &g);
#endif
        char from[16], to[16];
        snprintf(from, sizeof(from), "%04d%02d%02d000000", g.tm_year + 1900 - 1, g.tm_mon + 1, g.tm_mday > 28 ? 28 : g.tm_mday);
        snprintf(to, sizeof(to), "%04d%02d%02d000000", g.tm_year + 1900 + 10, g.tm_mon + 1, g.tm_mday > 28 ? 28 : g.tm_mday);
        mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
        mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
        mbedtls_x509write_crt_set_subject_key(&crt, &key);
        mbedtls_x509write_crt_set_issuer_key(&crt, &key);
        ok = mbedtls_x509write_crt_set_subject_name(&crt, "CN=SU2000 VR") == 0 &&
             mbedtls_x509write_crt_set_issuer_name(&crt, "CN=SU2000 VR") == 0 &&
             mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial)) == 0 &&
             mbedtls_x509write_crt_set_validity(&crt, from, to) == 0;
        /* Chromium refuses a certificate without the usual server extensions as invalid (no "proceed" button):
           subject alternative names (localhost and this machine's IPv4 addresses), CA:false, key usage, serverAuth */
        static unsigned char ips[16][4];
        static mbedtls_x509_san_list san[17];
        unsigned nsan = 0;
        memset(san, 0, sizeof(san));
        san[nsan].node.type = MBEDTLS_X509_SAN_DNS_NAME;
        san[nsan].node.san.unstructured_name.p = (unsigned char *)"localhost";
        san[nsan].node.san.unstructured_name.len = 9;
        nsan++;
        for (const auto &ip : lan_addresses()) {
            unsigned a, b, c, d;
            if (nsan > 16 || sscanf(ip.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) continue;
            unsigned char *q = ips[nsan - 1];
            q[0] = (unsigned char)a; q[1] = (unsigned char)b; q[2] = (unsigned char)c; q[3] = (unsigned char)d;
            san[nsan].node.type = MBEDTLS_X509_SAN_IP_ADDRESS;
            san[nsan].node.san.unstructured_name.p = q;
            san[nsan].node.san.unstructured_name.len = 4;
            nsan++;
        }
        for (unsigned i = 0; i + 1 < nsan; i++) san[i].next = &san[i + 1];
        static const char server_auth[] = MBEDTLS_OID_SERVER_AUTH;
        mbedtls_asn1_sequence eku;
        memset(&eku, 0, sizeof(eku));
        eku.buf.tag = MBEDTLS_ASN1_OID;
        eku.buf.p = (unsigned char *)server_auth;
        eku.buf.len = sizeof(server_auth) - 1;
        ok = ok && mbedtls_x509write_crt_set_subject_alternative_name(&crt, san) == 0 &&
             mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1) == 0 &&
             mbedtls_x509write_crt_set_key_usage(&crt, MBEDTLS_X509_KU_DIGITAL_SIGNATURE | MBEDTLS_X509_KU_KEY_AGREEMENT) == 0 &&
             mbedtls_x509write_crt_set_ext_key_usage(&crt, &eku) == 0 &&
             mbedtls_x509write_crt_set_subject_key_identifier(&crt) == 0 &&
             mbedtls_x509write_crt_set_authority_key_identifier(&crt) == 0;
    }
    static unsigned char crt_pem[4096], key_pem[2048];
    ok = ok && mbedtls_x509write_crt_pem(&crt, crt_pem, sizeof(crt_pem), locked_random, &tls_drbg) == 0 &&
         mbedtls_pk_write_key_pem(&key, key_pem, sizeof(key_pem)) == 0 &&
         write_file(crt_path, (const char *)crt_pem) && write_file(key_path, (const char *)key_pem);
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&key);
    if (ok) LOG_MSG("SU2000: VR server created its self-signed certificate %s", crt_path.c_str());
    return ok;
}

bool tls_init(void) {
    if (tls_ready) return true;
    psa_crypto_init();
    mbedtls_entropy_init(&tls_entropy);
    mbedtls_ctr_drbg_init(&tls_drbg);
    mbedtls_ssl_config_init(&tls_conf);
    mbedtls_x509_crt_init(&tls_cert);
    mbedtls_pk_init(&tls_key);
    static const char pers[] = "su2000 vr";
    if (mbedtls_ctr_drbg_seed(&tls_drbg, mbedtls_entropy_func, &tls_entropy, (const unsigned char *)pers, sizeof(pers)) != 0) return false;
    const std::string crt_path = cert_base + ".crt", key_path = cert_base + ".key";
    std::string crt, key;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (read_file(crt_path, crt) && read_file(key_path, key) &&
            mbedtls_x509_crt_parse(&tls_cert, (const unsigned char *)crt.c_str(), crt.size() + 1) == 0 &&
            mbedtls_pk_parse_key(&tls_key, (const unsigned char *)key.c_str(), key.size() + 1, NULL, 0, locked_random, &tls_drbg) == 0 &&
            mbedtls_x509_crt_has_ext_type(&tls_cert, MBEDTLS_X509_EXT_SUBJECT_ALT_NAME))
            break;
        /* missing, unreadable, or made by an older build without extensions: make a new one */
        mbedtls_x509_crt_free(&tls_cert); mbedtls_x509_crt_init(&tls_cert);
        mbedtls_pk_free(&tls_key); mbedtls_pk_init(&tls_key);
        if (attempt || !tls_make_certificate(crt_path, key_path)) {
            LOG_MSG("SU2000: VR server: no TLS certificate (%s), https disabled", crt_path.c_str());
            return false;
        }
    }
    if (mbedtls_ssl_config_defaults(&tls_conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0)
        return false;
    /* TLS 1.2: its code paths share no global state between connections (TLS 1.3 needs PSA, not thread safe here) */
    mbedtls_ssl_conf_max_tls_version(&tls_conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_rng(&tls_conf, locked_random, &tls_drbg);
    if (mbedtls_ssl_conf_own_cert(&tls_conf, &tls_cert, &tls_key) != 0) return false;
    tls_ready = true;
    return true;
}

int bio_send(void *ctx, const unsigned char *buf, size_t len) {
    const int r = send(*(sock_t *)ctx, (const char *)buf, (int)len, SEND_FLAGS);
    return r < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : r;
}

int bio_recv(void *ctx, unsigned char *buf, size_t len) {
    const int r = recv(*(sock_t *)ctx, (char *)buf, (int)len, 0);
    return r < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : r;
}
#endif

/* One client connection. TLS is detected from the first byte (0x16 = handshake), so the same port serves http:// and
   https:// (the Quest browser only allows WebXR on https or localhost). */
struct Conn {
    sock_t s = SOCK_BAD;
    bool tls = false;
#if C_SU2000_VRLINK
    mbedtls_ssl_context ssl;
    bool have_ssl = false;
#endif
    ~Conn() {
#if C_SU2000_VRLINK
        if (have_ssl) mbedtls_ssl_free(&ssl);
#endif
    }
};

bool send_all(Conn &c, const char *p, size_t n) {
    if (!c.tls) return raw_send(c.s, p, n);
#if C_SU2000_VRLINK
    while (n) {
        const int r = mbedtls_ssl_write(&c.ssl, (const unsigned char *)p, n);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (r <= 0) return false;
        p += r; n -= (size_t)r;
    }
    return true;
#else
    return false;
#endif
}

/* Returns bytes read into buf, 0 on close / error. Blocks until data arrives. */
int conn_recv(Conn &c, char *buf, size_t cap) {
    if (!c.tls) return (int)recv(c.s, buf, (int)cap, 0);
#if C_SU2000_VRLINK
    for (;;) {
        const int r = mbedtls_ssl_read(&c.ssl, (unsigned char *)buf, cap);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        return r > 0 ? r : 0;
    }
#else
    return 0;
#endif
}

/* data already buffered inside the TLS layer (select() on the socket would not see it) */
bool conn_buffered(Conn &c) {
#if C_SU2000_VRLINK
    if (c.tls) return mbedtls_ssl_get_bytes_avail(&c.ssl) > 0 || mbedtls_ssl_check_pending(&c.ssl);
#endif
    (void)c;
    return false;
}

bool ws_send(Conn &c, uint8_t opcode, const char *data, size_t n) {
    std::string m;
    m.reserve(n + 10);
    m.push_back((char)(0x80 | opcode));
    if (n < 126) m.push_back((char)n);
    else if (n < 65536) { m.push_back(126); m.push_back((char)(n >> 8)); m.push_back((char)n); }
    else { m.push_back(127); for (int i = 7; i >= 0; i--) m.push_back((char)((uint64_t)n >> (i * 8))); }
    m.append(data, n);
    return send_all(c, m.data(), m.size());
}

/* ---- one WebSocket client, with its optional WebRTC peer connection ---- */
struct Client {
    std::atomic<int> player{ 1 };
    std::mutex out_mtx;
    std::vector<std::string> out_text;     /* signalling produced on libdatachannel threads, sent by the client thread */
    int pc = -1;
    std::atomic<int> dc{ -1 };
    std::atomic<bool> dc_open{ false };
    void queue_text(const std::string &s) { std::lock_guard<std::mutex> lk(out_mtx); out_text.push_back(s); }
};

#if C_SU2000_VRLINK
void RTC_API rtc_log(rtcLogLevel level, const char *msg) {
    static std::atomic<int> logged(0);
    if (logged++ < 200) LOG_MSG("SU2000: WebRTC %s: %s", level <= RTC_LOG_ERROR ? "error" : "warning", msg);
}

void RTC_API on_local_description(int, const char *sdp, const char *type, void *ptr) {
    if (strcmp(type, "answer") == 0) ((Client *)ptr)->queue_text(std::string("A ") + sdp);
}

void RTC_API on_local_candidate(int, const char *cand, const char *mid, void *ptr) {
    ((Client *)ptr)->queue_text(std::string("C ") + cand + "\n" + (mid ? mid : "0"));
}

void RTC_API on_dc_open(int, void *ptr) {
    Client *c = (Client *)ptr;
    if (!c->dc_open.exchange(true)) LOG_MSG("SU2000: VR client on WebRTC (UDP)");
}

void RTC_API on_dc_closed(int, void *ptr) { ((Client *)ptr)->dc_open = false; }

void RTC_API on_dc_message(int, const char *msg, int size, void *ptr) {
    Client *c = (Client *)ptr;
    if (size < 0) handle_input(msg, c->player);
    else handle_mic(msg, (size_t)size, c->player);
}

void RTC_API on_data_channel(int, int dc, void *ptr) {
    Client *c = (Client *)ptr;
    rtcSetUserPointer(dc, c);
    rtcSetOpenCallback(dc, on_dc_open);
    rtcSetClosedCallback(dc, on_dc_closed);
    rtcSetMessageCallback(dc, on_dc_message);
    c->dc = dc;
    if (rtcIsOpen(dc)) on_dc_open(dc, c);
}

void rtc_offer(Client &c, const std::string &sdp) {
    if (c.pc >= 0) { rtcDeletePeerConnection(c.pc); c.pc = -1; c.dc = -1; c.dc_open = false; }
    rtcConfiguration cfg;
    memset(&cfg, 0, sizeof(cfg));
    c.pc = rtcCreatePeerConnection(&cfg);
    if (c.pc < 0) return;
    rtcSetUserPointer(c.pc, &c);
    rtcSetLocalDescriptionCallback(c.pc, on_local_description);
    rtcSetLocalCandidateCallback(c.pc, on_local_candidate);
    rtcSetDataChannelCallback(c.pc, on_data_channel);
    rtcSetRemoteDescription(c.pc, sdp.c_str(), "offer");
}
#endif

/* sends a binary message on the data channel when it is open, else on the WebSocket */
bool client_send(Client &cl, Conn &c, const std::string &m, bool droppable) {
#if C_SU2000_VRLINK
    if (cl.dc_open) {
        const int dc = cl.dc;
        if (droppable && rtcGetBufferedAmount(dc) > (1 << 20)) return true;
        rtcSendMessage(dc, m.data(), (int)m.size());
        return true;
    }
#endif
    (void)cl;
    return ws_send(c, 2, m.data(), m.size());
}

bool send_frame(Client &cl, Conn &c, const Encoded &e) {
    const size_t max_piece = 60000;
    const unsigned eyes = (unsigned)e.jpg.size();
    for (unsigned k = 0; k < eyes; k++) {
        const std::string &j = e.jpg[k];
        const unsigned chunks = (unsigned)((j.size() + max_piece - 1) / max_piece);
        for (unsigned ch = 0; ch < chunks; ch++) {
            std::string m = "SUJ1";
            put32(m, e.seq);
            m.push_back((char)k); m.push_back((char)eyes);
            put16(m, (uint16_t)ch); put16(m, (uint16_t)chunks);
            put16(m, (uint16_t)e.w); put16(m, (uint16_t)e.h); put16(m, (uint16_t)((e.stereo ? 1 : 0) | (use_png ? 2 : 0)));
            m.append((const char *)e.pose, sizeof(e.pose));
            const size_t off = ch * max_piece;
            m.append(j, off, j.size() - off < max_piece ? j.size() - off : max_piece);
            if (!client_send(cl, c, m, true)) return false;
        }
    }
    return true;
}

void handle_ws_text(Client &cl, const std::string &msg) {
    if (msg.empty()) return;
    switch (msg[0]) {
    case 'S': {
        const int p = atoi(msg.c_str() + 1) == 2 ? 2 : 1;
        if (p != cl.player) {
            if (cl.player == 1) p1_clients--;
            if (p == 1) p1_clients++;
            cl.player = p;
            LOG_MSG("SU2000: VR client now on player %d", p);
        }
        break;
    }
#if C_SU2000_VRLINK
    case 'O':
        if (rtc_on && msg.size() > 2) rtc_offer(cl, msg.substr(2));
        break;
    case 'C':
        if (cl.pc >= 0 && msg.size() > 2) {
            const size_t nl = msg.find('\n');
            const std::string cand = msg.substr(2, nl == std::string::npos ? std::string::npos : nl - 2);
            const std::string mid = nl == std::string::npos ? "0" : msg.substr(nl + 1);
            rtcAddRemoteCandidate(cl.pc, cand.c_str(), mid.c_str());
        }
        break;
#endif
    default:
        handle_input(msg, cl.player);
    }
}

void serve_ws(Conn &c, std::string buf) {
    Client cl;
    p1_clients++;
    nclients++;
    LOG_MSG("SU2000: VR client connected%s", c.tls ? " (https)" : "");
    uint32_t sent_seq = 0;
    uint64_t audio_sent, mic_sent;
    {
        std::lock_guard<std::mutex> lk(aud_mtx);
        audio_sent = audio_seq; mic_sent = mic_seq;
    }
    bool ok = true;
    double stat_t0 = now_ms();
    unsigned stat_frames = 0;
    size_t stat_bytes = 0;
    while (ok && running) {
        if (now_ms() - stat_t0 >= 10000.0) {
            const double s = (now_ms() - stat_t0) / 1000.0;
            LOG_MSG("SU2000: VR client (player %d) via %s: %.0f frames/s, %.0f KB/s", cl.player.load(),
                    cl.dc_open ? "WebRTC" : "WebSocket", stat_frames / s, stat_bytes / s / 1024.0);
            stat_t0 = now_ms(); stat_frames = 0; stat_bytes = 0;
        }
        /* signalling from libdatachannel threads */
        std::vector<std::string> texts;
        {
            std::lock_guard<std::mutex> lk(cl.out_mtx);
            texts.swap(cl.out_text);
        }
        for (const auto &t : texts) if (!ws_send(c, 1, t.data(), t.size())) ok = false;
        /* new frame? */
        uint32_t seq;
        {
            std::lock_guard<std::mutex> lk(mtx);
            seq = frame_seq;
        }
        if (seq != sent_seq) {
            sent_seq = seq;
            auto e = encoded(cl.player);
            if (e) {
                if (!send_frame(cl, c, *e)) break;
                stat_frames++;
                for (const auto &j : e->jpg) stat_bytes += j.size();
            }
        }
        /* game audio and the other player's microphone */
        std::vector<std::string> pk;
        {
            std::lock_guard<std::mutex> lk(aud_mtx);
            for (const auto &p : audio_q) if (p.seq > audio_sent) pk.push_back(p.data);
            audio_sent = audio_seq;
            for (const auto &p : mic_q) if (p.seq > mic_sent && p.from != cl.player) pk.push_back(p.data);
            mic_sent = mic_seq;
        }
        for (const auto &p : pk) if (!client_send(cl, c, p, false)) ok = false;
        if (!conn_buffered(c)) {
            fd_set rs;
            FD_ZERO(&rs);
            FD_SET(c.s, &rs);
            timeval tv = { 0, 2000 };
            const int r = select((int)c.s + 1, &rs, NULL, NULL, &tv);
            if (r < 0) break;
            if (r == 0) continue;
        }
        char tmp[4096];
        const int n = conn_recv(c, tmp, sizeof(tmp));
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
            if (op == 1) handle_ws_text(cl, payload);
            else if (op == 2) handle_mic(payload.data(), payload.size(), cl.player);
            else if (op == 8) { ws_send(c, 8, payload.data(), payload.size() < 2 ? payload.size() : 2); ok = false; break; }
            else if (op == 9) ws_send(c, 10, payload.data(), payload.size());
        }
    }
#if C_SU2000_VRLINK
    if (cl.pc >= 0) rtcDeletePeerConnection(cl.pc);
#endif
    if (cl.player == 1) p1_clients--;
    nclients--;
    LOG_MSG("SU2000: VR client disconnected");
}

void serve_conn(Conn &c) {
    unsigned char first = 0;
    if (recv(c.s, (char *)&first, 1, MSG_PEEK) != 1) return;
    if (first == 0x16) {
#if C_SU2000_VRLINK
        if (!tls_ready) return;
        mbedtls_ssl_init(&c.ssl);
        c.have_ssl = true;
        if (mbedtls_ssl_setup(&c.ssl, &tls_conf) != 0) return;
        mbedtls_ssl_set_bio(&c.ssl, &c.s, bio_send, bio_recv, NULL);
        int r;
        while ((r = mbedtls_ssl_handshake(&c.ssl)) != 0) {
            if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            if (r == MBEDTLS_ERR_SSL_CONN_EOF) return;     /* browsers open spare connections and close them unused */
            static std::atomic<int> logged(0);
            if (logged++ < 8) LOG_MSG("SU2000: VR TLS handshake failed (-%04x)", (unsigned)-r);
            return;
        }
        c.tls = true;
#else
        return;
#endif
    }
    std::string req;
    char tmp[2048];
    while (req.find("\r\n\r\n") == std::string::npos && req.size() < 16384) {
        const int n = conn_recv(c, tmp, sizeof(tmp));
        if (n <= 0) return;
        req.append(tmp, (size_t)n);
    }
    const size_t hend = req.find("\r\n\r\n");
    const std::string rest = hend == std::string::npos ? std::string() : req.substr(hend + 4);
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
        if (send_all(c, resp.data(), resp.size())) {
            int one = 1;
            setsockopt(c.s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
            serve_ws(c, rest);
        }
    } else if (path == "/" || path.compare(0, 2, "/?") == 0 || path == "/index.html") {
        const std::string body = XR_ClientPage();
        char hdr[256];
        snprintf(hdr, sizeof(hdr), "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %u\r\n"
                 "Cache-Control: no-cache\r\nConnection: close\r\n\r\n", (unsigned)body.size());
        send_all(c, hdr, strlen(hdr));
        send_all(c, body.data(), body.size());
    } else {
        static const char nf[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send_all(c, nf, sizeof(nf) - 1);
    }
}

void serve(sock_t s) {
    {
        Conn c;
        c.s = s;
        serve_conn(c);
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

/* IPv4 addresses of this machine (for the https URLs in the log) */
std::vector<std::string> lan_addresses(void) {
    std::vector<std::string> out;
#ifdef _WIN32
    char host[256];
    addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    if (gethostname(host, sizeof(host)) == 0 && getaddrinfo(host, NULL, &hints, &res) == 0) {
        for (addrinfo *r = res; r; r = r->ai_next) {
            const uint8_t *b = (const uint8_t *)&((sockaddr_in *)r->ai_addr)->sin_addr;
            char ip[32];
            snprintf(ip, sizeof(ip), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
            out.push_back(ip);
        }
        freeaddrinfo(res);
    }
#else
    ifaddrs *ifa = NULL;
    if (getifaddrs(&ifa) == 0) {
        for (ifaddrs *i = ifa; i; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
            const uint8_t *b = (const uint8_t *)&((sockaddr_in *)i->ifa_addr)->sin_addr;
            if (b[0] == 127) continue;
            char ip[32];
            snprintf(ip, sizeof(ip), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
            out.push_back(ip);
        }
        freeifaddrs(ifa);
    }
#endif
    return out;
}

} // namespace

void XR_Configure(const char *format, int quality, bool audio, bool webrtc, const char *certificate) {
    use_png = !(format && (strcmp(format, "jpeg") == 0 || strcmp(format, "jpg") == 0));
    jpeg_quality = quality < 10 ? 10 : quality > 100 ? 100 : quality;
    audio_on = audio;
    rtc_on = webrtc;
    if (certificate && *certificate) cert_base = certificate;
}

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
#if C_SU2000_VRLINK
    static bool rtc_logger = false;
    if (!rtc_logger) { rtcInitLogger(RTC_LOG_WARNING, rtc_log); rtc_logger = true; }
    const bool https = tls_init();
#else
    const bool https = false;
#endif
    for (int i = 0; i < 2; i++) { mic_level[i] = 0; mic_time[i] = 0.0; for (int k = 0; k < 7; k++) frame_pose[i][k] = k == 6 ? 1.0f : 0.0f; }
    MIXER_TapCallback = audio_tap;
    running = true;
    accept_thread = std::thread(accept_loop);
    LOG_MSG("SU2000: VR server on http://localhost:%d/", port);
    if (https)
        for (const auto &ip : lan_addresses())
            LOG_MSG("SU2000: VR server on https://%s:%d/ (self-signed: accept the warning once)", ip.c_str(), port);
#if !C_SU2000_VRLINK
    LOG_MSG("SU2000: VR server built without mbedTLS / libdatachannel: no https, no WebRTC");
#endif
}

void XR_Shutdown(void) {
    if (!running) return;
    MIXER_TapCallback = NULL;
    running = false;
    if (accept_thread.joinable()) accept_thread.join();
    sock_close(listener);
    listener = SOCK_BAD;
    for (int i = 0; i < 300 && live_threads > 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::lock_guard<std::mutex> lk(mtx);
    channels.clear();
}

bool XR_Active(void) { return running && p1_clients > 0; }

void XR_PushFrame(const PixFrame *ch, unsigned n, bool stereo) {
    if (!running || nclients <= 0) return;
    std::lock_guard<std::mutex> lk(mtx);
    channels.assign(ch, ch + n);
    chan_stereo = stereo && n >= 2;
    for (int p = 0; p < 2; p++)
        for (int i = 0; i < 7; i++) frame_pose[p][i] = (float)inputs[p].head[i];
    frame_seq++;
}

void XR_Poll(void) {
    if (!running) return;
    /* microphone level -> format card (CTRL_GetMic), silent after 200 ms without packets */
    const double t = now_ms();
    for (int p = 0; p < 2; p++) FCARD_SetMicLevel((unsigned)p, (uint8_t)(t - mic_time[p] < 200.0 ? mic_level[p].load() : 0));
    Input in;
    bool fresh;
    {
        std::lock_guard<std::mutex> lk(mtx);
        in = inputs[0];
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
