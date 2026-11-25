

// wait.c - Blocking wait queues for TigxOS 1.3.8
#include "wait.h"
#include "console.h"
#include "string.h"
#include "timer.h"

#define WAIT_QUEUE_MAX_REGISTERED 128

static struct wait_queue *queues[WAIT_QUEUE_MAX_REGISTERED];
static int queue_count = 0;

/* A registered queue may have lived on a stack/heap block that was freed and
 * reused. Only touch it while it still looks like a live wait queue. */
static int queue_valid(const struct wait_queue *wq) {
    return wq && wq->magic == WAIT_QUEUE_MAGIC &&
           wq->count >= 0 && wq->count <= WAIT_QUEUE_MAX;
}

/* Drop registry entries whose memory no longer holds a live queue. */
static void compact_registry(void) {
    int j = 0;
    for (int i = 0; i < queue_count; i++)
        if (queue_valid(queues[i]))
            queues[j++] = queues[i];
    queue_count = j;
}

/* Returns 0 if registered (or already present), -1 if registry full. */
static int register_queue(struct wait_queue *wq) {
    if (!wq)
        return -1;
    for (int i = 0; i < queue_count; i++)
        if (queues[i] == wq)
            return 0;
    if (queue_count >= WAIT_QUEUE_MAX_REGISTERED)
        compact_registry();
    if (queue_count >= WAIT_QUEUE_MAX_REGISTERED)
        return -1;
    queues[queue_count++] = wq;
    return 0;
}

static int find_pid(const struct wait_queue *wq, int pid) {
    for (int i = 0; i < wq->count; i++)
        if (wq->waiters[i] == pid)
            return i;
    return -1;
}

static void remove_at(struct wait_queue *wq, int idx) {
    for (int i = idx; i < wq->count - 1; i++)
        wq->waiters[i] = wq->waiters[i + 1];
    wq->count--;
    wq->waiters[wq->count] = -1;
}

int wait_queue_init(struct wait_queue *wq, const char *name) {
    if (!wq)
        return -1;
    for (int i = 0; i < WAIT_QUEUE_MAX; i++)
        wq->waiters[i] = -1;
    wq->count = 0;
    wq->name  = name;
    wq->magic = WAIT_QUEUE_MAGIC;
    if (register_queue(wq) != 0) {
        kprintf("wait: registry full (%d), '%s' not tracked for exit cleanup\n",
                WAIT_QUEUE_MAX_REGISTERED, name ? name : "?");
        return -1;
    }
    return 0;
}

void wait_queue_block(struct wait_queue *wq, uint32_t timeout_ticks) {
    /* Caller holds IF=0.  We return with IF=0. */
    if (!wq || !process_current() || process_current()->pid == 0)
        return;

    int pid = process_current()->pid;
    if (wq->count >= WAIT_QUEUE_MAX) {
        kprintf("wait: queue '%s' full\n", wq->name ? wq->name : "?");
        return;
    }

    /* Enqueue first, then block — waker must be able to find us. */
    wq->waiters[wq->count++] = pid;

    uint32_t wake = 0;
    if (timeout_ticks) {
        wake = timer_get_ticks() + timeout_ticks;
        if (wake == 0)
            wake = 1;
    }

    process_block_current(wake); /* returns with IF still 0 */

    /* Resumed: remove ourselves if still listed (timeout path). */
    int idx = find_pid(wq, pid);
    if (idx >= 0)
        remove_at(wq, idx);
}

void wait_queue_wake_one(struct wait_queue *wq) {
    if (!wq || wq->count == 0)
        return;

    int pid = wq->waiters[0];
    remove_at(wq, 0);

    struct process *p = process_lookup_pid(pid);
    if (p && p->state == PROC_BLOCKED) {
        p->wake_tick = 0;
        p->state = PROC_READY;
    }
}

void wait_queue_wake_all(struct wait_queue *wq) {
    if (!wq)
        return;
    while (wq->count > 0)
        wait_queue_wake_one(wq);
}

void wait_queue_remove_pid(int pid) {
    for (int q = 0; q < queue_count; q++) {
        struct wait_queue *wq = queues[q];
        int idx;
        if (!queue_valid(wq))
            continue; /* stale entry: its memory was freed/reused, never write to it */
        while ((idx = find_pid(wq, pid)) >= 0)
            remove_at(wq, idx);
    }
}
