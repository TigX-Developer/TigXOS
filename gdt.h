/* gdt.h — kernel GDT + TSS (ring 0 / ring 3) */



#ifndef GDT_H
#define GDT_H

#include <stdint.h>

/* Selectors (RPL bits OR'd by caller for user: | 3) */
#define GDT_KERNEL_CS   0x08
#define GDT_KERNEL_DS   0x10
#define GDT_USER_DS     0x18
#define GDT_USER_CS     0x20
#define GDT_TSS         0x28

#define USER_CS         (GDT_USER_CS | 3)
#define USER_DS         (GDT_USER_DS | 3)

void gdt_init(void);

/* Point TSS privilege-0 stack at this kernel stack top (call before user entry
 * and on every context switch into a task that may be preempted in user mode). */
void tss_set_kernel_stack(uintptr_t stack_top);

#endif
