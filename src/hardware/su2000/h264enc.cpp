/*
 *  H.264 encoding of the headset pictures with OpenH264, loaded at run time (see h264enc.h).
 *  Settings follow findings/video_streaming.md: no B-frames (Constrained Baseline), two reference pictures so a
 *  stream of alternating left / right eye pictures predicts each eye from the other one and from itself, quality
 *  rate control with a cap, a key picture every 2 s and on request.
 */

#include "h264enc.h"
#include "logging.h"

#include <string.h>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "openh264/codec_api.h"

namespace {

typedef int (*CreateFn)(ISVCEncoder **);
typedef void (*DestroyFn)(ISVCEncoder *);
CreateFn create_fn = nullptr;
DestroyFn destroy_fn = nullptr;
std::mutex load_mtx;
bool tried = false;
std::string status = "not loaded";

void *open_lib(const char *name) {
#ifdef _WIN32
    return (void *)LoadLibraryA(name);
#else
    return dlopen(name, RTLD_NOW | RTLD_LOCAL);
#endif
}

void *sym(void *lib, const char *s) {
#ifdef _WIN32
    return (void *)GetProcAddress((HMODULE)lib, s);
#else
    return dlsym(lib, s);
#endif
}

/* 0x00RRGGBB -> I420 (BT.601 limited range, what browsers assume for SD video without colour information) */
void to_i420(const uint32_t *px, unsigned stride, unsigned w, unsigned h, uint8_t *y, uint8_t *u, uint8_t *v) {
    for (unsigned j = 0; j < h; j++) {
        const uint32_t *row = px + (size_t)j * stride;
        uint8_t *yr = y + (size_t)j * w;
        for (unsigned i = 0; i < w; i++) {
            const int r = (row[i] >> 16) & 255, g = (row[i] >> 8) & 255, b = row[i] & 255;
            yr[i] = (uint8_t)((66 * r + 129 * g + 25 * b + 128) / 256 + 16);
        }
    }
    for (unsigned j = 0; j < h / 2; j++) {
        const uint32_t *r0 = px + (size_t)(2 * j) * stride, *r1 = r0 + stride;
        for (unsigned i = 0; i < w / 2; i++) {
            int r = 0, g = 0, b = 0;
            const uint32_t q[4] = { r0[2 * i], r0[2 * i + 1], r1[2 * i], r1[2 * i + 1] };
            for (int k = 0; k < 4; k++) { r += (q[k] >> 16) & 255; g += (q[k] >> 8) & 255; b += q[k] & 255; }
            r /= 4; g /= 4; b /= 4;
            u[(size_t)j * (w / 2) + i] = (uint8_t)((-38 * r - 74 * g + 112 * b + 128) / 256 + 128);
            v[(size_t)j * (w / 2) + i] = (uint8_t)((112 * r - 94 * g - 18 * b + 128) / 256 + 128);
        }
    }
}

} // namespace

unsigned H264Encoder::s_kbps = 4000, H264Encoder::s_max_qp = 36;

bool H264_Load(const char *name) {
    std::lock_guard<std::mutex> lk(load_mtx);
    if (tried) return create_fn != nullptr;
    tried = true;
#ifdef _WIN32
    static const char *defaults[] = { "openh264-2.4.1-win64.dll", "openh264-2.5.0-win64.dll", "openh264-2.6.0-win64.dll", "openh264.dll", nullptr };
#else
    static const char *defaults[] = { "libopenh264.so.7", "libopenh264.so.8", "libopenh264.so", nullptr };
#endif
    void *lib = nullptr;
    std::string used;
    if (name && *name) { lib = open_lib(name); used = name; }
    for (int i = 0; !lib && defaults[i]; i++) { lib = open_lib(defaults[i]); used = defaults[i]; }
    if (!lib) {
        status = "OpenH264 library not found (headset pictures stay PNG / JPEG)";
        LOG_MSG("SU2000: %s", status.c_str());
        return false;
    }
    create_fn = (CreateFn)sym(lib, "WelsCreateSVCEncoder");
    destroy_fn = (DestroyFn)sym(lib, "WelsDestroySVCEncoder");
    if (!create_fn || !destroy_fn) {
        create_fn = nullptr;
        status = "OpenH264 library without an encoder";
        LOG_MSG("SU2000: %s (%s)", status.c_str(), used.c_str());
        return false;
    }
    status = "OpenH264 loaded from " + used;
    LOG_MSG("SU2000: %s: headsets that support it get H.264 video", status.c_str());
    return true;
}

bool H264_Available(void) { std::lock_guard<std::mutex> lk(load_mtx); return create_fn != nullptr; }
const char *H264_Status(void) { return status.c_str(); }

H264Encoder::~H264Encoder() { close(); }

