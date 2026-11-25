/* ps.c — process/thread list (TigxOS 1.3.8) */
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
    if (psinfo(&info) < 0) {
        printf("ps: failed\n");
        return 1;
    }
    printf("  TID   PID  TGID  PPID  STATE  TYPE  NAME\n");
    for (int i = 0; i < info.count; i++) {
        struct sys_proc_info *p = &info.procs[i];
        int tid = p->tid ? p->tid : p->pid;
        int tgid = p->tgid ? p->tgid : p->pid;
        printf("%5d %5d %5d %5d  %-5s  %-4s  %s\n",
               tid, tgid, tgid, p->ppid,
               state_name(p->state),
               p->is_thread ? "thr" : (p->is_user ? "proc" : "kern"),
               p->name[0] ? p->name : "?");
    }
    return 0;
}
