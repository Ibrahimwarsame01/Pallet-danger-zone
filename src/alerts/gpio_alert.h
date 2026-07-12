/*
 * gpio_alert.h — Role D GPIO alert outputs (PRD: hours 8–10).
 *
 * Drives an LED + buzzer from the Role C SystemState:
 *   STATE_NORMAL         LED off,        buzzer off
 *   STATE_HAZARD_ACTIVE  LED slow blink, buzzer off
 *   STATE_ALARMING       LED fast blink, buzzer on
 * After ALARMING ends, outputs latch for alarm_hold_ms so short alarms are
 * still clearly visible/audible.
 *
 * Backends (AUTO tries in order, demo never dies from missing hardware):
 *   1. qnx-msg    — /dev/gpio/msg + MsgSend (QNX rpi_gpio resource manager)
 *   2. devfs-text — textual writes to /dev/gpio/<pin>
 *   3. console    — prints transitions to stderr (host dev / wiring failure)
 *
 * Integration sketch (main.c):
 *     GpioAlert *ga = gpio_alert_open(NULL);          // defaults
 *     while (running) {
 *         ...
 *         gpio_alert_update(ga, state, frame->timestamp_ms);
 *     }
 *     gpio_alert_close(ga);                           // turns outputs off
 *
 * Latency: gpio_alert_update() writes the pins in the same call — alert
 * latency is bounded by the frame period, well inside the PRD's 1.0 s.
 */
#ifndef GPIO_ALERT_H
#define GPIO_ALERT_H

#include <stdbool.h>
#include <stdint.h>

#include "common_types.h"

typedef enum {
    GPIO_BACKEND_AUTO = 0,
    GPIO_BACKEND_QNX_MSG,
    GPIO_BACKEND_DEVFS_TEXT,
    GPIO_BACKEND_CONSOLE
} GpioBackend;

typedef struct {
    int      led_pin;                /* BCM number; default 17 (header pin 11) */
    int      buzzer_pin;             /* BCM number; default 27 (header pin 13); < 0 disables */
    bool     active_low;             /* invert electrical level (sinking wiring) */
    uint32_t alarm_blink_period_ms;  /* LED blink while ALARMING; default 250 */
    uint32_t hazard_blink_period_ms; /* LED blink while HAZARD_ACTIVE; default 1000 */
    uint32_t alarm_hold_ms;          /* latch alarm outputs after ALARMING; default 1000 */
    GpioBackend backend;             /* default GPIO_BACKEND_AUTO */
} GpioAlertConfig;

void gpio_alert_config_defaults(GpioAlertConfig *cfg);

typedef struct GpioAlert GpioAlert;

/* Open + configure outputs (falls back to console sim rather than failing).
 * Returns NULL only on out-of-memory. Pass NULL for defaults. */
GpioAlert *gpio_alert_open(const GpioAlertConfig *cfg);

/* Call once per processed frame (or timer tick). now_ms must be monotonic —
 * use Frame.timestamp_ms (or alerts_now_ms()). */
void gpio_alert_update(GpioAlert *ga, SystemState state, uint64_t now_ms);

/* True while alarm outputs are active, including the hold window.
 * Feed this to OverlayData.alert_on / LogRecord.alert_on. */
bool gpio_alert_is_alarm_on(const GpioAlert *ga);

const char *gpio_alert_backend_name(const GpioAlert *ga);

/* Direct write of any pin for wiring bring-up (scripts/gpio_test.c).
 * `level` is logical; active_low inversion is applied internally. */
bool gpio_alert_raw_write(GpioAlert *ga, int pin, bool level);

/* Force everything off (shutdown / SIGINT paths). */
void gpio_alert_all_off(GpioAlert *ga);

void gpio_alert_close(GpioAlert *ga);

/* Test hook: when set, logical pin writes go to the hook instead of hardware. */
typedef void (*GpioWriteHook)(int pin, bool level, void *user);
void gpio_alert_set_write_hook(GpioAlert *ga, GpioWriteHook hook, void *user);

#endif /* GPIO_ALERT_H */
