#ifndef MOTION_DETECTOR_H
#define MOTION_DETECTOR_H

#include "../../include/common_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------------------------
 * Motion detector (Role C) — pallet-falling signal.
 *
 * Pipeline per frame:
 *   1. Gray current + gray previous
 *   2. cvAbsDiff -> cvThreshold -> cvCountNonZero
 *   3. If changed_fraction > area_fraction_trigger, count a "hit"
 *   4. Fire only after `persistence` consecutive hits (debounce)
 *
 * Threading: not thread-safe. One MotionDetector per frame source.
 * -------------------------------------------------------------------------- */

typedef struct MotionDetector MotionDetector;

/* pixel_threshold        : per-pixel diff (0-255) to count as changed. PRD default 25.
 * area_fraction_trigger  : fraction of pixels changed to count as motion (0.0-1.0). ~0.05.
 * persistence            : consecutive frames a "hit" must repeat before returning true.
 */
MotionDetector *motion_detector_create(int pixel_threshold,
                                       double area_fraction_trigger,
                                       int persistence);
void            motion_detector_destroy(MotionDetector *md);

/* Feed the next frame. Returns true when persistence threshold met. */
bool            motion_detector_update(MotionDetector *md, Frame *frame);

/* Last computed changed-fraction (for logging/tuning). */
double          motion_detector_get_score(const MotionDetector *md);

#ifdef __cplusplus
}
#endif

#endif /* MOTION_DETECTOR_H */
