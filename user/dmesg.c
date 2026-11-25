/* dmesg — dump the kernel log ring buffer (TigxOS 1.3.8) */
#include "libc.h"

#define CHUNK 1024

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    char buf[CHUNK];
    size_t off = 0;
    for (;;) {
        long n = dmesg(buf, sizeof(buf) - 1, off);
        if (n <= 0)
            break;
        buf[n] = 0;
        write(STDOUT_FILENO, buf, (size_t)n);
        off += (size_t)n;
        if ((size_t)n < sizeof(buf) - 1)
            break;
    }
    return 0;
}
