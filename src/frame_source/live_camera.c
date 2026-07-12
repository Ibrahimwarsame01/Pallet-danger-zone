// live_camera.c — Role A
//
// Live capture on QNX 8 via the QNX Sensor Framework camera API
// (camera/camera_api.h) — the same stack camera_example3_viewfinder uses.
// OpenCV's own capture path is not usable here: the C capture API (CvCapture)
// was removed in OpenCV 4, and QNX has no V4L2 backend for the Pi camera.
//
// The viewfinder callback converts whatever pixel format the driver delivers
// (RGB8888 / NV12 / YCbYCr / CbYCrY) into a 3-channel BGR IplImage — the
// format every downstream module expects. get_next() then swaps that buffer
// out under a mutex, so the callback never scribbles on a frame the caller
// is still analyzing.

#include "live_camera.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

#ifdef __QNXNTO__

#include <camera/camera_api.h>
#include <camera/camera_3a.h>   /* camera_set_3a_lock */

#define REQ_WIDTH             640
#define REQ_HEIGHT            480
#define REQ_FPS               30.0
#define FIRST_FRAME_TIMEOUT_S 5
#define FRAME_TIMEOUT_S       2

/* Freeze exposure after warmup: the reference and motion detectors diff raw
 * pixel values, and measured AE drift alone shifts 20-50 gray levels across
 * the whole frame over a few minutes — far above any usable pixel threshold.
 * The Pi camera driver supports no 3A lock and no auto WB control (probed:
 * lock modes = NONE, wb modes = DEFAULT only), but it does support
 * CAMERA_EXPOSUREMODE_ISO_SHUTTER_PRIORITY with manual ISO (112..960) and
 * manual shutter in MICROSECONDS (1..65487). So after warmup we switch to
 * that mode and feedback-tune ISO+shutter until mean frame brightness lands
 * in [EXP_MEAN_LO, EXP_MEAN_HI], then leave the values frozen. */
#define EXP_WARMUP_FRAMES 30
#define EXP_SETTLE_FRAMES 8     /* frames to wait after each set before measuring */
#define EXP_MAX_ITERS     6
#define EXP_MEAN_LO       70
#define EXP_MEAN_HI       150
#define EXP_MEAN_TARGET   110.0

/* Indoor lights flicker at 2x the mains frequency; a shutter that is not a
 * whole multiple of the half-cycle rolls visible bands through every frame
 * (measured: +/-50 gray levels frame-to-frame). Snap shutter to multiples of
 * this and let ISO absorb the brightness difference.
 * 8333 us = 60 Hz mains; use 10000.0 for 50 Hz regions. */
#define EXP_FLICKER_HALF_PERIOD_US 8333.0

struct LiveCamera {
    camera_handle_t handle;
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    IplImage       *pending;   /* written by the viewfinder callback */
    IplImage       *current;   /* handed out to the caller */
    uint64_t        pending_ts;
    bool            has_new;
    bool            got_first;
    bool            running;
    int             next_frame_id;
    Frame           frame;

    /* manual-exposure freeze state */
    enum { EXP_AUTO = 0, EXP_TUNING, EXP_FROZEN } exp_state;
    int      exp_iter;
    int      exp_settle;
    unsigned iso_lo, iso_hi, cur_iso;
    double   sh_lo, sh_hi, cur_sh;   /* shutter, driver units (microseconds) */
};

/* Mean brightness of a BGR frame, sparsely sampled. */
static double frame_mean(const IplImage *img) {
    if (!img) return 0.0;
    long sum = 0, cnt = 0;
    for (int y = 0; y < img->height; y += 16) {
        const uint8_t *row = (const uint8_t *)img->imageData + (size_t)y * img->widthStep;
        for (int x = 0; x < img->width; x += 16) {
            const uint8_t *p = row + (size_t)x * img->nChannels;
            sum += p[0] + p[1] + p[2];
            cnt += 3;
        }
    }
    return cnt ? (double)sum / cnt : 0.0;
}

