/* memory.c — PMM / VMM / kernel heap (TigxOS 1.3.8) */



#include "memory.h"
#include "console.h"
#include "string.h"
#include "spinlock.h"
#include "arch/arch.h"
#include <stddef.h>
#include <stdint.h>

#define BOOT_MEMMAP_COUNT   ARCH_BOOT_MEMMAP_COUNT
#define BOOT_MEMMAP_DATA    ((struct e820_entry*)ARCH_BOOT_MEMMAP_DATA)

struct e820_entry {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t acpi;
} __attribute__((packed));

static struct memory_map_entry memory_map[MEMORY_MAP_ENTRIES_MAX];
static int memory_map_entries = 0;
static uint64_t total_memory = 0;
static uint64_t usable_memory = 0;

/* Frame index = physical / PAGE_SIZE. MAX_PHYS_ADDR from memory.h */
#define MAX_FRAMES        (MAX_PHYS_ADDR / PAGE_SIZE)
#define BITMAP_BYTES      ((MAX_FRAMES + 7) / 8)

static uint8_t frame_bitmap[BITMAP_BYTES];
static uint64_t total_frames = 0;   /* = max_phys / PAGE_SIZE */
static uint64_t free_frames  = 0;
static uint64_t used_frames  = 0;

#define RESERVED_END 0x800000ULL   /* 4MB boot/kernel + 4MB user-ELF landing zone @ 0x400000 */

static inline void bset(uint64_t b) { frame_bitmap[b/8] |=  (1u << (b%8)); }
static inline void bclr(uint64_t b) { frame_bitmap[b/8] &= ~(1u << (b%8)); }
static inline int  btst(uint64_t b) { return (frame_bitmap[b/8] >> (b%8)) & 1; }

static uint64_t find_free_frame(void) {
    for (uint64_t i = 0; i < total_frames; i++)
        if (!btst(i)) return i;
    return (uint64_t)-1;
}

static uint64_t find_free_contiguous(uint64_t count) {
    uint64_t start = 0, found = 0;
    for (uint64_t i = 0; i < total_frames; i++) {
        if (!btst(i)) {
            if (found == 0) start = i;
            if (++found == count) return start;
        } else found = 0;
    }
    return (uint64_t)-1;
}

void memory_detect(void) {
    kprintf("Reading E820 from bootloader...\n");
    uint32_t count = *BOOT_MEMMAP_COUNT;
    if (count == 0 || count > MEMORY_MAP_ENTRIES_MAX) {
        kprintf("E820 unavailable - fallback 128MB\n");
        memory_map_entries = 2;
        memory_map[0] = (struct memory_map_entry){0, 0x100000, MEMORY_TYPE_RESERVED, 0};
        memory_map[1] = (struct memory_map_entry){0x100000, 127ULL*1024*1024, MEMORY_TYPE_USABLE, 0};
    } else {
        memory_map_entries = 0;
        for (uint32_t i = 0; i < count && memory_map_entries < MEMORY_MAP_ENTRIES_MAX; i++) {
            struct e820_entry *e = &BOOT_MEMMAP_DATA[i];
            if (e->length == 0) continue;
            memory_map[memory_map_entries].base   = e->base;
            memory_map[memory_map_entries].length = e->length;
            memory_map[memory_map_entries].type   = e->type;
            memory_map[memory_map_entries].acpi   = e->acpi;
            memory_map_entries++;
        }
        kprintf("E820 returned %d entries\n", memory_map_entries);
    }

    total_memory = usable_memory = 0;
    uint64_t max_end = 0;
    for (int i = 0; i < memory_map_entries; i++) {
        uint64_t base = memory_map[i].base;
        uint64_t length = memory_map[i].length;
        if (length && base + length < base) {
            kprintf("E820: entry %d overflows (base=%x len=%x), clamped\n",
                    i, (unsigned long)base, (unsigned long)length);
            length = UINT64_MAX - base;
            memory_map[i].length = length;
        }
        total_memory += length;
        if (memory_map[i].type == MEMORY_TYPE_USABLE)
            usable_memory += length;
        uint64_t end = base + length;
        if (end > max_end) max_end = end;
    }

    // Debug output
    kprintf("Usable RAM detected: %d MB\n", (uint32_t)(usable_memory / (1024*1024)));
    kprintf("Total RAM detected: %d MB\n", (uint32_t)(total_memory / (1024*1024)));

    /* Enforce usable floor, not installed size.
       128 MB machines typically have ~127 MB usable after BIOS reserved regions. */
    if (usable_memory < MIN_MEMORY_REQUIRED) {
        kprintf("ERROR: only %dMB usable RAM, need %dMB usable (128MB installed)\n",
                (uint32_t)(usable_memory / (1024*1024)),
                (uint32_t)(MIN_MEMORY_REQUIRED / (1024*1024)));
        kprintf("System halted.\n");
        while (1) __asm__ volatile("hlt");
    }

    /* Frame space covers up to highest address (capped), not sum of lengths */
    if (max_end > MAX_PHYS_ADDR) max_end = MAX_PHYS_ADDR;
    total_frames = max_end / PAGE_SIZE;
    if (total_frames > MAX_FRAMES) total_frames = MAX_FRAMES;

    kprintf("Total:%dMB Usable:%dMB FrameSpace:%d (%dMB)\n",
            (uint32_t)(total_memory / (1024*1024)),
            (uint32_t)(usable_memory / (1024*1024)),
            (uint32_t)total_frames,
            (uint32_t)(total_frames * PAGE_SIZE / (1024*1024)));
}

