#include "libc.h"
int main(void) {
    struct sys_sysinfo si;
    if (sysinfo(&si) != 0) {
        printf("free: sysinfo failed\n");
        return 1;
    }
    unsigned used = 0;
    if (si.mem_total_kb >= si.mem_free_kb)
        used = si.mem_total_kb - si.mem_free_kb;
    printf("              total        used        free\n");
    printf("Mem:     %8u KB %8u KB %8u KB\n",
           (unsigned)si.mem_total_kb, used, (unsigned)si.mem_free_kb);
    return 0;
}
