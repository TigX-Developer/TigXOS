

// sync.c - Mutex and semaphore for TigxOS 1.3.8
#include "sync.h"
#include "process.h"
#include "console.h"
#include "spinlock.h"
#include "wait.h"
#include "string.h"
#include "log.h"

/* Deadlock detector (1.3.8):
 *  - refuse recursive lock on same mutex by same task
 *  - track a small per-task lock stack; warn on lock-order inversion
 *    when the same pair of mutexes is taken in opposite order
 */
#define DL_MAX_HELD 8
#define DL_MAX_PAIRS 32
static struct mutex *dl_held[64][DL_MAX_HELD]; /* indexed by pid % 64 */
static int           dl_held_n[64];
static struct { struct mutex *a, *b; int pid; } dl_pairs[DL_MAX_PAIRS];
static int dl_pair_n;

static void dl_push(struct mutex *m) {
    int pid = process_current() ? process_current()->pid : 0;
    int s = pid & 63;
    if (dl_held_n[s] < DL_MAX_HELD)
        dl_held[s][dl_held_n[s]++] = m;
    /* record ordered pairs with already-held locks */
    for (int i = 0; i < dl_held_n[s] - 1 && dl_pair_n < DL_MAX_PAIRS; i++) {
        struct mutex *earlier = dl_held[s][i];
        /* check inverse order was seen */
        for (int p = 0; p < dl_pair_n; p++) {
            if (dl_pairs[p].a == m && dl_pairs[p].b == earlier) {
                pr_warn("deadlock: lock-order inversion %s vs %s (pid %d)\n",
                        m->wq.name ? m->wq.name : "?",
                        earlier->wq.name ? earlier->wq.name : "?",
                        pid);
                break;
            }
        }
        dl_pairs[dl_pair_n].a = earlier;
        dl_pairs[dl_pair_n].b = m;
        dl_pairs[dl_pair_n].pid = pid;
        dl_pair_n++;
    }
}
static void dl_pop(struct mutex *m) {
    int pid = process_current() ? process_current()->pid : 0;
    int s = pid & 63;
    for (int i = dl_held_n[s] - 1; i >= 0; i--) {
        if (dl_held[s][i] == m) {
            for (int j = i; j < dl_held_n[s] - 1; j++)
                dl_held[s][j] = dl_held[s][j + 1];
            dl_held_n[s]--;
            break;
        }
    }
}

static int dl_holds(struct mutex *m) {
    int pid = process_current() ? process_current()->pid : 0;
    int s = pid & 63;
    for (int i = 0; i < dl_held_n[s]; i++)
        if (dl_held[s][i] == m) return 1;
    return 0;
}


/* ---------- Mutex ---------- */

void mutex_init(struct mutex *m, const char *name) {
    if (!m)
        return;
    m->locked = 0;
    m->owner  = -1;
    if (wait_queue_init(&m->wq, name ? name : "mutex") != 0)
        kprintf("mutex: wait-queue registry full for '%s'\n",
                name ? name : "mutex");
}

void mutex_lock(struct mutex *m) {
    if (!m)
        return;
    if (dl_holds(m)) {
        pr_warn("deadlock: recursive mutex %s\n", m->wq.name ? m->wq.name : "?");
        return;
    }

    for (;;) {
        unsigned long flags = irq_save();

        if (!m->locked) {
            m->locked = 1;
            dl_push(m);
            m->owner  = process_current() ? process_current()->pid : -1;
            irq_restore(flags);
            return;
        }

        /* Non-recursive: same owner trying again is a bug */
        if (process_current() && m->owner == process_current()->pid) {
            irq_restore(flags);
            kprintf("mutex: recursive lock by pid %d rejected\n",
                    process_current()->pid);
            return;
        }

        /* Contended. wait_queue_block requires IF=0; returns with IF=0. */
        wait_queue_block(&m->wq, 0);
        irq_restore(flags);
        /* Loop and re-check — another waiter may have taken the lock. */
    }
}

int mutex_trylock(struct mutex *m) {
    if (!m)
        return 0;
    unsigned long flags = irq_save();
    if (m->locked) {
        irq_restore(flags);
        return 0;
    }
    m->locked = 1;
    m->owner  = process_current() ? process_current()->pid : -1;
    irq_restore(flags);
    return 1;
}

void mutex_unlock(struct mutex *m) {
    if (!m)
        return;
    dl_pop(m);

    unsigned long flags = irq_save();

    if (!m->locked) {
        irq_restore(flags);
        kprintf("mutex: unlock of free lock (pid %d)\n",
                process_current() ? process_current()->pid : -1);
        return;
    }

    /* Ownership check — only the holder may release */
    int me = process_current() ? process_current()->pid : -1;
    if (m->owner != me) {
        irq_restore(flags);
        kprintf("mutex: unlock by non-owner pid %d (owner=%d) rejected\n",
                me, m->owner);
        return;
    }

    m->locked = 0;
    m->owner  = -1;
    wait_queue_wake_one(&m->wq);
    irq_restore(flags);
}

/* ---------- Semaphore ---------- */

void sem_init(struct semaphore *s, int initial, const char *name) {
    if (!s)
        return;
    if (initial < 0)
        initial = 0;
    if (initial > SEM_MAX_COUNT)
        initial = SEM_MAX_COUNT;
    s->count = initial;
    if (wait_queue_init(&s->wq, name ? name : "sem") != 0)
        kprintf("sem: wait-queue registry full for '%s'\n",
                name ? name : "sem");
}

void sem_wait(struct semaphore *s) {
    if (!s)
        return;

    for (;;) {
        unsigned long flags = irq_save();
        if (s->count > 0) {
            s->count--;
            irq_restore(flags);
            return;
        }
        wait_queue_block(&s->wq, 0);
        irq_restore(flags);
    }
}

int sem_trywait(struct semaphore *s) {
    if (!s)
        return 0;
    unsigned long flags = irq_save();
    if (s->count <= 0) {
        irq_restore(flags);
        return 0;
    }
    s->count--;
    irq_restore(flags);
    return 1;
}

int sem_post(struct semaphore *s) {
    if (!s)
        return -1;

    unsigned long flags = irq_save();
    if (s->count >= SEM_MAX_COUNT) {
        irq_restore(flags);
        kprintf("sem: post refused (at SEM_MAX_COUNT)\n");
        return -1;
    }
    s->count++;
    wait_queue_wake_one(&s->wq);
    irq_restore(flags);
    return 0;
}


void rwlock_init(struct rwlock *rw, const char *name)
{
    spin_lock_init(&rw->lock);
    rw->readers = 0;
    wait_queue_init(&rw->wq, name ? name : "rwlock");
    rw->name = name;
}

void rwlock_rdlock(struct rwlock *rw)
{
    for (;;) {
        spin_lock(&rw->lock);
        if (rw->readers >= 0) {
            rw->readers++;
            spin_unlock(&rw->lock);
            return;
        }
        spin_unlock(&rw->lock);
        process_yield();
    }
}

void rwlock_wrlock(struct rwlock *rw)
{
    for (;;) {
        spin_lock(&rw->lock);
        if (rw->readers == 0) {
            rw->readers = -1;
            spin_unlock(&rw->lock);
            return;
        }
        spin_unlock(&rw->lock);
        process_yield();
    }
}

void rwlock_unlock(struct rwlock *rw)
{
    spin_lock(&rw->lock);
    if (rw->readers < 0)
        rw->readers = 0;
    else if (rw->readers > 0)
        rw->readers--;
    spin_unlock(&rw->lock);
    wait_queue_wake_all(&rw->wq);
}
