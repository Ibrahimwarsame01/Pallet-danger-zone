/*
 * gpio_alert.c — Role D GPIO alert outputs.
 *
 * QNX rpi_gpio message interface (primary backend), per the QNX RPi image
 * documentation / "Embedded fun with QNX" examples:
 *
 *     #include <sys/rpi_gpio.h>
 *     int fd = open("/dev/gpio/msg", O_RDWR);
 *     rpi_gpio_msg_t msg = {
 *         .hdr.type    = _IO_MSG,
 *         .hdr.subtype = RPI_GPIO_SET_SELECT,   // or RPI_GPIO_WRITE
 *         .hdr.mgrid   = RPI_GPIO_IOMGR,
 *         .gpio        = 16,
 *         .value       = RPI_GPIO_FUNC_OUT,     // or 0/1 for WRITE
 *     };
 *     MsgSend(fd, &msg, sizeof(msg), NULL, 0);
 *
 * The devfs-text fallback writes token strings to /dev/gpio/<pin>; token
 * sets vary between image versions, so level writes try "1"/"0" first and
 * fall back to "on"/"off" (remembered after first success). Verify on the
 * target with scripts/gpio_test.c BEFORE the hour 8–10 integration slot.
 */
#include "gpio_alert.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "alerts_internal.h"

#if defined(__QNXNTO__) && defined(__has_include)
#  if __has_include(<sys/rpi_gpio.h>)
#    include <sys/neutrino.h>
#    include <sys/rpi_gpio.h>
#    define GPIO_HAVE_QNX_MSG 1
#  endif
#endif
#ifndef GPIO_HAVE_QNX_MSG
#  define GPIO_HAVE_QNX_MSG 0
#endif

typedef struct {
    int         pin;
    const char *label;
    int         text_fd;     /* devfs-text backend fd, -1 otherwise */
    bool        level;       /* last logical level written */
    bool        level_known;
} GpioPin;

struct GpioAlert {
    GpioAlertConfig cfg;
    GpioBackend     backend;      /* resolved backend (never AUTO) */
    int             msg_fd;       /* qnx-msg backend fd */
    GpioPin         led, buz;
    bool            have_state;
    SystemState     shown_state;  /* state currently driving outputs */
    uint64_t        phase_start_ms;
    uint64_t        hold_until_ms;
    bool            alarm_on;
    int             text_style;   /* devfs level tokens: 0 = "1"/"0", 1 = "on"/"off" */
    GpioWriteHook   hook;
    void           *hook_user;
};

/* ---------- qnx-msg backend ---------- */

#if GPIO_HAVE_QNX_MSG
static bool qnx_msg_cmd(int fd, unsigned subtype, unsigned gpio, unsigned value)
{
    rpi_gpio_msg_t msg = {
        .hdr.type    = _IO_MSG,
        .hdr.subtype = subtype,
        .hdr.mgrid   = RPI_GPIO_IOMGR,
        .gpio        = gpio,
        .value       = value,
    };
    return MsgSend(fd, &msg, sizeof msg, NULL, 0) != -1;
}
#endif

static bool try_open_qnx_msg(GpioAlert *ga)
{
#if GPIO_HAVE_QNX_MSG
    int fd = open("/dev/gpio/msg", O_RDWR);
    if (fd < 0)
        return false;
    bool ok = qnx_msg_cmd(fd, RPI_GPIO_SET_SELECT, (unsigned)ga->led.pin,
                          RPI_GPIO_FUNC_OUT);
    if (ok && ga->buz.pin >= 0)
        ok = qnx_msg_cmd(fd, RPI_GPIO_SET_SELECT, (unsigned)ga->buz.pin,
                         RPI_GPIO_FUNC_OUT);
    if (!ok) {
        close(fd);
        return false;
    }
    ga->msg_fd = fd;
    ga->backend = GPIO_BACKEND_QNX_MSG;
    return true;
#else
    (void)ga;
    return false;
#endif
}

/* ---------- devfs-text backend ---------- */

static int devfs_open_pin(int pin)
{
    char path[32];
    snprintf(path, sizeof path, "/dev/gpio/%d", pin);
    return open(path, O_RDWR);
}

static bool devfs_write_str(int fd, const char *s)
{
    size_t n = strlen(s);
    return write(fd, s, n) == (ssize_t)n;
}

/* Token set differs between image versions; discover once, remember. */
static bool devfs_write_level(GpioAlert *ga, int fd, bool level)
{
    static const char *styles[2][2] = { { "0", "1" }, { "off", "on" } };
    if (devfs_write_str(fd, styles[ga->text_style][level ? 1 : 0]))
        return true;
    int alt = 1 - ga->text_style;
    if (devfs_write_str(fd, styles[alt][level ? 1 : 0])) {
        ga->text_style = alt;
        return true;
    }
    return false;
}

