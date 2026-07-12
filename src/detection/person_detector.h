#ifndef PERSON_DETECTOR_H
#define PERSON_DETECTOR_H

#include "../../include/common_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------------------------
 * Person detector (Role B).
 *
 * Backend: OpenCV C API Haar cascade (haarcascade_fullbody.xml).
 * Rationale: cv::HOGDescriptor is C++-only in OpenCV 4.x used by QNX SDP 8.0.
 *            The Haar C API is present and stable across 3.x / 4.x.
 *
 * Runtime dependency:
 *   A cascade XML file. By default the detector searches:
 *     - assets/haarcascade_fullbody.xml   (project-local)
 *     - /usr/share/opencv4/haarcascades/haarcascade_fullbody.xml
 *     - /usr/share/opencv/haarcascades/haarcascade_fullbody.xml
 *   Grab the file from OpenCV's data/haarcascades and drop it into assets/.
 *
 * Threading: not thread-safe. Main loop is single-threaded per PRD.
 * -------------------------------------------------------------------------- */

/* Initialize the detector.
 * cascade_path: absolute or relative XML path, or NULL to try defaults.
 * Returns 0 on success, non-zero on failure. Safe to call twice (no-op if init'd).
 */
int person_detector_init(const char *cascade_path);

/* Cross-role contract from common_types.h:
 *   Detect people in the frame.
 *   Returns a heap-allocated Point[] of foot positions (bottom-center of bbox,
 *   in ORIGINAL frame coordinates — internal downscaling is undone).
 *   Caller MUST free() the returned array (unless NULL).
 *   On no detections or error: *count_out = 0, returns NULL.
 *
 *   If person_detector_init() has not been called, this lazily inits with
 *   default cascade paths; if that also fails, returns NULL.
 */
/* Declared in common_types.h; do not redeclare here to avoid drift. */

/* Release cascade + memory storage. Call once at shutdown. */
void person_detector_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* PERSON_DETECTOR_H */
