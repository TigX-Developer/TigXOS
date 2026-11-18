#include "libc.h"
int main(void) {
    struct sys_sysinfo si;
    if (sysinfo(&si) != 0) {
        printf("uptime: sysinfo failed\n");
        return 1;
    }
    unsigned s = si.uptime_sec;
    unsigned h = s / 3600;
    unsigned m = (s % 3600) / 60;
    unsigned sec = s % 60;
    printf("up %u:%02u:%02u (%u ticks @ %u Hz)\n",
           h, m, sec, (unsigned)si.ticks, (unsigned)si.hz);
    return 0;
}
