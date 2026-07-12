/*
 * integration_check.c — host-side end-to-end wiring test for PalletGuard.
 *
 * Exercises every module together except person_detector (whose OpenCV C API
 * dependency does not exist in OpenCV 4 — person points are simulated with
 * the same animation the synthetic footage uses):
 *
 *   frame_source(fallback BMPs) -> motion + reference + angle detectors
 *     -> zone_check -> state_machine -> gpio_alert(console) -> overlay -> logger
 *
 * Expectations on the synthetic clip (40 frames):
 *   - pallet falls at frame 15  -> instability -> HAZARD_ACTIVE
 *   - person enters zone ~25    -> ALARMING + alert on
 * Exit 0 iff both were observed and frame flow never broke.
 */
#include "frame_source.h"
#include "motion_detector.h"
#include "reference_detector.h"
#include "angle_detector.h"
#include "zone_check.h"
#include "state_machine.h"
#include "gpio_alert.h"
#include "overlay.h"
#include "logger.h"

#include <opencv2/core/core_c.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "footage";
    const char *outdir = argc > 2 ? argv[2] : "/tmp/pallet_integration";

    FrameSource *fs = frame_source_create(FRAME_SOURCE_FALLBACK, 0, dir);
    if (!fs) { fprintf(stderr, "FAIL: cannot open fallback dir %s\n", dir); return 1; }
    frame_source_set_loop(fs, false);
    frame_source_set_fps(fs, 0);            /* no pacing, run flat out */

    Frame *fr = frame_source_get_next(fs);
    if (!fr || !fr->image) { fprintf(stderr, "FAIL: no first frame\n"); return 1; }
    IplImage *ref = cvCloneImage(fr->image);

    /* pallet ROI in the synthetic scene */
    const int rx = 180, ry = 80, rw = 120, rh = 75;
    MotionDetector    *md = motion_detector_create(25, 0.01, 3);
    ReferenceDetector *rd = reference_detector_create(ref, rx, ry, rw, rh, 25, 0.08, 3);
    AngleDetector     *ad = angle_detector_create(90.0, 12.0, rx, ry, rw, rh, 3);
    StateMachine      *sm = state_machine_create(5);
    GpioAlert         *ga = gpio_alert_open(NULL);

    LoggerConfig lc; logger_config_defaults(&lc);
    char logpath[512]; snprintf(logpath, sizeof logpath, "%s/integration_log.csv", outdir);
    lc.path = logpath; lc.log_every_n = 1;
    Logger *lg = logger_open(&lc);
    if (!md || !rd || !ad || !sm || !ga || !lg) {
        fprintf(stderr, "FAIL: a module failed to initialize (md=%p rd=%p ad=%p sm=%p ga=%p lg=%p)\n",
                (void *)md, (void *)rd, (void *)ad, (void *)sm, (void *)ga, (void *)lg);
        return 1;
    }
    logger_log_event(lg, "integration check start, gpio backend=%s", gpio_alert_backend_name(ga));

    Point zone_pts[4] = { {60, 155}, {300, 155}, {300, 235}, {60, 235} };
    Polygon zone = { zone_pts, 4 };

    bool saw_hazard = false, saw_alarm = false, saw_alert = false;
    int i = 0, got = 0;
    do {
        /* simulated Role B output: same walk the footage generator draws */
        Point person; int pcount = 0;
        if (i >= 20) {
            int px = 20 + (i - 20) * 8; if (px > 140) px = 140;
            person.x = px; person.y = 215; pcount = 1;
        }

        bool motion = motion_detector_update(md, fr);
        bool lean   = reference_detector_update(rd, fr);
        bool tilt   = angle_detector_update(ad, fr);
        bool breach = zone_check_is_breached(&zone, pcount ? &person : NULL, pcount);
        SystemState st = state_machine_update(sm, motion || lean || tilt, breach);
        gpio_alert_update(ga, st, fr->timestamp_ms);
        bool alert = gpio_alert_is_alarm_on(ga);

        OverlayData od = {0};
        od.zone = &zone; od.persons = pcount ? &person : NULL; od.person_count = pcount;
        od.state = st; od.motion_active = motion; od.lean_active = lean || tilt;
        od.occupancy_pct = (float)(reference_detector_get_occupancy(rd) * 100.0);
        od.angle_deg = (float)angle_detector_get_deviation(ad); od.angle_valid = tilt;
        od.person_in_zone = breach; od.alert_on = alert; od.fps = -1.0f;
        overlay_render(fr, &od);
        if (i == 30) {
            char bmp[512]; snprintf(bmp, sizeof bmp, "%s/frame30_annotated.bmp", outdir);
            if (overlay_write_bmp(fr, bmp) == 0) printf("wrote %s\n", bmp);
        }

        LogRecord rec = {0};
        rec.frame_id = fr->frame_id; rec.uptime_ms = fr->timestamp_ms; rec.state = st;
        rec.motion_active = motion; rec.lean_active = lean; rec.occupancy_pct = od.occupancy_pct;
        rec.angle_deg = od.angle_deg; rec.angle_valid = tilt;
        rec.person_count = pcount; rec.person_in_zone = breach; rec.alert_on = alert;
        logger_log_frame(lg, &rec);

        printf("frame %2d  motion=%d lean=%d tilt=%d occ=%5.1f%% breach=%d  state=%-13s alert=%d\n",
               i, motion, lean, tilt, od.occupancy_pct, breach, state_machine_label(st), alert);

        saw_hazard |= (st == STATE_HAZARD_ACTIVE);
        saw_alarm  |= (st == STATE_ALARMING);
        saw_alert  |= alert;
        got++; i++;
    } while ((fr = frame_source_get_next(fs)) != NULL);

    logger_log_event(lg, "integration check end, frames=%d", got);
    logger_close(lg);
    gpio_alert_close(ga);
    state_machine_destroy(sm);
    angle_detector_destroy(ad);
    reference_detector_destroy(rd);
    motion_detector_destroy(md);
    cvReleaseImage(&ref);
    frame_source_destroy(fs);

    printf("\nframes=%d hazard_seen=%d alarm_seen=%d alert_seen=%d\n",
           got, saw_hazard, saw_alarm, saw_alert);
    if (got == 40 && saw_hazard && saw_alarm && saw_alert) {
        printf("INTEGRATION CHECK PASSED\n");
        return 0;
    }
    printf("INTEGRATION CHECK FAILED\n");
    return 1;
}