void memory_print_map(void) {
    kprintf("\nMemory Map:\n");
    for (int i = 0; i < memory_map_entries; i++) {
        const char *t = "Unknown";
        switch (memory_map[i].type) {
            case 1: t = "Usable"; break;
            case 2: t = "Reserved"; break;
            case 3: t = "ACPI Reclaim"; break;
            case 4: t = "ACPI NVS"; break;
            case 5: t = "Bad"; break;
        }
        kprintf("  [%d] 0x%016x +0x%016x %s\n",
                i, memory_map[i].base, memory_map[i].length, t);
    }
    kprintf("\n");
}

uint64_t memory_get_total(void) { return total_memory; }
uint64_t memory_get_usable(void) { return usable_memory; }
uint64_t memory_get_free(void)  { return free_frames * PAGE_SIZE; }
uint64_t memory_get_used(void)  { return used_frames * PAGE_SIZE; }

void pmm_init(void) {
    kprintf("PMM init...\n");
    /* Everything used until we free usable regions */
    memset(frame_bitmap, 0xFF, BITMAP_BYTES);
    free_frames = used_frames = 0;

    for (int i = 0; i < memory_map_entries; i++) {
        if (memory_map[i].type != MEMORY_TYPE_USABLE) continue;
        uint64_t start = PAGE_ALIGN_UP(memory_map[i].base) / PAGE_SIZE;
        uint64_t end   = PAGE_ALIGN_DOWN(memory_map[i].base + memory_map[i].length) / PAGE_SIZE;
        if (end > total_frames) end = total_frames;
        for (uint64_t f = start; f < end; f++) {
            if (btst(f)) { bclr(f); free_frames++; }
        }
    }

    /* Reserve low memory (bootloader, kernel, early page tables, E820 blob) */
    uint64_t res = RESERVED_END / PAGE_SIZE;
    for (uint64_t f = 0; f < res && f < total_frames; f++) {
        if (!btst(f)) { bset(f); free_frames--; used_frames++; }
    }

    kprintf("PMM ready: %d free frames (%d MB)\n",
            (uint32_t)free_frames,
            (uint32_t)(free_frames * PAGE_SIZE / (1024*1024)));
}

void *pmm_alloc_frame(void) {
    uint64_t f = find_free_frame();
    if (f == (uint64_t)-1) { kprintf("PMM: OOM\n"); return NULL; }
    bset(f); free_frames--; used_frames++;
    return (void *)(uintptr_t)(f * PAGE_SIZE);
}

