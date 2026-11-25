/* kthread.h — Kernel threads (TigxOS 1.3.8)
 *
 * Kernel threads share the kernel address space, each with its own stack and
 * scheduling identity. They are first-class scheduler entities (same run
 * queues as processes) but are marked is_kthread and support join/detach.
 */
#ifndef TIGX_KTHREAD_H
#define TIGX_KTHREAD_H

#include <stdint.h>

/* Create a joinable kernel thread. Returns tid (>0) or -1.
 * entry(arg) runs in kernel mode on a fresh stack. */
int  kthread_create(const char *name, void (*entry)(void *), void *arg);

/* Wait for a joinable kthread to exit. Stores exit status in *status if non-NULL.
 * Returns 0 on success, -1 on error. Reaps the thread slot. */
int  kthread_join(int tid, int *status);

/* Mark a kthread detached: exit reaps immediately; join is invalid. */
int  kthread_detach(int tid);

/* Exit the current kernel thread (same as process_exit for kthreads). */
void kthread_exit(int status);

/* Non-blocking: 1 if tid is a live kthread, 0 otherwise. */
int  kthread_alive(int tid);

#endif /* TIGX_KTHREAD_H */
