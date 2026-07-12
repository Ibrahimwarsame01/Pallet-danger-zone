/*
 * alerts_internal.h — Role D private helpers shared by gpio_alert / overlay /
 * logger and their tests. Not part of the cross-role contract.
 */
#ifndef ALERTS_INTERNAL_H
#define ALERTS_INTERNAL_H

#include <stdint.h>
#include <time.h>

#include "common_types.h"

/* Monotonic milliseconds — same timebase Role A uses for Frame.timestamp_ms. */
static inline uint64_t alerts_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static inline const char *alerts_state_name(SystemState s)
{
    switch (s) {
    case STATE_NORMAL:        return "NORMAL";
    case STATE_HAZARD_ACTIVE: return "HAZARD_ACTIVE";
    case STATE_ALARMING:      return "ALARMING";
    default:                  return "UNKNOWN";
    }
}

#endif /* ALERTS_INTERNAL_H */
