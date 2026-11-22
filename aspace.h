/* aspace.h — per-process address spaces (TigxOS 1.0.76) */


#ifndef ASPACE_H
#define ASPACE_H

#include <stdint.h>
#include <stddef.h>
#include "memory.h"

#define ASPACE_USER_MIN      0x00400000UL
#define ASPACE_USER_MAX      0x04000000UL   /* 64MB user window */

/* Fixed user stack region (grows down). Guard page below base is never mapped. */
#define USER_STACK_TOP       0x03F00000UL
#define USER_STACK_PAGES     4              /* 16 KB */
#define USER_STACK_SIZE      (USER_STACK_PAGES * PAGE_SIZE)
#define USER_STACK_BASE      (USER_STACK_TOP - USER_STACK_SIZE)
#define USER_STACK_GUARD     (USER_STACK_BASE - PAGE_SIZE)

/* User heap grows up from USER_HEAP_BASE toward the stack guard page.
 * USER_HEAP_MAX is the exclusive ceiling (stack guard page is unmapped).
 * USER_HEAP_LIMIT is the default per-process soft cap (SYS_BRK). */
#define USER_HEAP_BASE       0x02000000UL
#define USER_HEAP_MAX        (USER_STACK_GUARD)  /* exclusive; guard page below stack */
#define USER_HEAP_LIMIT      (4UL * 1024 * 1024) /* 4 MB default per process */

#define ASPACE_MAX_PAGES     512
#define ASPACE_MAX_TABLES    64             /* intermediate PT/PD frames to free */

struct aspace_page {
    uintptr_t virt;
    uintptr_t phys;
    uint64_t  flags;
};

struct address_space {
    uintptr_t cr3;
    void     *root;
    int       npages;
    struct aspace_page pages[ASPACE_MAX_PAGES];
    int       ntables;
    void     *tables[ASPACE_MAX_TABLES];    /* intermediate tables to free */
};

void aspace_init(void);
struct address_space *aspace_create(void);
void aspace_destroy(struct address_space *as);
int aspace_map(struct address_space *as, uintptr_t virt, uintptr_t phys,
               uint64_t flags);
int aspace_user_range_ok(struct address_space *as, uintptr_t addr, size_t len);
int aspace_user_range_writable(struct address_space *as, uintptr_t addr, size_t len);
uintptr_t aspace_virt_to_phys(struct address_space *as, uintptr_t virt);
void aspace_switch(struct address_space *as);
void aspace_tlb_flush_page(uintptr_t page); /* SMP TLB shootdown */
struct address_space *aspace_kernel(void);

/* Map USER_STACK_PAGES contiguous frames at USER_STACK_BASE; guard left empty.
 * Returns virtual SP top (USER_STACK_TOP) or 0 on failure. *out_phys = base phys. */
uintptr_t aspace_map_user_stack(struct address_space *as, void **out_phys);

#endif
