/* kill.c — send signal / force-exit a process (TigxOS 1.3.8) */
#include "libc.h"

static int parse_int(const char *s, int *out) {
    int v = 0, neg = 0;
    if (!s || !*s) return -1;
    if (*s == '-') { neg = 1; s++; }
    if (*s < '0' || *s > '9') return -1;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    if (*s) return -1;
    *out = neg ? -v : v;
    return 0;
}

int main(int argc, char **argv) {
    int sig = 9; /* SIGKILL default */
    int argi = 1;
    if (argc < 2) {
        printf("usage: kill [-sig] PID\n");
        return 1;
    }
    if (argv[1][0] == '-' && argv[1][1] >= '0' && argv[1][1] <= '9') {
        if (parse_int(argv[1] + 1, &sig) != 0 || sig < 0) {
            printf("kill: bad signal\n");
            return 1;
        }
        argi = 2;
    }
    if (argi >= argc) {
        printf("usage: kill [-sig] PID\n");
        return 1;
    }
    int pid;
    if (parse_int(argv[argi], &pid) != 0 || pid <= 0) {
        printf("kill: bad pid\n");
        return 1;
    }
    if (kill_sig(pid, sig) != 0) {
        printf("kill: (%d) failed\n", pid);
        return 1;
    }
    return 0;
}
