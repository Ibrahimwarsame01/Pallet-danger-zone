/*
 * Minimal OpenCV C-API stand-in so the PalletGuard pipeline builds and runs
 * on machines with no OpenCV at all — most importantly the QNX Pi image,
 * which ships none. Provides the IplImage struct plus the exact C functions
 * Role A's frame_source and Role C's motion/reference detectors call
 * (implemented in mini_cv_core.c). The angle detector needs contours
 * (cvFindContours/cvMinAreaRect2) and is NOT covered — build without it and
 * run occupancy-only, the PRD's blessed fallback.
 *
 * NOT binary compatible with real OpenCV — never link objects built against
 * this shim together with objects built against real OpenCV headers.
 */
#ifndef MINI_CV_CORE_C_H
#define MINI_CV_CORE_C_H

#ifdef __cplusplus
extern "C" {
#endif

#define IPL_DEPTH_8U 8

typedef void CvArr;

typedef struct _IplROI {
    int coi;
    int xOffset;
    int yOffset;
    int width;
    int height;
} IplROI;

typedef struct _IplImage {
    int      nChannels;  /* 1 = gray, 3 = BGR, 4 = BGRA */
    int      depth;      /* IPL_DEPTH_8U expected */
    int      width;
    int      height;
    char    *imageData;  /* row-major pixel data */
    int      widthStep;  /* bytes per row (4-byte aligned like OpenCV) */
    IplROI  *roi;        /* NULL = whole image */
} IplImage;

typedef struct CvSize {
    int width;
    int height;
} CvSize;

typedef struct CvRect {
    int x;
    int y;
    int width;
    int height;
} CvRect;

static inline CvSize cvSize(int width, int height) {
    CvSize s;
    s.width = width;
    s.height = height;
    return s;
}

static inline CvRect cvRect(int x, int y, int width, int height) {
    CvRect r;
    r.x = x;
    r.y = y;
    r.width = width;
    r.height = height;
    return r;
}

IplImage *cvCreateImage(CvSize size, int depth, int channels);
void      cvReleaseImage(IplImage **image);
IplImage *cvCloneImage(const IplImage *image);

/* Returns ROI size when a ROI is set (matches OpenCV). */
CvSize    cvGetSize(const CvArr *arr);

void      cvSetImageROI(IplImage *image, CvRect rect);
void      cvResetImageROI(IplImage *image);

/* mask must be NULL (the only form the project uses). ROI-aware. */
void      cvCopy(const CvArr *src, CvArr *dst, const CvArr *mask);

void      cvAbsDiff(const CvArr *src1, const CvArr *src2, CvArr *dst);
double    cvThreshold(const CvArr *src, CvArr *dst,
                      double threshold, double max_value, int threshold_type);
int       cvCountNonZero(const CvArr *arr);

/* cvCvtColor lives in imgproc_c.h in real OpenCV; declared there (shim). */

#ifdef __cplusplus
}
#endif

#endif /* MINI_CV_CORE_C_H */