static double clampd(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static void exp_apply(LiveCamera *cam) {
    camera_set_manual_iso(cam->handle, cam->cur_iso);
    camera_set_manual_shutter_speed(cam->handle, cam->cur_sh);
    cam->exp_settle = 0;
}

/* Switch to ISO+shutter priority and start the brightness feedback loop. */
static void exp_begin(LiveCamera *cam) {
    if (camera_set_exposure_mode(cam->handle,
            CAMERA_EXPOSUREMODE_ISO_SHUTTER_PRIORITY) != CAMERA_EOK) {
        fprintf(stderr, "[live_camera] warning: manual exposure unsupported; "
                        "expect AE drift in reference diffs\n");
        cam->exp_state = EXP_FROZEN;
        return;
    }
    unsigned n = 0; bool maxmin = false;
    unsigned isos[2] = { 400, 400 };
    double   shs[2]  = { 16667.0, 16667.0 };
    cam->iso_lo = 112; cam->iso_hi = 960;         /* probed defaults */
    if (camera_get_supported_manual_iso_values(cam->handle, 2, &n, isos, &maxmin)
            == CAMERA_EOK && n >= 2) {
        cam->iso_lo = isos[0] < isos[1] ? isos[0] : isos[1];
        cam->iso_hi = isos[0] < isos[1] ? isos[1] : isos[0];
    }
    cam->sh_lo = 1.0; cam->sh_hi = 65487.0;
    if (camera_get_supported_manual_shutter_speeds(cam->handle, 2, &n, shs, &maxmin)
            == CAMERA_EOK && n >= 2) {
        cam->sh_lo = shs[0] < shs[1] ? shs[0] : shs[1];
        cam->sh_hi = shs[0] < shs[1] ? shs[1] : shs[0];
    }
    cam->cur_iso = (unsigned)clampd(400, cam->iso_lo, cam->iso_hi);
    cam->cur_sh  = clampd(2.0 * EXP_FLICKER_HALF_PERIOD_US,
                          cam->sh_lo, cam->sh_hi);  /* ~1/60 s, flicker-safe */
    exp_apply(cam);
    cam->exp_iter  = 0;
    cam->exp_state = EXP_TUNING;
}

/* One tuning step: nudge shutter (then ISO at the rails) toward the target
 * mean, or freeze if brightness is already acceptable. */
static void exp_tune(LiveCamera *cam, const IplImage *img) {
    if (++cam->exp_settle < EXP_SETTLE_FRAMES) return;

    double mean = frame_mean(img);
    if ((mean >= EXP_MEAN_LO && mean <= EXP_MEAN_HI) ||
        cam->exp_iter >= EXP_MAX_ITERS) {
        cam->exp_state = EXP_FROZEN;
        fprintf(stderr, "[live_camera] exposure frozen: iso=%u shutter=%.0fus "
                        "mean=%.0f (%s)\n",
                cam->cur_iso, cam->cur_sh, mean,
                cam->exp_iter >= EXP_MAX_ITERS ? "iteration cap" : "in band");
        return;
    }
    double factor = clampd(EXP_MEAN_TARGET / (mean < 1.0 ? 1.0 : mean), 0.3, 3.5);
    double want   = cam->cur_sh * factor;
    /* snap to a flicker-safe multiple of the mains half-cycle (min 1 cycle) */
    double cycles = (double)(long)(want / EXP_FLICKER_HALF_PERIOD_US + 0.5);
    if (cycles < 1.0) cycles = 1.0;
    double sh = clampd(cycles * EXP_FLICKER_HALF_PERIOD_US, cam->sh_lo, cam->sh_hi);
    if (want != sh) {
        /* snapping/rails changed the light gathered — push the rest into ISO */
        double iso = clampd((double)cam->cur_iso * (want / sh),
                            (double)cam->iso_lo, (double)cam->iso_hi);
        cam->cur_iso = (unsigned)iso;
    }
    cam->cur_sh = sh;
    cam->exp_iter++;
    exp_apply(cam);
}

static uint8_t clamp8(int v) {
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

/* BT.601 full-swing integer conversion. */
static void yuv_to_bgr(int y, int cb, int cr, uint8_t *bgr) {
    int c = y - 16, d = cb - 128, e = cr - 128;
    bgr[0] = clamp8((298 * c + 516 * d + 128) >> 8);
    bgr[1] = clamp8((298 * c - 100 * d - 208 * e + 128) >> 8);
    bgr[2] = clamp8((298 * c + 409 * e + 128) >> 8);
}

/* (Re)allocate the callback-side buffer if the frame geometry changed. */
static bool ensure_pending(LiveCamera *cam, int w, int h) {
    if (cam->pending && (cam->pending->width != w || cam->pending->height != h)) {
        cvReleaseImage(&cam->pending);
    }
    if (!cam->pending) {
        cam->pending = cvCreateImage(cvSize(w, h), IPL_DEPTH_8U, 3);
    }
    return cam->pending != NULL;
}

/* QNX RGB8888 is B,G,R,A in memory — drop alpha, keep BGR order. */
static void convert_rgb8888(IplImage *dst, const uint8_t *src,
                            uint32_t w, uint32_t h, uint32_t stride) {
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *s = src + (size_t)y * stride;
        uint8_t *d = (uint8_t *)dst->imageData + (size_t)y * dst->widthStep;
        for (uint32_t x = 0; x < w; x++) {
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
            d += 3; s += 4;
        }
    }
}

static void convert_nv12(IplImage *dst, const uint8_t *ybase, const uint8_t *uvbase,
                         uint32_t w, uint32_t h, uint32_t stride, uint32_t uv_stride) {
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *yrow  = ybase + (size_t)y * stride;
        const uint8_t *uvrow = uvbase + (size_t)(y / 2) * uv_stride;
        uint8_t *d = (uint8_t *)dst->imageData + (size_t)y * dst->widthStep;
        for (uint32_t x = 0; x < w; x++) {
            uint32_t uvx = x & ~1u;
            yuv_to_bgr(yrow[x], uvrow[uvx], uvrow[uvx + 1], d);
            d += 3;
        }
    }
}

