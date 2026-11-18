/* aspace.c — Address Spaces (TigxOS 1.2.0)
 * Full teardown of intermediate tables, mapped user stacks with guard page,
 * permission flags (RO text, NX data on x86_64).
 */

#include "aspace.h"
#include "console.h"
#include "string.h"
#include "arch/arch.h"
#include <stddef.h>

static struct address_space kernel_as;
static struct address_space *current_as;
static int aspace_ready;

void aspace_init(void) {
#if defined(__x86_64__)
    /* Ensure IA32_EFER.NXE so PAGE_NX is not a reserved-bit fault */
    {
        uint32_t lo, hi;
        __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080u));
        if (!(lo & (1u << 11))) {
            lo |= (1u << 11);
            __asm__ volatile("wrmsr" : : "a"(lo), "d"(hi), "c"(0xC0000080u));
        }
    }
#endif
    kernel_as.cr3 = arch_read_cr3();
    kernel_as.root = ARCH_BOOT_PAGE_ROOT;
    kernel_as.npages = 0;
    kernel_as.ntables = 0;
    current_as = &kernel_as;
    aspace_ready = 1;
    kprintf("ASpace: kernel CR3=%p; user stack @ %p-%p (guard below)\n",
            (void *)(uintptr_t)kernel_as.cr3,
            (void *)USER_STACK_BASE, (void *)USER_STACK_TOP);
}

struct address_space *aspace_kernel(void) { return &kernel_as; }

static int track_table(struct address_space *as, void *frame) {
    if (!as || as->ntables >= ASPACE_MAX_TABLES)
        return -1;
    as->tables[as->ntables++] = frame;
    return 0;
}

#if defined(__i386__)

struct address_space *aspace_create(void) {
    pte_t *boot_pd = (pte_t *)ARCH_BOOT_PAGE_ROOT;
    void *frame = pmm_alloc_frame();
    if (!frame) return NULL;
    memset(frame, 0, PAGE_SIZE);
    pte_t *pd = (pte_t *)frame;
    for (int i = 0; i < 1024; i++)
        pd[i] = boot_pd[i];

    struct address_space *as = kmalloc(sizeof(*as));
    if (!as) { pmm_free_frame(frame); return NULL; }
    memset(as, 0, sizeof(*as));
    as->root = frame;
    as->cr3 = (uintptr_t)frame;
    return as;
}

void aspace_destroy(struct address_space *as) {
    if (!as || as == &kernel_as) return;
    for (int i = 0; i < as->npages; i++)
        if (as->pages[i].phys)
            pmm_free_frame((void *)as->pages[i].phys);
    for (int i = 0; i < as->ntables; i++)
        if (as->tables[i])
            pmm_free_frame(as->tables[i]);
    pmm_free_frame(as->root);
    kfree(as);
}

static pte_t *i386_get_pt(struct address_space *as, pte_t *pd, int pde_i) {
    if (pd[pde_i] & PAGE_HUGE) {
        /* Split 4 MB PSE page into a page table without losing mappings.
         * Identity phys base = pde_i * 4MB; replicate each 4 KB entry. */
        void *ptf = pmm_alloc_frame();
        if (!ptf) return NULL;
        pte_t *pt = (pte_t *)ptf;
        uint32_t base = (uint32_t)pde_i << 22;
        uint32_t flags = (pd[pde_i] & (PAGE_PRESENT | PAGE_WRITE | PAGE_USER));
        for (int i = 0; i < 1024; i++)
            pt[i] = (base + (uint32_t)i * PAGE_SIZE) | flags;
        if (track_table(as, ptf) != 0) { pmm_free_frame(ptf); return NULL; }
        pd[pde_i] = ((uint32_t)ptf) | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
        return pt;
    }
    if (!(pd[pde_i] & PAGE_PRESENT)) {
        void *ptf = pmm_alloc_frame();
        if (!ptf) return NULL;
        memset(ptf, 0, PAGE_SIZE);
        if (track_table(as, ptf) != 0) { pmm_free_frame(ptf); return NULL; }
        pd[pde_i] = ((uint32_t)ptf) | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
        return (pte_t *)ptf;
    }
    return (pte_t *)(pd[pde_i] & ~0xFFFu);
}

