/* elf.c — ELF32 / ELF64 ET_EXEC loader — 1.1.12 (hardened) */


#include "elf.h"
#include "aspace.h"
#include "memory.h"
#include "console.h"
#include "string.h"
#include "memory.h"
#include "fs.h"
#include <stdint.h>

#define EI_MAG0 0
#define EI_MAG1 1
#define EI_MAG2 2
#define EI_MAG3 3
#define EI_CLASS 4
#define EI_DATA  5

#define ELFCLASS32 1
#define ELFCLASS64 2
#define ELFDATA2LSB 1

#define ET_EXEC 2
#define PT_LOAD 1

#define EM_386    3
#define EM_X86_64 62

#define USER_LOAD_MIN  (0x400000UL)
#define USER_LOAD_MAX  (0x04000000UL)
#define PF_X 1
#define PF_W 2
#define PF_R 4


struct elf32_hdr {
    uint8_t  e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint32_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum;
    uint16_t e_shentsize, e_shnum, e_shstrndx;
} __attribute__((packed));

struct elf32_phdr {
    uint32_t p_type, p_offset, p_vaddr, p_paddr;
    uint32_t p_filesz, p_memsz, p_flags, p_align;
} __attribute__((packed));

struct elf64_hdr {
    uint8_t  e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum;
    uint16_t e_shentsize, e_shnum, e_shstrndx;
} __attribute__((packed));

struct elf64_phdr {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr;
    uint64_t p_filesz, p_memsz, p_align;
} __attribute__((packed));

static int check_magic(const uint8_t *id) {
    return id[EI_MAG0] == 0x7F && id[EI_MAG1] == 'E' &&
           id[EI_MAG2] == 'L' && id[EI_MAG3] == 'F';
}

static int load_segment(uintptr_t vaddr, const void *src, size_t filesz,
                        size_t memsz, struct address_space *as, uint64_t map_flags) {
    if (filesz > memsz) return -1;
    if (vaddr < USER_LOAD_MIN || vaddr + memsz < vaddr || vaddr + memsz > USER_LOAD_MAX)
        return -1;
    if (!as) {
        if (filesz) memcpy((void *)vaddr, src, filesz);
        if (memsz > filesz) memset((void *)(vaddr + filesz), 0, memsz - filesz);
        return 0;
    }
    uintptr_t endv = vaddr + memsz;
    uintptr_t page = vaddr & ~(PAGE_SIZE - 1);
    const uint8_t *srcp = (const uint8_t *)src;
    for (; page < endv; page += PAGE_SIZE) {
        void *frame = pmm_alloc_frame();
        if (!frame) return -1;
        memset(frame, 0, PAGE_SIZE);
        uintptr_t seg_off = (page > vaddr) ? (page - vaddr) : 0;
        uintptr_t page_off = (vaddr > page) ? (vaddr - page) : 0;
        if (seg_off < filesz) {
            size_t n = filesz - seg_off;
            size_t room = PAGE_SIZE - page_off;
            if (n > room) n = room;
            memcpy((uint8_t *)frame + page_off, srcp + seg_off, n);
        }
        if (aspace_map(as, page, (uintptr_t)frame, map_flags) != 0) {
            kprintf("elf: aspace_map failed virt=%p\n", (void *)page);
            pmm_free_frame(frame);
            return -1;
        }
    }
    return 0;
}

static int ranges_overlap(uintptr_t a0, uintptr_t a1, uintptr_t b0, uintptr_t b1) {
    return !(a1 <= b0 || a0 >= b1);
}