void pmm_free_frame(void *addr) {
    if (!addr) return;
    if ((uintptr_t)addr & PAGE_SIZE_MASK) {
        kprintf("PMM: free unaligned 0x%016x\n", (unsigned long long)(uintptr_t)addr);
        return;
    }
    uint64_t f = (uintptr_t)addr / PAGE_SIZE;
    if (f >= total_frames) return;
    if (!btst(f)) { kprintf("PMM: double free 0x%016x\n", (unsigned long long)(uintptr_t)addr); return; }
    bclr(f); free_frames++; used_frames--;
}

void *pmm_alloc_frames(size_t count) {
    if (count == 0) return NULL;
    if (count == 1) return pmm_alloc_frame();
    uint64_t s = find_free_contiguous(count);
    if (s == (uint64_t)-1) return NULL;
    for (size_t i = 0; i < count; i++) bset(s + i);
    free_frames -= count; used_frames += count;
    return (void *)(uintptr_t)(s * PAGE_SIZE);
}

void pmm_free_frames(void *addr, size_t count) {
    if (!addr || !count) return;
    if ((uintptr_t)addr & PAGE_SIZE_MASK) {
        kprintf("PMM: free_frames unaligned 0x%016x\n", (unsigned long long)(uintptr_t)addr);
        return;
    }
    uint64_t s = (uintptr_t)addr / PAGE_SIZE;
    for (size_t i = 0; i < count; i++) {
        if (s + i < total_frames && btst(s + i)) {
            bclr(s + i); free_frames++; used_frames--;
        }
    }
}

uint64_t pmm_get_free_frames(void) { return free_frames; }

/* -------------------- VMM -------------------- */
/* Page-table root is set up by the arch boot layer (identity map 0-1GB). */
#define do_invlpg(addr) arch_invlpg(addr)

#if defined(__i386__)
/*
 * i386: boot installed a PSE page directory at 0x1000 with 4MB identity
 * pages covering 0–1GB.  Fine-grained 4KB mapping is not implemented yet;
 * the heap and kernel run entirely inside that identity window, so map/unmap
 * for identity addresses are treated as already-mapped no-ops.
 */
static pte_t *page_dir = (pte_t *)ARCH_BOOT_PAGE_ROOT;

int vmm_map_page(void *physical, void *virtual_addr, uintptr_t flags) {
    (void)flags;
    uintptr_t virt = (uintptr_t)virtual_addr;
    uintptr_t phys = (uintptr_t)physical;
    if ((virt & PAGE_SIZE_MASK) || (phys & PAGE_SIZE_MASK))
        return -1;
    /* Identity region already covered by 4MB PSE pages from boot */
    if (virt < 0x40000000UL && virt == phys)
        return 0;
    kprintf("vmm_map: i386 only supports identity 0-1GB (virt=%p phys=%p)\n",
            virtual_addr, physical);
    return -1;
}

int vmm_unmap_page(void *virtual_addr) {
    (void)virtual_addr;
    return -1; /* do not tear down boot identity map */
}

int vmm_unmap_and_free(void *virtual_addr) {
    (void)virtual_addr;
    return -1;
}

void *vmm_get_physical(void *virtual_addr) {
    uintptr_t virt = (uintptr_t)virtual_addr;
    if (virt < 0x40000000UL)
        return virtual_addr; /* identity */
    return NULL;
}

void *vmm_alloc_page(void) {
    /* Return an identity-mapped physical frame */
    return pmm_alloc_frame();
}

void vmm_free_page(void *virtual_addr) {
    pmm_free_frame(virtual_addr);
}

void vmm_init(void) {
    /*
     * Only the user window is U/S=1 (4MB pages): 4MB .. 64MB covers
     * ELF load (0x400000) and shell (0x600000) and early heap stacks.
     * Kernel image at low addresses stays supervisor-only.
     */
    for (int i = 0; i < 1024; i++) {
        if (!(page_dir[i] & PAGE_PRESENT))
            continue;
        if (i >= 1 && i < 16) /* 4MB .. 64MB */
            page_dir[i] |= PAGE_USER;
        else
            page_dir[i] &= ~((uint32_t)PAGE_USER);
    }
    arch_write_cr3(arch_read_cr3());
    kprintf("VMM: i386 PSE PD @ %p, USER window 4MB-64MB\n",
            ARCH_BOOT_PAGE_ROOT);
}

