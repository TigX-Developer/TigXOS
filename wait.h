// wait.h - Blocking wait queues for TigxOS V1.0.58
#ifndef WAIT_H
#define WAIT_H

#include <stdint.h>
#include "process.h"

#define WAIT_QUEUE_MAX MAX_PROCESSES

/*
 * Wait queue.
 *
 * Contract:
 *   - wait_queue_block()  MUST be called with IRQs disabled.
 *                         Returns with IRQs still disabled (caller restores).
 *   - Condition check + block must be atomic under IF=0 (no lost wakeup).
 *   - wait_queue_wake_one/all() may be called from any context (including IRQ).
 *   - wait_queue_init() returns 0 on success, -1 if the exit-cleanup
 *     registry is full (queue still works; exit may not scrub this queue).
 */
struct wait_queue {
    int         waiters[WAIT_QUEUE_MAX];  /* pids; -1 terminated */
    int         count;
    const char *name;
};

int  wait_queue_init(struct wait_queue *wq, const char *name);

void wait_queue_block(struct wait_queue *wq, uint32_t timeout_ticks);
void wait_queue_wake_one(struct wait_queue *wq);
void wait_queue_wake_all(struct wait_queue *wq);
void wait_queue_remove_pid(int pid);

#endif
