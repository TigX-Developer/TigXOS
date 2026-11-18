/* top.c — simple process monitor (TigxOS 1.1.9) */
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
    int rounds = 3;
    if (argc >= 2) {
        int v = 0;
        const char *s = argv[1];
        while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
        if (v > 0 && v <= 30) rounds = v;
    }

    for (int r = 0; r < rounds; r++) {
        struct sys_sysinfo si;
        struct sys_psinfo pi;
        if (sysinfo(&si) != 0 || psinfo(&pi) < 0) {
            printf("top: failed\n");
            return 1;
        }
        /* Clear-ish: print separator; full clear needs ANSI support */
        printf("\n--- TigxOS top  up %us  mem %u/%u KB  procs %d  (round %d/%d) ---\n",
               (unsigned)si.uptime_sec,
               (unsigned)si.mem_free_kb, (unsigned)si.mem_total_kb,
               pi.count, r + 1, rounds);
        printf("  PID  PPID  STATE  USER  NAME\n");
        for (int i = 0; i < pi.count; i++) {
            struct sys_proc_info *p = &pi.procs[i];
            printf("%5d %5d  %-5s  %s  %s\n",
                   p->pid, p->ppid, state_name(p->state),
                   p->is_user ? "user" : "kern",
                   p->name[0] ? p->name : "?");
        }
        if (r + 1 < rounds)
            sleep_ms(1000);
    }
    return 0;
}
