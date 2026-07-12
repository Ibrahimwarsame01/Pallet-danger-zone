// frame_source.h — Role A
#ifndef FRAME_SOURCE_H
#define FRAME_SOURCE_H

#include "common_types.h"

/* Which backend a FrameSource reads from. */
typedef enum {
    FRAME_SOURCE_LIVE,
    FRAME_SOURCE_FALLBACK
} FrameSourceType;

/* Opens a frame source.
 * - FRAME_SOURCE_LIVE: opens the Pi camera via the QNX camera API.
 *   camera_device_index 0 -> CAMERA_UNIT_1, 1 -> CAMERA_UNIT_2. fallback_dir ignored.
 * - FRAME_SOURCE_FALLBACK: plays the numbered .bmp/.ppm sequence in fallback_dir
 *   (NULL -> "assets/fallback_footage"), sorted by filename. camera_device_index ignored.
 * Returns NULL on failure (camera unavailable / no readable frames in dir).
 */
FrameSource *frame_source_create(FrameSourceType type,
                                 int camera_device_index,
                                 const char *fallback_dir);

/* frame_source_get_next(FrameSource*) -> Frame* is declared in common_types.h
 * as the locked cross-role contract; implemented in frame_source.c.
 * The returned Frame (and its image) is owned by the source and stays valid
 * only until the next get_next() or destroy() — copy out anything you keep.
 * Returns NULL on camera timeout, or at end-of-sequence when looping is off. */

/* Fallback only: wrap to frame 0 at end-of-sequence (default true), or return
 * NULL once for a single pass. No-op on a live source. */
void frame_source_set_loop(FrameSource *src, bool loop);

/* Fallback only: playback pacing in frames/sec (default 15.0; <= 0 disables
 * pacing and plays as fast as frames can be decoded). No-op on live. */
void frame_source_set_fps(FrameSource *src, double fps);

void frame_source_destroy(FrameSource *src);

#endif /* FRAME_SOURCE_H */
