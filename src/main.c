/*
 * main.c — PalletGuard end-to-end pipeline.
 *
 *   frame_source (live camera / fallback BMPs)
 *     -> motion detector      (falling: frame-to-frame diff)      Role C
 *     -> reference detector   (leaning: diff vs reference in ROI) Role C
 *     -> zone presence        (person proxy: diff vs reference inside the
 *                              danger-zone polygon; stands in for Role B's
 *                              Haar detector, which needs an OpenCV build
 *                              the QNX Pi image does not have)
 *     -> state machine        (NORMAL / HAZARD_ACTIVE / ALARMING) Role C
 *     -> gpio_alert (LED+buzzer) + overlay + CSV logger           Role D
 *
 * Angle detection is intentionally not wired in: it needs OpenCV contours
 * (unavailable without a real OpenCV build) and the PRD names occupancy-only
 * as the planned fallback.
 *
 * Build (Pi native or any host, no OpenCV needed): tools/build_nocv.sh
 *
 * Typical demo run on the Pi:
 *   ./palletguard --live                      # reference from assets/reference
 *   ./palletguard --fallback assets/fallback_footage --reference assets/reference
 */
#include "frame_source/frame_source.h"
#include "instability/motion_detector.h"
#include "instability/reference_detector.h"
#include "zone/zone_check.h"
#include "zone/state_machine.h"
#include "alerts/gpio_alert.h"
#include "alerts/overlay.h"
#include "alerts/logger.h"

#include <opencv2/core/core_c.h>
#include <opencv2/imgproc/imgproc_c.h>
#include <opencv2/imgproc/types_c.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- defaults (CLAUDE.md config table; geometry as frame fractions) ------ */
#define DEF_MOTION_PIXEL_THR   25
#define DEF_MOTION_AREA_TRIG   0.05
#define DEF_MOTION_PERSIST     3
#define DEF_REF_PIXEL_THR      25
#define DEF_REF_OCC_TRIG       0.40  /* lean must move >40% of the ROI to alarm.
                                      * Kept well under the silhouette shift of a
                                      * side-on lean: tipping toward/away from the
                                      * lens changes far fewer pixels, and a high
                                      * trigger simply misses those. */
#define DEF_REF_PERSIST        3
#define DEF_STABLE_THRESHOLD   15     /* frames of calm before back to NORMAL */
#define DEF_ZONE_PIXEL_THR     25
#define DEF_ZONE_PRESENCE_TRIG 0.02   /* fraction of zone pixels changed = person */

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

/* ---- zone presence: person-in-zone proxy --------------------------------- */

typedef struct {
    IplImage *ref_gray;   /* owned */
    IplImage *cur_gray;   /* owned scratch */
    uint8_t  *mask;       /* 1 = pixel inside zone polygon */
    long      mask_count; /* pixels inside the polygon */
    int       w, h;
    int       pixel_thr;
    double    trigger;
    double    last_fraction;
    Point     centroid;
} ZonePresence;

static bool zp_init(ZonePresence *zp, const IplImage *reference,
                    const Polygon *zone, int pixel_thr, double trigger) {
    memset(zp, 0, sizeof(*zp));
    zp->w = reference->width;
    zp->h = reference->height;
    zp->pixel_thr = pixel_thr;
    zp->trigger   = trigger;

    zp->ref_gray = cvCreateImage(cvSize(zp->w, zp->h), IPL_DEPTH_8U, 1);
    zp->cur_gray = cvCreateImage(cvSize(zp->w, zp->h), IPL_DEPTH_8U, 1);
    zp->mask     = malloc((size_t)zp->w * zp->h);
    if (!zp->ref_gray || !zp->cur_gray || !zp->mask) return false;

    if (reference->nChannels == 1)      cvCopy(reference, zp->ref_gray, NULL);
    else if (reference->nChannels == 3) cvCvtColor(reference, zp->ref_gray, CV_BGR2GRAY);
    else                                cvCvtColor(reference, zp->ref_gray, CV_BGRA2GRAY);

    for (int y = 0; y < zp->h; y++) {
        for (int x = 0; x < zp->w; x++) {
            Point p = { x, y };
            uint8_t in = zone_check_point_in_polygon(zone, p) ? 1 : 0;
            zp->mask[(size_t)y * zp->w + x] = in;
            zp->mask_count += in;
        }
    }
    return zp->mask_count > 0;
}

