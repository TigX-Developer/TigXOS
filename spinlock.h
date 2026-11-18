

// spinlock.h - IRQ-safe spinlocks for TigxOS (UP, preemptive)
#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <stdint.h>

/*
 * Uniprocessor + preemptive kernel.
 *
 * Plain spin without disabling IRQs deadlocks if the holder is preempted
 * while another task (or an IRQ handler) spins on the same lock.
 *
 * Rules for this kernel:
 *   - spin_lock() / spin_unlock() always save/restore IF and cli while the
 *     lock is held (UP-safe, non-nestable on the same lock).
 *   - spin_lock_irqsave() / spin_unlock_irqrestore() are the explicit form
 *     when you already manage flags yourself; same implementation.
 *   - Do not take the same spinlock recursively.
 */

typedef struct spinlock {
    volatile int locked;       /* 0 = free, 1 = held */
    unsigned long irq_flags;   /* IF state of the holder; valid while locked */
} spinlock_t;

#define SPINLOCK_INIT { .locked = 0, .irq_flags = 0 }

void spin_lock_init(spinlock_t *lock);

/* IRQ-safe acquire/release (cli while held, restore IF on unlock) */
void spin_lock(spinlock_t *lock);
void spin_unlock(spinlock_t *lock);
int  spin_trylock(spinlock_t *lock);   /* 1 = acquired, 0 = busy */

/* Explicit flags form — identical semantics; use when nesting with other
   irq_save regions or when you need the flags value. */
unsigned long spin_lock_irqsave(spinlock_t *lock);
void          spin_unlock_irqrestore(spinlock_t *lock, unsigned long flags);

/* Shared IRQ flag helpers (also used by disk path) */
unsigned long irq_save(void);
void          irq_restore(unsigned long flags);

#endif /* SPINLOCK_H */
