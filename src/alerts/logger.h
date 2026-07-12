/*
 * logger.h — Role D CSV logger (PRD: hours 8–10, low risk).
 *
 * One row per sampled frame plus forced rows on every state/alert change
 * (so alert latency can be measured from the log even with decimation),
 * and free-form event rows. Transitions are flushed immediately so the
 * file is usable even after a crash.
 */
#ifndef LOGGER_H
#define LOGGER_H

#include <stdbool.h>
#include <stdint.h>

#include "common_types.h"

typedef struct Logger Logger;

typedef struct {
    int         frame_id;        /* Frame.frame_id                          */
    uint64_t    uptime_ms;       /* Frame.timestamp_ms (monotonic)          */
    SystemState state;
    bool        motion_active;
    bool        lean_active;
    float       occupancy_pct;   /* pass < 0 for n/a (blank field)          */
    float       angle_deg;
    bool        angle_valid;     /* false = angle field left blank          */
    int         person_count;
    bool        person_in_zone;
    bool        alert_on;
} LogRecord;

typedef struct {
    const char *path;            /* NULL = logs/run_YYYYMMDD_HHMMSS.csv     */
    int         log_every_n;     /* decimation: log every Nth frame (>=1);
                                    state/alert changes are always logged   */
    int         flush_every_rows;/* fflush cadence for normal rows          */
} LoggerConfig;

void logger_config_defaults(LoggerConfig *cfg);

/* Opens the CSV and writes the header row. Returns NULL on I/O failure
 * (callers may keep running without logging). */
Logger *logger_open(const LoggerConfig *cfg /* NULL = defaults */);

/* Call once per processed frame. */
void logger_log_frame(Logger *lg, const LogRecord *rec);

/* Free-form event row (printf-style), e.g. "demo start", "gpio backend=%s".
 * Flushed immediately. Commas/quotes are CSV-escaped. */
void logger_log_event(Logger *lg, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

const char *logger_path(const Logger *lg);

void logger_close(Logger *lg);

#endif /* LOGGER_H */