/* True while enough of the zone differs from the reference; centroid of the
 * changed pixels doubles as the "person point" for overlay + zone_check. */
static bool zp_update(ZonePresence *zp, const IplImage *img) {
    if (img->width != zp->w || img->height != zp->h) return false;
    if (img->nChannels == 1)      cvCopy(img, zp->cur_gray, NULL);
    else if (img->nChannels == 3) cvCvtColor(img, zp->cur_gray, CV_BGR2GRAY);
    else                          cvCvtColor(img, zp->cur_gray, CV_BGRA2GRAY);

    long hits = 0, sx = 0, sy = 0;
    const uint8_t *ref = (const uint8_t *)zp->ref_gray->imageData;
    const uint8_t *cur = (const uint8_t *)zp->cur_gray->imageData;
    for (int y = 0; y < zp->h; y++) {
        const uint8_t *m  = zp->mask + (size_t)y * zp->w;
        const uint8_t *pr = ref + (size_t)y * zp->ref_gray->widthStep;
        const uint8_t *pc = cur + (size_t)y * zp->cur_gray->widthStep;
        for (int x = 0; x < zp->w; x++) {
            if (!m[x]) continue;
            int d = (int)pc[x] - (int)pr[x];
            if (d < 0) d = -d;
            if (d > zp->pixel_thr) {
                hits++; sx += x; sy += y;
            }
        }
    }
    zp->last_fraction = (double)hits / (double)zp->mask_count;
    if (hits > 0) {
        zp->centroid.x = (int)(sx / hits);
        zp->centroid.y = (int)(sy / hits);
    }
    return zp->last_fraction > zp->trigger;
}

static void zp_free(ZonePresence *zp) {
    if (zp->ref_gray) cvReleaseImage(&zp->ref_gray);
    if (zp->cur_gray) cvReleaseImage(&zp->cur_gray);
    free(zp->mask);
}

/* ---- helpers -------------------------------------------------------------- */

static IplImage *load_reference(const char *dir) {
    FrameSource *rs = frame_source_create(FRAME_SOURCE_FALLBACK, 0, dir);
    if (!rs) {
        fprintf(stderr, "palletguard: no reference image in %s "
                        "(capture one: ./test_frame_source --live --record %s --frames 31, keep the last)\n",
                dir, dir);
        return NULL;
    }
    Frame *fr = frame_source_get_next(rs);
    IplImage *ref = fr && fr->image ? cvCloneImage(fr->image) : NULL;
    frame_source_destroy(rs);
    return ref;
}

static void print_banner(SystemState st, bool person) {
    if (st == STATE_ALARMING) {
        printf("\n*** ALARM: pallet unstable — angle differs from reference%s ***\n\n",
               person ? " (person in danger zone!)" : "");
    }
}

static void usage(const char *argv0) {
    fprintf(stderr,
        "usage: %s [--live [--device N] | --fallback DIR]   (default: --live)\n"
        "          [--reference DIR]        (default assets/reference)\n"
        "          [--roi X Y W H]          (pallet region; default centered upper block)\n"
        "          [--zone X1 Y1 X2 Y2 X3 Y3 X4 Y4]  (danger quad; default lower floor)\n"
        "          [--occ PCT]              (lean trigger, %% of ROI changed; default 40)\n"
        "          [--frames N]             (0 = run until source ends / Ctrl-C)\n"
        "          [--dump N]               (write annotated BMP every N frames to out/)\n",
        argv0);
}