void H264Encoder::close() {
    if (!enc) return;
    ISVCEncoder *e = (ISVCEncoder *)enc;
    e->Uninitialize();
    destroy_fn(e);
    enc = nullptr;
    ew = eh = 0;
}

bool H264Encoder::open(unsigned w, unsigned h) {
    close();
    if (!create_fn) return false;
    ISVCEncoder *e = nullptr;
    if (create_fn(&e) != 0 || !e) return false;
    SEncParamExt p;
    memset(&p, 0, sizeof(p));
    e->GetDefaultParams(&p);
    p.iUsageType = SCREEN_CONTENT_REAL_TIME;      /* sharp flat-shaded pictures, low latency */
    p.iPicWidth = (int)w;
    p.iPicHeight = (int)h;
    p.fMaxFrameRate = 100.0f;                      /* alternating eyes at 50 stereo frames/s */
    p.iRCMode = RC_QUALITY_MODE;
    p.iTargetBitrate = (int)(s_kbps * 1000);
    p.iMaxBitrate = (int)(s_kbps * 1500);
    p.iMaxQp = (int)s_max_qp;
    p.iMinQp = 12;
    p.bEnableFrameSkip = false;                    /* every picture is needed: the eyes alternate */
    p.uiIntraPeriod = 200;                         /* key picture every 2 s (both eyes) */
    p.iNumRefFrame = 2;                            /* the other eye and the same eye */
    p.iTemporalLayerNum = 1;
    p.iSpatialLayerNum = 1;
    p.iEntropyCodingModeFlag = 0;                  /* CAVLC: Constrained Baseline, decoded everywhere */
    p.eSpsPpsIdStrategy = CONSTANT_ID;
    p.iMultipleThreadIdc = 1;
    p.bEnableDenoise = false;
    p.bEnableBackgroundDetection = false;
    p.bEnableAdaptiveQuant = false;
    p.bEnableSceneChangeDetect = false;            /* would turn every left / right switch into a key picture */
    p.bEnableLongTermReference = false;
    p.bEnableFrameCroppingFlag = true;
    p.sSpatialLayers[0].iVideoWidth = (int)w;
    p.sSpatialLayers[0].iVideoHeight = (int)h;
    p.sSpatialLayers[0].fFrameRate = 100.0f;
    p.sSpatialLayers[0].iSpatialBitrate = p.iTargetBitrate;
    p.sSpatialLayers[0].iMaxSpatialBitrate = p.iMaxBitrate;
    p.sSpatialLayers[0].sSliceArgument.uiSliceMode = SM_SINGLE_SLICE;
    if (e->InitializeExt(&p) != 0) { destroy_fn(e); return false; }
    int fmt = videoFormatI420;
    e->SetOption(ENCODER_OPTION_DATAFORMAT, &fmt);
    enc = e;
    ew = w; eh = h;
    frames = 0;
    return true;
}

bool H264Encoder::encode(const uint32_t *pixels, unsigned stride, unsigned w, unsigned h, bool force_key, std::string &out, bool &key) {
    out.clear();
    key = false;
    w &= ~1u; h &= ~1u;
    if (!w || !h) return false;
    if (!enc || w != ew || h != eh) {
        if (!open(w, h)) return false;
        force_key = true;
    }
    ISVCEncoder *e = (ISVCEncoder *)enc;
    yuv.resize((size_t)w * h * 3 / 2);
    uint8_t *y = (uint8_t *)&yuv[0], *u = y + (size_t)w * h, *v = u + (size_t)w * h / 4;
    to_i420(pixels, stride, w, h, y, u, v);
    if (force_key) e->ForceIntraFrame(true);
    SSourcePicture pic;
    memset(&pic, 0, sizeof(pic));
    pic.iColorFormat = videoFormatI420;
    pic.iPicWidth = (int)w;
    pic.iPicHeight = (int)h;
    pic.iStride[0] = (int)w; pic.iStride[1] = pic.iStride[2] = (int)w / 2;
    pic.pData[0] = y; pic.pData[1] = u; pic.pData[2] = v;
    pic.uiTimeStamp = (long long)(frames++ * 10);   /* ms at 100 pictures/s */
    SFrameBSInfo info;
    memset(&info, 0, sizeof(info));
    if (e->EncodeFrame(&pic, &info) != cmResultSuccess) return false;
    if (info.eFrameType == videoFrameTypeSkip || info.eFrameType == videoFrameTypeInvalid) return false;
    key = info.eFrameType == videoFrameTypeIDR;
    for (int l = 0; l < info.iLayerNum; l++) {
        const SLayerBSInfo &li = info.sLayerInfo[l];
        size_t n = 0;
        for (int k = 0; k < li.iNalCount; k++) n += (size_t)li.pNalLengthInByte[k];
        out.append((const char *)li.pBsBuf, n);
    }
    return !out.empty();
}
