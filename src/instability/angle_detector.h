#ifndef ANGLE_DETECTOR_H
#define ANGLE_DETECTOR_H

#include "../../include/common_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------------------------
 * Angle detector (Role C) — pallet leaning via contour orientation.
 *
 * HIGHEST-RISK MODULE per PRD: if this is not clean by hour 9, fall back to
 * reference-image occupancy only. Toggle at runtime by not calling _update()
 * — the module contributes nothing unless updated.
 *
 * Pipeline per frame:
 *   1. Gray + ROI
 *   2. Otsu threshold -> binary mask
 *   3. cvFindContours -> pick largest by area
 *   4. cvMinAreaRect2 -> extract "long-edge" angle (0-180)
 *   5. Deviation = |current - reference| normalized to [0, 90]
 *   6. Fire when deviation > threshold for `persistence` frames
 * -------------------------------------------------------------------------- */

typedef struct AngleDetector AngleDetector;

/* reference_angle_deg      : baseline "stable pallet" long-edge angle, [0, 180)
 * deviation_threshold_deg  : |cur - ref| that trips hazard. PRD default 10-15.
 * roi_x/y/w/h              : ROI containing pallet region; 0,0,0,0 = full image
 * persistence              : consecutive tripping frames before returning true
 */
AngleDetector *angle_detector_create(double reference_angle_deg,
                                     double deviation_threshold_deg,
                                     int roi_x, int roi_y,
                                     int roi_w, int roi_h,
                                     int persistence);
void           angle_detector_destroy(AngleDetector *ad);

bool           angle_detector_update(AngleDetector *ad, Frame *frame);

double         angle_detector_get_angle(const AngleDetector *ad);
double         angle_detector_get_deviation(const AngleDetector *ad);

#ifdef __cplusplus
}
#endif

#endif /* ANGLE_DETECTOR_H */