#if defined(__i386__)
static int load_elf32(const uint8_t *data, size_t len, struct elf_load_info *out,
                      struct address_space *as) {
    if (len < sizeof(struct elf32_hdr))
        return -1;
    const struct elf32_hdr *eh = (const struct elf32_hdr *)data;
    if (eh->e_ident[EI_CLASS] != ELFCLASS32 || eh->e_ident[EI_DATA] != ELFDATA2LSB)
        return -1;
    if (eh->e_type != ET_EXEC || eh->e_machine != EM_386) {
        kprintf("elf: need ET_EXEC EM_386\n");
        return -1;
    }
    if (eh->e_phentsize < sizeof(struct elf32_phdr) || eh->e_phnum == 0)
        return -1;
    if (eh->e_phnum > 1024)
        return -1;
    {
        uint32_t phsz = (uint32_t)eh->e_phnum * (uint32_t)eh->e_phentsize;
        if (eh->e_phentsize && phsz / eh->e_phentsize != eh->e_phnum)
            return -1;
        if (eh->e_phoff > len || phsz > len - eh->e_phoff)
            return -1;
    }

    uintptr_t lo = (uintptr_t)-1, hi = 0;
    uintptr_t seg_base[32], seg_end[32];
    int nseg = 0;
    uintptr_t entry = eh->e_entry;
    int entry_in_seg = 0;

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const struct elf32_phdr *ph =
            (const struct elf32_phdr *)(data + eh->e_phoff +
                                       (uint32_t)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD)
            continue;
        if (nseg >= 32) {
            kprintf("elf: too many PT_LOAD segments (max 32)\n");
            return -1;
        }
        if (ph->p_filesz > ph->p_memsz) {
            kprintf("elf: PHDR %u filesz > memsz\n", i);
            return -1;
        }
        if (ph->p_offset > len || ph->p_filesz > len - ph->p_offset) {
            kprintf("elf: PHDR %u filesz past EOF\n", i);
            return -1;
        }
        /* ld may set p_align to 2MB while vaddr is only 4K-aligned — require page align */
        if ((ph->p_vaddr & (PAGE_SIZE - 1)) != 0) {
            kprintf("elf: PHDR %u vaddr %p not page-aligned\n", i, (void *)(uintptr_t)ph->p_vaddr);
            return -1;
        }
        uintptr_t s0 = ph->p_vaddr, s1 = ph->p_vaddr + ph->p_memsz;
        for (int j = 0; j < nseg; j++) {
            if (ranges_overlap(s0, s1, seg_base[j], seg_end[j])) {
                kprintf("elf: PHDR %u overlaps earlier segment\n", i);
                return -1;
            }
        }
        seg_base[nseg] = s0;
        seg_end[nseg] = s1;
        nseg++;
        if (entry >= s0 && entry < s1)
            entry_in_seg = 1;
        {
            uint64_t mf = 0;
            if (ph->p_flags & PF_W) mf |= PAGE_WRITE;
            if (!(ph->p_flags & PF_X)) mf |= PAGE_NX;
            if (load_segment(ph->p_vaddr, data + ph->p_offset,
                         ph->p_filesz, ph->p_memsz, as, mf) != 0)
                return -1;
        }
        if (s0 < lo) lo = s0;
        if (s1 > hi) hi = s1;
    }

    out->load_min = (lo == (uintptr_t)-1) ? 0 : lo;
    out->load_max = hi;
    out->brk = (hi + 0xFFFUL) & ~0xFFFUL;
    out->entry = entry;
    if (!entry_in_seg ||
        out->entry < USER_LOAD_MIN || out->entry >= USER_LOAD_MAX) {
        kprintf("elf: entry %p not inside any PT_LOAD segment\n",
                (void *)out->entry);
        return -1;
    }
    return 0;
}
#else
static int load_elf64(const uint8_t *data, size_t len, struct elf_load_info *out,
                      struct address_space *as) {
    if (len < sizeof(struct elf64_hdr))
        return -1;
    const struct elf64_hdr *eh = (const struct elf64_hdr *)data;
    if (eh->e_ident[EI_CLASS] != ELFCLASS64 || eh->e_ident[EI_DATA] != ELFDATA2LSB)
        return -1;
    if (eh->e_type != ET_EXEC || eh->e_machine != EM_X86_64) {
        kprintf("elf: need ET_EXEC EM_X86_64\n");
        return -1;
    }
    if (eh->e_phentsize < sizeof(struct elf64_phdr) || eh->e_phnum == 0)
        return -1;
    if (eh->e_phnum > 1024)
        return -1;
    {
        uint64_t phsz = (uint64_t)eh->e_phnum * (uint64_t)eh->e_phentsize;
        if (eh->e_phentsize && phsz / eh->e_phentsize != eh->e_phnum)
            return -1;
        if (eh->e_phoff > len || phsz > len - eh->e_phoff)
            return -1;
    }

    uintptr_t lo = (uintptr_t)-1, hi = 0;
    uintptr_t seg_base[32], seg_end[32];
    int nseg = 0;
    uintptr_t entry = (uintptr_t)eh->e_entry;
    int entry_in_seg = 0;

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const struct elf64_phdr *ph =
            (const struct elf64_phdr *)(data + eh->e_phoff +
                                       (uint64_t)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD)
            continue;
        if (nseg >= 32) {
            kprintf("elf: too many PT_LOAD segments (max 32)\n");
            return -1;
        }
        if (ph->p_filesz > ph->p_memsz) {
            kprintf("elf: PHDR %u filesz > memsz\n", i);
            return -1;
        }
        if (ph->p_offset > len || ph->p_filesz > len - ph->p_offset) {
            kprintf("elf: PHDR %u filesz past EOF\n", i);
            return -1;
        }
        if ((ph->p_vaddr & (PAGE_SIZE - 1)) != 0) {
            kprintf("elf: PHDR %u vaddr %p not page-aligned\n",
                    i, (void *)(uintptr_t)ph->p_vaddr);
            return -1;
        }
        uintptr_t s0 = (uintptr_t)ph->p_vaddr;
        uintptr_t s1 = s0 + (uintptr_t)ph->p_memsz;
        for (int j = 0; j < nseg; j++) {
            if (ranges_overlap(s0, s1, seg_base[j], seg_end[j])) {
                kprintf("elf: PHDR %u overlaps earlier segment\n", i);
                return -1;
            }
        }
        seg_base[nseg] = s0;
        seg_end[nseg] = s1;
        nseg++;
        if (entry >= s0 && entry < s1)
            entry_in_seg = 1;
        {
            uint64_t mf = 0;
            if (ph->p_flags & PF_W) mf |= PAGE_WRITE;
            if (!(ph->p_flags & PF_X)) mf |= PAGE_NX;
            if (load_segment(s0, data + ph->p_offset,
                         (size_t)ph->p_filesz, (size_t)ph->p_memsz, as, mf) != 0)
                return -1;
        }
        if (s0 < lo) lo = s0;
        if (s1 > hi) hi = s1;
    }

    out->load_min = (lo == (uintptr_t)-1) ? 0 : lo;
    out->load_max = hi;
    out->brk = (hi + 0xFFFUL) & ~0xFFFUL;
    out->entry = entry;
    if (!entry_in_seg ||
        out->entry < USER_LOAD_MIN || out->entry >= USER_LOAD_MAX) {
        kprintf("elf: entry %p not inside any PT_LOAD segment\n",
                (void *)out->entry);
        return -1;
    }
    return 0;
}
#endif

