// sync.h - Mutex and semaphore for TigxOS V1.0.58
#ifndef SYNC_H
#define SYNC_H

#include "wait.h"

/*
 * Mutex: exclusive ownership, non-recursive.
 *   - mutex_unlock() refuses non-owner unlock (diagnostic + no release)
 *   - Interrupt flags are saved/restored (safe if called with IF=0)
 *
 * Semaphore: counting, bounded by SEM_MAX_COUNT.
 *   - sem_post() refuses to overflow; returns -1 if at cap
 */

#define SEM_MAX_COUNT  0x3fffffff

struct mutex {
    volatile int locked;
    int          owner;   /* pid, or -1 if free */
    struct wait_queue wq;
};

struct semaphore {
    volatile int count;
    struct wait_queue wq;
};

void mutex_init(struct mutex *m, const char *name);
void mutex_lock(struct mutex *m);
void mutex_unlock(struct mutex *m);
int  mutex_trylock(struct mutex *m); /* 1 = acquired, 0 = busy */

void sem_init(struct semaphore *s, int initial, const char *name);
void sem_wait(struct semaphore *s);
int  sem_post(struct semaphore *s);     /* 0 = ok, -1 = would overflow */
int  sem_trywait(struct semaphore *s);  /* 1 = got it, 0 = would block */

#endif
