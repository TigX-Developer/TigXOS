/* rtc.c — read PC CMOS real-time clock (TigxOS 1.3.8) */
#include "rtc.h"
#include "io.h"
#include "console.h"

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

static uint8_t cmos_read(uint8_t reg) {
    /* Keep NMI enabled (bit 7 clear) */
    outb(CMOS_ADDR, reg & 0x7F);
    return inb(CMOS_DATA);
}

static uint8_t bcd_to_bin(uint8_t v) {
    return (uint8_t)((v & 0x0F) + ((v >> 4) * 10));
}

void rtc_init(void) {
    /* nothing to program for read-only use */
}

int rtc_read(struct rtc_time *out) {
    if (!out)
        return -1;
    /* Wait briefly if update in progress (reg 0x0A bit 7) */
    for (int i = 0; i < 10000; i++) {
        if ((cmos_read(0x0A) & 0x80) == 0)
            break;
    }
    uint8_t sec  = cmos_read(0x00);
    uint8_t min  = cmos_read(0x02);
    uint8_t hour_raw = cmos_read(0x04);
    uint8_t day  = cmos_read(0x07);
    uint8_t mon  = cmos_read(0x08);
    uint8_t year = cmos_read(0x09);
    uint8_t cent = cmos_read(0x32); /* may be 0 on some VMs */
    uint8_t regb = cmos_read(0x0B);

    int is_12h = ((regb & 0x02) == 0);
    int pm = (hour_raw & 0x80) != 0;
    uint8_t hour = hour_raw & 0x7F; /* always mask PM / 12h flag */

    if ((regb & 0x04) == 0) {
        /* BCD mode */
        sec  = bcd_to_bin(sec);
        min  = bcd_to_bin(min);
        hour = bcd_to_bin(hour);
        day  = bcd_to_bin(day);
        mon  = bcd_to_bin(mon);
        year = bcd_to_bin(year);
        if (cent)
            cent = bcd_to_bin(cent);
    }
    /* Binary mode: hour already masked above */

    /* Convert 12-hour + PM to 24-hour */
    if (is_12h) {
        if (hour == 12)
            hour = 0; /* 12 AM → 0, 12 PM adjusted below */
        if (pm)
            hour = (uint8_t)((hour + 12) % 24);
    }

    out->sec  = sec;
    out->min  = min;
    out->hour = hour;
    out->day  = day;
    out->mon  = mon;
    if (cent >= 19 && cent <= 21)
        out->year = cent * 100u + year;
    else
        out->year = 2000u + year;
    return 0;
}
