/* -----------------------------------------------------------------------------
 * motion_detector.c — Role C
 *
 * Detects pallet falling via frame-to-frame differencing.
 * Uses only the OpenCV C API (present in QNX SDP 8.0 OpenCV 4.x port).
 * -------------------------------------------------------------------------- */

#include "motion_detector.h"

#include <opencv2/core/core_c.h>
#include <opencv2/imgproc/imgproc_c.h>
#include <opencv2/imgproc/types_c.h>

#include <stdio.h>
#include <stdlib.h>

struct MotionDetector {
    IplImage *prev_gray;
    IplImage *cur_gray;
    IplImage *diff;
    IplImage *mask;

    int    pixel_threshold;
    double area_fraction_trigger;
    int    persistence;

    int    hit_count;
    double last_score;
};

/* Ensure `*buf` is a same-size 8UC1 image as `ref`; recreate on mismatch. */
static void ensure_gray_buffer(IplImage **buf, const IplImage *ref)
{
    CvSize sz = cvGetSize(ref);
    if (*buf && ((*buf)->width != sz.width || (*buf)->height != sz.height)) {
        cvReleaseImage(buf);
    }
    if (!*buf) {
        *buf = cvCreateImage(sz, IPL_DEPTH_8U, 1);
    }
}

/* Fill `dst` (owned by caller) with grayscale version of `src`. */
static bool to_gray(IplImage *dst, const IplImage *src)
{
    if (!dst || !src) return false;
    if (src->nChannels == 1) {
        cvCopy(src, dst, NULL);
    } else if (src->nChannels == 3) {
        cvCvtColor(src, dst, CV_BGR2GRAY);
    } else if (src->nChannels == 4) {
        cvCvtColor(src, dst, CV_BGRA2GRAY);
    } else {
        return false;
    }
    return true;
}

MotionDetector *motion_detector_create(int pixel_threshold,
                                       double area_fraction_trigger,
                                       int persistence)
{
    MotionDetector *md = (MotionDetector *)calloc(1, sizeof(MotionDetector));
    if (!md) return NULL;
    md->pixel_threshold       = (pixel_threshold < 0) ? 25 : pixel_threshold;
    md->area_fraction_trigger = (area_fraction_trigger <= 0.0) ? 0.05 : area_fraction_trigger;
    md->persistence           = (persistence < 1) ? 1 : persistence;
    return md;
}

void motion_detector_destroy(MotionDetector *md)
{
    if (!md) return;
    if (md->prev_gray) cvReleaseImage(&md->prev_gray);
    if (md->cur_gray)  cvReleaseImage(&md->cur_gray);
    if (md->diff)      cvReleaseImage(&md->diff);
    if (md->mask)      cvReleaseImage(&md->mask);
    free(md);
}

bool motion_detector_update(MotionDetector *md, Frame *frame)
{
    if (!md || !frame || !frame->image) return false;
    IplImage *img = frame->image;

    /* Ensure buffers sized to input */
    ensure_gray_buffer(&md->cur_gray, img);
    ensure_gray_buffer(&md->diff,     img);
    ensure_gray_buffer(&md->mask,     img);
    if (!md->cur_gray || !md->diff || !md->mask) return false;

    if (!to_gray(md->cur_gray, img)) return false;

    /* First frame: no prev to compare against */
    if (!md->prev_gray) {
        md->prev_gray = cvCloneImage(md->cur_gray);
        md->last_score = 0.0;
        md->hit_count  = 0;
        return false;
    }
    /* If prev buffer stale-sized, drop it and skip this frame */
    if (md->prev_gray->width  != md->cur_gray->width ||
        md->prev_gray->height != md->cur_gray->height) {
        cvReleaseImage(&md->prev_gray);
        md->prev_gray = cvCloneImage(md->cur_gray);
        md->last_score = 0.0;
        md->hit_count  = 0;
        return false;
    }

    /* Diff -> threshold -> count */
    cvAbsDiff(md->cur_gray, md->prev_gray, md->diff);
    cvThreshold(md->diff, md->mask,
                (double)md->pixel_threshold, 255.0, CV_THRESH_BINARY);
    int changed = cvCountNonZero(md->mask);
    int total   = md->cur_gray->width * md->cur_gray->height;

    md->last_score = (total > 0) ? ((double)changed / (double)total) : 0.0;

    /* Roll previous forward */
    cvCopy(md->cur_gray, md->prev_gray, NULL);

    /* Persistence debounce */
    if (md->last_score > md->area_fraction_trigger) {
        md->hit_count++;
    } else {
        md->hit_count = 0;
    }
    return md->hit_count >= md->persistence;
}

double motion_detector_get_score(const MotionDetector *md)
{
    return md ? md->last_score : 0.0;
}
