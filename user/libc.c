/* user/libc.c — minimal libc for ring-3 programs (1.1.12) */
#include "libc.h"
#include <stdarg.h>

long syscall(long n, long a0, long a1, long a2) {
    long ret;
#if defined(__i386__)
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(n), "b"(a0), "c"(a1), "d"(a2)
        : "memory");
#else
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(n), "D"(a0), "S"(a1), "d"(a2)
        : "memory");
#endif
    return ret;
}

void _exit(int code) {
    syscall(SYS_EXIT, (long)code, 0, 0);
    for (;;)
        ;
}

void exit(int code) { _exit(code); }

long write(int fd, const void *buf, size_t n) {
    return syscall(SYS_WRITE, fd, (long)buf, (long)n);
}

long read(int fd, void *buf, size_t n) {
    return syscall(SYS_READ, fd, (long)buf, (long)n);
}

int open(const char *path, int flags) {
    return (int)syscall(SYS_OPEN, (long)path, flags, 0);
}

int chdir(const char *path) {
    return (int)syscall(SYS_CHDIR, (long)path, 0, 0);
}

int getcwd(char *buf, size_t size) {
    return (int)syscall(SYS_GETCWD, (long)buf, (long)size, 0);
}

int sysinfo(struct sys_sysinfo *info) {
    return (int)syscall(SYS_SYSINFO, (long)info, 0, 0);
}

long dmesg(char *buf, size_t len, size_t off) {
    return syscall(SYS_DMESG, (long)buf, (long)len, (long)off);
}

void poweroff(void) {
    syscall(SYS_POWEROFF, 0, 0, 0);
}

void reboot(void) {
    syscall(SYS_REBOOT, 0, 0, 0);
}

int psinfo(struct sys_psinfo *info) {
    return (int)syscall(SYS_PSINFO, (long)info, 0, 0);
}

void halt(void) {
    (void)syscall(SYS_HALT, 0, 0, 0);
    for (;;) ;
}

int close(int fd) {
    return (int)syscall(SYS_CLOSE, fd, 0, 0);
}

int dup(int oldfd) {
    return (int)syscall(SYS_DUP, oldfd, 0, 0);
}

int dup2(int oldfd, int newfd) {
    return (int)syscall(SYS_DUP2, oldfd, newfd, 0);
}

int pipe(int fd[2]) {
    return (int)syscall(SYS_PIPE, (long)fd, 0, 0);
}

int kill(int pid) {
    return (int)syscall(SYS_KILL, pid, 9, 0); /* SIGKILL */
}

int kill_sig(int pid, int sig) {
    return (int)syscall(SYS_KILL, pid, sig, 0);
}

int setfg(int pid) {
    return (int)syscall(SYS_SETFG, pid, 0, 0);
}

int getdents(void *buf, size_t n) {
    return (int)syscall(SYS_GETDENTS, (long)buf, (long)n, 0);
}

int unlink(const char *path) {
    return (int)syscall(SYS_UNLINK, (long)path, 0, 0);
}

int mkdir(const char *path) {
    return (int)syscall(SYS_MKDIR, (long)path, 0, 0);
}

int stat(const char *path, struct stat *st) {
    return (int)syscall(SYS_STAT, (long)path, (long)st, 0);
}

int spawn(const char *path, char *const argv[]) {
    return (int)syscall(SYS_SPAWN, (long)path, (long)argv, 0);
}

int spawn_env(const char *path, char *const argv[], char *const envp[]) {
    return (int)syscall(SYS_SPAWN, (long)path, (long)argv, (long)envp);
}

char **environ;

char *getenv(const char *name) {
    if (!name || !environ) return 0;
    size_t nlen = strlen(name);
    for (int i = 0; environ[i]; i++) {
        const char *e = environ[i];
        size_t j = 0;
        while (j < nlen && e[j] && e[j] == name[j]) j++;
        if (j == nlen && e[j] == '=')
            return (char *)(e + j + 1);
    }
    return 0;
}


int wait(int pid) {
    return (int)syscall(SYS_WAIT, pid, 0, 0);
}

int wait_noblock(int pid) {
    return (int)syscall(SYS_WAIT, pid, 1, 0);
}

int getpid(void) {
    return (int)syscall(SYS_GETPID, 0, 0, 0);
}

