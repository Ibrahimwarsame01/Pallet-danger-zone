/* -----------------------------------------------------------------------------
 * person_detector.c — Role B
 *
 * Pure C person detector using OpenCV C API Haar cascade
 * (cvHaarDetectObjects + haarcascade_fullbody.xml).
 *
 * Pipeline per frame:
 *   1. Convert to grayscale if BGR/BGRA.
 *   2. Downscale (0.5x) for detection speed if wider than 320px.
 *   3. Histogram equalization for lighting robustness.
 *   4. Run cvHaarDetectObjects with CV_HAAR_DO_CANNY_PRUNING.
 *   5. Convert each bbox to a foot point (bottom-center) in ORIGINAL coords.
 *
 * Contract (locked in include/common_types.h):
 *   Point *person_detector_get_points(Frame *frame, int *count_out);
 * -------------------------------------------------------------------------- */

#include "person_detector.h"

#include <opencv2/core/core_c.h>
#include <opencv2/core/types_c.h>
#include <opencv2/imgproc/imgproc_c.h>
#include <opencv2/imgproc/types_c.h>
#include <opencv2/objdetect/objdetect_c.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Tunables ------------------------------------------------------------- */
#define DETECT_DOWNSCALE      0.5     /* frame shrink for detection stage */
#define DETECT_MIN_WIDTH_PX   320     /* only downscale if wider than this */
#define HAAR_SCALE_FACTOR     1.1
#define HAAR_MIN_NEIGHBORS    3
#define HAAR_MIN_W            24      /* min bbox in downscaled image */
#define HAAR_MIN_H            48      /* fullbody aspect ~1:2 */

/* --- Module state --------------------------------------------------------- */
static CvHaarClassifierCascade *g_cascade = NULL;
static CvMemStorage            *g_storage = NULL;

static const char *DEFAULT_CASCADE_PATHS[] = {
    "assets/haarcascade_fullbody.xml",
    "haarcascade_fullbody.xml",
    "/usr/share/opencv4/haarcascades/haarcascade_fullbody.xml",
    "/usr/share/opencv/haarcascades/haarcascade_fullbody.xml",
    NULL
};

/* --- Helpers -------------------------------------------------------------- */

static const char *find_default_cascade(void)
{
    for (int i = 0; DEFAULT_CASCADE_PATHS[i] != NULL; i++) {
        FILE *f = fopen(DEFAULT_CASCADE_PATHS[i], "rb");
        if (f) {
            fclose(f);
            return DEFAULT_CASCADE_PATHS[i];
        }
    }
    return NULL;
}

/* Ensure detection buffer: grayscale, possibly downscaled, owned by caller. */
static IplImage *build_detect_image(const IplImage *src, double *out_scale)
{
    if (!src || !out_scale) return NULL;
    *out_scale = 1.0;

    /* Step 1: grayscale */
    IplImage *gray = NULL;
    if (src->nChannels == 1) {
        gray = cvCloneImage(src);
    } else if (src->nChannels == 3) {
        gray = cvCreateImage(cvGetSize(src), IPL_DEPTH_8U, 1);
        if (!gray) return NULL;
        cvCvtColor(src, gray, CV_BGR2GRAY);
    } else if (src->nChannels == 4) {
        gray = cvCreateImage(cvGetSize(src), IPL_DEPTH_8U, 1);
        if (!gray) return NULL;
        cvCvtColor(src, gray, CV_BGRA2GRAY);
    } else {
        return NULL;
    }

    /* Step 2: optional downscale */
    IplImage *work = gray;
    if (gray->width > DETECT_MIN_WIDTH_PX) {
        CvSize sz = cvSize((int)(gray->width  * DETECT_DOWNSCALE),
                           (int)(gray->height * DETECT_DOWNSCALE));
        IplImage *small = cvCreateImage(sz, IPL_DEPTH_8U, 1);
        if (!small) {
            cvReleaseImage(&gray);
            return NULL;
        }
        cvResize(gray, small, CV_INTER_LINEAR);
        cvReleaseImage(&gray);
        work = small;
        *out_scale = DETECT_DOWNSCALE;
    }

    /* Step 3: equalize (in-place on our owned buffer) */
    cvEqualizeHist(work, work);
    return work;
}

/* --- Public API ----------------------------------------------------------- */

int person_detector_init(const char *cascade_path)
{
    if (g_cascade) return 0;

    const char *path = cascade_path ? cascade_path : find_default_cascade();
    if (!path) {
        fprintf(stderr,
                "[person_detector] no cascade XML found. Place "
                "haarcascade_fullbody.xml at assets/ or pass path to init.\n");
        return -1;
    }

    g_cascade = (CvHaarClassifierCascade *)cvLoad(path, NULL, NULL, NULL);
    if (!g_cascade) {
        fprintf(stderr, "[person_detector] cvLoad failed for: %s\n", path);
        return -1;
    }

    g_storage = cvCreateMemStorage(0);
    if (!g_storage) {
        fprintf(stderr, "[person_detector] cvCreateMemStorage failed\n");
        cvReleaseHaarClassifierCascade(&g_cascade);
        g_cascade = NULL;
        return -1;
    }

    fprintf(stderr, "[person_detector] initialized with %s\n", path);
    return 0;
}

void person_detector_shutdown(void)
{
    if (g_cascade) {
        cvReleaseHaarClassifierCascade(&g_cascade);
        g_cascade = NULL;
    }
    if (g_storage) {
        cvReleaseMemStorage(&g_storage);
        g_storage = NULL;
    }
}

Point *person_detector_get_points(Frame *frame, int *count_out)
{
    if (count_out) *count_out = 0;
    if (!frame || !frame->image || !count_out) return NULL;

    if (!g_cascade) {
        if (person_detector_init(NULL) != 0) return NULL;
    }

    /* HOG default template is 64x128; Haar fullbody trained on ~14x28.
     * We require frame to be at least the min bbox size after downscaling. */
    if (frame->image->width < 64 || frame->image->height < 64) return NULL;

    cvClearMemStorage(g_storage);

    double scale = 1.0;
    IplImage *detect_img = build_detect_image(frame->image, &scale);
    if (!detect_img) return NULL;

    CvSeq *found = cvHaarDetectObjects(
        detect_img,
        g_cascade,
        g_storage,
        HAAR_SCALE_FACTOR,
        HAAR_MIN_NEIGHBORS,
        CV_HAAR_DO_CANNY_PRUNING,
        cvSize(HAAR_MIN_W, HAAR_MIN_H),
        cvSize(0, 0)  /* max_size: no limit */
    );

    int n = (found && found->total > 0) ? found->total : 0;
    Point *points = NULL;

    if (n > 0) {
        points = (Point *)calloc((size_t)n, sizeof(Point));
        if (points) {
            /* Undo any downscaling: scale in [0,1], divide to get back to original */
            double up = (scale > 0.0) ? (1.0 / scale) : 1.0;

            for (int i = 0; i < n; i++) {
                CvRect *r = (CvRect *)cvGetSeqElem(found, i);
                if (!r) continue;
                /* Foot position = bottom-center of bbox, in ORIGINAL frame coords.
                 * This is the point zone_check compares against the ground-plane
                 * polygon — a person's feet are what actually enters the zone. */
                points[i].x = (int)((r->x + r->width  * 0.5) * up);
                points[i].y = (int)((r->y + r->height       ) * up);
            }
            *count_out = n;
        }
    }

    cvReleaseImage(&detect_img);
    return points;
}