/* Packed 4:2:2 — YCbYCr (yoff=0,cb=1,cr=3) or CbYCrY (yoff=1,cb=0,cr=2). */
static void convert_yuv422(IplImage *dst, const uint8_t *src,
                           uint32_t w, uint32_t h, uint32_t stride,
                           int yoff, int cboff, int croff) {
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *row = src + (size_t)y * stride;
        uint8_t *d = (uint8_t *)dst->imageData + (size_t)y * dst->widthStep;
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t *pair = row + (size_t)(x & ~1u) * 2;
            int yv = pair[yoff + ((x & 1u) ? 2 : 0)];
            yuv_to_bgr(yv, pair[cboff], pair[croff], d);
            d += 3;
        }
    }
}

static void vf_callback(camera_handle_t handle, camera_buffer_t *buf, void *arg) {
    (void)handle;
    LiveCamera *cam = (LiveCamera *)arg;
    bool converted = false;

    pthread_mutex_lock(&cam->lock);
    if (!cam->running) {
        pthread_mutex_unlock(&cam->lock);
        return;
    }

    switch (buf->frametype) {
    case CAMERA_FRAMETYPE_RGB8888: {
        uint32_t w = buf->framedesc.rgb8888.width;
        uint32_t h = buf->framedesc.rgb8888.height;
        if (ensure_pending(cam, (int)w, (int)h)) {
            convert_rgb8888(cam->pending, buf->framebuf, w, h,
                            buf->framedesc.rgb8888.stride);
            converted = true;
        }
        break;
    }
    case CAMERA_FRAMETYPE_NV12: {
        uint32_t w = buf->framedesc.nv12.width;
        uint32_t h = buf->framedesc.nv12.height;
        if (ensure_pending(cam, (int)w, (int)h)) {
            convert_nv12(cam->pending, buf->framebuf,
                         buf->framebuf + (size_t)buf->framedesc.nv12.uv_offset,
                         w, h, buf->framedesc.nv12.stride,
                         buf->framedesc.nv12.uv_stride);
            converted = true;
        }
        break;
    }
    case CAMERA_FRAMETYPE_YCBYCR: {
        uint32_t w = buf->framedesc.ycbycr.width;
        uint32_t h = buf->framedesc.ycbycr.height;
        if (ensure_pending(cam, (int)w, (int)h)) {
            convert_yuv422(cam->pending, buf->framebuf, w, h,
                           buf->framedesc.ycbycr.stride, 0, 1, 3);
            converted = true;
        }
        break;
    }
    case CAMERA_FRAMETYPE_CBYCRY: {
        uint32_t w = buf->framedesc.cbycry.width;
        uint32_t h = buf->framedesc.cbycry.height;
        if (ensure_pending(cam, (int)w, (int)h)) {
            convert_yuv422(cam->pending, buf->framebuf, w, h,
                           buf->framedesc.cbycry.stride, 1, 0, 2);
            converted = true;
        }
        break;
    }
    default: {
        static bool warned = false;
        if (!warned) {
            warned = true;
            fprintf(stderr, "[live_camera] unsupported frametype %d from driver\n",
                    (int)buf->frametype);
        }
        break;
    }
    }

    if (converted) {
        cam->pending_ts = now_ms();
        cam->has_new = true;
        pthread_cond_signal(&cam->cond);
    }
    pthread_mutex_unlock(&cam->lock);
}

static void status_callback(camera_handle_t handle, camera_devstatus_t status,
                            uint16_t extra, void *arg) {
    (void)handle; (void)arg;
    fprintf(stderr, "[live_camera] device status: %d (extra %u)\n",
            (int)status, (unsigned)extra);
}

