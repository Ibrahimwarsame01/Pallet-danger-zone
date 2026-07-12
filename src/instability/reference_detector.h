#ifndef REFERENCE_DETECTOR_H
#define REFERENCE_DETECTOR_H

#include "../../include/common_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------------------------
 * Reference-image detector (Role C) — pallet-leaning signal.
 *
 * Compares the current frame's ROI against a pre-captured "stable pallet"
 * reference. When the fraction of pixels that changed exceeds
 * `occupancy_trigger` for `persistence` consecutive frames, hazard fires.
 *
 * The CALLER loads the reference — this module does not depend on OpenCV's
 * imgcodecs (cvLoadImage was dropped from OpenCV 4 C API and is not present
 * in the QNX port). Recommended loader: open a directory with fallback_source
 * (Role A) and read one frame:
 *
 *     FallbackSource   *ref_src = fallback_source_open("assets/reference/");
 *     Frame            *ref     = fallback_source_read(ref_src, false);
 *     ReferenceDetector *rd    = reference_detector_create(ref->image, ...);
 *     fallback_source_close(ref_src);   // safe: create() clones the image
 *
 * The reference is CLONED internally as grayscale; caller may release theirs
 * immediately after create.
 * -------------------------------------------------------------------------- */

typedef struct ReferenceDetector ReferenceDetector;

/* reference_image      : any-channel IplImage (1/3/4); cloned+grayscaled internally
 * roi_x/y/w/h          : rectangle in reference/frame coords; pass 0,0,0,0 for full image
 * pixel_threshold      : per-pixel diff (0-255) to count as changed. PRD default 25.
 * occupancy_trigger    : ROI fraction to trip hazard (0.0-1.0). PRD default 0.08.
 * persistence          : consecutive tripping frames before returning true.
 */
ReferenceDetector *reference_detector_create(IplImage *reference_image,
                                             int roi_x, int roi_y,
                                             int roi_w, int roi_h,
                                             int pixel_threshold,
                                             double occupancy_trigger,
                                             int persistence);
void               reference_detector_destroy(ReferenceDetector *rd);

bool               reference_detector_update(ReferenceDetector *rd, Frame *frame);
double             reference_detector_get_occupancy(const ReferenceDetector *rd);

#ifdef __cplusplus
}
#endif

#endif /* REFERENCE_DETECTOR_H */
