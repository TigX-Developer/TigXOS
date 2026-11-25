/* rtc.h — CMOS RTC (MC146818) for TigxOS 1.3.8 */


#ifndef TIGX_RTC_H
#define TIGX_RTC_H

#include <stdint.h>

struct rtc_time {
    uint32_t year;  /* full year, e.g. 2026 */
    uint32_t mon;   /* 1-12 */
    uint32_t day;   /* 1-31 */
    uint32_t hour;  /* 0-23 */
    uint32_t min;   /* 0-59 */
    uint32_t sec;   /* 0-59 */
};

void rtc_init(void);
int  rtc_read(struct rtc_time *out); /* 0 ok, -1 fail */

#endif
