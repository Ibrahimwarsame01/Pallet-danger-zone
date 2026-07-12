#ifndef COMMON_TYPES_H
#define COMMON_TYPES_H

#include <stdbool.h>
#include <stdint.h>
#include <opencv2/core/core_c.h>

/* ---------------------------------------------------------------------------
 * Frame — one captured image plus metadata.
 * Owned by the FrameSource; callers must not free image directly.
 * --------------------------------------------------------------------------- */
typedef struct {
    IplImage  *image;
    uint64_t   timestamp_ms;
    int        frame_id;
} Frame;

/* ---------------------------------------------------------------------------
 * Point — pixel coordinate.
 * Used for person centroid points and zone polygon vertices.
 * --------------------------------------------------------------------------- */
typedef struct {
    int x;
    int y;
} Point;

/* ---------------------------------------------------------------------------
 * Polygon — fixed danger zone.
 * Load vertices from config at startup; do not change at runtime.
 * --------------------------------------------------------------------------- */
typedef struct {
    Point *vertices;
    int    count;
} Polygon;

/* ---------------------------------------------------------------------------
 * SystemState — global alarm state machine.
 * Transitions:  NORMAL -> HAZARD_ACTIVE -> ALARMING -> NORMAL
 * --------------------------------------------------------------------------- */
typedef enum {
    STATE_NORMAL = 0,
    STATE_HAZARD_ACTIVE,
    STATE_ALARMING
} SystemState;

/* ---------------------------------------------------------------------------
 * FrameSource — opaque handle; use frame_source_get_next() to read frames.
 * --------------------------------------------------------------------------- */
typedef struct FrameSource FrameSource;

/* ---------------------------------------------------------------------------
 * Cross-role function contracts — implement in respective modules,
 * declared here so every role can depend on the signature.
 * --------------------------------------------------------------------------- */
Frame  *frame_source_get_next(FrameSource *src);
Point  *person_detector_get_points(Frame *frame, int *count_out);
bool    zone_check_is_breached(Polygon *zone, Point *points, int count);

#endif /* COMMON_TYPES_H */
