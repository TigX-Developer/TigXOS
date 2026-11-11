/* embed_hello.h — embedded user ELFs seeded into TigFS at boot */
#ifndef EMBED_HELLO_H
#define EMBED_HELLO_H

#include <stddef.h>

struct embedded_elf {
    const char *name;
    const unsigned char *data;
    unsigned int len;
};

extern const struct embedded_elf embedded_elves[];
extern const unsigned int embedded_elves_count;

/* Back-compat: first entry is hello.elf */
extern const unsigned char *embedded_hello_elf;
extern const unsigned int embedded_hello_elf_len;

#endif
