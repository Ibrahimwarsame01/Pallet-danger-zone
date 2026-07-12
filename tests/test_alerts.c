/*
 * test_alerts.c — Role D unit tests (host build, ASan/UBSan).
 *
 *   make test
 *
 * Covers: gpio_alert state/blink/hold semantics via the write hook,
 * logger CSV shape (column counts, decimation, forced transition rows,
 * escaping), overlay clipping safety on hostile coordinates, and the BMP
 * writer. Also renders demo HUD images into out/ for visual inspection:
 * out/overlay_normal.bmp, out/overlay_hazard.bmp, out/overlay_alarm.bmp.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "common_types.h"
#include "gpio_alert.h"
#include "logger.h"
#include "overlay.h"

static int g_fail = 0;
#define CHECK(cond) check_((cond) ? 1 : 0, #cond, __LINE__)
static void check_(int ok, const char *expr, int line)
{
    if (!ok) {
        g_fail++;
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, line, expr);
    }
}

/* ---------- image helpers ---------- */

static IplImage *make_image(int w, int h, int ch)
{
    IplImage *img = calloc(1, sizeof *img);
    if (!img)
        exit(1);
    img->nChannels = ch;
    img->depth = IPL_DEPTH_8U;
    img->width = w;
    img->height = h;
    img->widthStep = (w * ch + 3) & ~3;
    img->imageData = calloc(1, (size_t)img->widthStep * (size_t)h);
    if (!img->imageData)
        exit(1);
    return img;
}

static void free_image(IplImage *img)
{
    free(img->imageData);
    free(img);
}

static size_t buf_size(const IplImage *img)
{
    return (size_t)img->widthStep * (size_t)img->height;
}

/* ---------- gpio_alert ---------- */

typedef struct { int pin; bool level; } WriteEv;
static WriteEv g_writes[256];
static int g_nwrites = 0;

static void rec_hook(int pin, bool level, void *user)
{
    (void)user;
    if (g_nwrites < 256)
        g_writes[g_nwrites++] = (WriteEv){ pin, level };
}

static bool last_level(int pin, bool *out)
{
    for (int i = g_nwrites - 1; i >= 0; i--) {
        if (g_writes[i].pin == pin) {
            *out = g_writes[i].level;
            return true;
        }
    }
    return false;
}

static void test_gpio(void)
{
    GpioAlertConfig cfg;
    gpio_alert_config_defaults(&cfg);
    cfg.backend = GPIO_BACKEND_CONSOLE;
    cfg.alarm_blink_period_ms = 250; /* half period = 125 ms */
    cfg.alarm_hold_ms = 1000;
    GpioAlert *ga = gpio_alert_open(&cfg);
    CHECK(ga != NULL);
    if (!ga)
        return;
    gpio_alert_set_write_hook(ga, rec_hook, NULL);
    g_nwrites = 0;

    bool lvl = false;

    gpio_alert_update(ga, STATE_NORMAL, 1000);
    CHECK(!gpio_alert_is_alarm_on(ga));

    /* Alarm asserts within the same update call — latency well under 1s. */
    gpio_alert_update(ga, STATE_ALARMING, 2000);
    CHECK(gpio_alert_is_alarm_on(ga));
    CHECK(last_level(17, &lvl) && lvl);  /* LED on (blink starts ON) */
    CHECK(last_level(27, &lvl) && lvl);  /* buzzer on */

    gpio_alert_update(ga, STATE_ALARMING, 2100); /* 100/125 -> still ON */
    CHECK(last_level(17, &lvl) && lvl);

    gpio_alert_update(ga, STATE_ALARMING, 2130); /* 130/125 -> OFF phase */
    CHECK(last_level(17, &lvl) && !lvl);
    CHECK(last_level(27, &lvl) && lvl);          /* buzzer solid */

    /* Hold window: last ALARMING at t=2130 -> latched until 3130. */
    gpio_alert_update(ga, STATE_NORMAL, 2500);
    CHECK(gpio_alert_is_alarm_on(ga));
    CHECK(last_level(27, &lvl) && lvl);

    gpio_alert_update(ga, STATE_NORMAL, 3200); /* hold expired */
    CHECK(!gpio_alert_is_alarm_on(ga));
    CHECK(last_level(17, &lvl) && !lvl);
    CHECK(last_level(27, &lvl) && !lvl);

    /* Hazard state blinks LED but keeps buzzer off. */
    gpio_alert_update(ga, STATE_HAZARD_ACTIVE, 4000);
    CHECK(!gpio_alert_is_alarm_on(ga));
    CHECK(last_level(17, &lvl) && lvl);
    CHECK(last_level(27, &lvl) && !lvl);

    gpio_alert_all_off(ga);
    CHECK(last_level(17, &lvl) && !lvl);

    gpio_alert_close(ga);
}

/* ---------- logger ---------- */

