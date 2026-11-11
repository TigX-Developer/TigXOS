#ifndef ELF_H
#define ELF_H
#include <stdint.h>
#include <stddef.h>
struct address_space;
struct elf_load_info {
    uintptr_t entry, brk, load_min, load_max;
};
int elf_load_image(const void *data, size_t len, struct elf_load_info *out, struct address_space *as);
int elf_load_file(const char *path, struct elf_load_info *out, struct address_space *as);
#endif
