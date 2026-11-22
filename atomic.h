/* atomic.h — basic atomic ops for TigxOS (1.2.7, SMP-ready) */
#ifndef TIGX_ATOMIC_H
#define TIGX_ATOMIC_H

#include <stdint.h>

/* All ops use the x86 LOCK prefix. Safe on UP and SMP. */

static inline int atomic_xchg(volatile int *ptr, int val) {
    __asm__ volatile("xchg %0, %1"
                     : "+r"(val), "+m"(*ptr)
                     :
                     : "memory");
    return val;
}

static inline int atomic_cmpxchg(volatile int *ptr, int expected, int desired) {
    int prev = expected;
    __asm__ volatile("lock cmpxchg %2, %1"
                     : "+a"(prev), "+m"(*ptr)
                     : "r"(desired)
                     : "memory", "cc");
    return prev; /* returns the value that was at *ptr */
}

static inline void atomic_inc(volatile int *ptr) {
    __asm__ volatile("lock incl %0" : "+m"(*ptr) : : "memory");
}

static inline void atomic_dec(volatile int *ptr) {
    __asm__ volatile("lock decl %0" : "+m"(*ptr) : : "memory");
}

static inline int atomic_add_fetch(volatile int *ptr, int val) {
    int result;
    __asm__ volatile("lock xaddl %0, %1"
                     : "=r"(result), "+m"(*ptr)
                     : "0"(val)
                     : "memory");
    return result + val;
}

static inline uint32_t atomic_load_u32(volatile uint32_t *ptr) {
    return *ptr; /* aligned loads are atomic on x86 */
}

static inline void atomic_store_u32(volatile uint32_t *ptr, uint32_t val) {
    *ptr = val;
}

#endif /* TIGX_ATOMIC_H */
