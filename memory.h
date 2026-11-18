

#ifndef MEMORY_H
#define MEMORY_H
#include <stdint.h>
#include <stddef.h>

#define PAGE_SIZE          4096UL
#define PAGE_SIZE_MASK     (PAGE_SIZE - 1)
#define PAGE_ALIGN_UP(x)   (((x) + PAGE_SIZE_MASK) & ~PAGE_SIZE_MASK)
#define PAGE_ALIGN_DOWN(x) ((x) & ~PAGE_SIZE_MASK)

/* System requirements (intentional for this version):
 *   - Documented machine RAM: 128 MB installed
 *   - Boot gate: 120 MB *usable* (BIOS/firmware always reserves a few MB,
 *     so QEMU -m 128 typically reports ~127 MB usable — that is OK)
 *   - Disk: 512 MB (documented; boot image stays small for QEMU)
 *   - PMM address space capped at 512 MB (design choice, not a bug)
 */
#define MAX_PHYS_ADDR       (512ULL * 1024 * 1024)
#define MIN_MEMORY_REQUIRED (120ULL * 1024 * 1024)

#define MEMORY_MAP_ENTRIES_MAX 64
#define MEMORY_TYPE_USABLE       1
#define MEMORY_TYPE_RESERVED     2
#define MEMORY_TYPE_ACPI_RECLAIM 3
#define MEMORY_TYPE_ACPI_NVS     4
#define MEMORY_TYPE_BAD          5

struct memory_map_entry {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t acpi;
} __attribute__((packed));

#define PAGE_PRESENT   (1UL << 0)
#define PAGE_WRITE     (1UL << 1)
#define PAGE_USER      (1UL << 2)
#define PAGE_HUGE      (1UL << 7)   /* 2MB (x86_64) or 4MB PSE (i386) */
#if defined(__x86_64__)
#define PAGE_NX        (1ULL << 63) /* no-execute */
#else
#define PAGE_NX        0UL          /* i386: no NX without PAE+NX */
#endif

#if defined(__i386__)
typedef uint32_t pte_t;
#else
typedef uint64_t pte_t;
#endif

void memory_init(void);
void memory_detect(void);
void memory_print_map(void);
uint64_t memory_get_total(void);
uint64_t memory_get_usable(void);
uint64_t memory_get_free(void);
uint64_t memory_get_used(void);

void  pmm_init(void);
void* pmm_alloc_frame(void);
void  pmm_free_frame(void* addr);
void* pmm_alloc_frames(size_t count);
void  pmm_free_frames(void* addr, size_t count);
uint64_t pmm_get_free_frames(void);

void  vmm_init(void);
int   vmm_map_page(void* physical, void* virtual_addr, uintptr_t flags);
int   vmm_unmap_page(void* virtual_addr);
int   vmm_unmap_and_free(void* virtual_addr);
void* vmm_get_physical(void* virtual_addr);
void* vmm_alloc_page(void);
void  vmm_free_page(void* virtual_addr);

void  heap_init(void);
void* kmalloc(size_t size);
void* kcalloc(size_t num, size_t size);
void* krealloc(void* ptr, size_t new_size);
void  kfree(void* ptr);
size_t kmalloc_usable_size(void* ptr);
void  kmalloc_print_stats(void);

void memory_test(void);
void memory_print_stats(void);
#endif