void yield(void) {
    syscall(SYS_YIELD, 0, 0, 0);
}

void sleep_ms(unsigned ms) {
    syscall(SYS_SLEEP_MS, (long)ms, 0, 0);
}

size_t strlen(const char *s) {
    size_t n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

char *strcpy(char *d, const char *s) {
    char *r = d;
    while ((*d++ = *s++))
        ;
    return r;
}

void *memset(void *d, int c, size_t n) {
    unsigned char *p = d;
    while (n--)
        *p++ = (unsigned char)c;
    return d;
}

void *memcpy(void *d, const void *s, size_t n) {
    unsigned char *dd = d;
    const unsigned char *ss = s;
    while (n--)
        *dd++ = *ss++;
    return d;
}

int putchar(int c) {
    char ch = (char)c;
    write(STDOUT_FILENO, &ch, 1);
    return c;
}

int puts(const char *s) {
    if (!s) s = "(null)";
    write(STDOUT_FILENO, s, strlen(s));
    putchar('\n');
    return 0;
}

/* Print an unsigned number with optional width, zero-pad and left-justify. */
static int print_num(unsigned long v, int base, int width, int zero, int left) {
    char buf[24];
    int i = 0, n = 0;
    if (v == 0) buf[i++] = '0';
    while (v) {
        int d = (int)(v % (unsigned)base);
        buf[i++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        v /= (unsigned)base;
    }
    int pad = width > i ? width - i : 0;
    if (!left)
        while (pad-- > 0) { putchar(zero ? '0' : ' '); n++; }
    else
        pad = pad > 0 ? pad : 0;
    while (i--) { putchar(buf[i]); n++; }
    if (left)
        while (pad-- > 0) { putchar(' '); n++; }
    return n;
}

static int print_sint(long v, int width, int zero, int left) {
    if (v < 0) {
        putchar('-');
        return 1 + print_num(-(unsigned long)v, 10, width > 1 ? width - 1 : 0, zero, left);
    }
    return print_num((unsigned long)v, 10, width, zero, left);
}

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = 0;
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            putchar(*fmt);
            n++;
            continue;
        }
        fmt++;
        if (!*fmt) break;

        /* flags: '-' left-justify, '0' zero-pad */
        int left = 0, zero = 0, width = 0, is_long = 0;
        for (;; fmt++) {
            if (*fmt == '-') left = 1;
            else if (*fmt == '0') zero = 1;
            else break;
        }
        /* width */
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }
        /* length modifier */
        if (*fmt == 'l') { is_long = 1; fmt++; }
        if (!*fmt) break;
        if (left) zero = 0;

        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int len = (int)strlen(s);
            int pad = width > len ? width - len : 0;
            if (!left) while (pad-- > 0) { putchar(' '); n++; }
            write(STDOUT_FILENO, s, (size_t)len);
            n += len;
            if (left) while (pad-- > 0) { putchar(' '); n++; }
            break;
        }
        case 'd': case 'i':
            n += print_sint(is_long ? va_arg(ap, long) : (long)va_arg(ap, int),
                            width, zero, left);
            break;
        case 'u':
            n += print_num(is_long ? va_arg(ap, unsigned long)
                                   : (unsigned long)va_arg(ap, unsigned),
                           10, width, zero, left);
            break;
        case 'x': case 'X':
            n += print_num(is_long ? va_arg(ap, unsigned long)
                                   : (unsigned long)va_arg(ap, unsigned),
                           16, width, zero, left);
            break;
        case 'p':
            putchar('0'); putchar('x');
            n += 2 + print_num((unsigned long)va_arg(ap, void *), 16, 0, 0, 0);
            break;
        case 'c': putchar(va_arg(ap, int)); n++; break;
        case '%': putchar('%'); n++; break;
        default:  putchar('%'); putchar(*fmt); n += 2; break;
        }
    }
    va_end(ap);
    return n;
}

/* ---- user heap (SYS_BRK) + malloc/free ---- */
void *sbrk(long incr) {
    long cur = syscall(SYS_BRK, 0, 0, 0);
    if (cur == -1)
        return (void *)-1;
    if (incr == 0)
        return (void *)cur;
    long next = cur + incr;
    /* overflow / underflow */
    if (incr > 0 && next < cur)
        return (void *)-1;
    if (incr < 0 && next > cur)
        return (void *)-1;
    if (next < 0)
        return (void *)-1;
    long prev = syscall(SYS_BRK, next, 0, 0);
    if (prev == -1)
        return (void *)-1;
    return (void *)prev;
}


