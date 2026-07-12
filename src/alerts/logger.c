/*
 * logger.c — Role D CSV logger.
 *
 * Design points:
 *  - state/alert transitions are ALWAYS logged (with a transition note in the
 *    event column) and flushed immediately, so alert latency can be measured
 *    from the CSV even if the process dies mid-demo;
 *  - normal rows honor log_every_n decimation and a periodic flush;
 *  - event text is CSV-escaped.
 */
#include "logger.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "alerts_internal.h"

struct Logger {
    FILE        *f;
    char         path[512];
    LoggerConfig cfg;
    uint64_t     frames_seen;
    int          rows_since_flush;
    bool         have_last;
    SystemState  last_state;
    bool         last_alert;
};

static const char CSV_HEADER[] =
    "wall_time,frame_id,uptime_ms,state,motion,lean,occupancy_pct,angle_deg,"
    "angle_valid,person_count,person_in_zone,alert_on,event\n";

void logger_config_defaults(LoggerConfig *cfg)
{
    if (!cfg)
        return;
    cfg->path = NULL;
    cfg->log_every_n = 1;
    cfg->flush_every_rows = 20;
}

static void wall_time_str(char *buf, size_t n)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_;
    localtime_r(&ts.tv_sec, &tm_);
    snprintf(buf, n, "%04d-%02d-%02dT%02d:%02d:%02d.%03d",
             tm_.tm_year + 1900, tm_.tm_mon + 1, tm_.tm_mday,
             tm_.tm_hour, tm_.tm_min, tm_.tm_sec,
             (int)(ts.tv_nsec / 1000000L));
}

/* Quote-and-double CSV escaping; newlines become spaces. */
static void csv_escape(char *dst, size_t dstsz, const char *src)
{
    if (dstsz == 0)
        return;
    if (!src)
        src = "";
    if (!strpbrk(src, ",\"\n\r")) {
        snprintf(dst, dstsz, "%s", src);
        return;
    }
    size_t o = 0;
    if (o < dstsz - 1)
        dst[o++] = '"';
    for (const char *p = src; *p && o + 2 < dstsz; p++) {
        if (*p == '"') {
            if (o + 3 >= dstsz)
                break;
            dst[o++] = '"';
            dst[o++] = '"';
        } else if (*p == '\n' || *p == '\r') {
            dst[o++] = ' ';
        } else {
            dst[o++] = *p;
        }
    }
    if (o < dstsz - 1)
        dst[o++] = '"';
    dst[o] = '\0';
}

/* rec == NULL writes an event-only row (metric fields left blank). */
static void write_row(Logger *lg, const LogRecord *rec, const char *event)
{
    char t[40];
    wall_time_str(t, sizeof t);
    char esc[256];
    csv_escape(esc, sizeof esc, event);

    if (rec) {
        char occ[24] = "", ang[24] = "";
        if (rec->occupancy_pct >= 0.0f) {
            float v = rec->occupancy_pct > 999.9f ? 999.9f : rec->occupancy_pct;
            snprintf(occ, sizeof occ, "%.1f", (double)v);
        }
        if (rec->angle_valid) {
            float v = rec->angle_deg;
            if (v > 999.9f)
                v = 999.9f;
            if (v < -999.9f)
                v = -999.9f;
            snprintf(ang, sizeof ang, "%.1f", (double)v);
        }
        fprintf(lg->f, "%s,%d,%llu,%s,%d,%d,%s,%s,%d,%d,%d,%d,%s\n",
                t, rec->frame_id, (unsigned long long)rec->uptime_ms,
                alerts_state_name(rec->state),
                rec->motion_active ? 1 : 0, rec->lean_active ? 1 : 0,
                occ, ang, rec->angle_valid ? 1 : 0,
                rec->person_count, rec->person_in_zone ? 1 : 0,
                rec->alert_on ? 1 : 0, esc);
    } else {
        /* 12 commas => 13 columns, matching the header */
        fprintf(lg->f, "%s,,,,,,,,,,,,%s\n", t, esc);
    }
}

Logger *logger_open(const LoggerConfig *cfg_in)
{
    LoggerConfig cfg;
    logger_config_defaults(&cfg);
    if (cfg_in)
        cfg = *cfg_in;
    if (cfg.log_every_n < 1)
        cfg.log_every_n = 1;
    if (cfg.flush_every_rows < 1)
        cfg.flush_every_rows = 1;

    Logger *lg = calloc(1, sizeof *lg);
    if (!lg)
        return NULL;
    lg->cfg = cfg;

    if (cfg.path && cfg.path[0]) {
        snprintf(lg->path, sizeof lg->path, "%s", cfg.path);
    } else {
        (void)mkdir("logs", 0775); /* EEXIST is fine */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        struct tm tm_;
        localtime_r(&ts.tv_sec, &tm_);
        snprintf(lg->path, sizeof lg->path, "logs/run_%04d%02d%02d_%02d%02d%02d.csv",
                 tm_.tm_year + 1900, tm_.tm_mon + 1, tm_.tm_mday,
                 tm_.tm_hour, tm_.tm_min, tm_.tm_sec);
    }
    lg->cfg.path = NULL; /* lg->path[] is the single source of truth */

    lg->f = fopen(lg->path, "w");
    if (!lg->f) {
        fprintf(stderr, "logger: cannot open %s: %s\n", lg->path, strerror(errno));
        free(lg);
        return NULL;
    }
    fputs(CSV_HEADER, lg->f);
    fflush(lg->f);
    fprintf(stderr, "logger: writing %s\n", lg->path);
    return lg;
}

void logger_log_frame(Logger *lg, const LogRecord *rec)
{
    if (!lg || !lg->f || !rec)
        return;
    lg->frames_seen++;

    const bool state_changed = lg->have_last && rec->state != lg->last_state;
    const bool alert_changed = lg->have_last && rec->alert_on != lg->last_alert;
    const bool changed = !lg->have_last || state_changed || alert_changed;
    const bool due = (lg->frames_seen - 1) % (uint64_t)lg->cfg.log_every_n == 0;
    if (!changed && !due)
        return;

    char ev[80] = "";
    if (state_changed && alert_changed) {
        snprintf(ev, sizeof ev, "STATE %s->%s %s",
                 alerts_state_name(lg->last_state), alerts_state_name(rec->state),
                 rec->alert_on ? "ALERT-ON" : "ALERT-OFF");
    } else if (state_changed) {
        snprintf(ev, sizeof ev, "STATE %s->%s",
                 alerts_state_name(lg->last_state), alerts_state_name(rec->state));
    } else if (alert_changed) {
        snprintf(ev, sizeof ev, "%s", rec->alert_on ? "ALERT-ON" : "ALERT-OFF");
    }

    write_row(lg, rec, ev);
    lg->have_last = true;
    lg->last_state = rec->state;
    lg->last_alert = rec->alert_on;

    if (changed) {
        fflush(lg->f);
        lg->rows_since_flush = 0;
    } else if (++lg->rows_since_flush >= lg->cfg.flush_every_rows) {
        fflush(lg->f);
        lg->rows_since_flush = 0;
    }
}

void logger_log_event(Logger *lg, const char *fmt, ...)
{
    if (!lg || !lg->f || !fmt)
        return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    write_row(lg, NULL, buf);
    fflush(lg->f);
}

const char *logger_path(const Logger *lg)
{
    return lg ? lg->path : "";
}

void logger_close(Logger *lg)
{
    if (!lg)
        return;
    if (lg->f) {
        fflush(lg->f);
        fclose(lg->f);
    }
    free(lg);
}
