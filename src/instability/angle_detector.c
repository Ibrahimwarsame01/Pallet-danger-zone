/* -----------------------------------------------------------------------------
 * angle_detector.c — Role C
 *
 * Extract the largest contour in the pallet ROI, fit a minimum-area rectangle,
 * and measure the deviation of its long-edge angle from a reference angle.
 *
 * Fallback behavior: if no contour is found, no min-rect can be fit, or the
 * largest contour is too small to be meaningful, we do NOT count a hit —
 * ambiguous frames must not cause false hazards.
 * -------------------------------------------------------------------------- */

#include "angle_detector.h"

#include <opencv2/core/core_c.h>
#include <opencv2/imgproc/imgproc_c.h>
#include <opencv2/imgproc/types_c.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* Ignore contours smaller than this many pixels — usually noise. */
#define MIN_CONTOUR_AREA 500.0

struct AngleDetector {
    IplImage    *cur_gray;
    IplImage    *binary;
    CvMemStorage *storage;
    CvRect       roi;

    double       reference_angle_deg;   /* 0..180 */
    double       deviation_threshold;
    int          persistence;

    int          hit_count;
    double       last_angle_deg;        /* NaN if not yet measured */
    double       last_deviation_deg;
};

/* --- Helpers -------------------------------------------------------------- */

static void ensure_gray_buffer(IplImage **buf, CvSize sz)
{
    if (*buf && ((*buf)->width != sz.width || (*buf)->height != sz.height)) {
        cvReleaseImage(buf);
    }
    if (!*buf) *buf = cvCreateImage(sz, IPL_DEPTH_8U, 1);
}

static bool to_gray(IplImage *dst, const IplImage *src)
{
    if (!dst || !src) return false;
    if (src->nChannels == 1)      cvCopy(src, dst, NULL);
    else if (src->nChannels == 3) cvCvtColor(src, dst, CV_BGR2GRAY);
    else if (src->nChannels == 4) cvCvtColor(src, dst, CV_BGRA2GRAY);
    else return false;
    return true;
}

static CvRect resolve_roi(const IplImage *img, CvRect roi)
{
    if (roi.width <= 0 || roi.height <= 0) {
        return cvRect(0, 0, img->width, img->height);
    }
    if (roi.x < 0) roi.x = 0;
    if (roi.y < 0) roi.y = 0;
    if (roi.x + roi.width  > img->width)  roi.width  = img->width  - roi.x;
    if (roi.y + roi.height > img->height) roi.height = img->height - roi.y;
    return roi;
}

/* Convert CvBox2D to "long-edge angle" in [0, 180). Robust to width/height flip.
 *
 * OpenCV convention: box.angle is the rotation of the width edge, in (-90, 0].
 * When width < height, add 90 to describe the height edge instead — this gives
 * a stable angle for the *long* edge regardless of which side was called w/h.
 */
static double box_long_edge_angle(CvBox2D box)
{
    double angle = box.angle;
    if (box.size.width < box.size.height) angle += 90.0;
    /* Normalize to [0, 180) */
    while (angle <    0.0) angle += 180.0;
    while (angle >= 180.0) angle -= 180.0;
    return angle;
}

/* Shortest angular deviation on a 180-degree circle (angles are undirected). */
static double angular_deviation_180(double a, double b)
{
    double d = fabs(a - b);
    if (d > 90.0) d = 180.0 - d;
    return d;   /* in [0, 90] */
}

/* --- Public API ----------------------------------------------------------- */

AngleDetector *angle_detector_create(double reference_angle_deg,
                                     double deviation_threshold_deg,
                                     int roi_x, int roi_y,
                                     int roi_w, int roi_h,
                                     int persistence)
{
    AngleDetector *ad = (AngleDetector *)calloc(1, sizeof(AngleDetector));
    if (!ad) return NULL;
    ad->storage             = cvCreateMemStorage(0);
    if (!ad->storage) { free(ad); return NULL; }

    ad->roi                 = cvRect(roi_x, roi_y, roi_w, roi_h);
    ad->reference_angle_deg = reference_angle_deg;
    ad->deviation_threshold = (deviation_threshold_deg <= 0.0)
                                ? 12.0 : deviation_threshold_deg;
    ad->persistence         = (persistence < 1) ? 1 : persistence;
    ad->last_angle_deg      = NAN;
    ad->last_deviation_deg  = 0.0;
    return ad;
}

void angle_detector_destroy(AngleDetector *ad)
{
    if (!ad) return;
    if (ad->cur_gray) cvReleaseImage(&ad->cur_gray);
    if (ad->binary)   cvReleaseImage(&ad->binary);
    if (ad->storage)  cvReleaseMemStorage(&ad->storage);
    free(ad);
}

bool angle_detector_update(AngleDetector *ad, Frame *frame)
{
    if (!ad || !frame || !frame->image) return false;
    IplImage *img = frame->image;

    CvSize full = cvGetSize(img);
    ensure_gray_buffer(&ad->cur_gray, full);
    ensure_gray_buffer(&ad->binary,   full);
    if (!ad->cur_gray || !ad->binary) return false;

    if (!to_gray(ad->cur_gray, img)) return false;

    CvRect roi = resolve_roi(img, ad->roi);
    cvSetImageROI(ad->cur_gray, roi);
    cvSetImageROI(ad->binary,   roi);

    /* Otsu threshold: adapts to lighting rather than a fixed cutoff */
    cvThreshold(ad->cur_gray, ad->binary,
                0.0, 255.0, CV_THRESH_BINARY | CV_THRESH_OTSU);

    /* Find contours (external only — we care about outer silhouette) */
    cvClearMemStorage(ad->storage);
    CvSeq *contours = NULL;
    int n = cvFindContours(ad->binary, ad->storage, &contours,
                           sizeof(CvContour),
                           CV_RETR_EXTERNAL, CV_CHAIN_APPROX_SIMPLE,
                           cvPoint(0, 0));

    cvResetImageROI(ad->cur_gray);
    cvResetImageROI(ad->binary);

    if (n <= 0 || !contours) {
        ad->hit_count = 0;   /* no evidence -> reset debounce */
        return false;
    }

    /* Pick largest contour by area */
    CvSeq *largest = NULL;
    double largest_area = 0.0;
    for (CvSeq *c = contours; c != NULL; c = c->h_next) {
        double a = fabs(cvContourArea(c, CV_WHOLE_SEQ, 0));
        if (a > largest_area) { largest_area = a; largest = c; }
    }
    if (!largest || largest_area < MIN_CONTOUR_AREA) {
        ad->hit_count = 0;
        return false;
    }

    CvBox2D box = cvMinAreaRect2(largest, ad->storage);
    ad->last_angle_deg     = box_long_edge_angle(box);
    ad->last_deviation_deg = angular_deviation_180(ad->last_angle_deg,
                                                   ad->reference_angle_deg);

    if (ad->last_deviation_deg > ad->deviation_threshold) {
        ad->hit_count++;
    } else {
        ad->hit_count = 0;
    }
    return ad->hit_count >= ad->persistence;
}

double angle_detector_get_angle(const AngleDetector *ad)
{
    return ad ? ad->last_angle_deg : NAN;
}

double angle_detector_get_deviation(const AngleDetector *ad)
{
    return ad ? ad->last_deviation_deg : 0.0;
}
