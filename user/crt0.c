/* user/crt0.c — process entry: stack -> main(argc, argv) -> exit
 * Layout: [argc][argv...][NULL][envp...][NULL][strings]
 */
#include "libc.h"
#include <stdint.h>

extern int main(int argc, char **argv);

extern char **environ;

void _start_c(uintptr_t *sp) __attribute__((noreturn, used));

void _start_c(uintptr_t *sp) {
    int argc = (int)sp[0];
    char **argv = (char **)&sp[1];
    /* envp starts after argv NULL */
    char **envp = argv + argc + 1;
    environ = envp;
    exit(main(argc, argv));
    for (;;)
        ;
}

#if defined(__i386__)
__asm__(
    ".text\n"
    ".global _start\n"
    "_start:\n"
    "    xorl %ebp, %ebp\n"
    "    movl %esp, %eax\n"
    "    andl $-16, %esp\n"
    "    subl $12, %esp\n"
    "    pushl %eax\n"
    "    call _start_c\n"
    "    hlt\n"
);
#else
__asm__(
    ".text\n"
    ".global _start\n"
    "_start:\n"
    "    xorl %ebp, %ebp\n"
    "    movq %rsp, %rdi\n"
    "    andq $-16, %rsp\n"
    "    call _start_c\n"
    "    hlt\n"
);
#endif