static void test_logger(void)
{
    (void)mkdir("out", 0775);
    const char *path = "out/test_log.csv";

    LoggerConfig cfg;
    logger_config_defaults(&cfg);
    cfg.path = path;
    cfg.log_every_n = 3;
    Logger *lg = logger_open(&cfg);
    CHECK(lg != NULL);
    if (!lg)
        return;
    CHECK(strcmp(logger_path(lg), path) == 0);

    LogRecord r = {0};
    r.occupancy_pct = -1.0f;
    r.state = STATE_NORMAL;

    r.frame_id = 1; r.uptime_ms = 100;
    logger_log_frame(lg, &r);                      /* first -> logged */
    r.frame_id = 2; r.uptime_ms = 200;
    logger_log_frame(lg, &r);                      /* decimated away */
    r.frame_id = 3; r.uptime_ms = 300;
    r.state = STATE_ALARMING; r.alert_on = true;
    r.occupancy_pct = 12.34f; r.angle_deg = 14.2f; r.angle_valid = true;
    logger_log_frame(lg, &r);                      /* change -> forced row */
    r.frame_id = 4; r.uptime_ms = 400;
    logger_log_frame(lg, &r);                      /* 4th call is due (n=3) */
    r.frame_id = 5; r.uptime_ms = 500;
    r.state = STATE_NORMAL; r.alert_on = false;
    logger_log_frame(lg, &r);                      /* change -> forced row */
    logger_log_event(lg, "demo note, with %s", "comma");
    logger_close(lg);

    FILE *f = fopen(path, "r");
    CHECK(f != NULL);
    if (!f)
        return;
    char lines[8][512];
    int nlines = 0, bad_fields = 0;
    char line[512];
    while (fgets(line, sizeof line, f) && nlines < 8) {
        int commas = 0;
        bool q = false;
        for (char *p = line; *p; p++) {
            if (*p == '"')
                q = !q;
            else if (*p == ',' && !q)
                commas++;
        }
        if (commas != 12) /* 13 columns everywhere */
            bad_fields++;
        snprintf(lines[nlines], sizeof lines[0], "%s", line);
        nlines++;
    }
    fclose(f);

    CHECK(nlines == 6); /* header + 4 frame rows + 1 event row */
    CHECK(bad_fields == 0);
    CHECK(strstr(lines[0], "wall_time,frame_id,uptime_ms,state") != NULL);
    CHECK(strstr(lines[1], ",1,100,NORMAL,") != NULL);
    CHECK(strstr(lines[2], ",3,300,ALARMING,") != NULL);
    CHECK(strstr(lines[2], "STATE NORMAL->ALARMING ALERT-ON") != NULL);
    CHECK(strstr(lines[2], ",12.3,14.2,1,") != NULL);
    CHECK(strstr(lines[3], ",4,400,ALARMING,") != NULL);
    CHECK(strstr(lines[4], "STATE ALARMING->NORMAL ALERT-OFF") != NULL);
    CHECK(strstr(lines[5], "\"demo note, with comma\"") != NULL);
}

/* ---------- overlay ---------- */

static void test_overlay_safety(void)
{
    static const int chans[] = { 1, 3, 4 };
    for (size_t i = 0; i < sizeof chans / sizeof chans[0]; i++) {
        IplImage *img = make_image(320, 240, chans[i]);
        Frame fr = { .image = img, .timestamp_ms = 12345, .frame_id = 7 };

        Point zone_pts[4] = { {-50, -50}, {400, 30}, {310, 250}, {10, 500} };
        Polygon zone = { zone_pts, 4 };
        Point persons[3] = { {-10, -10}, {1000, 1000}, {160, 120} };
        OverlayData od = {
            .zone = &zone,
            .persons = persons, .person_count = 3,
            .state = STATE_ALARMING,
            .motion_active = true, .lean_active = true,
            .occupancy_pct = 12.3f,
            .angle_deg = 14.2f, .angle_valid = true,
            .person_in_zone = true, .alert_on = true,
            .fps = 21.0f,
        };

        uint8_t *before = malloc(buf_size(img));
        if (!before)
            exit(1);
        memcpy(before, img->imageData, buf_size(img));
        overlay_render(&fr, &od);
        CHECK(memcmp(before, img->imageData, buf_size(img)) != 0);
        fr.timestamp_ms += 300; /* exercise the other blink phase */
        overlay_render(&fr, &od);
        free(before);
        free_image(img);
    }

    /* Hostile input must be a no-op, never a crash. */
    overlay_render(NULL, NULL);
    Frame empty = {0};
    OverlayData od0 = {0};
    overlay_render(&empty, &od0);
    CHECK(overlay_write_bmp(&empty, "out/never.bmp") == -1);
    CHECK(overlay_write_bmp(NULL, "out/never.bmp") == -1);

    /* Degenerate polygon/persons */
    IplImage *img = make_image(64, 48, 3);
    Frame fr = { .image = img, .timestamp_ms = 0, .frame_id = 1 };
    Polygon zone = { NULL, 0 };
    OverlayData od = { .zone = &zone, .persons = NULL, .person_count = 5,
                       .state = STATE_NORMAL, .occupancy_pct = -1.0f };
    overlay_render(&fr, &od);
    free_image(img);
}

