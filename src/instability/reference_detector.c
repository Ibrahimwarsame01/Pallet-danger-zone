/* -----------------------------------------------------------------------------
 * reference_detector.c — Role C
 *
 * Reference-image occupancy comparison for detecting pallet leaning.
 * -------------------------------------------------------------------------- */

#include "reference_detector.h"

#include <opencv2/core/core_c.h>
#include <opencv2/imgcodecs/imgcodecs_c.h>
#include <opencv2/imgproc/imgproc_c.h>
#include <opencv2/imgproc/types_c.h>

#include <stdio.h>
#include <stdlib.h>

struct ReferenceDetector {
    IplImage *ref_gray;      /* full reference, gray, owned */
    IplImage *cur_gray;      /* per-frame gray buffer */
    IplImage *diff;          /* per-frame diff buffer */
    IplImage *mask;          /* per-frame thresholded mask */
    CvRect    roi;           /* zero-size => use full image */

    int    pixel_threshold;
    double occupancy_trigger;
    int    persistence;

    int    hit_count;
    double last_occupancy;
};

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

/* Clamp roi to image bounds; return valid roi or full image if zero-sized. */
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

ReferenceDetector *reference_detector_create(const char *reference_path,
                                             int roi_x, int roi_y,
                                             int roi_w, int roi_h,
                                             int pixel_threshold,
                                             double occupancy_trigger,
                                             int persistence)
{
    if (!reference_path) {
        fprintf(stderr, "[reference_detector] reference_path is NULL\n");
        return NULL;
    }

    IplImage *loaded = cvLoadImage(reference_path, CV_LOAD_IMAGE_GRAYSCALE);
    if (!loaded) {
        fprintf(stderr, "[reference_detector] failed to load: %s\n", reference_path);
        return NULL;
    }

    ReferenceDetector *rd = (ReferenceDetector *)calloc(1, sizeof(ReferenceDetector));
    if (!rd) { cvReleaseImage(&loaded); return NULL; }

    rd->ref_gray          = loaded;
    rd->roi               = cvRect(roi_x, roi_y, roi_w, roi_h);
    rd->pixel_threshold   = (pixel_threshold < 0) ? 25 : pixel_threshold;
    rd->occupancy_trigger = (occupancy_trigger <= 0.0) ? 0.08 : occupancy_trigger;
    rd->persistence       = (persistence < 1) ? 1 : persistence;

    fprintf(stderr, "[reference_detector] loaded %s (%dx%d), roi=(%d,%d,%d,%d)\n",
            reference_path, loaded->width, loaded->height,
            rd->roi.x, rd->roi.y, rd->roi.width, rd->roi.height);
    return rd;
}

void reference_detector_destroy(ReferenceDetector *rd)
{
    if (!rd) return;
    if (rd->ref_gray) cvReleaseImage(&rd->ref_gray);
    if (rd->cur_gray) cvReleaseImage(&rd->cur_gray);
    if (rd->diff)     cvReleaseImage(&rd->diff);
    if (rd->mask)     cvReleaseImage(&rd->mask);
    free(rd);
}

bool reference_detector_update(ReferenceDetector *rd, Frame *frame)
{
    if (!rd || !rd->ref_gray || !frame || !frame->image) return false;
    IplImage *img = frame->image;

    /* Reference must match frame dimensions (avoid ROI shift surprises) */
    if (img->width  != rd->ref_gray->width ||
        img->height != rd->ref_gray->height) {
        static int warned = 0;
        if (!warned) {
            fprintf(stderr,
                    "[reference_detector] frame %dx%d != reference %dx%d — "
                    "recapture reference or resize frame source.\n",
                    img->width, img->height,
                    rd->ref_gray->width, rd->ref_gray->height);
            warned = 1;
        }
        return false;
    }

    CvSize full = cvGetSize(img);
    ensure_gray_buffer(&rd->cur_gray, full);
    ensure_gray_buffer(&rd->diff,     full);
    ensure_gray_buffer(&rd->mask,     full);
    if (!rd->cur_gray || !rd->diff || !rd->mask) return false;

    if (!to_gray(rd->cur_gray, img)) return false;

    CvRect roi = resolve_roi(img, rd->roi);
    cvSetImageROI(rd->cur_gray, roi);
    cvSetImageROI(rd->ref_gray, roi);
    cvSetImageROI(rd->diff,     roi);
    cvSetImageROI(rd->mask,     roi);

    cvAbsDiff(rd->cur_gray, rd->ref_gray, rd->diff);
    cvThreshold(rd->diff, rd->mask,
                (double)rd->pixel_threshold, 255.0, CV_THRESH_BINARY);
    int changed = cvCountNonZero(rd->mask);
    int total   = roi.width * roi.height;

    cvResetImageROI(rd->cur_gray);
    cvResetImageROI(rd->ref_gray);
    cvResetImageROI(rd->diff);
    cvResetImageROI(rd->mask);

    rd->last_occupancy = (total > 0) ? ((double)changed / (double)total) : 0.0;

    if (rd->last_occupancy > rd->occupancy_trigger) {
        rd->hit_count++;
    } else {
        rd->hit_count = 0;
    }
    return rd->hit_count >= rd->persistence;
}

double reference_detector_get_occupancy(const ReferenceDetector *rd)
{
    return rd ? rd->last_occupancy : 0.0;
}