int main(int argc, char **argv) {
    FrameSourceType type = FRAME_SOURCE_LIVE;   /* default: our camera */
    const char *src_dir = "assets/fallback_footage";
    const char *ref_dir = "assets/reference";
    int device = 0, max_frames = 0, dump_every = 0;
    double occ_trig = DEF_REF_OCC_TRIG;
    int roi[4] = { -1, -1, -1, -1 };
    int zq[8]; bool zone_set = false;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--live")) type = FRAME_SOURCE_LIVE;
        else if (!strcmp(argv[i], "--fallback") && i + 1 < argc) {
            type = FRAME_SOURCE_FALLBACK; src_dir = argv[++i];
        } else if (!strcmp(argv[i], "--reference") && i + 1 < argc) ref_dir = argv[++i];
        else if (!strcmp(argv[i], "--device") && i + 1 < argc) device = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--occ") && i + 1 < argc) occ_trig = atof(argv[++i]) / 100.0;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) max_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump_every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--roi") && i + 4 < argc) {
            for (int k = 0; k < 4; k++) roi[k] = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--zone") && i + 8 < argc) {
            for (int k = 0; k < 8; k++) zq[k] = atoi(argv[++i]);
            zone_set = true;
        } else { usage(argv[0]); return 1; }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    IplImage *reference = load_reference(ref_dir);
    if (!reference) return 1;
    int W = reference->width, H = reference->height;

    /* geometry defaults as frame fractions (calibrated on the demo scene) */
    if (roi[0] < 0) {
        roi[0] = W * 27 / 100; roi[1] = 0;
        roi[2] = W * 28 / 100; roi[3] = H * 45 / 100;
    }
    Point zone_pts[4];
    if (zone_set) {
        for (int k = 0; k < 4; k++) {
            zone_pts[k].x = zq[2 * k]; zone_pts[k].y = zq[2 * k + 1];
        }
    } else {
        zone_pts[0] = (Point){ W * 22 / 100, H * 54 / 100 };
        zone_pts[1] = (Point){ W * 95 / 100, H * 54 / 100 };
        zone_pts[2] = (Point){ W - 1,        H - 1 };
        zone_pts[3] = (Point){ W * 17 / 100, H - 1 };
    }
    Polygon zone = { zone_pts, 4 };

    FrameSource *fs = frame_source_create(type, device, src_dir);
    if (!fs) {
        fprintf(stderr, "palletguard: cannot open %s source\n",
                type == FRAME_SOURCE_LIVE ? "live" : "fallback");
        cvReleaseImage(&reference);
        return 1;
    }
    if (type == FRAME_SOURCE_FALLBACK) frame_source_set_loop(fs, false);

    MotionDetector *md = motion_detector_create(DEF_MOTION_PIXEL_THR,
                                                DEF_MOTION_AREA_TRIG,
                                                DEF_MOTION_PERSIST);
    ReferenceDetector *rd = reference_detector_create(reference,
                                                      roi[0], roi[1], roi[2], roi[3],
                                                      DEF_REF_PIXEL_THR,
                                                      occ_trig,
                                                      DEF_REF_PERSIST);
    StateMachine *sm = state_machine_create(DEF_STABLE_THRESHOLD);
    GpioAlert    *ga = gpio_alert_open(NULL);
    Logger       *lg = logger_open(NULL);
    ZonePresence  zp;
    bool zp_ok = zp_init(&zp, reference, &zone,
                         DEF_ZONE_PIXEL_THR, DEF_ZONE_PRESENCE_TRIG);
    cvReleaseImage(&reference);
    if (!md || !rd || !sm || !ga || !zp_ok) {
        fprintf(stderr, "palletguard: init failed (md=%p rd=%p sm=%p ga=%p zone=%d)\n",
                (void *)md, (void *)rd, (void *)sm, (void *)ga, (int)zp_ok);
        return 1;
    }
    printf("palletguard: %dx%d ref, roi=(%d,%d %dx%d), zone=(%d,%d)-(%d,%d)-(%d,%d)-(%d,%d)\n"
           "palletguard: gpio backend=%s, log=%s\n",
           W, H, roi[0], roi[1], roi[2], roi[3],
           zone_pts[0].x, zone_pts[0].y, zone_pts[1].x, zone_pts[1].y,
           zone_pts[2].x, zone_pts[2].y, zone_pts[3].x, zone_pts[3].y,
           gpio_alert_backend_name(ga), lg ? logger_path(lg) : "(disabled)");
    if (lg) logger_log_event(lg, "start source=%s gpio=%s",
                             type == FRAME_SOURCE_LIVE ? "live" : src_dir,
                             gpio_alert_backend_name(ga));

    /* Live camera spends its first frames locking exposure; frames during
     * that window diff wildly against the reference, so don't judge them. */
    int warmup = (type == FRAME_SOURCE_LIVE) ? 90 : 0;

    SystemState prev_state = STATE_NORMAL;
    int frames = 0;
    Frame *fr;
    while (!g_stop && (fr = frame_source_get_next(fs)) != NULL) {
        bool motion = motion_detector_update(md, fr);
        bool lean   = reference_detector_update(rd, fr);
        bool person = zp_update(&zp, fr->image);

        if (frames < warmup) {
            if (frames % 30 == 0)
                printf("[frame %5d] warming up (camera exposure locking, %d to go)\n",
                       frames, warmup - frames);
            frames++;
            continue;
        }

        Point pts[1]; int npts = 0;
        if (person) { pts[0] = zp.centroid; npts = 1; }
        bool breach = zone_check_is_breached(&zone, npts ? pts : NULL, npts);

        /* Demo policy: the alarm is the lean (persistent deviation from the
         * reference angle). Motion is full-frame and fires on background
         * people, so it is reported but does not alarm on its own;
         * person-in-zone is reported but not required either. */
        bool unstable = lean;
        SystemState st = state_machine_update(sm, unstable, unstable || breach);
        gpio_alert_update(ga, st, fr->timestamp_ms);
        bool alert = gpio_alert_is_alarm_on(ga);

        OverlayData od = {0};
        od.zone = &zone;
        od.persons = npts ? pts : NULL;   od.person_count = npts;
        od.state = st;
        od.motion_active = motion;        od.lean_active = lean;
        od.occupancy_pct = (float)(reference_detector_get_occupancy(rd) * 100.0);
        od.angle_deg = 0.0f;              od.angle_valid = false;
        od.person_in_zone = breach;       od.alert_on = alert;
        od.fps = -1.0f;
        overlay_render(fr, &od);

        if (dump_every > 0 && frames % dump_every == 0) {
            char path[256];
            snprintf(path, sizeof path, "out/frame_%05d.bmp", frames);
            overlay_write_bmp(fr, path);
        }

        if (lg) {
            LogRecord rec = {0};
            rec.frame_id = fr->frame_id;   rec.uptime_ms = fr->timestamp_ms;
            rec.state = st;
            rec.motion_active = motion;    rec.lean_active = lean;
            rec.occupancy_pct = od.occupancy_pct;
            rec.angle_deg = 0.0f;          rec.angle_valid = false;
            rec.person_count = npts;       rec.person_in_zone = breach;
            rec.alert_on = alert;
            logger_log_frame(lg, &rec);
        }

        if (st != prev_state) {
            printf("[state] %s -> %s (frame %d, motion=%d lean=%d occ=%.1f%% person=%d)\n",
                   state_machine_label(prev_state), state_machine_label(st),
                   frames, motion, lean, od.occupancy_pct, breach);
            print_banner(st, breach);
            prev_state = st;
        } else if (frames % 30 == 0) {
            printf("[frame %5d] %-13s motion=%d lean=%d occ=%5.1f%% zone=%4.1f%% person=%d alert=%d\n",
                   frames, state_machine_label(st), motion, lean,
                   od.occupancy_pct, zp.last_fraction * 100.0, breach, alert);
        }
        frames++;
        if (max_frames > 0 && frames >= max_frames) break;
    }

    printf("palletguard: stopping after %d frame(s)\n", frames);
    if (lg) { logger_log_event(lg, "stop frames=%d", frames); logger_close(lg); }
    gpio_alert_all_off(ga);
    gpio_alert_close(ga);
    zp_free(&zp);
    state_machine_destroy(sm);
    reference_detector_destroy(rd);
    motion_detector_destroy(md);
    frame_source_destroy(fs);
    return 0;
}
