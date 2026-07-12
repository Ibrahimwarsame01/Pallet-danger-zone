/*
 * overlay.c — Role D visual overlay. Pure C pixel drawing on IplImage
 * buffers (gray/BGR/BGRA, 8-bit). No OpenCV function calls; only struct
 * field access, so it builds even if the QNX OpenCV port lacks imgproc.
 *
 * All primitives clip against the image bounds, so out-of-range zone or
 * person coordinates are safe.
 */
#include "overlay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alerts_internal.h"

typedef struct { uint8_t r, g, b; } Color;

static const Color COL_GREEN  = { 40, 200,  70 };
static const Color COL_AMBER  = { 255, 170,  0 };
static const Color COL_RED    = { 235,  45, 45 };
static const Color COL_WHITE  = { 235, 235, 235 };
static const Color COL_BANNER = {  22,  22, 22 };

/*
 * Classic 5x7 (with descender bit 8) bitmap font, ASCII 0x20..0x5A.
 * 5 bytes per glyph, column-major, bit 0 = top row.
 * From Adafruit-GFX-Library glcdfont.c (BSD license).
 */
static const uint8_t FONT5X7[][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, /* ' ' */
    {0x00, 0x00, 0x5F, 0x00, 0x00}, /* '!' */
    {0x00, 0x07, 0x00, 0x07, 0x00}, /* '"' */
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, /* '#' */
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, /* '$' */
    {0x23, 0x13, 0x08, 0x64, 0x62}, /* '%' */
    {0x36, 0x49, 0x56, 0x20, 0x50}, /* '&' */
    {0x00, 0x08, 0x07, 0x03, 0x00}, /* '\'' */
    {0x00, 0x1C, 0x22, 0x41, 0x00}, /* '(' */
    {0x00, 0x41, 0x22, 0x1C, 0x00}, /* ')' */
    {0x2A, 0x1C, 0x7F, 0x1C, 0x2A}, /* '*' */
    {0x08, 0x08, 0x3E, 0x08, 0x08}, /* '+' */
    {0x00, 0x80, 0x70, 0x30, 0x00}, /* ',' */
    {0x08, 0x08, 0x08, 0x08, 0x08}, /* '-' */
    {0x00, 0x00, 0x60, 0x60, 0x00}, /* '.' */
    {0x20, 0x10, 0x08, 0x04, 0x02}, /* '/' */
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, /* '0' */
    {0x00, 0x42, 0x7F, 0x40, 0x00}, /* '1' */
    {0x72, 0x49, 0x49, 0x49, 0x46}, /* '2' */
    {0x21, 0x41, 0x49, 0x4D, 0x33}, /* '3' */
    {0x18, 0x14, 0x12, 0x7F, 0x10}, /* '4' */
    {0x27, 0x45, 0x45, 0x45, 0x39}, /* '5' */
    {0x3C, 0x4A, 0x49, 0x49, 0x31}, /* '6' */
    {0x41, 0x21, 0x11, 0x09, 0x07}, /* '7' */
    {0x36, 0x49, 0x49, 0x49, 0x36}, /* '8' */
    {0x46, 0x49, 0x49, 0x29, 0x1E}, /* '9' */
    {0x00, 0x00, 0x14, 0x00, 0x00}, /* ':' */
    {0x00, 0x40, 0x34, 0x00, 0x00}, /* ';' */
    {0x00, 0x08, 0x14, 0x22, 0x41}, /* '<' */
    {0x14, 0x14, 0x14, 0x14, 0x14}, /* '=' */
    {0x00, 0x41, 0x22, 0x14, 0x08}, /* '>' */
    {0x02, 0x01, 0x59, 0x09, 0x06}, /* '?' */
    {0x3E, 0x41, 0x5D, 0x59, 0x4E}, /* '@' */
    {0x7C, 0x12, 0x11, 0x12, 0x7C}, /* 'A' */
    {0x7F, 0x49, 0x49, 0x49, 0x36}, /* 'B' */
    {0x3E, 0x41, 0x41, 0x41, 0x22}, /* 'C' */
    {0x7F, 0x41, 0x41, 0x41, 0x3E}, /* 'D' */
    {0x7F, 0x49, 0x49, 0x49, 0x41}, /* 'E' */
    {0x7F, 0x09, 0x09, 0x09, 0x01}, /* 'F' */
    {0x3E, 0x41, 0x41, 0x51, 0x73}, /* 'G' */
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, /* 'H' */
    {0x00, 0x41, 0x7F, 0x41, 0x00}, /* 'I' */
    {0x20, 0x40, 0x41, 0x3F, 0x01}, /* 'J' */
    {0x7F, 0x08, 0x14, 0x22, 0x41}, /* 'K' */
    {0x7F, 0x40, 0x40, 0x40, 0x40}, /* 'L' */
    {0x7F, 0x02, 0x1C, 0x02, 0x7F}, /* 'M' */
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, /* 'N' */
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, /* 'O' */
    {0x7F, 0x09, 0x09, 0x09, 0x06}, /* 'P' */
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, /* 'Q' */
    {0x7F, 0x09, 0x19, 0x29, 0x46}, /* 'R' */
    {0x26, 0x49, 0x49, 0x49, 0x32}, /* 'S' */
    {0x03, 0x01, 0x7F, 0x01, 0x03}, /* 'T' */
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, /* 'U' */
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, /* 'V' */
    {0x3F, 0x40, 0x38, 0x40, 0x3F}, /* 'W' */
    {0x63, 0x14, 0x08, 0x14, 0x63}, /* 'X' */
    {0x03, 0x04, 0x78, 0x04, 0x03}, /* 'Y' */
    {0x61, 0x59, 0x49, 0x4D, 0x43}, /* 'Z' */
};

