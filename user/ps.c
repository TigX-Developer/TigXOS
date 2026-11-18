/* ps.c — list processes (TigxOS 1.1.9) */
#include "libc.h"

static const char *state_name(int st) {
    switch (st) {
    case 1: return "READY";
    case 2: return "RUN";
    case 3: return "BLOCK";
    case 4: return "DEAD";
    default: return "?";
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    struct sys_psinfo info;
    int n = psinfo(&info);
    if (n < 0) {
        printf("ps: failed\n");
        return 1;
    }
    printf("  PID  PPID  STATE  USER  NAME\n");
    for (int i = 0; i < info.count; i++) {
        struct sys_proc_info *p = &info.procs[i];
        printf("%5d %5d  %-5s  %s  %s\n",
               p->pid, p->ppid, state_name(p->state),
               p->is_user ? "user" : "kern",
               p->name[0] ? p->name : "?");
    }
    return 0;
}