static bool try_open_devfs(GpioAlert *ga)
{
    int lfd = devfs_open_pin(ga->led.pin);
    if (lfd < 0)
        return false;
    int bfd = -1;
    bool ok = devfs_write_str(lfd, "out");
    if (ok && ga->buz.pin >= 0) {
        bfd = devfs_open_pin(ga->buz.pin);
        ok = bfd >= 0 && devfs_write_str(bfd, "out");
    }
    if (!ok) {
        close(lfd);
        if (bfd >= 0)
            close(bfd);
        return false;
    }
    ga->led.text_fd = lfd;
    ga->buz.text_fd = bfd;
    ga->backend = GPIO_BACKEND_DEVFS_TEXT;
    return true;
}

/* ---------- core ---------- */

static void pin_apply(GpioAlert *ga, GpioPin *p, bool level)
{
    if (p->pin < 0)
        return;
    if (p->level_known && p->level == level)
        return;
    p->level = level;
    p->level_known = true;

    if (ga->hook) { /* tests capture logical levels instead of hardware */
        ga->hook(p->pin, level, ga->hook_user);
        return;
    }

    bool electrical = ga->cfg.active_low ? !level : level;
    switch (ga->backend) {
#if GPIO_HAVE_QNX_MSG
    case GPIO_BACKEND_QNX_MSG:
        if (!qnx_msg_cmd(ga->msg_fd, RPI_GPIO_WRITE, (unsigned)p->pin,
                         electrical ? 1u : 0u))
            fprintf(stderr, "gpio_alert: RPI_GPIO_WRITE GPIO%d failed: %s\n",
                    p->pin, strerror(errno));
        break;
#endif
    case GPIO_BACKEND_DEVFS_TEXT:
        if (!devfs_write_level(ga, p->text_fd, electrical))
            fprintf(stderr, "gpio_alert: write GPIO%d failed: %s\n",
                    p->pin, strerror(errno));
        break;
    case GPIO_BACKEND_CONSOLE:
    default:
        fprintf(stderr, "[gpio-sim] %-6s GPIO%-2d -> %s\n",
                p->label, p->pin, level ? "ON" : "off");
        break;
    }
}

static bool blink_on(uint64_t now, uint64_t start, uint32_t period_ms)
{
    if (period_ms == 0)
        return true; /* solid */
    uint64_t half = period_ms / 2;
    if (half == 0)
        half = 1;
    return ((now - start) / half) % 2 == 0;
}

/* ---------- public API ---------- */

void gpio_alert_config_defaults(GpioAlertConfig *cfg)
{
    if (!cfg)
        return;
    cfg->led_pin = 17;
    cfg->buzzer_pin = 27;
    cfg->active_low = false;
    cfg->alarm_blink_period_ms = 250;
    cfg->hazard_blink_period_ms = 1000;
    cfg->alarm_hold_ms = 1000;
    cfg->backend = GPIO_BACKEND_AUTO;
}

GpioAlert *gpio_alert_open(const GpioAlertConfig *cfg_in)
{
    GpioAlert *ga = calloc(1, sizeof *ga);
    if (!ga)
        return NULL;
    gpio_alert_config_defaults(&ga->cfg);
    if (cfg_in)
        ga->cfg = *cfg_in;

    ga->msg_fd = -1;
    ga->led = (GpioPin){ .pin = ga->cfg.led_pin, .label = "LED", .text_fd = -1 };
    ga->buz = (GpioPin){ .pin = ga->cfg.buzzer_pin, .label = "BUZZER", .text_fd = -1 };
    ga->backend = GPIO_BACKEND_CONSOLE;

    bool opened;
    switch (ga->cfg.backend) {
    case GPIO_BACKEND_QNX_MSG:
        opened = try_open_qnx_msg(ga);
        break;
    case GPIO_BACKEND_DEVFS_TEXT:
        opened = try_open_devfs(ga);
        break;
    case GPIO_BACKEND_CONSOLE:
        opened = true;
        break;
    case GPIO_BACKEND_AUTO:
    default:
        opened = try_open_qnx_msg(ga) || try_open_devfs(ga);
        break;
    }
    if (!opened) {
        /* The demo must not die because a wire is loose — simulate loudly. */
        fprintf(stderr,
                "gpio_alert: hardware backend unavailable, using console sim\n");
        ga->backend = GPIO_BACKEND_CONSOLE;
    }

    char bz[24];
    if (ga->buz.pin >= 0)
        snprintf(bz, sizeof bz, "GPIO%d", ga->buz.pin);
    else
        snprintf(bz, sizeof bz, "disabled");
    fprintf(stderr, "gpio_alert: backend=%s LED=GPIO%d BUZZER=%s\n",
            gpio_alert_backend_name(ga), ga->led.pin, bz);

    /* Known safe start: everything off. */
    pin_apply(ga, &ga->led, false);
    pin_apply(ga, &ga->buz, false);
    return ga;
}

