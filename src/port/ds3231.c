/* ds3231.c — the DS3231 on i2c0 (ds3231.h; design.md §3.8).
 *
 * Registers, from Maxim's DS3231 data sheet (19-5170, rev. 10): 0x00-0x06
 * the seconds, minutes, hours, day of the week, date, month (bit 7 the
 * century) and year, in BCD; 0x0E control (bit 7 EOSC, which stops the
 * oscillator on the battery); 0x0F status (bit 7 OSF, set when the
 * oscillator stopped, cleared only by writing 0). A read of the seven
 * time registers in one transfer is consistent: the chip copies them as
 * the transfer starts. Writing the seconds resets its countdown, so a new
 * second starts then.
 *
 * The century bit counts 2000-2099 and 2100-2199; the shell writes the
 * hours in 24-hour form, but reads either. */

#include "ds3231.h"

#include <stdbool.h>
#include <stddef.h>

#include "hardware/gpio.h"
#include "hardware/i2c.h"

#include "clock.h"

#define RTC_I2C     i2c0
#define RTC_PIN_SDA 4
#define RTC_PIN_SCL 5
#define RTC_ADDR    0x68u
#define RTC_HZ      100000u

/* Eight bytes and their ACKs are under a millisecond at 100 kHz. */
#define RTC_TIMEOUT_US 20000u

#define REG_SECONDS  0x00u
#define REG_CONTROL  0x0Eu
#define REG_STATUS   0x0Fu
#define CONTROL_EOSC 0x80u
#define STATUS_OSF   0x80u

static ds3231_status_t status(int rc, size_t want) {
    if (rc == (int)want) return DS3231_OK;
    return rc == PICO_ERROR_TIMEOUT ? DS3231_TIMEOUT : DS3231_ABSENT;
}

/* With the HDMI mirror (PICO_SHELL_HDMI) GP4 and GP5 are two of its data
 * lines: there is no bus here, and nothing answers (design.md §3.11). */
static ds3231_status_t get(uint8_t reg, uint8_t *in, size_t n) {
#if PICO_SHELL_HDMI
    (void)reg; (void)in; (void)n;
    return DS3231_ABSENT;
#endif
    ds3231_status_t st =
        status(i2c_write_timeout_us(RTC_I2C, RTC_ADDR, &reg, 1, true, RTC_TIMEOUT_US), 1);
    if (st == DS3231_OK)
        st = status(i2c_read_timeout_us(RTC_I2C, RTC_ADDR, in, n, false, RTC_TIMEOUT_US), n);
    return st;
}

static ds3231_status_t put(const uint8_t *out, size_t n) {
#if PICO_SHELL_HDMI
    (void)out; (void)n;
    return DS3231_ABSENT;
#endif
    return status(i2c_write_timeout_us(RTC_I2C, RTC_ADDR, out, n, false, RTC_TIMEOUT_US), n);
}

void ds3231_init(void) {
#if PICO_SHELL_HDMI
    return;
#endif
    i2c_init(RTC_I2C, RTC_HZ);
    gpio_set_function(RTC_PIN_SDA, GPIO_FUNC_I2C);
    gpio_set_function(RTC_PIN_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(RTC_PIN_SDA);
    gpio_pull_up(RTC_PIN_SCL);
}

/* A BCD byte, or -1 if a digit is not one. */
static int from_bcd(uint8_t b) {
    if ((b & 15) > 9 || b >> 4 > 9) return -1;
    return (b >> 4) * 10 + (b & 15);
}

static uint8_t to_bcd(int n) {
    return (uint8_t)(n / 10 << 4 | n % 10);
}

ds3231_status_t ds3231_read(int64_t *utc_s) {
    uint8_t s, r[7];
    ds3231_status_t st = get(REG_STATUS, &s, 1);
    if (st != DS3231_OK) return st;
    if (s & STATUS_OSF) return DS3231_STOPPED;
    st = get(REG_SECONDS, r, sizeof r);
    if (st != DS3231_OK) return st;

    int sec = from_bcd(r[0] & 0x7F), min = from_bcd(r[1] & 0x7F), hour;
    if (r[2] & 0x40) {   /* 12-hour form: bit 5 is PM */
        hour = from_bcd(r[2] & 0x1F);
        if (hour < 1 || hour > 12) return DS3231_BAD;
        hour = hour % 12 + (r[2] & 0x20 ? 12 : 0);
    } else
        hour = from_bcd(r[2] & 0x3F);
    int mday = from_bcd(r[4] & 0x3F), mon = from_bcd(r[5] & 0x1F), yy = from_bcd(r[6]);
    if (sec < 0 || sec > 59 || min < 0 || min > 59 || hour < 0 || hour > 23 || yy < 0)
        return DS3231_BAD;
    int year = 2000 + yy + (r[5] & 0x80 ? 100 : 0);
    if (!clock_valid(year, mon, mday)) return DS3231_BAD;
    *utc_s = clock_days(year, mon, mday) * 86400 + hour * 3600 + min * 60 + sec;
    return DS3231_OK;
}

ds3231_status_t ds3231_write(int64_t utc_s) {
    clock_tm_t tm;
    clock_gmt(utc_s, &tm);
    if (tm.year < 2000 || tm.year > 2199) return DS3231_BAD;
    const uint8_t time[8] = {
        REG_SECONDS,
        to_bcd(tm.sec),
        to_bcd(tm.min),
        to_bcd(tm.hour),
        (uint8_t)(tm.wday + 1),
        to_bcd(tm.mday),
        (uint8_t)(to_bcd(tm.mon) | (tm.year >= 2100 ? 0x80 : 0)),
        to_bcd(tm.year % 100),
    };
    ds3231_status_t st = put(time, sizeof time);

    /* EOSC and OSF last, so a chip that took the time runs and says so. */
    uint8_t c, s;
    if (st == DS3231_OK) st = get(REG_CONTROL, &c, 1);
    if (st == DS3231_OK && (c & CONTROL_EOSC))
        st = put((const uint8_t[]){ REG_CONTROL, (uint8_t)(c & ~CONTROL_EOSC) }, 2);
    if (st == DS3231_OK) st = get(REG_STATUS, &s, 1);
    if (st == DS3231_OK && (s & STATUS_OSF))
        st = put((const uint8_t[]){ REG_STATUS, (uint8_t)(s & ~STATUS_OSF) }, 2);
    return st;
}