int elf_load_image(const void *data, size_t len, struct elf_load_info *out,
                   struct address_space *as) {
    if (!data || !out || len < 16)
        return -1;
    const uint8_t *id = (const uint8_t *)data;
    if (!check_magic(id)) {
        kprintf("elf: bad magic\n");
        return -1;
    }
    memset(out, 0, sizeof(*out));
#if defined(__i386__)
    return load_elf32(id, len, out, as);
#else
    return load_elf64(id, len, out, as);
#endif
}

int elf_load_file(const char *path, struct elf_load_info *out,
                  struct address_space *as) {
    if (!path || !out || !fs_ready()) {
        kprintf("elf: load_file bad args or fs not ready\n");
        return -1;
    }
    int sz = fs_size(path);
    if (sz <= 0) {
        /* Silent on missing — PATH search probes many candidates. */
        return -1;
    }
    void *buf = kmalloc((size_t)sz);
    if (!buf) {
        kprintf("elf: kmalloc %d failed for '%s'\n", sz, path);
        return -1;
    }
    int n = fs_read(path, buf, (uint32_t)sz);
    if (n != sz) {
        kfree(buf);
        kprintf("elf: short read on '%s' (%d/%d)\n", path, n, sz);
        return -1;
    }
    int rc = elf_load_image(buf, (size_t)sz, out, as);
    if (rc != 0)
        kprintf("elf: load_image failed for '%s' (%d bytes)\n", path, sz);
    kfree(buf);
    return rc;
}