void gpio_alert_update(GpioAlert *ga, SystemState state, uint64_t now_ms)
{
    if (!ga)
        return;
    if (state == STATE_ALARMING)
        ga->hold_until_ms = now_ms + ga->cfg.alarm_hold_ms;

    SystemState shown = state;
    if (state != STATE_ALARMING && now_ms < ga->hold_until_ms)
        shown = STATE_ALARMING; /* latch through the hold window */

    if (!ga->have_state || shown != ga->shown_state) {
        ga->shown_state = shown;
        ga->have_state = true;
        ga->phase_start_ms = now_ms; /* blink starts in the ON phase */
    }

    bool led = false, buz = false;
    switch (ga->shown_state) {
    case STATE_ALARMING:
        led = blink_on(now_ms, ga->phase_start_ms, ga->cfg.alarm_blink_period_ms);
        buz = true;
        break;
    case STATE_HAZARD_ACTIVE:
        led = blink_on(now_ms, ga->phase_start_ms, ga->cfg.hazard_blink_period_ms);
        break;
    case STATE_NORMAL:
    default:
        break;
    }
    ga->alarm_on = ga->shown_state == STATE_ALARMING;
    pin_apply(ga, &ga->led, led);
    pin_apply(ga, &ga->buz, buz);
}

bool gpio_alert_is_alarm_on(const GpioAlert *ga)
{
    return ga && ga->alarm_on;
}

const char *gpio_alert_backend_name(const GpioAlert *ga)
{
    if (!ga)
        return "none";
    switch (ga->backend) {
    case GPIO_BACKEND_QNX_MSG:    return "qnx-msg";
    case GPIO_BACKEND_DEVFS_TEXT: return "devfs-text";
    case GPIO_BACKEND_CONSOLE:    return "console";
    default:                      return "auto";
    }
}

bool gpio_alert_raw_write(GpioAlert *ga, int pin, bool level)
{
    if (!ga || pin < 0)
        return false;
    if (ga->hook) {
        ga->hook(pin, level, ga->hook_user);
        return true;
    }
    bool electrical = ga->cfg.active_low ? !level : level;
    switch (ga->backend) {
#if GPIO_HAVE_QNX_MSG
    case GPIO_BACKEND_QNX_MSG:
        return qnx_msg_cmd(ga->msg_fd, RPI_GPIO_SET_SELECT, (unsigned)pin,
                           RPI_GPIO_FUNC_OUT) &&
               qnx_msg_cmd(ga->msg_fd, RPI_GPIO_WRITE, (unsigned)pin,
                           electrical ? 1u : 0u);
#endif
    case GPIO_BACKEND_DEVFS_TEXT: {
        if (pin == ga->led.pin && ga->led.text_fd >= 0)
            return devfs_write_level(ga, ga->led.text_fd, electrical);
        if (pin == ga->buz.pin && ga->buz.text_fd >= 0)
            return devfs_write_level(ga, ga->buz.text_fd, electrical);
        int fd = devfs_open_pin(pin);
        if (fd < 0)
            return false;
        bool ok = devfs_write_str(fd, "out") &&
                  devfs_write_level(ga, fd, electrical);
        close(fd);
        return ok;
    }
    case GPIO_BACKEND_CONSOLE:
    default:
        fprintf(stderr, "[gpio-sim] RAW    GPIO%-2d -> %s\n",
                pin, level ? "ON" : "off");
        return true;
    }
}

void gpio_alert_all_off(GpioAlert *ga)
{
    if (!ga)
        return;
    ga->hold_until_ms = 0;
    ga->alarm_on = false;
    ga->have_state = false;
    pin_apply(ga, &ga->led, false);
    pin_apply(ga, &ga->buz, false);
}

void gpio_alert_close(GpioAlert *ga)
{
    if (!ga)
        return;
    gpio_alert_all_off(ga);
    if (ga->msg_fd >= 0)
        close(ga->msg_fd);
    if (ga->led.text_fd >= 0)
        close(ga->led.text_fd);
    if (ga->buz.text_fd >= 0)
        close(ga->buz.text_fd);
    free(ga);
}

void gpio_alert_set_write_hook(GpioAlert *ga, GpioWriteHook hook, void *user)
{
    if (!ga)
        return;
    ga->hook = hook;
    ga->hook_user = user;
}
