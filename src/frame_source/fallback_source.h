// fallback_source.h — Role A
#ifndef FALLBACK_SOURCE_H
#define FALLBACK_SOURCE_H

#include "common_types.h"

typedef struct FallbackSource FallbackSource;

/* Opens a directory of recorded frames and sorts them by filename, so use
 * zero-padded numbering (frame_00001.bmp, frame_00002.bmp, ...).
 * Supported formats: 24/32-bit uncompressed BMP and binary PPM (P6) — decoded
 * in-project, no OpenCV imgcodecs needed (OpenCV 4 dropped the C codec API).
 * Produce footage with `test_frame_source --live --record <dir>` on the Pi, or
 * `ffmpeg -i clip.mp4 -pix_fmt bgr24 <dir>/frame_%05d.bmp` from any video.
 * Returns NULL if the directory has no readable frames. */
FallbackSource *fallback_source_open(const char *dir);

/* Returns the next frame in sequence. If loop is true, wraps back to the
 * first frame after the last one; if false, returns NULL once exhausted.
 * The returned Frame is owned by the FallbackSource and is valid only until
 * the next fallback_source_read() or fallback_source_close(). */
Frame *fallback_source_read(FallbackSource *src, bool loop);

void fallback_source_close(FallbackSource *src);

#endif /* FALLBACK_SOURCE_H */