static inline uint8_t luma(Color c)
{
    return (uint8_t)((77u * c.r + 150u * c.g + 29u * c.b) >> 8);
}

/* Clipped filled rectangle — the one primitive everything else uses. */
static void fill_rect(IplImage *img, int x, int y, int w, int h, Color c)
{
    if (w <= 0 || h <= 0)
        return;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    long x1l = (long)x + w;
    long y1l = (long)y + h;
    int x1 = x1l > img->width ? img->width : (int)x1l;
    int y1 = y1l > img->height ? img->height : (int)y1l;
    if (x0 >= x1 || y0 >= y1)
        return;

    for (int yy = y0; yy < y1; yy++) {
        uint8_t *row = (uint8_t *)img->imageData + (size_t)yy * (size_t)img->widthStep;
        if (img->nChannels == 1) {
            memset(row + x0, luma(c), (size_t)(x1 - x0));
        } else {
            for (int xx = x0; xx < x1; xx++) {
                uint8_t *p = row + (size_t)xx * (size_t)img->nChannels;
                p[0] = c.b;  /* OpenCV channel order is BGR */
                p[1] = c.g;
                p[2] = c.r;
            }
        }
    }
}

static void draw_rect_border(IplImage *img, int x, int y, int w, int h, int t, Color c)
{
    fill_rect(img, x, y, w, t, c);
    fill_rect(img, x, y + h - t, w, t, c);
    fill_rect(img, x, y, t, h, c);
    fill_rect(img, x + w - t, y, t, h, c);
}

static inline int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Bresenham line with square "dab" thickness. Endpoints are clamped to a box
 * around the image (extreme configs distort slightly instead of hanging). */
