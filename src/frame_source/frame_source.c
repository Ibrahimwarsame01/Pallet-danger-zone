// frame_source.c — Role A
//
// Dispatcher behind the locked cross-role contract
// frame_source_get_next(FrameSource*) -> Frame*  (see include/common_types.h).
// Also paces fallback playback to a realistic frame rate so recorded footage
// behaves like the live camera for every downstream module ("fallback parity").

#include "frame_source.h"
#include "live_camera.h"
#include "fallback_source.h"

#include <stdlib.h>
#include <time.h>

#define DEFAULT_FALLBACK_DIR "assets/fallback_footage"
#define DEFAULT_FALLBACK_FPS 15.0

struct FrameSource {
    FrameSourceType type;
    bool            loop;
    double          fps;
    uint64_t        last_emit_ms;
    union {
        LiveCamera     *live;
        FallbackSource *fallback;
    } backend;
};

static uint64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void sleep_ms(uint64_t ms) {
    struct timespec ts;
    ts.tv_sec  = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    nanosleep(&ts, NULL);
}

FrameSource *frame_source_create(FrameSourceType type,
                                 int camera_device_index,
                                 const char *fallback_dir) {
    FrameSource *src = calloc(1, sizeof(*src));
    if (!src) {
        return NULL;
    }
    src->type = type;
    src->loop = true;
    src->fps  = DEFAULT_FALLBACK_FPS;

    if (type == FRAME_SOURCE_LIVE) {
        src->backend.live = live_camera_open(camera_device_index);
        if (!src->backend.live) {
            free(src);
            return NULL;
        }
    } else {
        src->backend.fallback =
            fallback_source_open(fallback_dir ? fallback_dir : DEFAULT_FALLBACK_DIR);
        if (!src->backend.fallback) {
            free(src);
            return NULL;
        }
    }
    return src;
}

Frame *frame_source_get_next(FrameSource *src) {
    if (!src) {
        return NULL;
    }
    if (src->type == FRAME_SOURCE_LIVE) {
        return live_camera_read(src->backend.live);
    }

    if (src->fps > 0 && src->last_emit_ms > 0) {
        uint64_t interval = (uint64_t)(1000.0 / src->fps);
        uint64_t elapsed  = mono_ms() - src->last_emit_ms;
        if (elapsed < interval) {
            sleep_ms(interval - elapsed);
        }
    }
    Frame *frame = fallback_source_read(src->backend.fallback, src->loop);
    src->last_emit_ms = mono_ms();
    return frame;
}

void frame_source_set_loop(FrameSource *src, bool loop) {
    if (src) {
        src->loop = loop;
    }
}

void frame_source_set_fps(FrameSource *src, double fps) {
    if (src) {
        src->fps = fps;
    }
}

void frame_source_destroy(FrameSource *src) {
    if (!src) {
        return;
    }
    if (src->type == FRAME_SOURCE_LIVE) {
        live_camera_close(src->backend.live);
    } else {
        fallback_source_close(src->backend.fallback);
    }
    free(src);
}
