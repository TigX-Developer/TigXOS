#include "libc.h"
int main(void) {
    struct sys_sysinfo si;
    if (sysinfo(&si) != 0) {
        printf("date: sysinfo failed\n");
        return 1;
    }
    printf("%u-%02u-%02u %02u:%02u:%02u\n",
           (unsigned)si.rtc_year, (unsigned)si.rtc_mon, (unsigned)si.rtc_day,
           (unsigned)si.rtc_hour, (unsigned)si.rtc_min, (unsigned)si.rtc_sec);
    return 0;
}
