/* futex.h — futex-style wait/wake (TigxOS 1.3.8) */
#ifndef TIGX_FUTEX_H
#define TIGX_FUTEX_H

#include <stdint.h>
#include <stddef.h>

#define FUTEX_WAIT  0
#define FUTEX_WAKE  1

/* Kernel entry: op on user word at uaddr.
 * WAIT: if *(int*)uaddr == val, block until wake or timeout_ms (0 = forever).
 * WAKE: wake up to val waiters on uaddr. Returns number woken.
 * Returns 0 / woken count, or negative SY_* error. */
int futex_do(uintptr_t uaddr, int op, int val, uint32_t timeout_ms);

void futex_init(void);

#endif
