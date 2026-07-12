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
 * reference image ROI. If the fraction of pixels that changed exceeds
 * `occupancy_trigger` for `persistence` consecutive frames, hazard fires.
 *
 * Capture the reference with scripts/capture_reference.py (before demo).
 * -------------------------------------------------------------------------- */

typedef struct ReferenceDetector ReferenceDetector;

/* reference_path       : file loaded via cvLoadImage (any format)
 * roi_x/y/w/h          : rectangle in reference/frame coords; pass 0,0,0,0 for full image
 * pixel_threshold      : per-pixel diff (0-255) to count as changed. PRD default 25.
 * occupancy_trigger    : ROI fraction to trip hazard (0.0-1.0). PRD default 0.08.
 * persistence          : consecutive tripping frames before returning true.
 */
ReferenceDetector *reference_detector_create(const char *reference_path,
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
