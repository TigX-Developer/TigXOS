#include "libc.h"
int main(void) {
    struct sys_sysinfo si;
    if (sysinfo(&si) != 0) {
        printf("sysinfo: failed\n");
        return 1;
    }
    printf("\n");
    printf("          TigxOS %s\n", si.version);
    printf("          --------\n");
    printf(" OS:      TigxOS\n");
    printf(" Host:    tigx\n");
    printf(" Kernel:  %s\n", si.version);
    printf(" Arch:    %s\n", si.arch);
    unsigned s = si.uptime_sec;
    printf(" Uptime:  %u:%02u:%02u\n", s / 3600, (s % 3600) / 60, s % 60);
    printf(" Time:    %u-%02u-%02u %02u:%02u:%02u (RTC)\n",
           (unsigned)si.rtc_year, (unsigned)si.rtc_mon, (unsigned)si.rtc_day,
           (unsigned)si.rtc_hour, (unsigned)si.rtc_min, (unsigned)si.rtc_sec);
    printf(" Memory:  %u KB free / %u KB total\n",
           (unsigned)si.mem_free_kb, (unsigned)si.mem_total_kb);
    printf(" Disk:    %u free / %u blocks (512 B), %u files\n",
           (unsigned)si.fs_free_blocks, (unsigned)si.fs_blocks,
           (unsigned)si.fs_files);
    printf(" Shell:   TigxOS shell\n");
    printf("\n");
    return 0;
}
