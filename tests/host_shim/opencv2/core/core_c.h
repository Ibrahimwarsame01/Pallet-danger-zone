/*
 * Host-only stand-in for OpenCV's C core header so Role D units compile and
 * unit-test on a dev machine without OpenCV installed
 * (include/common_types.h does #include <opencv2/core/core_c.h>).
 *
 * QNX target builds MUST use the real QNX-ported OpenCV headers instead:
 * this directory is only added to the include path for host test builds and
 * for the OpenCV-free gpio_test tool (see Makefile). It is NOT binary
 * compatible with real OpenCV — never link code built against this shim
 * together with code built against the real headers.
 *
 * Only the fields Role D actually touches are provided.
 */
#ifndef HOST_SHIM_OPENCV_CORE_C_H
#define HOST_SHIM_OPENCV_CORE_C_H

#define IPL_DEPTH_8U 8

typedef struct _IplImage {
    int   nChannels;  /* 1 = gray, 3 = BGR, 4 = BGRA */
    int   depth;      /* IPL_DEPTH_8U expected */
    int   width;
    int   height;
    char *imageData;  /* row-major pixel data */
    int   widthStep;  /* bytes per row (may include padding) */
} IplImage;

#endif /* HOST_SHIM_OPENCV_CORE_C_H */