/* ---------- demo images ---------- */

static void paint_block(IplImage *img, int x, int y, int w, int h,
                        uint8_t b, uint8_t g, uint8_t r)
{
    for (int yy = y; yy < y + h; yy++) {
        if (yy < 0 || yy >= img->height)
            continue;
        uint8_t *row = (uint8_t *)img->imageData + (size_t)yy * (size_t)img->widthStep;
        for (int xx = x; xx < x + w; xx++) {
            if (xx < 0 || xx >= img->width)
                continue;
            uint8_t *p = row + (size_t)xx * (size_t)img->nChannels;
            if (img->nChannels == 1) {
                p[0] = g;
            } else {
                p[0] = b; p[1] = g; p[2] = r;
            }
        }
    }
}

static void paint_bg(IplImage *img)
{
    for (int y = 0; y < img->height; y++) {
        uint8_t *row = (uint8_t *)img->imageData + (size_t)y * (size_t)img->widthStep;
        uint8_t v = (uint8_t)(90 + y * 60 / img->height);
        for (int x = 0; x < img->width; x++) {
            uint8_t *p = row + (size_t)x * (size_t)img->nChannels;
            uint8_t px = (uint8_t)(v + (((x / 40) % 2) ? 6 : 0));
            if (img->nChannels == 1) {
                p[0] = px;
            } else {
                p[0] = px;
                p[1] = (uint8_t)(px + 4);
                p[2] = (uint8_t)(px + 8);
            }
        }
    }
}

static void render_demo(const char *path, SystemState st, bool person, bool alert)
{
    IplImage *img = make_image(640, 480, 3);
    paint_bg(img);
    paint_block(img, 360, 250, 150, 130, 40, 90, 150); /* pallet stand-in */

    Frame fr = { .image = img, .timestamp_ms = 0, .frame_id = 42 }; /* blink ON phase */
    Point zone_pts[4] = { {300, 200}, {560, 210}, {600, 470}, {260, 460} };
    Polygon zone = { zone_pts, 4 };
    Point persons[1] = { {380, 330} };

    OverlayData od = {
        .zone = &zone,
        .persons = person ? persons : NULL,
        .person_count = person ? 1 : 0,
        .state = st,
        .motion_active = st != STATE_NORMAL,
        .lean_active = st == STATE_ALARMING,
        .occupancy_pct = st == STATE_NORMAL ? 1.2f : 23.7f,
        .angle_deg = 14.2f,
        .angle_valid = st != STATE_NORMAL,
        .person_in_zone = person,
        .alert_on = alert,
        .fps = 21.0f,
    };
    overlay_render(&fr, &od);
    CHECK(overlay_write_bmp(&fr, path) == 0);
    free_image(img);
}

static void test_demo_bmps(void)
{
    (void)mkdir("out", 0775);
    render_demo("out/overlay_normal.bmp", STATE_NORMAL, false, false);
    render_demo("out/overlay_hazard.bmp", STATE_HAZARD_ACTIVE, false, false);
    render_demo("out/overlay_alarm.bmp", STATE_ALARMING, true, true);

    /* gray pipeline too */
    IplImage *img = make_image(320, 240, 1);
    paint_bg(img);
    Frame fr = { .image = img, .timestamp_ms = 0, .frame_id = 1 };
    OverlayData od = { .state = STATE_HAZARD_ACTIVE, .occupancy_pct = 9.9f };
    overlay_render(&fr, &od);
    CHECK(overlay_write_bmp(&fr, "out/overlay_gray.bmp") == 0);
    free_image(img);

    /* BMP sanity: magic + declared size == actual size */
    FILE *f = fopen("out/overlay_alarm.bmp", "rb");
    CHECK(f != NULL);
    if (f) {
        uint8_t h[6];
        CHECK(fread(h, 1, 6, f) == 6);
        CHECK(h[0] == 'B' && h[1] == 'M');
        uint32_t sz = (uint32_t)h[2] | ((uint32_t)h[3] << 8) |
                      ((uint32_t)h[4] << 16) | ((uint32_t)h[5] << 24);
        CHECK(fseek(f, 0, SEEK_END) == 0);
        CHECK((long)sz == ftell(f));
        fclose(f);
    }
}

int main(void)
{
    test_gpio();
    test_logger();
    test_overlay_safety();
    test_demo_bmps();

    if (g_fail) {
        fprintf(stderr, "test_alerts: %d FAILURE(S)\n", g_fail);
        return 1;
    }
    printf("test_alerts: ALL TESTS PASSED\n");
    return 0;
}
