// test_frame_source.c — Role A standalone smoke test + footage recorder.
//
// Verifies the frame source end-to-end without any other module:
//   ./test_frame_source --fallback assets/fallback_footage        # play footage
//   ./test_frame_source --live --frames 30                        # camera check (on the Pi)
//   ./test_frame_source --live --record assets/fallback_footage --frames 300
//                                                                 # record demo footage
//   ./test_frame_source --live --record . --frames 1              # grab a reference still
//
// Exit codes: 0 ok, 1 bad usage/source failed to open, 2 frames stopped mid-run.

#include "frame_source.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#endif

static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* Plain 24-bit BI_RGB bottom-up BMP — matches what fallback_source reads. */
static bool write_bmp(const char *path, const IplImage *img) {
    if (!img || img->nChannels != 3) {
        return false;
    }
    int      w = img->width, h = img->height;
    size_t   rowbytes = (((size_t)w * 3) + 3) & ~(size_t)3;
    uint32_t filesize = 54 + (uint32_t)(rowbytes * (size_t)h);

    uint8_t hdr[54];
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    wr32(hdr + 2, filesize);
    wr32(hdr + 10, 54);              /* pixel data offset */
    wr32(hdr + 14, 40);              /* BITMAPINFOHEADER size */
    wr32(hdr + 18, (uint32_t)w);
    wr32(hdr + 22, (uint32_t)h);     /* positive = bottom-up */
    wr16(hdr + 26, 1);               /* planes */
    wr16(hdr + 28, 24);              /* bpp */
    wr32(hdr + 34, (uint32_t)(rowbytes * (size_t)h));
    wr32(hdr + 38, 2835);            /* ~72 dpi */
    wr32(hdr + 42, 2835);

    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "cannot write %s\n", path);
        return false;
    }
    bool ok = fwrite(hdr, 1, sizeof(hdr), f) == sizeof(hdr);
    static const uint8_t pad[3] = {0, 0, 0};
    for (int y = h - 1; y >= 0 && ok; y--) {
        const uint8_t *row = (const uint8_t *)img->imageData + (size_t)y * img->widthStep;
        ok = fwrite(row, 1, (size_t)w * 3, f) == (size_t)w * 3 &&
             fwrite(pad, 1, rowbytes - (size_t)w * 3, f) == rowbytes - (size_t)w * 3;
    }
    fclose(f);
    if (!ok) {
        fprintf(stderr, "short write on %s\n", path);
    }
    return ok;
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s [--live [--device N] | --fallback DIR]\n"
            "          [--frames N] [--fps F] [--no-loop] [--record DIR]\n"
            "defaults: --fallback assets/fallback_footage, --frames 60\n",
            argv0);
}

int main(int argc, char **argv) {
    FrameSourceType type = FRAME_SOURCE_FALLBACK;
    const char *dir = "assets/fallback_footage";
    const char *record_dir = NULL;
    int device = 0, frames = 60;
    double fps = -1.0;
    bool no_loop = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--live") == 0) {
            type = FRAME_SOURCE_LIVE;
        } else if (strcmp(argv[i], "--fallback") == 0 && i + 1 < argc) {
            type = FRAME_SOURCE_FALLBACK;
            dir = argv[++i];
        } else if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
            device = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
            fps = atof(argv[++i]);
        } else if (strcmp(argv[i], "--no-loop") == 0) {
            no_loop = true;
        } else if (strcmp(argv[i], "--record") == 0 && i + 1 < argc) {
            record_dir = argv[++i];
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    if (record_dir) {
        mkdir(record_dir, 0755);
    }

    FrameSource *src = frame_source_create(type, device, dir);
    if (!src) {
        fprintf(stderr, "FAIL: could not open %s source\n",
                type == FRAME_SOURCE_LIVE ? "live" : "fallback");
        return 1;
    }
    if (no_loop) {
        frame_source_set_loop(src, false);
    }
    if (fps > 0) {
        frame_source_set_fps(src, fps);
    }

    uint64_t prev_ts = 0;
    int got = 0;
    for (int i = 0; i < frames; i++) {
        Frame *fr = frame_source_get_next(src);
        if (!fr) {
            if (no_loop && type == FRAME_SOURCE_FALLBACK) {
                printf("end of sequence after %d frames\n", got);
                break;
            }
            fprintf(stderr, "FAIL: no frame at iteration %d (got %d before that)\n", i, got);
            frame_source_destroy(src);
            return 2;
        }
        printf("frame %4d  id=%-5d  %dx%d  ts=%" PRIu64 "  (+%" PRIu64 " ms)\n",
               i, fr->frame_id, fr->image->width, fr->image->height,
               fr->timestamp_ms, prev_ts ? fr->timestamp_ms - prev_ts : 0);
        prev_ts = fr->timestamp_ms;
        got++;

        if (record_dir) {
            char path[600];
            snprintf(path, sizeof(path), "%s/frame_%05d.bmp", record_dir, i);
            if (!write_bmp(path, fr->image)) {
                frame_source_destroy(src);
                return 2;
            }
        }
    }

    frame_source_destroy(src);
    printf("OK — received %d frame(s)%s\n", got,
           record_dir ? " and recorded them as BMP" : "");
    return 0;
}
