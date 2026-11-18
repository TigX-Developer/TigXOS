

// spinlock.c - IRQ-safe spinlocks for TigxOS
#include "spinlock.h"

/*
 * xchg is an implicit lock-prefixed atomic on x86. Returns the previous
 * value of *ptr; if that was 0 the lock was free and we now own it.
 */
static inline int atomic_xchg(volatile int *ptr, int val) {
    __asm__ volatile("xchg %0, %1"
                     : "+r"(val), "+m"(*ptr)
                     :
                     : "memory");
    return val;
}

static inline void cpu_relax(void) {
    __asm__ volatile("pause" ::: "memory");
}

unsigned long irq_save(void) {
    unsigned long flags;
#if defined(__i386__)
    __asm__ volatile("pushfl; pop %0; cli" : "=r"(flags) :: "memory");
#else
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
#endif
    return flags;
}

void irq_restore(unsigned long flags) {
#if defined(__i386__)
    __asm__ volatile("push %0; popfl" :: "r"(flags) : "memory", "cc");
#else
    __asm__ volatile("push %0; popfq" :: "r"(flags) : "memory", "cc");
#endif
}

void spin_lock_init(spinlock_t *lock) {
    if (!lock)
        return;
    lock->locked = 0;
    lock->irq_flags = 0;
}

void spin_lock(spinlock_t *lock) {
    if (!lock)
        return;
    for (;;) {
        unsigned long flags = irq_save();
        if (atomic_xchg(&lock->locked, 1) == 0) {
            lock->irq_flags = flags;
            return; /* acquired, IRQs stay off until unlock */
        }
        /* Busy: restore IF so the holder can run / be preempted, then retry */
        irq_restore(flags);
        while (lock->locked)
            cpu_relax();
    }
}

void spin_unlock(spinlock_t *lock) {
    if (!lock)
        return;
    unsigned long flags = lock->irq_flags;
    /* Release with a full barrier so prior stores are visible first */
    __asm__ volatile("movl $0, %0" : "=m"(lock->locked) : : "memory");
    irq_restore(flags);
}

int spin_trylock(spinlock_t *lock) {
    if (!lock)
        return 0;
    unsigned long flags = irq_save();
    if (atomic_xchg(&lock->locked, 1) == 0) {
        lock->irq_flags = flags;
        return 1;
    }
    irq_restore(flags);
    return 0;
}

unsigned long spin_lock_irqsave(spinlock_t *lock) {
    spin_lock(lock);
    return lock->irq_flags;
}

void spin_unlock_irqrestore(spinlock_t *lock, unsigned long flags) {
    /* Unlock then restore the *caller-provided* flags (not lock->irq_flags). */
    if (!lock)
        return;
    __asm__ volatile("movl $0, %0" : "=m"(lock->locked) : : "memory");
    irq_restore(flags);
}
