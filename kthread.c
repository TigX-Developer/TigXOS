/* kthread.c — Kernel threads (TigxOS 1.3.8+)
 *
 * Built on the existing process scheduler: each kthread is a process_table
 * slot with is_kthread=1, its own kernel stack, and join/detach semantics.
 */

#include "kthread.h"
#include "process.h"
#include "console.h"
#include "string.h"
#include "spinlock.h"
#include "smp.h"
#include "timer.h"
#include <stddef.h>

static void kthread_trampoline(void) {
    struct process *cur = process_current();
    void (*fn)(void *) = NULL;
    void *arg = NULL;
    if (cur) {
        fn = cur->kthread_fn;
        arg = cur->kthread_arg;
        cur->kthread_fn = NULL;
        cur->kthread_arg = NULL;
    }
    if (fn)
        fn(arg);
    kthread_exit(0);
}

int kthread_create(const char *name, void (*entry)(void *), void *arg) {
    if (!entry)
        return -1;
    char nm[PROCESS_NAME_LEN];
    if (name && name[0]) {
        strncpy(nm, name, PROCESS_NAME_LEN - 1);
        nm[PROCESS_NAME_LEN - 1] = 0;
    } else {
        strncpy(nm, "kthread", PROCESS_NAME_LEN - 1);
    }

    /*
     * process_create leaves the task PROC_READY.  If a timer tick runs
     * before we install kthread_fn, the trampoline sees a NULL entry and
     * exits with status 0 — the classic i386 race that failed make test.
     * Hold IF=0 across create + field install so the new task cannot run.
     */
    unsigned long flags = irq_save();
    int tid = process_create(nm, kthread_trampoline);
    if (tid < 0) {
        irq_restore(flags);
        return -1;
    }

    struct process *p = process_lookup_pid(tid);
    if (!p) {
        irq_restore(flags);
        return -1;
    }

    p->is_kthread = 1;
    p->joinable = 1;
    p->joiner_pid = -1;
    p->kthread_fn = entry;
    p->kthread_arg = arg;
    p->is_user = 0;
    p->as = NULL;
    irq_restore(flags);
    return tid;
}

int kthread_alive(int tid) {
    struct process *p = process_lookup_pid(tid);
    if (!p || !p->is_kthread)
        return 0;
    return 1;
}

int kthread_detach(int tid) {
    extern int kthread_slot_detach(int tid);
    return kthread_slot_detach(tid);
}

int kthread_join(int tid, int *status) {
    extern int kthread_slot_join(int tid, int *status);
    return kthread_slot_join(tid, status);
}

void kthread_exit(int status) {
    process_exit(status);
}
