/*
 * overlay.h — Role D visual overlay (PRD: hours 8–10).
 *
 * Draws the demo HUD directly into the frame's pixel buffer: danger-zone
 * polygon, person markers, state banner, detector metrics, and a blinking
 * alarm border. Pure C pixel writes — no OpenCV drawing calls — so it works
 * regardless of which OpenCV modules the QNX port provides.
 *
 * Whatever sink Role A uses to display frames will therefore show the
 * annotated image; overlay_write_bmp() is the debug/fallback sink.
 */
#ifndef OVERLAY_H
#define OVERLAY_H

#include <stdbool.h>

#include "common_types.h"

typedef struct {
    const Polygon *zone;          /* danger zone; NULL = don't draw          */
    const Point   *persons;       /* person points from Role B; may be NULL  */
    int            person_count;
    SystemState    state;         /* from Role C state machine               */
    bool  motion_active;          /* falling (motion) detector triggered     */
    bool  lean_active;            /* leaning (reference/angle) detector      */
    float occupancy_pct;          /* reference occupancy, pass < 0 to hide   */
    float angle_deg;              /* angle deviation from reference          */
    bool  angle_valid;            /* false = occupancy-only fallback mode    */
    bool  person_in_zone;         /* zone_check_is_breached() result         */
    bool  alert_on;               /* gpio_alert_is_alarm_on()                */
    float fps;                    /* pass <= 0 to hide                       */
} OverlayData;

/*
 * Annotate frame->image in place. Supports 8-bit gray (1ch), BGR (3ch) and
 * BGRA (4ch) images. Blink phases derive from frame->timestamp_ms, so
 * rendering is deterministic and needs no clock. Out-of-range zone/person
 * coordinates are clipped safely. No-op on NULL/unsupported input.
 */
void overlay_render(Frame *frame, const OverlayData *od);

/*
 * Write the frame as a 24-bit BMP (gray frames are expanded). Used by the
 * host tests and as a demo fallback when no display sink is available.
 * Returns 0 on success, -1 on error.
 */
int overlay_write_bmp(const Frame *frame, const char *path);

#endif /* OVERLAY_H */
