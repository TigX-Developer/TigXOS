#include "libc.h"
int main(int argc, char **argv) {
    struct sys_sysinfo si;
    if (sysinfo(&si) != 0) {
        printf("uname: sysinfo failed\n");
        return 1;
    }
    int all = 0;
    for (int i = 1; i < argc; i++)
        if (argv[i][0] == '-' && argv[i][1] == 'a') all = 1;
    if (all)
        printf("TigxOS %s %s\n", si.version, si.arch);
    else
        printf("TigxOS\n");
    return 0;
}