/* Simple freelist allocator on top of sbrk. */
typedef struct mhdr {
    size_t size;           /* payload size */
    struct mhdr *next;     /* freelist */
    int free;
} mhdr_t;

static mhdr_t *free_list;

void *malloc(size_t n) {
    if (n == 0)
        n = 1;
    n = (n + 7) & ~(size_t)7; /* 8-byte align */
    mhdr_t *prev = 0, *c = free_list;
    while (c) {
        if (c->free && c->size >= n) {
            c->free = 0;
            if (prev)
                prev->next = c->next;
            else
                free_list = c->next;
            c->next = 0;
            return (char *)c + sizeof(mhdr_t);
        }
        prev = c;
        c = c->next;
    }
    /* sbrk new block */
    size_t total = sizeof(mhdr_t) + n;
    void *p = sbrk((long)total);
    if (p == (void *)-1)
        return 0;
    mhdr_t *h = (mhdr_t *)p;
    h->size = n;
    h->next = 0;
    h->free = 0;
    return (char *)h + sizeof(mhdr_t);
}

void free(void *ptr) {
    if (!ptr)
        return;
    mhdr_t *h = (mhdr_t *)((char *)ptr - sizeof(mhdr_t));
    if (h->free)
        return; /* double-free: ignore */
    h->free = 1;
    h->next = free_list;
    free_list = h;
}

void *realloc(void *ptr, size_t n) {
    if (!ptr)
        return malloc(n);
    if (n == 0) {
        free(ptr);
        return 0;
    }
    mhdr_t *h = (mhdr_t *)((char *)ptr - sizeof(mhdr_t));
    if (h->size >= n)
        return ptr;
    void *q = malloc(n);
    if (!q)
        return 0;
    size_t copy = h->size < n ? h->size : n;
    memcpy(q, ptr, copy);
    free(ptr);
    return q;
}

/* ---- green lizard string concat (malloc — safe for loop append) ----
 * Each call returns a fresh heap buffer. Caller may free() when done;
 * for typical glc scripts, process exit reclaims the heap via AS teardown.
 * 10_000-iteration `s = s + x` is safe.
 */
const char *str_concat(const char *a, const char *b) {
    if (!a) a = "";
    if (!b) b = "";
    size_t la = strlen(a);
    size_t lb = strlen(b);
    char *dst = (char *)malloc(la + lb + 1);
    if (!dst)
        return ""; /* OOM: return empty rather than crash */
    memcpy(dst, a, la);
    memcpy(dst + la, b, lb);
    dst[la + lb] = 0;
    return dst;
}

/* Bounds-safe s[i] for glc. OOB or NULL → 0 (no crash). */
long str_index(const char *s, int i) {
    if (!s || i < 0)
        return 0;
    int n = 0;
    while (s[n])
        n++;
    if (i >= n)
        return 0;
    return (long)(unsigned char)s[i];
}

/* Bounds-safe argv[i] for glc. OOB or NULL slot → "".
 * Silent empty is intentional (no crash); scripts that care should
 * check argc first. */
const char *argv_get(char **argv, int argc, int i) {
    if (!argv || i < 0 || i >= argc || !argv[i])
        return "";
    return argv[i];
}

/* ---- green lizard tool helpers ---- */
long readch(int fd) {
    unsigned char b;
    long n = read(fd, &b, 1);
    if (n <= 0)
        return -1;
    return (long)b;
}

long writech(int fd, int ch) {
    unsigned char b = (unsigned char)ch;
    long n = write(fd, &b, 1);
    return n == 1 ? 0 : -1;
}

void listdir(void) {
    char buf[2048];  /* ~64 dirents */
    int n = getdents(buf, sizeof(buf));
    if (n < 0) {
        puts("ls: getdents failed");
        return;
    }
    int off = 0;
    while (off + (int)sizeof(struct dirent) <= n) {
        struct dirent *d = (struct dirent *)(buf + off);
        if (d->name[0])
            printf("%s  %u\n", d->name, (unsigned)d->size);
        off += (int)sizeof(struct dirent);
    }
}