int aspace_map(struct address_space *as, uintptr_t virt, uintptr_t phys,
               uint64_t flags) {
    if (!as || (virt & PAGE_SIZE_MASK) || (phys & PAGE_SIZE_MASK)) return -1;
    if (virt < ASPACE_USER_MIN || virt >= ASPACE_USER_MAX) return -1;
    if (as->npages >= ASPACE_MAX_PAGES) return -1;

    pte_t *pd = (pte_t *)as->root;
    int pde = (int)(virt >> 22);
    int pte_i = (int)((virt >> 12) & 0x3FF);
    pte_t *pt = i386_get_pt(as, pd, pde);
    if (!pt) return -1;

    uint32_t f = PAGE_PRESENT | PAGE_USER | (uint32_t)(flags & PAGE_WRITE);
    /* i386: PAGE_NX is 0 */
    pt[pte_i] = ((uint32_t)phys & ~0xFFFu) | f;
    as->pages[as->npages].virt = virt;
    as->pages[as->npages].phys = phys;
    as->pages[as->npages].flags = f;
    as->npages++;
    return 0;
}

#else /* x86_64 */

struct address_space *aspace_create(void) {
    pte_t *boot_pml4 = (pte_t *)ARCH_BOOT_PAGE_ROOT;
    void *frame = pmm_alloc_frame();
    if (!frame) return NULL;
    memset(frame, 0, PAGE_SIZE);
    pte_t *pml4 = (pte_t *)frame;
    for (int i = 0; i < 512; i++)
        pml4[i] = boot_pml4[i];

    struct address_space *as = kmalloc(sizeof(*as));
    if (!as) { pmm_free_frame(frame); return NULL; }
    memset(as, 0, sizeof(*as));
    as->root = frame;
    as->cr3 = (uintptr_t)frame;
    return as;
}

void aspace_destroy(struct address_space *as) {
    if (!as || as == &kernel_as) return;
    for (int i = 0; i < as->npages; i++)
        if (as->pages[i].phys)
            pmm_free_frame((void *)as->pages[i].phys);
    for (int i = 0; i < as->ntables; i++)
        if (as->tables[i])
            pmm_free_frame(as->tables[i]);
    pmm_free_frame(as->root);
    kfree(as);
}

/* True if frame is private to this AS (root or tracked intermediate). */
static int as_owns_table(struct address_space *as, void *f) {
    if (!as || !f) return 0;
    if (f == as->root) return 1;
    for (int i = 0; i < as->ntables; i++)
        if (as->tables[i] == f) return 1;
    return 0;
}

/*
 * Get a writable page-table level for this AS.  Shallow-copied roots share
 * kernel tables; the first write COWs so we never mutate the kernel map.
 */
static pte_t *x64_get_or_alloc(struct address_space *as, pte_t *parent,
                               int index, int user) {
    if (!(parent[index] & PAGE_PRESENT)) {
        void *f = pmm_alloc_frame();
        if (!f) return NULL;
        memset(f, 0, PAGE_SIZE);
        if (track_table(as, f) != 0) { pmm_free_frame(f); return NULL; }
        uint64_t fl = PAGE_PRESENT | PAGE_WRITE | (user ? PAGE_USER : 0);
        parent[index] = ((uint64_t)f) | fl;
        return (pte_t *)f;
    }
    if (parent[index] & PAGE_HUGE)
        return NULL; /* caller splits */
    void *old = (void *)(parent[index] & ~0xFFFULL);
    if (!as_owns_table(as, old)) {
        /* Copy-on-write: private copy of shared kernel table */
        void *f = pmm_alloc_frame();
        if (!f) return NULL;
        memcpy(f, old, PAGE_SIZE);
        if (track_table(as, f) != 0) { pmm_free_frame(f); return NULL; }
        parent[index] = ((uint64_t)f) | (parent[index] & 0xFFFULL);
        return (pte_t *)f;
    }
    return (pte_t *)old;
}

