// fallback_source.c — Role A
//
// Plays a recorded frame sequence from a directory, sorted by filename.
// Decodes 24/32-bit uncompressed BMP and binary PPM (P6) itself: OpenCV 4
// removed the C imgcodecs API (cvLoadImage), so depending on it would not
// link against the QNX OpenCV port. BMP/PPM keep this path dependency-free —
// `ffmpeg -i clip.mp4 -pix_fmt bgr24 dir/frame_%05d.bmp` converts anything.

#include "fallback_source.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define FALLBACK_MAX_FRAMES 4096
#define FALLBACK_MAX_PATH   512
#define FALLBACK_MAX_NAME   256

struct FallbackSource {
    char  dir[FALLBACK_MAX_PATH];
    char (*filenames)[FALLBACK_MAX_NAME];
    int   count;
    int   index;
    Frame frame;
    int   next_frame_id;
};

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int filename_cmp(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

static bool has_supported_extension(const char *name) {
    size_t len = strlen(name);
    if (name[0] == '.' || len < 5) {
        return false;
    }
    const char *ext = name + len - 4;
    return strcasecmp(ext, ".bmp") == 0 || strcasecmp(ext, ".ppm") == 0;
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Uncompressed BI_RGB, 24 or 32 bpp, top-down or bottom-up. Output is BGR
 * (BMP's native byte order, which is also what OpenCV code expects). */
static IplImage *load_bmp(FILE *f, const char *path) {
    uint8_t hdr[54];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) || hdr[0] != 'B' || hdr[1] != 'M') {
        fprintf(stderr, "[fallback] %s: not a BMP\n", path);
        return NULL;
    }
    uint32_t data_off = rd32(hdr + 10);
    uint32_t info_sz  = rd32(hdr + 14);
    int32_t  w        = (int32_t)rd32(hdr + 18);
    int32_t  h        = (int32_t)rd32(hdr + 22);
    uint16_t bpp      = rd16(hdr + 28);
    uint32_t comp     = rd32(hdr + 30);
    if (info_sz < 40 || w <= 0 || h == 0 || (bpp != 24 && bpp != 32) || comp != 0) {
        fprintf(stderr, "[fallback] %s: unsupported BMP variant "
                        "(need uncompressed 24/32-bit; got bpp=%u comp=%u)\n",
                path, (unsigned)bpp, (unsigned)comp);
        return NULL;
    }
    bool bottom_up = h > 0;
    int  ah        = bottom_up ? h : -h;
    if (fseek(f, (long)data_off, SEEK_SET) != 0) {
        return NULL;
    }

    size_t   srow   = (((size_t)w * (bpp / 8)) + 3) & ~(size_t)3;
    uint8_t *rowbuf = malloc(srow);
    IplImage *img   = cvCreateImage(cvSize(w, ah), IPL_DEPTH_8U, 3);
    if (!rowbuf || !img) {
        free(rowbuf);
        if (img) cvReleaseImage(&img);
        return NULL;
    }
    for (int i = 0; i < ah; i++) {
        if (fread(rowbuf, 1, srow, f) != srow) {
            fprintf(stderr, "[fallback] %s: truncated pixel data\n", path);
            free(rowbuf);
            cvReleaseImage(&img);
            return NULL;
        }
        int      dy  = bottom_up ? (ah - 1 - i) : i;
        uint8_t *dst = (uint8_t *)img->imageData + (size_t)dy * img->widthStep;
        if (bpp == 24) {
            memcpy(dst, rowbuf, (size_t)w * 3);
        } else {
            const uint8_t *s = rowbuf;
            for (int x = 0; x < w; x++) {
                dst[0] = s[0]; dst[1] = s[1]; dst[2] = s[2];
                dst += 3; s += 4;
            }
        }
    }
    free(rowbuf);
    return img;
}

/* Reads the next integer in a PPM header, skipping whitespace and #comments.
 * Consumes exactly one delimiter after the digits — which is precisely the
 * single whitespace byte the spec puts between the maxval and pixel data. */
static int ppm_next_int(FILE *f) {
    int c;
    for (;;) {
        c = fgetc(f);
        if (c == '#') {
            while ((c = fgetc(f)) != EOF && c != '\n') {}
        } else if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            continue;
        } else {
            break;
        }
    }
    if (c == EOF || c < '0' || c > '9') {
        return -1;
    }
    int v = 0;
    while (c >= '0' && c <= '9') {
        v = v * 10 + (c - '0');
        if (v > (1 << 24)) {
            return -1;
        }
        c = fgetc(f);
    }
    return v;
}

