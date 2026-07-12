/* Shim for the imgproc C API subset PalletGuard uses — see core_c.h here. */
#ifndef MINI_CV_IMGPROC_C_H
#define MINI_CV_IMGPROC_C_H

#include <opencv2/core/core_c.h>
#include <opencv2/imgproc/types_c.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Only gray conversions are implemented (CV_BGR2GRAY / CV_BGRA2GRAY). */
void cvCvtColor(const CvArr *src, CvArr *dst, int code);

#ifdef __cplusplus
}
#endif

#endif /* MINI_CV_IMGPROC_C_H */
