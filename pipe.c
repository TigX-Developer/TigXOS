/* pipe.c — ring-buffer pipes, blocking R/W via wait queues (TigxOS 1.1.12) */

#include "pipe.h"
#include "wait.h"
#include "console.h"
#include "string.h"
#include "process.h"
#include "spinlock.h"
#include <stddef.h>

struct pipe {
    int used;
    char buf[PIPE_BUFSZ];
    int head;       /* read index */
    int tail;       /* write index */
    int count;      /* bytes in buffer */
    int readers;
    int writers;
    struct wait_queue rq; /* readers wait for data / EOF */
    struct wait_queue wq; /* writers wait for space */
};

static struct pipe pipes[MAX_PIPES];

void pipe_init(void) {
    for (int i = 0; i < MAX_PIPES; i++) {
        pipes[i].used = 0;
        pipes[i].readers = pipes[i].writers = 0;
        pipes[i].head = pipes[i].tail = pipes[i].count = 0;
        wait_queue_init(&pipes[i].rq, "pipe-r");
        wait_queue_init(&pipes[i].wq, "pipe-w");
    }
    kprintf("Pipes: %d x %d bytes (blocking R/W)\n", MAX_PIPES, PIPE_BUFSZ);
}

int pipe_alloc(void) {
    unsigned long flags = irq_save();
    int id = -1;
    for (int i = 0; i < MAX_PIPES; i++) {
        if (!pipes[i].used) {
            pipes[i].used = 1;
            pipes[i].head = pipes[i].tail = pipes[i].count = 0;
            pipes[i].readers = 1;
            pipes[i].writers = 1;
            id = i;
            break;
        }
    }
    irq_restore(flags);
    return id;
}

void pipe_incref(int pipe_id, int is_write) {
    if (pipe_id < 0 || pipe_id >= MAX_PIPES)
        return;
    unsigned long flags = irq_save();
    if (pipes[pipe_id].used) {
        if (is_write)
            pipes[pipe_id].writers++;
        else
            pipes[pipe_id].readers++;
    }
    irq_restore(flags);
}

void pipe_decref(int pipe_id, int is_write) {
    if (pipe_id < 0 || pipe_id >= MAX_PIPES)
        return;
    unsigned long flags = irq_save();
    if (!pipes[pipe_id].used) {
        irq_restore(flags);
        return;
    }
    struct pipe *p = &pipes[pipe_id];
    if (is_write) {
        if (p->writers > 0)
            p->writers--;
        if (p->writers == 0)
            wait_queue_wake_all(&p->rq);
    } else {
        if (p->readers > 0)
            p->readers--;
        if (p->readers == 0)
            wait_queue_wake_all(&p->wq);
    }
    if (p->readers == 0 && p->writers == 0) {
        p->used = 0;
        p->head = p->tail = p->count = 0;
    }
    irq_restore(flags);
}

long pipe_read(int pipe_id, char *dst, size_t len) {
    if (pipe_id < 0 || pipe_id >= MAX_PIPES || !pipes[pipe_id].used || !dst)
        return -1;
    if (len == 0)
        return 0;
    struct pipe *p = &pipes[pipe_id];

    for (;;) {
        unsigned long flags = irq_save();
        if (p->count > 0) {
            size_t n = len;
            if (n > (size_t)p->count)
                n = (size_t)p->count;
            for (size_t i = 0; i < n; i++) {
                dst[i] = p->buf[p->head];
                p->head = (p->head + 1) % PIPE_BUFSZ;
                p->count--;
            }
            /* Wake writers while still IF=0 to avoid wait-list races */
            wait_queue_wake_all(&p->wq);
            irq_restore(flags);
            return (long)n;
        }
        if (p->writers == 0) {
            irq_restore(flags);
            return 0; /* EOF */
        }
        wait_queue_block(&p->rq, 0);
        /* returns with IF still 0 */
        irq_restore(flags);
    }
}

long pipe_write(int pipe_id, const char *src, size_t len) {
    if (pipe_id < 0 || pipe_id >= MAX_PIPES || !pipes[pipe_id].used || !src)
        return -1;
    if (len == 0)
        return 0;
    struct pipe *p = &pipes[pipe_id];
    size_t written = 0;

    while (written < len) {
        unsigned long flags = irq_save();
        if (p->readers == 0) {
            irq_restore(flags);
            return written > 0 ? (long)written : -1;
        }
        while (p->count >= PIPE_BUFSZ) {
            if (p->readers == 0) {
                irq_restore(flags);
                return written > 0 ? (long)written : -1;
            }
            wait_queue_block(&p->wq, 0);
            /* still IF=0 */
            if (p->readers == 0) {
                irq_restore(flags);
                return written > 0 ? (long)written : -1;
            }
        }
        size_t space = (size_t)(PIPE_BUFSZ - p->count);
        size_t n = len - written;
        if (n > space)
            n = space;
        for (size_t i = 0; i < n; i++) {
            p->buf[p->tail] = src[written + i];
            p->tail = (p->tail + 1) % PIPE_BUFSZ;
            p->count++;
        }
        written += n;
        wait_queue_wake_all(&p->rq); /* data available — still IF=0 */
        irq_restore(flags);
    }
    return (long)written;
}