static void draw_line(IplImage *img, int x0, int y0, int x1, int y1, int t, Color c)
{
    int bw = img->width, bh = img->height;
    x0 = clampi(x0, -bw, 2 * bw);
    x1 = clampi(x1, -bw, 2 * bw);
    y0 = clampi(y0, -bh, 2 * bh);
    y1 = clampi(y1, -bh, 2 * bh);

    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    for (;;) {
        fill_rect(img, x0 - t / 2, y0 - t / 2, t, t, c);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void draw_polygon(IplImage *img, const Polygon *poly, int t, Color c)
{
    if (!poly || !poly->vertices || poly->count < 2)
        return;
    for (int i = 0; i < poly->count; i++) {
        Point a = poly->vertices[i];
        Point b = poly->vertices[(i + 1) % poly->count];
        draw_line(img, a.x, a.y, b.x, b.y, t, c);
    }
    for (int i = 0; i < poly->count; i++)
        fill_rect(img, poly->vertices[i].x - 3, poly->vertices[i].y - 3, 6, 6, c);
}

static void draw_marker(IplImage *img, Point p, Color arms, Color center)
{
    draw_line(img, p.x - 9, p.y, p.x + 9, p.y, 2, arms);
    draw_line(img, p.x, p.y - 9, p.x, p.y + 9, 2, arms);
    fill_rect(img, p.x - 2, p.y - 2, 5, 5, center);
}

static void draw_char(IplImage *img, int x, int y, int scale, Color c, int ch)
{
    if (ch >= 'a' && ch <= 'z')
        ch -= 'a' - 'A';
    if (ch < 0x20 || ch > 0x5A)
        ch = '?';
    const uint8_t *g = FONT5X7[ch - 0x20];
    for (int col = 0; col < 5; col++)
        for (int row = 0; row < 8; row++)
            if (g[col] & (1u << row))
                fill_rect(img, x + col * scale, y + row * scale, scale, scale, c);
}

static void draw_text(IplImage *img, int x, int y, int scale, Color c, const char *s)
{
    for (; *s; s++) {
        draw_char(img, x, y, scale, c, (unsigned char)*s);
        x += 6 * scale; /* 5 columns + 1 spacing */
    }
}

static bool image_ok(const IplImage *img)
{
    return img && img->imageData &&
           img->depth == IPL_DEPTH_8U &&
           (img->nChannels == 1 || img->nChannels == 3 || img->nChannels == 4) &&
           img->width > 0 && img->height > 0;
}

void overlay_render(Frame *frame, const OverlayData *od)
{
    if (!frame || !od || !image_ok(frame->image))
        return;
    IplImage *img = frame->image;

    const bool blink = (frame->timestamp_ms / 300u) % 2u == 0u;

    Color state_col = COL_GREEN;
    if (od->state == STATE_HAZARD_ACTIVE)
        state_col = COL_AMBER;
    else if (od->state == STATE_ALARMING)
        state_col = COL_RED;

    if (od->zone)
        draw_polygon(img, od->zone, 2, state_col);

    if (od->persons) {
        Color center = od->person_in_zone ? COL_RED : COL_WHITE;
        for (int i = 0; i < od->person_count; i++)
            draw_marker(img, od->persons[i], COL_WHITE, center);
    }

    const bool alarm_visual = od->state == STATE_ALARMING || od->alert_on;
    if (alarm_visual && blink)
        draw_rect_border(img, 0, 0, img->width, img->height, 6, COL_RED);

    /* --- banner: state line + metrics line --- */
    int s1 = img->width >= 480 ? 2 : 1;
    int s2 = img->width >= 960 ? 2 : 1;
    int banner_h = 4 + 8 * s1 + 2 + 8 * s2 + 4;
    fill_rect(img, 0, 0, img->width, banner_h, COL_BANNER);

    char line1[64];
    snprintf(line1, sizeof line1, "STATE: %s", alerts_state_name(od->state));
    draw_text(img, 6, 4, s1, state_col, line1);

    if (alarm_visual && blink) {
        const char *alert = "ALERT!";
        int aw = (int)strlen(alert) * 6 * s1;
        draw_text(img, img->width - aw - 6, 4, s1, COL_RED, alert);
    }

    /* Clamp metric values so the formatted strings stay short. */
    char occ[24], ang[24], fps[24] = "";
    if (od->occupancy_pct >= 0.0f) {
        float v = od->occupancy_pct > 999.0f ? 999.0f : od->occupancy_pct;
        snprintf(occ, sizeof occ, "%.1f%%", (double)v);
    } else {
        snprintf(occ, sizeof occ, "--");
    }
    if (od->angle_valid) {
        float v = od->angle_deg;
        if (!(v >= -999.0f)) /* also catches NaN */
            v = -999.0f;
        if (v > 999.0f)
            v = 999.0f;
        snprintf(ang, sizeof ang, "%.1f DEG", (double)v);
    } else {
        snprintf(ang, sizeof ang, "--");
    }
    if (od->fps > 0.0f) {
        float v = od->fps > 999.0f ? 999.0f : od->fps;
        snprintf(fps, sizeof fps, " FPS:%.0f", (double)v);
    }

    char line2[160];
    snprintf(line2, sizeof line2, "MOT:%s LEAN:%s OCC:%s ANG:%s P:%d%s%s",
             od->motion_active ? "ON" : "--",
             od->lean_active ? "ON" : "--",
             occ, ang, od->person_count,
             od->person_in_zone ? " IN-ZONE" : "", fps);
    draw_text(img, 6, 4 + 8 * s1 + 2, s2, COL_WHITE, line2);
}

static inline void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static inline void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

int overlay_write_bmp(const Frame *frame, const char *path)
{
    if (!frame || !path || !image_ok(frame->image))
        return -1;
    const IplImage *img = frame->image;
    const int nc = img->nChannels;

    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;

    const uint32_t w = (uint32_t)img->width;
    const uint32_t h = (uint32_t)img->height;
    const uint32_t row_size = (w * 3u + 3u) & ~3u; /* rows pad to 4 bytes */
    const uint32_t data_size = row_size * h;

    uint8_t hdr[54] = {0};
    hdr[0] = 'B';
    hdr[1] = 'M';
    put_le32(hdr + 2, 54u + data_size); /* file size */
    put_le32(hdr + 10, 54u);            /* pixel data offset */
    put_le32(hdr + 14, 40u);            /* BITMAPINFOHEADER size */
    put_le32(hdr + 18, w);
    put_le32(hdr + 22, h);              /* positive height = bottom-up rows */
    put_le16(hdr + 26, 1);              /* planes */
    put_le16(hdr + 28, 24);             /* bpp */
    put_le32(hdr + 34, data_size);
    put_le32(hdr + 38, 2835u);          /* 72 dpi */
    put_le32(hdr + 42, 2835u);

    uint8_t *row = calloc(1, row_size);
    if (!row) {
        fclose(f);
        return -1;
    }

    bool ok = fwrite(hdr, 1, sizeof hdr, f) == sizeof hdr;
    for (int y = (int)h - 1; ok && y >= 0; y--) {
        const uint8_t *src =
            (const uint8_t *)img->imageData + (size_t)y * (size_t)img->widthStep;
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t *p = src + (size_t)x * (size_t)nc;
            uint8_t *d = row + (size_t)x * 3u;
            if (nc == 1) {
                d[0] = d[1] = d[2] = p[0];
            } else {
                d[0] = p[0]; /* already BGR */
                d[1] = p[1];
                d[2] = p[2];
            }
        }
        ok = ok && fwrite(row, 1, row_size, f) == (size_t)row_size;
    }

    free(row);
    if (fclose(f) != 0)
        ok = false;
    return ok ? 0 : -1;
}