int aspace_map(struct address_space *as, uintptr_t virt, uintptr_t phys,
               uint64_t flags) {
    if (!as || (virt & PAGE_SIZE_MASK) || (phys & PAGE_SIZE_MASK)) return -1;
    if (virt < ASPACE_USER_MIN || virt >= ASPACE_USER_MAX) return -1;
    if (as->npages >= ASPACE_MAX_PAGES) return -1;

    pte_t *pml4 = (pte_t *)as->root;
    int i4 = (virt >> 39) & 0x1FF;
    int i3 = (virt >> 30) & 0x1FF;
    int i2 = (virt >> 21) & 0x1FF;
    int i1 = (virt >> 12) & 0x1FF;

    pte_t *pdpt = x64_get_or_alloc(as, pml4, i4, 1);
    if (!pdpt) return -1;
    pte_t *pd = x64_get_or_alloc(as, pdpt, i3, 1);
    if (!pd) return -1;
    if (pd[i2] & PAGE_HUGE) {
        /* Split 2 MB page into a PT without losing existing identity maps.
         * Phys base of the huge page is bits 51:21 of the PDE. */
        void *ptf = pmm_alloc_frame();
        if (!ptf) return -1;
        pte_t *npt = (pte_t *)ptf;
        uint64_t base = pd[i2] & ~0x1FFFFFULL; /* 2 MB aligned phys */
        uint64_t fl = pd[i2] & (PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
        for (int k = 0; k < 512; k++)
            npt[k] = (base + (uint64_t)k * PAGE_SIZE) | fl;
        if (track_table(as, ptf) != 0) { pmm_free_frame(ptf); return -1; }
        pd[i2] = ((uint64_t)ptf) | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
    }
    pte_t *pt = x64_get_or_alloc(as, pd, i2, 1);
    if (!pt) return -1;

    uint64_t f = PAGE_PRESENT | PAGE_USER;
    if (flags & PAGE_WRITE)
        f |= PAGE_WRITE;
    if (flags & PAGE_NX)
        f |= PAGE_NX;
    pt[i1] = (phys & ~0xFFFULL) | f;

    as->pages[as->npages].virt = virt;
    as->pages[as->npages].phys = phys;
    as->pages[as->npages].flags = f;
    as->npages++;
    return 0;
}

#endif /* arch */

uintptr_t aspace_virt_to_phys(struct address_space *as, uintptr_t virt) {
    if (!as) return 0;
    uintptr_t page = virt & ~PAGE_SIZE_MASK;
    for (int i = 0; i < as->npages; i++)
        if (as->pages[i].virt == page)
            return as->pages[i].phys + (virt & PAGE_SIZE_MASK);
    return 0;
}

int aspace_user_range_ok(struct address_space *as, uintptr_t addr, size_t len) {
    if (!as || len == 0) return 1;
    if (addr < ASPACE_USER_MIN || addr >= ASPACE_USER_MAX) return 0;
    if (len > ASPACE_USER_MAX - addr) return 0;
    uintptr_t end = addr + len;
    for (uintptr_t v = addr & ~PAGE_SIZE_MASK; v < end; v += PAGE_SIZE)
        if (!aspace_virt_to_phys(as, v)) return 0;
    return 1;
}

/* Same as range_ok but every page must be PAGE_WRITE. */
int aspace_user_range_writable(struct address_space *as, uintptr_t addr, size_t len) {
    if (!as || len == 0) return 1;
    if (addr < ASPACE_USER_MIN || addr >= ASPACE_USER_MAX) return 0;
    if (len > ASPACE_USER_MAX - addr) return 0;
    uintptr_t end = addr + len;
    for (uintptr_t v = addr & ~PAGE_SIZE_MASK; v < end; v += PAGE_SIZE) {
        int found = 0;
        for (int i = 0; i < as->npages; i++) {
            if (as->pages[i].virt == v) {
                if (!(as->pages[i].flags & PAGE_WRITE))
                    return 0;
                found = 1;
                break;
            }
        }
        if (!found) return 0;
    }
    return 1;
}

void aspace_switch(struct address_space *as) {
    if (!as || !aspace_ready) return;
    if (current_as == as) return;
    arch_write_cr3(as->cr3);
    current_as = as;
}

uintptr_t aspace_map_user_stack(struct address_space *as, void **out_phys) {
    if (!as) return 0;
    void *base = pmm_alloc_frames(USER_STACK_PAGES);
    if (!base) return 0;
    memset(base, 0, USER_STACK_SIZE);
    for (int i = 0; i < USER_STACK_PAGES; i++) {
        uintptr_t virt = USER_STACK_BASE + (uintptr_t)i * PAGE_SIZE;
        uintptr_t phys = (uintptr_t)base + (uintptr_t)i * PAGE_SIZE;
        /* stack: writable, non-exec */
        uint64_t fl = PAGE_WRITE | PAGE_NX;
        if (aspace_map(as, virt, phys, fl) != 0) {
            /* Mapped pages 0..i-1 are tracked in as and freed by destroy.
             * Free the still-unmapped tail of the contiguous allocation. */
            for (int j = i; j < USER_STACK_PAGES; j++) {
                void *fp = (void *)((uintptr_t)base + (uintptr_t)j * PAGE_SIZE);
                pmm_free_frame(fp);
            }
            return 0;
        }
    }
    if (out_phys)
        *out_phys = base;
    return USER_STACK_TOP;
}
