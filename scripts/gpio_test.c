/*
 * gpio_test — Role D GPIO wiring bring-up. PRD: run this standalone BEFORE
 * the hour 8–10 alert-integration slot, not during it.
 *
 * Wiring (BCM numbering, physical header pin in parens):
 *   LED    : GPIO17 (pin 11) -> 330R resistor -> LED anode; LED cathode -> GND (pin 9)
 *   BUZZER : GPIO27 (pin 13) -> active buzzer (+);          buzzer (-)  -> GND (pin 14)
 *
 * On QNX target : ./gpio_test                    (auto: msg -> devfs -> console)
 * On host       : ./gpio_test --backend console  (pure simulation)
 * Single pin    : ./gpio_test --raw 17           (5 slow blinks, scope-friendly)
 *
 * PASS = LED fast-blinks and buzzer sounds during each ALARMING phase, both
 * latch ~1s into NORMAL, then go quiet. Ctrl-C turns everything off.
 *
 * Build: make gpio_test   (source the QNX SDP env first for a target build)
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gpio_alert.h"
#include "alerts_internal.h"

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void msleep(unsigned ms)
{
    struct timespec ts = { (time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L };
    nanosleep(&ts, NULL);
}

static GpioBackend parse_backend(const char *s)
{
    if (strcmp(s, "msg") == 0)
        return GPIO_BACKEND_QNX_MSG;
    if (strcmp(s, "text") == 0)
        return GPIO_BACKEND_DEVFS_TEXT;
    if (strcmp(s, "console") == 0)
        return GPIO_BACKEND_CONSOLE;
    return GPIO_BACKEND_AUTO;
}

int main(int argc, char **argv)
{
    GpioAlertConfig cfg;
    gpio_alert_config_defaults(&cfg);
    int cycles = 3;
    int raw_pin = -1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--led") == 0 && i + 1 < argc) {
            cfg.led_pin = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--buzzer") == 0 && i + 1 < argc) {
            cfg.buzzer_pin = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--backend") == 0 && i + 1 < argc) {
            cfg.backend = parse_backend(argv[++i]);
        } else if (strcmp(argv[i], "--cycles") == 0 && i + 1 < argc) {
            cycles = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--active-low") == 0) {
            cfg.active_low = true;
        } else if (strcmp(argv[i], "--raw") == 0 && i + 1 < argc) {
            raw_pin = atoi(argv[++i]);
        } else {
            fprintf(stderr,
                    "usage: %s [--led N] [--buzzer N|-1] "
                    "[--backend auto|msg|text|console]\n"
                    "          [--cycles N] [--active-low] [--raw PIN]\n",
                    argv[0]);
            return 2;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    GpioAlert *ga = gpio_alert_open(&cfg);
    if (!ga) {
        fprintf(stderr, "gpio_test: out of memory\n");
        return 1;
    }

    if (raw_pin >= 0) {
        printf("gpio_test: raw blink GPIO%d, 5 x 500ms on / 500ms off\n", raw_pin);
        for (int i = 0; i < 5 && !g_stop; i++) {
            if (!gpio_alert_raw_write(ga, raw_pin, true))
                fprintf(stderr, "gpio_test: raw write GPIO%d failed\n", raw_pin);
            msleep(500);
            gpio_alert_raw_write(ga, raw_pin, false);
            msleep(500);
        }
    } else {
        printf("gpio_test: %d cycle(s): 2s ALARMING (LED fast blink + buzzer), "
               "then NORMAL (outputs latch ~1s, then quiet)\n", cycles);
        for (int c = 0; c < cycles && !g_stop; c++) {
            printf("gpio_test: cycle %d/%d ALARMING\n", c + 1, cycles);
            uint64_t t0 = alerts_now_ms();
            while (!g_stop && alerts_now_ms() - t0 < 2000) {
                gpio_alert_update(ga, STATE_ALARMING, alerts_now_ms());
                msleep(50);
            }
            printf("gpio_test: cycle %d/%d NORMAL\n", c + 1, cycles);
            t0 = alerts_now_ms();
            while (!g_stop && alerts_now_ms() - t0 < 1500) {
                gpio_alert_update(ga, STATE_NORMAL, alerts_now_ms());
                msleep(50);
            }
        }
    }

    gpio_alert_all_off(ga);
    printf("gpio_test: done (backend=%s).\n"
           "PASS if the LED fast-blinked and the buzzer sounded during ALARMING,\n"
           "latched ~1s into NORMAL, and everything is quiet now.\n",
           gpio_alert_backend_name(ga));
    gpio_alert_close(ga);
    return g_stop ? 130 : 0;
}