static IplImage *load_ppm(FILE *f, const char *path) {
    if (fgetc(f) != 'P' || fgetc(f) != '6') {
        fprintf(stderr, "[fallback] %s: not a binary PPM (P6)\n", path);
        return NULL;
    }
    int w = ppm_next_int(f);
    int h = ppm_next_int(f);
    int maxv = ppm_next_int(f);
    if (w <= 0 || h <= 0 || maxv != 255) {
        fprintf(stderr, "[fallback] %s: unsupported PPM header (w=%d h=%d maxval=%d)\n",
                path, w, h, maxv);
        return NULL;
    }
    IplImage *img = cvCreateImage(cvSize(w, h), IPL_DEPTH_8U, 3);
    size_t    rowbytes = (size_t)w * 3;
    uint8_t  *rowbuf   = malloc(rowbytes);
    if (!img || !rowbuf) {
        free(rowbuf);
        if (img) cvReleaseImage(&img);
        return NULL;
    }
    for (int y = 0; y < h; y++) {
        if (fread(rowbuf, 1, rowbytes, f) != rowbytes) {
            fprintf(stderr, "[fallback] %s: truncated pixel data\n", path);
            free(rowbuf);
            cvReleaseImage(&img);
            return NULL;
        }
        uint8_t       *dst = (uint8_t *)img->imageData + (size_t)y * img->widthStep;
        const uint8_t *s   = rowbuf;
        for (int x = 0; x < w; x++) {          /* PPM is RGB; flip to BGR */
            dst[0] = s[2]; dst[1] = s[1]; dst[2] = s[0];
            dst += 3; s += 3;
        }
    }
    free(rowbuf);
    return img;
}

static IplImage *load_frame(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[fallback] cannot open %s\n", path);
        return NULL;
    }
    size_t      len = strlen(path);
    IplImage   *img;
    if (len >= 4 && strcasecmp(path + len - 4, ".ppm") == 0) {
        img = load_ppm(f, path);
    } else {
        img = load_bmp(f, path);
    }
    fclose(f);
    return img;
}

FallbackSource *fallback_source_open(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) {
        fprintf(stderr, "[fallback] cannot open directory %s\n", dir);
        return NULL;
    }

    FallbackSource *src = calloc(1, sizeof(*src));
    if (src) {
        src->filenames = calloc(FALLBACK_MAX_FRAMES, FALLBACK_MAX_NAME);
    }
    if (!src || !src->filenames) {
        if (src) free(src->filenames);
        free(src);
        closedir(d);
        return NULL;
    }
    strncpy(src->dir, dir, sizeof(src->dir) - 1);

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL && src->count < FALLBACK_MAX_FRAMES) {
        if (has_supported_extension(entry->d_name)) {
            strncpy(src->filenames[src->count], entry->d_name, FALLBACK_MAX_NAME - 1);
            src->count++;
        }
    }
    closedir(d);

    if (src->count == 0) {
        fprintf(stderr, "[fallback] no .bmp/.ppm frames in %s "
                        "(convert footage: ffmpeg -i clip.mp4 -pix_fmt bgr24 %s/frame_%%05d.bmp)\n",
                dir, dir);
        free(src->filenames);
        free(src);
        return NULL;
    }

    qsort(src->filenames, src->count, FALLBACK_MAX_NAME, filename_cmp);
    return src;
}

Frame *fallback_source_read(FallbackSource *src, bool loop) {
    if (!src) {
        return NULL;
    }
    if (src->index >= src->count) {
        if (!loop) {
            return NULL;
        }
        src->index = 0;
    }

    char path[FALLBACK_MAX_PATH + FALLBACK_MAX_NAME + 2];
    snprintf(path, sizeof(path), "%s/%s", src->dir, src->filenames[src->index]);

    if (src->frame.image) {
        cvReleaseImage(&src->frame.image);
    }
    src->frame.image = load_frame(path);
    if (!src->frame.image) {
        return NULL;
    }

    src->frame.timestamp_ms = now_ms();
    src->frame.frame_id     = src->next_frame_id++;
    src->index++;
    return &src->frame;
}

void fallback_source_close(FallbackSource *src) {
    if (!src) {
        return;
    }
    if (src->frame.image) {
        cvReleaseImage(&src->frame.image);
    }
    free(src->filenames);
    free(src);
}
