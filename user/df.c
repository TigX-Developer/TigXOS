#include "libc.h"
int main(void) {
    struct sys_sysinfo si;
    if (sysinfo(&si) != 0) {
        printf("df: sysinfo failed\n");
        return 1;
    }
    unsigned used = 0;
    if (si.fs_blocks >= si.fs_free_blocks)
        used = si.fs_blocks - si.fs_free_blocks;
    /* each block = 512 bytes */
    printf("Filesystem     blocks       used       free  files\n");
    printf("TigFS        %8u  %8u  %8u  %5u\n",
           (unsigned)si.fs_blocks, used, (unsigned)si.fs_free_blocks,
           (unsigned)si.fs_files);
    return 0;
}
