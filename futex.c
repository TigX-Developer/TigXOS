/* futex.c — simple hashed wait queues keyed by user virtual address */
#include "futex.h"
#include "process.h"
#include "wait.h"
#include "aspace.h"
#include "spinlock.h"
#include "timer.h"
#include "syscall.h"
#include "string.h"
#include "console.h"

#define FUTEX_BUCKETS  32
#define FUTEX_MAX_KEYS 64

struct futex_key {
    uintptr_t        uaddr;
    int              as_id;   /* crude: leader tgid as space id */
    int              in_use;
    struct wait_queue wq;
};

static struct futex_key keys[FUTEX_MAX_KEYS];
static spinlock_t futex_lock;

void futex_init(void) {
    spin_lock_init(&futex_lock);
    for (int i = 0; i < FUTEX_MAX_KEYS; i++) {
        keys[i].in_use = 0;
        keys[i].uaddr = 0;
        keys[i].as_id = -1;
        wait_queue_init(&keys[i].wq, "futex");
    }
}

static int as_cookie(struct process *p) {
    if (!p) return -1;
    return p->tgid > 0 ? p->tgid : p->pid;
}

static struct futex_key *futex_find(uintptr_t uaddr, int as_id, int create) {
    int free_i = -1;
    for (int i = 0; i < FUTEX_MAX_KEYS; i++) {
        if (keys[i].in_use && keys[i].uaddr == uaddr && keys[i].as_id == as_id)
            return &keys[i];
        if (!keys[i].in_use && free_i < 0)
            free_i = i;
    }
    if (!create || free_i < 0)
        return NULL;
    keys[free_i].in_use = 1;
    keys[free_i].uaddr = uaddr;
    keys[free_i].as_id = as_id;
    wait_queue_init(&keys[free_i].wq, "futex");
    return &keys[free_i];
}

static int futex_read_user(struct process *p, uintptr_t uaddr, int *out) {
    if (!p || !p->as || !out)
        return -1;
    if (!aspace_user_range_ok(p->as, uaddr, sizeof(int)))
        return -1;
    /* User AS is currently mapped if this is the running process */
    *out = *(volatile int *)uaddr;
    return 0;
}

int futex_do(uintptr_t uaddr, int op, int val, uint32_t timeout_ms) {
    struct process *cur = process_current();
    if (!cur || !cur->is_user || !cur->as)
        return SY_ERR;
    if (uaddr < ASPACE_USER_MIN || uaddr >= ASPACE_USER_MAX)
        return SY_EFAULT;
    if (uaddr & 3)
        return SY_EINVAL; /* must be int-aligned */

    int as_id = as_cookie(cur);

    if (op == FUTEX_WAIT) {
        int curval = 0;
        if (futex_read_user(cur, uaddr, &curval) != 0)
            return SY_EFAULT;
        if (curval != val)
            return 0; /* condition already changed */

        spin_lock(&futex_lock);
        struct futex_key *k = futex_find(uaddr, as_id, 1);
        if (!k) {
            spin_unlock(&futex_lock);
            return SY_ENOMEM;
        }
        spin_unlock(&futex_lock);

        uint32_t timeout_ticks = 0;
        if (timeout_ms > 0) {
            uint32_t hz = timer_get_hz();
            if (!hz) hz = 100;
            timeout_ticks = (timeout_ms * hz + 999) / 1000;
            if (timeout_ticks == 0) timeout_ticks = 1;
        }

        /* Re-check under IF=0 then block */
        unsigned long flags = irq_save();
        if (futex_read_user(cur, uaddr, &curval) != 0) {
            irq_restore(flags);
            return SY_EFAULT;
        }
        if (curval != val) {
            irq_restore(flags);
            return 0;
        }
        wait_queue_block(&k->wq, timeout_ticks);
        irq_restore(flags);
        return 0;
    }

    if (op == FUTEX_WAKE) {
        int nwake = val;
        if (nwake < 0) nwake = 0;
        if (nwake > MAX_PROCESSES) nwake = MAX_PROCESSES;

        spin_lock(&futex_lock);
        struct futex_key *k = futex_find(uaddr, as_id, 0);
        spin_unlock(&futex_lock);
        if (!k)
            return 0;

        int before = k->wq.count;
        int woken = 0;
        for (int i = 0; i < nwake && k->wq.count > 0; i++) {
            wait_queue_wake_one(&k->wq);
            woken++;
        }
        (void)before;
        /* If queue empty, free the key slot */
        spin_lock(&futex_lock);
        if (k->wq.count == 0) {
            k->in_use = 0;
            k->uaddr = 0;
            k->as_id = -1;
        }
        spin_unlock(&futex_lock);
        return woken;
    }

    return SY_EINVAL;
}