#else /* __x86_64__ */

static pte_t *pml4 = (pte_t *)ARCH_BOOT_PAGE_ROOT;

static pte_t *get_or_alloc_table(pte_t *parent, int index) {
    if (!(parent[index] & PAGE_PRESENT)) {
        void *frame = pmm_alloc_frame();
        if (!frame) return NULL;
        memset(frame, 0, PAGE_SIZE);
        parent[index] = ((uint64_t)frame) | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
    }
    return (pte_t *)(parent[index] & (~0xFFFULL));
}

int vmm_map_page(void *physical, void *virtual_addr, uintptr_t flags) {
    uint64_t virt = (uint64_t)virtual_addr;
    uint64_t phys = (uint64_t)physical;

    if (virt & PAGE_SIZE_MASK) {
        kprintf("vmm_map: virt not aligned 0x%016x\n", virt);
        return -1;
    }
    if (phys & PAGE_SIZE_MASK) {
        kprintf("vmm_map: phys not aligned 0x%016x\n", phys);
        return -1;
    }

    int pml4_i = (virt >> 39) & 0x1FF;
    int pdpt_i = (virt >> 30) & 0x1FF;
    int pd_i   = (virt >> 21) & 0x1FF;
    int pt_i   = (virt >> 12) & 0x1FF;

    pte_t *pdpt = get_or_alloc_table(pml4, pml4_i);
    if (!pdpt) return -1;
    pte_t *pd = get_or_alloc_table(pdpt, pdpt_i);
    if (!pd) return -1;

    if (pd[pd_i] & PAGE_HUGE) {
        /* Split 2 MB identity page into a 4 KB page table, preserve maps. */
        void *ptf = pmm_alloc_frame();
        if (!ptf) {
            kprintf("vmm_map: cannot split 2MB page (OOM)\n");
            return -1;
        }
        pte_t *npt = (pte_t *)ptf;
        uint64_t base = pd[pd_i] & ~0x1FFFFFULL;
        uint64_t fl = pd[pd_i] & (PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
        for (int k = 0; k < 512; k++)
            npt[k] = (base + (uint64_t)k * PAGE_SIZE) | fl;
        pd[pd_i] = ((uint64_t)ptf) | PAGE_PRESENT | PAGE_WRITE |
                   (fl & PAGE_USER);
        /* fall through to map the requested 4 KB page */
    }

    pte_t *pt = get_or_alloc_table(pd, pd_i);
    if (!pt) return -1;

    if (pt[pt_i] & PAGE_PRESENT) {
        kprintf("vmm_map: already mapped virt=0x%016x\n", virt);
        return -1;
    }

    pt[pt_i] = (phys & (~0xFFFULL)) | (flags & 0xFFFULL) | PAGE_PRESENT;
    do_invlpg(virtual_addr);
    return 0;
}

int vmm_unmap_page(void *virtual_addr) {
    uint64_t virt = (uint64_t)virtual_addr;
    if (virt & PAGE_SIZE_MASK) return -1;

    int pml4_i = (virt >> 39) & 0x1FF;
    int pdpt_i = (virt >> 30) & 0x1FF;
    int pd_i   = (virt >> 21) & 0x1FF;
    int pt_i   = (virt >> 12) & 0x1FF;

    if (!(pml4[pml4_i] & PAGE_PRESENT)) return -1;
    pte_t *pdpt = (pte_t *)(pml4[pml4_i] & (~0xFFFULL));
    if (!(pdpt[pdpt_i] & PAGE_PRESENT)) return -1;
    pte_t *pd = (pte_t *)(pdpt[pdpt_i] & (~0xFFFULL));
    if (pd[pd_i] & PAGE_HUGE) return -1;
    if (!(pd[pd_i] & PAGE_PRESENT)) return -1;
    pte_t *pt = (pte_t *)(pd[pd_i] & (~0xFFFULL));

    if (!(pt[pt_i] & PAGE_PRESENT)) return -1;

    pt[pt_i] = 0;
    do_invlpg(virtual_addr);
    return 0;
}

int vmm_unmap_and_free(void *virtual_addr) {
    void *phys = vmm_get_physical(virtual_addr);
    if (vmm_unmap_page(virtual_addr) != 0) return -1;
    if (phys) pmm_free_frame(phys);
    return 0;
}

void *vmm_get_physical(void *virtual_addr) {
    uint64_t virt = (uint64_t)virtual_addr;
    int pml4_i = (virt >> 39) & 0x1FF;
    int pdpt_i = (virt >> 30) & 0x1FF;
    int pd_i   = (virt >> 21) & 0x1FF;
    int pt_i   = (virt >> 12) & 0x1FF;

    if (!(pml4[pml4_i] & PAGE_PRESENT)) return NULL;
    pte_t *pdpt = (pte_t *)(pml4[pml4_i] & (~0xFFFULL));
    if (!(pdpt[pdpt_i] & PAGE_PRESENT)) return NULL;
    pte_t *pd = (pte_t *)(pdpt[pdpt_i] & (~0xFFFULL));
    if (pd[pd_i] & PAGE_HUGE) {
        uint64_t base = pd[pd_i] & (~0x1FFFFFULL);
        return (void *)(base + (virt & 0x1FFFFF));
    }
    if (!(pd[pd_i] & PAGE_PRESENT)) return NULL;
    pte_t *pt = (pte_t *)(pd[pd_i] & (~0xFFFULL));
    if (!(pt[pt_i] & PAGE_PRESENT)) return NULL;
    return (void *)((pt[pt_i] & (~0xFFFULL)) + (virt & 0xFFF));
}

static uint64_t next_virt = 0x40000000ULL; /* above 1GB identity */

void *vmm_alloc_page(void) {
    void *phys = pmm_alloc_frame();
    if (!phys) return NULL;
    void *virt = (void *)next_virt;
    next_virt += PAGE_SIZE;
    if (vmm_map_page(phys, virt, PAGE_PRESENT | PAGE_WRITE) != 0) {
        pmm_free_frame(phys);
        return NULL;
    }
    return virt;
}

void vmm_free_page(void *virtual_addr) {
    vmm_unmap_and_free(virtual_addr);
}

void vmm_init(void) {
    /*
     * User window only: 2MB pages covering 4MB .. 64MB (indices 2..31).
     * PML4/PDPT still need U/S=1 so the walk is allowed; low PDEs stay S-only.
     */
    pte_t *pml4_e = pml4;
    if (pml4_e[0] & PAGE_PRESENT) {
        pml4_e[0] |= PAGE_USER;
        pte_t *pdpt = (pte_t *)(pml4_e[0] & ~0xFFFULL);
        if (pdpt[0] & PAGE_PRESENT) {
            pdpt[0] |= PAGE_USER;
            pte_t *pd = (pte_t *)(pdpt[0] & ~0xFFFULL);
            for (int i = 0; i < 512; i++) {
                if (!(pd[i] & PAGE_PRESENT))
                    continue;
                if (i >= 2 && i < 32) /* 4MB .. 64MB */
                    pd[i] |= PAGE_USER;
                else
                    pd[i] &= ~((uint64_t)PAGE_USER);
            }
        }
    }
    arch_write_cr3(arch_read_cr3());
    kprintf("VMM: PML4 @ %p, USER window 4MB-64MB\n", ARCH_BOOT_PAGE_ROOT);
}

#endif /* __x86_64__ */

/* -------------------- Heap (identity-mapped physical) — 1.2.2 -------------------- */
#define HEAP_MAGIC     0xDEADBEEFu
#define HEAP_CANARY    0xC0FFEE42u
#define HEAP_ALIGN     16
#define HEAP_MIN_SPLIT 32

struct heap_block {
    uint32_t magic, size;       /* size = whole block including header */
    uint8_t  used, pad[3];
    struct heap_block *next, *prev;
};
/* Payload must be 16-byte aligned; round header up to HEAP_ALIGN. */
#define HDR_SIZE ((sizeof(struct heap_block) + (HEAP_ALIGN - 1)) & ~((size_t)HEAP_ALIGN - 1))
#define CANARY_SIZE 4

static struct heap_block *heap_root = NULL;
static size_t heap_total = 0, heap_used = 0, heap_free_bytes = 0;
static size_t heap_allocs = 0, heap_frees = 0;
static size_t heap_peak = 0;           /* peak heap_used */
static size_t heap_fail_canary = 0;    /* canary mismatches detected */
static size_t heap_fail_magic = 0;
static spinlock_t heap_lock = SPINLOCK_INIT;

void heap_init(void) {
    kprintf("Heap init...\n");
    /* Kernel heap must live inside the identity-mapped window so kmalloc
     * returns directly usable addresses (no highmem mapping yet). */
    const size_t pages = 256; /* 1 MB */
    const uintptr_t ident_end = 0x40000000UL;
    void *mem = pmm_alloc_frames(pages);
    if (!mem) {
        kprintf("Heap: no frames\n");
        return;
    }
    if ((uintptr_t)mem >= ident_end ||
        (uintptr_t)mem + pages * PAGE_SIZE > ident_end) {
        kprintf("Heap: frames %p outside identity map — free and fail closed\n",
                mem);
        pmm_free_frames(mem, pages);
        /* Fail closed: never leave a half-init heap that makes kmalloc hang. */
        return;
    }

    /* Reserve the last page as an unallocated guard zone (still mapped for
     * simplicity on identity heap; allocator never hands it out). */
    size_t usable = (pages - 1) * PAGE_SIZE;
    heap_root = (struct heap_block *)mem;
    heap_root->magic = HEAP_MAGIC;
    heap_root->size  = (uint32_t)usable;
    heap_root->used  = 0;
    heap_root->next  = heap_root->prev = NULL;
    heap_total = usable;
    heap_free_bytes = heap_total - HDR_SIZE;
    heap_used = 0;
    heap_peak = 0;
    heap_fail_canary = heap_fail_magic = 0;
    spin_lock_init(&heap_lock);
    kprintf("Heap at %p (%u KB + 4KB guard)\n",
            (void *)heap_root,
            (unsigned)(heap_total / 1024));
}

static size_t align_up(size_t n) {
    return (n + HEAP_ALIGN - 1) & ~(size_t)(HEAP_ALIGN - 1);
}

void *kmalloc(size_t size) {
    if (!size || !heap_root) return NULL;
    if (size > (size_t)-1 - HDR_SIZE - CANARY_SIZE - HEAP_ALIGN) return NULL;
    size = align_up(size);
    /* Block size must stay 16-byte aligned so split successors stay aligned. */
    size_t need = align_up(size + HDR_SIZE + CANARY_SIZE);

    unsigned long flags = spin_lock_irqsave(&heap_lock);
    void *ret = NULL;
    for (struct heap_block *c = heap_root; c; c = c->next) {
        if (c->magic != HEAP_MAGIC)
            continue; /* skip corrupted nodes */
        if (!c->used && c->size >= need) {
            if (c->size >= need + HDR_SIZE + HEAP_MIN_SPLIT) {
                struct heap_block *n = (struct heap_block *)((uint8_t *)c + need);
                n->magic = HEAP_MAGIC;
                n->size  = c->size - (uint32_t)need;
                n->used  = 0;
                n->next  = c->next;
                n->prev  = c;
                if (c->next) c->next->prev = n;
                c->next = n;
                c->size = (uint32_t)need;
            }
            c->used = 1;
            heap_used += c->size;
            if (heap_used > heap_peak)
                heap_peak = heap_used;
            heap_free_bytes -= c->size;
            heap_allocs++;
            ret = (uint8_t *)c + HDR_SIZE;
            /* canary at end of block (survives extra alignment padding) */
            uint32_t *can = (uint32_t *)((uint8_t *)c + c->size - CANARY_SIZE);
            *can = HEAP_CANARY;
            break;
        }
    }
    spin_unlock_irqrestore(&heap_lock, flags);

    if (!ret)
        kprintf("kmalloc: OOM (%d bytes)\n", (uint32_t)size);
    return ret;
}

void *kcalloc(size_t num, size_t size) {
    if (size && num > (size_t)-1 / size) return NULL; /* overflow */
    size_t t = num * size;
    void *p = kmalloc(t);
    if (p) memset(p, 0, t);
    return p;
}

void kfree(void *ptr) {
    if (!ptr) return;
    struct heap_block *b = (struct heap_block *)((uint8_t *)ptr - HDR_SIZE);

    unsigned long flags = spin_lock_irqsave(&heap_lock);
    if (b->magic != HEAP_MAGIC) {
        heap_fail_magic++;
        spin_unlock_irqrestore(&heap_lock, flags);
        kprintf("kfree: bad magic\n");
        return;
    }
    if (!b->used) {
        spin_unlock_irqrestore(&heap_lock, flags);
        kprintf("kfree: double free\n");
        return;
    }
    /* payload size = block - header - canary */
    if (b->size >= HDR_SIZE + CANARY_SIZE) {
        uint32_t *can = (uint32_t *)((uint8_t *)b + b->size - CANARY_SIZE);
        if (*can != HEAP_CANARY) {
            heap_fail_canary++;
            kprintf("kfree: canary corrupt (overflow?)\n");
            /* still free to avoid permanent leak, but counted */
        }
    }
    b->used = 0;
    heap_used -= b->size;
    heap_free_bytes += b->size;
    heap_frees++;
    if (b->next && !b->next->used && b->next->magic == HEAP_MAGIC) {
        b->size += b->next->size;
        b->next = b->next->next;
        if (b->next) b->next->prev = b;
    }
    if (b->prev && !b->prev->used && b->prev->magic == HEAP_MAGIC) {
        b->prev->size += b->size;
        b->prev->next = b->next;
        if (b->next) b->next->prev = b->prev;
    }
    spin_unlock_irqrestore(&heap_lock, flags);
}

void *krealloc(void *ptr, size_t new_size) {
    if (!ptr) return kmalloc(new_size);
    if (!new_size) { kfree(ptr); return NULL; }
    struct heap_block *b = (struct heap_block *)((uint8_t *)ptr - HDR_SIZE);
    size_t old = (b->size > HDR_SIZE + CANARY_SIZE)
               ? (b->size - HDR_SIZE - CANARY_SIZE) : 0;
    void *n = kmalloc(new_size);
    if (!n) return NULL;
    memcpy(n, ptr, old < new_size ? old : new_size);
    kfree(ptr);
    return n;
}

size_t kmalloc_usable_size(void *ptr) {
    if (!ptr) return 0;
    struct heap_block *b = (struct heap_block *)((uint8_t *)ptr - HDR_SIZE);
    if (b->magic != HEAP_MAGIC || b->size < HDR_SIZE + CANARY_SIZE)
        return 0;
    return b->size - HDR_SIZE - CANARY_SIZE;
}

void kmalloc_print_stats(void) {
    unsigned long flags = spin_lock_irqsave(&heap_lock);
    size_t total = heap_total, used = heap_used, freeb = heap_free_bytes;
    size_t allocs = heap_allocs, frees = heap_frees, peak = heap_peak;
    size_t outstanding = (allocs >= frees) ? (allocs - frees) : 0;
    size_t fc = heap_fail_canary, fm = heap_fail_magic;
    spin_unlock_irqrestore(&heap_lock, flags);

    kprintf("Heap: total=%dKB used=%dKB peak=%dKB free=%dKB\n",
            (uint32_t)(total / 1024), (uint32_t)(used / 1024),
            (uint32_t)(peak / 1024), (uint32_t)(freeb / 1024));
    kprintf("      allocs=%d frees=%d outstanding=%d canary_fail=%d magic_fail=%d\n",
            (uint32_t)allocs, (uint32_t)frees, (uint32_t)outstanding,
            (uint32_t)fc, (uint32_t)fm);
}

/* Walk free list; return 0 if healthy, -1 if corruption found. */
int heap_validate(void) {
    if (!heap_root)
        return -1;
    unsigned long flags = spin_lock_irqsave(&heap_lock);
    int rc = 0;
    size_t seen = 0;
    for (struct heap_block *c = heap_root; c; c = c->next) {
        if (c->magic != HEAP_MAGIC) {
            rc = -1;
            break;
        }
        if (c->next && c->next->prev != c) {
            rc = -1;
            break;
        }
        seen += c->size;
        if (seen > heap_total + PAGE_SIZE) { /* include guard slack */
            rc = -1;
            break;
        }
    }
    spin_unlock_irqrestore(&heap_lock, flags);
    return rc;
}

size_t kmalloc_outstanding(void) {
    unsigned long flags = spin_lock_irqsave(&heap_lock);
    size_t o = (heap_allocs >= heap_frees) ? (heap_allocs - heap_frees) : 0;
    spin_unlock_irqrestore(&heap_lock, flags);
    return o;
}

size_t kmalloc_peak(void) {
    unsigned long flags = spin_lock_irqsave(&heap_lock);
    size_t p = heap_peak;
    spin_unlock_irqrestore(&heap_lock, flags);
    return p;
}


/* Map physical MMIO range uncached. virt==phys for identity-style MMIO. */
int vmm_map_mmio(uintptr_t phys, uintptr_t virt, size_t len) {
    if (!len) return 0;
    uintptr_t flags = PAGE_PRESENT | PAGE_WRITE | PAGE_PCD | PAGE_PWT;
    uintptr_t p = phys & ~PAGE_SIZE_MASK;
    uintptr_t v = virt & ~PAGE_SIZE_MASK;
    uintptr_t end = phys + len;
    while (p < end) {
        if (vmm_map_page((void *)p, (void *)v, flags) != 0) {
            /* On i386 identity only covers 0–1GB; high MMIO (e.g. LAPIC
             * at 0xFEE00000) cannot be mapped. Treat as failure so callers
             * (lapic_init) can fall back to UP. */
            return -1;
        }
        p += PAGE_SIZE;
        v += PAGE_SIZE;
    }
    return 0;
}

void memory_init(void) {
    kprintf("\n=== Memory Management ===\n");
    memory_detect();
    memory_print_map();
    pmm_init();
    vmm_init();
    heap_init();
    kprintf("=========================\n\n");
}

void memory_test(void) {
    kprintf("Memory smoke test...\n");
    void *a = pmm_alloc_frame();
    void *b = kmalloc(64);
    if (b) { strcpy((char *)b, "ok"); kprintf("  heap '%s'\n", (char *)b); kfree(b); }
    if (a) pmm_free_frame(a);
    kmalloc_print_stats();
}

void memory_print_stats(void) {
    kprintf("\nFrames: total=%d free=%d used=%d\n",
            (uint32_t)total_frames, (uint32_t)free_frames, (uint32_t)used_frames);
    kmalloc_print_stats();
}


/* ---- 1.3.8 DMA-safe contiguous allocator ----
 * Uses the PMM for physically contiguous frames. Under TigxOS identity
 * mapping of low memory, the returned virtual address equals the physical
 * address for frames below 4 GiB. Prefer low frames for 32-bit bus masters.
 */
void *dma_alloc_pages(size_t pages, uintptr_t *phys_out)
{
    if (pages == 0)
        return 0;
    void *p = pmm_alloc_frames(pages);
    if (!p)
        return 0;
    uintptr_t phys = (uintptr_t)p;
    /* Reject >4GiB for 32-bit DMA devices (should not happen on our PMM). */
    if (phys >= 0x100000000ULL) {
        pmm_free_frames(p, pages);
        return 0;
    }
    if (phys_out)
        *phys_out = phys;
    return p;
}

void dma_free_pages(void *ptr, size_t pages)
{
    if (ptr && pages)
        pmm_free_frames(ptr, pages);
}

void *dma_alloc(size_t size, uintptr_t *phys_out)
{
    if (size == 0)
        return 0;
    size_t pages = (size + 4095) / 4096;
    return dma_alloc_pages(pages, phys_out);
}

void dma_free(void *ptr, size_t size)
{
    if (!ptr || size == 0)
        return;
    size_t pages = (size + 4095) / 4096;
    dma_free_pages(ptr, pages);
}
