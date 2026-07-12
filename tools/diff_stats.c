/*
 * diff_stats — per-frame change analysis for a recorded BMP sequence.
 * Prints, for every frame vs frame 0 (the implicit reference):
 *   - occupancy % in the box ROI  (reference/lean detector's core metric)
 *   - occupancy % in the full frame (person/motion activity)
 * Usage: ./diff_stats DIR [roi_x roi_y roi_w roi_h] [pixel_threshold] [ref_index]
 */
#include "frame_source.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *gray_copy(const IplImage *img) {
    unsigned char *g = malloc((size_t)img->width * img->height);
    if (!g) return NULL;
    for (int y = 0; y < img->height; y++) {
        const unsigned char *row = (const unsigned char *)img->imageData + (size_t)y * img->widthStep;
        for (int x = 0; x < img->width; x++) {
            const unsigned char *p = row + (size_t)x * img->nChannels;
            g[(size_t)y * img->width + x] = (img->nChannels == 1)
                ? p[0]
                : (unsigned char)((p[0] + (int)p[1] + p[2]) / 3);
        }
    }
    return g;
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "assets/fallback_footage";
    int rx = 650, ry = 70, rw = 610, rh = 510, thr = 25;
    if (argc >= 6) { rx = atoi(argv[2]); ry = atoi(argv[3]); rw = atoi(argv[4]); rh = atoi(argv[5]); }
    if (argc >= 7) thr = atoi(argv[6]);
    int ref_index = argc >= 8 ? atoi(argv[7]) : 0;

    FrameSource *fs = frame_source_create(FRAME_SOURCE_FALLBACK, 0, dir);
    if (!fs) { fprintf(stderr, "cannot open %s\n", dir); return 1; }
    frame_source_set_loop(fs, false);
    frame_source_set_fps(fs, 0);

    unsigned char *ref = NULL;
    int W = 0, H = 0, i = 0;
    Frame *fr;
    printf("frame  roi_occ%%  full_occ%%\n");
    while ((fr = frame_source_get_next(fs)) != NULL) {
        unsigned char *g = gray_copy(fr->image);
        if (!g) break;
        if (!ref) {
            if (i < ref_index) { free(g); i++; continue; }
            ref = g;
            W = fr->image->width; H = fr->image->height;
            if (rx + rw > W) rw = W - rx;
            if (ry + rh > H) rh = H - ry;
            fprintf(stderr, "ref %dx%d roi=(%d,%d,%dx%d) thr=%d\n", W, H, rx, ry, rw, rh, thr);
            printf("%5d  %8.2f  %9.2f\n", i++, 0.0, 0.0);
            continue;
        }
        long roi_hits = 0, full_hits = 0;
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                int d = (int)g[(size_t)y * W + x] - (int)ref[(size_t)y * W + x];
                if (d < 0) d = -d;
                if (d > thr) {
                    full_hits++;
                    if (x >= rx && x < rx + rw && y >= ry && y < ry + rh) roi_hits++;
                }
            }
        }
        printf("%5d  %8.2f  %9.2f\n", i++,
               100.0 * roi_hits / ((double)rw * rh),
               100.0 * full_hits / ((double)W * H));
        free(g);
    }
    free(ref);
    frame_source_destroy(fs);
    return 0;
}
