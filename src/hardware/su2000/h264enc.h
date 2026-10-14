/*
 *  H.264 encoding of the headset pictures with OpenH264 (Cisco, BSD-2-Clause), loaded at run time so DOSBox-X needs
 *  no build dependency: Cisco's prebuilt binary (openh264-<version>-win64.dll / libopenh264.so.7) carries the H.264
 *  patent licence. Without the library the headset link keeps sending PNG / JPEG images.
 */
#ifndef DOSBOX_SU2000_H264ENC_H
#define DOSBOX_SU2000_H264ENC_H

#include <stdint.h>
#include <string>

/* load the library once; name = file name or path ("" = the default names). Returns false if it is not there. */
bool H264_Load(const char *name);
bool H264_Available(void);
const char *H264_Status(void);

class H264Encoder {
public:
    ~H264Encoder();
    /* Encode one 0x00RRGGBB picture (width / height even; reopened on a size change). force_key asks for an IDR
       picture. out gets the Annex B stream of the picture (SPS / PPS before every IDR); key tells whether it is one. */
    bool encode(const uint32_t *pixels, unsigned stride, unsigned w, unsigned h, bool force_key, std::string &out, bool &key);
    static void configure(unsigned kbps, unsigned max_qp) { s_kbps = kbps; s_max_qp = max_qp; }
private:
    bool open(unsigned w, unsigned h);
    void close();
    void *enc = nullptr;
    unsigned ew = 0, eh = 0;
    uint64_t frames = 0;
    std::string yuv;
    static unsigned s_kbps, s_max_qp;
};

#endif
