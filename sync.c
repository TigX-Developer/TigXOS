

// sync.c - Mutex and semaphore for TigxOS 1.1.12
#include "sync.h"
#include "process.h"
#include "console.h"
#include "spinlock.h"
#include "string.h"

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

    for (;;) {
        unsigned long flags = irq_save();

        if (!m->locked) {
            m->locked = 1;
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