LiveCamera *live_camera_open(int device_index) {
    LiveCamera *cam = calloc(1, sizeof(*cam));
    if (!cam) {
        return NULL;
    }
    pthread_mutex_init(&cam->lock, NULL);
    pthread_cond_init(&cam->cond, NULL);
    cam->running = true;

    camera_unit_t unit = (device_index == 1) ? CAMERA_UNIT_2 : CAMERA_UNIT_1;
    camera_error_t err = camera_open(unit, CAMERA_MODE_RW | CAMERA_MODE_ROLL, &cam->handle);
    if (err != CAMERA_EOK) {
        err = camera_open(unit, CAMERA_MODE_RO, &cam->handle);
    }
    if (err != CAMERA_EOK) {
        fprintf(stderr, "[live_camera] camera_open(unit %d) failed: %d\n",
                (int)unit, (int)err);
        goto fail_noclose;
    }

    if (camera_set_vf_mode(cam->handle, CAMERA_VFMODE_VIDEO) != CAMERA_EOK) {
        fprintf(stderr, "[live_camera] VFMODE_VIDEO not accepted; using driver default\n");
    }

    static const camera_frametype_t wanted[] = {
        CAMERA_FRAMETYPE_RGB8888,
        CAMERA_FRAMETYPE_NV12,
        CAMERA_FRAMETYPE_YCBYCR,
        CAMERA_FRAMETYPE_CBYCRY,
    };
    bool fmt_ok = false;
    for (size_t i = 0; i < sizeof(wanted) / sizeof(wanted[0]) && !fmt_ok; i++) {
        fmt_ok = camera_set_vf_property(cam->handle,
                                        CAMERA_IMGPROP_WIDTH,  REQ_WIDTH,
                                        CAMERA_IMGPROP_HEIGHT, REQ_HEIGHT,
                                        CAMERA_IMGPROP_FORMAT, wanted[i]) == CAMERA_EOK;
    }
    if (!fmt_ok) {
        fprintf(stderr, "[live_camera] driver rejected requested formats/size; "
                        "using its defaults (callback adapts to actual frames)\n");
    }
    /* Best effort — some drivers refuse an explicit rate. */
    (void)camera_set_vf_property(cam->handle, CAMERA_IMGPROP_FRAMERATE, (double)REQ_FPS);

    err = camera_start_viewfinder(cam->handle, vf_callback, status_callback, cam);
    if (err != CAMERA_EOK) {
        fprintf(stderr, "[live_camera] camera_start_viewfinder failed: %d\n", (int)err);
        camera_close(cam->handle);
        goto fail_noclose;
    }
    return cam;

fail_noclose:
    pthread_cond_destroy(&cam->cond);
    pthread_mutex_destroy(&cam->lock);
    free(cam);
    return NULL;
}

Frame *live_camera_read(LiveCamera *cam) {
    if (!cam) {
        return NULL;
    }

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += cam->got_first ? FRAME_TIMEOUT_S : FIRST_FRAME_TIMEOUT_S;

    pthread_mutex_lock(&cam->lock);
    int rc = 0;
    while (!cam->has_new && rc == 0) {
        rc = pthread_cond_timedwait(&cam->cond, &cam->lock, &deadline);
    }
    if (!cam->has_new) {
        pthread_mutex_unlock(&cam->lock);
        fprintf(stderr, "[live_camera] no frame from camera within timeout\n");
        return NULL;
    }
    /* Swap: caller gets the freshly converted frame; the callback reuses the
     * buffer the caller just finished with. */
    IplImage *tmp = cam->current;
    cam->current  = cam->pending;
    cam->pending  = tmp;
    cam->has_new  = false;
    cam->got_first = true;
    uint64_t ts = cam->pending_ts;
    pthread_mutex_unlock(&cam->lock);

    cam->frame.image        = cam->current;
    cam->frame.timestamp_ms = ts;
    cam->frame.frame_id     = cam->next_frame_id++;

    if (cam->exp_state == EXP_AUTO && cam->frame.frame_id >= EXP_WARMUP_FRAMES) {
        exp_begin(cam);
    } else if (cam->exp_state == EXP_TUNING) {
        exp_tune(cam, cam->frame.image);
    }
    return &cam->frame;
}

void live_camera_close(LiveCamera *cam) {
    if (!cam) {
        return;
    }
    pthread_mutex_lock(&cam->lock);
    cam->running = false;
    pthread_mutex_unlock(&cam->lock);

    camera_stop_viewfinder(cam->handle);
    camera_close(cam->handle);

    if (cam->pending) cvReleaseImage(&cam->pending);
    if (cam->current) cvReleaseImage(&cam->current);
    pthread_cond_destroy(&cam->cond);
    pthread_mutex_destroy(&cam->lock);
    free(cam);
}

#else /* !__QNXNTO__ — host build: live capture unavailable, keep linking. */

struct LiveCamera { int unused; };

LiveCamera *live_camera_open(int device_index) {
    (void)device_index;
    fprintf(stderr, "[live_camera] live capture requires QNX (camera_api); "
                    "use --source=fallback on this machine\n");
    return NULL;
}

Frame *live_camera_read(LiveCamera *cam) {
    (void)cam;
    return NULL;
}

void live_camera_close(LiveCamera *cam) {
    (void)cam;
}

#endif /* __QNXNTO__ */
