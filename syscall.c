/* syscall.c — int 0x80 dispatcher V1.0.63 */
#include "syscall.h"
#include "interrupt.h"
#include "console.h"
#include "process.h"
#include "filetable.h"
#include "timer.h"
#include "keyboard.h"
#include "fs.h"
#include "pipe.h"
#include "aspace.h"
#include "string.h"
#include "memory.h"
#include "arch/arch.h"
#include <stdint.h>

#define USER_VADDR_MIN  0x400000UL
#define USER_VADDR_MAX  0x04000000UL  /* 64MB */

static int user_range_ok(uintptr_t addr, size_t len) {
    if (len == 0)
        return 1;
    struct process *cur = process_current();
    if (!cur || !cur->as)
        return 0; /* no process AS → refuse user memory access */
    return aspace_user_range_ok(cur->as, addr, len);
}

static int user_range_writable(uintptr_t addr, size_t len) {
    if (len == 0)
        return 1;
    struct process *cur = process_current();
    if (!cur || !cur->as)
        return 0;
    return aspace_user_range_writable(cur->as, addr, len);
}

/* Safe byte fetch from user via process AS phys map. */
static int user_peek_byte(uintptr_t addr, char *out) {
    if (!user_range_ok(addr, 1))
        return -1;
    struct process *cur = process_current();
    uintptr_t phys = aspace_virt_to_phys(cur->as, addr);
    if (!phys)
        return -1;
    *out = *(volatile char *)phys;
    return 0;
}

static int user_poke_byte(uintptr_t addr, char c) {
    if (!user_range_writable(addr, 1))
        return -1;
    struct process *cur = process_current();
    uintptr_t phys = aspace_virt_to_phys(cur->as, addr);
    if (!phys)
        return -1;
    *(volatile char *)phys = c;
    return 0;
}

/* Copy user → kernel via phys pages (page-chunked). */
static int copy_from_user(void *kdst, uintptr_t usrc, size_t len) {
    if (!user_range_ok(usrc, len))
        return -1;
    struct process *cur = process_current();
    uint8_t *d = (uint8_t *)kdst;
    size_t off = 0;
    while (off < len) {
        uintptr_t va = usrc + off;
        uintptr_t phys = aspace_virt_to_phys(cur->as, va);
        if (!phys)
            return -1;
        size_t page_left = PAGE_SIZE - (va & (PAGE_SIZE - 1));
        size_t n = len - off;
        if (n > page_left) n = page_left;
        memcpy(d + off, (const void *)phys, n);
        off += n;
    }
    return 0;
}

/* Copy kernel → user via phys pages (page-chunked). */
static int copy_to_user(uintptr_t udst, const void *ksrc, size_t len) {
    if (!user_range_writable(udst, len))
        return -1;
    struct process *cur = process_current();
    const uint8_t *s = (const uint8_t *)ksrc;
    size_t off = 0;
    while (off < len) {
        uintptr_t va = udst + off;
        uintptr_t phys = aspace_virt_to_phys(cur->as, va);
        if (!phys)
            return -1;
        size_t page_left = PAGE_SIZE - (va & (PAGE_SIZE - 1));
        size_t n = len - off;
        if (n > page_left) n = page_left;
        memcpy((void *)phys, s + off, n);
        off += n;
    }
    return 0;
}

/* Cap file I/O buffer vs TigFS max file size */
#define TIGFS_MAX_FILE (TIGFS_DIRECT * 512u)

/* Serialize concurrent sys_spawn static buffers */
static int spawn_busy;

/* Copy a NUL-terminated path from user memory into kernel buffer. */
static int copy_user_path(char *dst, size_t dst_len, uintptr_t user_ptr) {
    if (!dst || dst_len == 0 || !user_ptr)
        return -1;
    size_t i = 0;
    while (i + 1 < dst_len) {
        char c;
        if (user_peek_byte(user_ptr + i, &c) != 0)
            return -1;
        dst[i] = c;
        if (c == 0)
            return 0;
        i++;
    }
    dst[dst_len - 1] = 0;
    return -1; /* too long */
}

static long sys_write(struct process *cur, long fd, uintptr_t buf, size_t len) {
    if (!cur || fd < 0 || fd >= MAX_FDS)
        return -1;
    if (len && !user_range_ok(buf, len))
        return -1;
    struct open_file *o = of_get(cur->fds[fd].of_id);
    if (!o)
        return -1;
    if (o->type == OF_CONSOLE) {
        /* Kernel bounce buffer — avoids per-byte peek edge cases */
        char kbuf[256];
        size_t done = 0;
        while (done < len) {
            size_t chunk = len - done;
            if (chunk > sizeof(kbuf)) chunk = sizeof(kbuf);
            if (copy_from_user(kbuf, buf + done, chunk) != 0)
                return (long)done;
            for (size_t i = 0; i < chunk; i++)
                console_putchar(kbuf[i]);
            done += chunk;
        }
        return (long)len;
    }
    if (o->type == OF_FILE) {
        int acc = o->flags & 3;
        if (acc == O_RDONLY)
            return -1;
        int sz = fs_size(o->name);
        if (sz < 0)
            sz = 0;
        if (o->flags & O_APPEND)
            o->offset = (uint32_t)sz;
        if (o->offset > (uint32_t)sz)
            o->offset = (uint32_t)sz;
        uint32_t end_off = o->offset + (uint32_t)len;
        if (end_off < o->offset)
            return -1;
        uint32_t final_len = end_off > (uint32_t)sz ? end_off : (uint32_t)sz;
        if (final_len > TIGFS_MAX_FILE)
            return -1;
        char *tmp = kmalloc(final_len ? final_len : 1);
        if (!tmp)
            return -1;
        if (sz > 0) {
            if (fs_read(o->name, tmp, (uint32_t)sz) != sz) {
                kfree(tmp);
                return -1;
            }
        }
        if (final_len > (uint32_t)sz)
            memset(tmp + sz, 0, final_len - (uint32_t)sz);
        if (len && copy_from_user(tmp + o->offset, buf, len) != 0) {
            kfree(tmp);
            return -1;
        }
        if (fs_write(o->name, tmp, final_len) != 0) {
            kfree(tmp);
            return -1;
        }
        kfree(tmp);
        o->offset += (uint32_t)len;
        return (long)len;
    }
    if (o->type == OF_PIPE) {
        if (len > 4096) len = 4096; /* stack-friendly chunk */
        char kbuf[4096];
        if (len && copy_from_user(kbuf, buf, len) != 0)
            return -1;
        return pipe_write(o->pipe_id, kbuf, len);
    }
    return -1;
}


static long sys_read(struct process *cur, long fd, uintptr_t buf, size_t len) {
    if (!cur || fd < 0 || fd >= MAX_FDS)
        return -1;
    if (len == 0)
        return 0;
    if (!user_range_writable(buf, len))
        return -1;
    struct open_file *o = of_get(cur->fds[fd].of_id);
    if (!o)
        return -1;
    if (o->type == OF_CONSOLE) {
        size_t n = 0;
        while (n < len) {
            char c = keyboard_getchar();
            /* Block happens above; now deliver to user */
            if (user_poke_byte(buf + n, c) != 0)
                return (long)n;
            n++;
            if (c == '\n' || c == '\r')
                break;
        }
        return (long)n;
    }
    if (o->type == OF_FILE) {
        int acc = o->flags & 3;
        if (acc == O_WRONLY)
            return -1;
        int sz = fs_size(o->name);
        if (sz < 0)
            return 0;
        if (o->offset >= (uint32_t)sz)
            return 0;
        uint32_t remain = (uint32_t)sz - o->offset;
        uint32_t take = remain < (uint32_t)len ? remain : (uint32_t)len;
        if ((uint32_t)sz > TIGFS_MAX_FILE)
            return -1;
        char *tmp = kmalloc((uint32_t)sz ? (uint32_t)sz : 1);
        if (!tmp)
            return -1;
        if (fs_read(o->name, tmp, (uint32_t)sz) != sz) {
            kfree(tmp);
            return -1;
        }
        if (copy_to_user(buf, tmp + o->offset, take) != 0) {
            kfree(tmp);
            return -1;
        }
        o->offset += take;
        kfree(tmp);
        return (long)take;
    }
    if (o->type == OF_PIPE) {
        size_t chunk = len > 4096 ? 4096 : len;
        char kbuf[4096];
        long n = pipe_read(o->pipe_id, kbuf, chunk);
        if (n <= 0)
            return n;
        if (copy_to_user(buf, kbuf, (size_t)n) != 0)
            return -1;
        return n;
    }
    return -1;
}

static long sys_open(struct process *cur, uintptr_t pathu, long flags) {
    if (!cur)
        return -1;
    char upath[PATH_MAX];
    if (copy_user_path(upath, sizeof(upath), pathu) != 0)
        return -1;
    char resolved[PATH_MAX];
    if (fs_resolve_path(cur->cwd, upath, resolved, sizeof(resolved)) != 0)
        return -1;
    const char *name = resolved;
    if (name[0] == '/' && name[1])
        name++;
    if (!name[0])
        return -1; /* pure root not a file */
    /* TigFS name limit */
    {
        size_t nlen = 0;
        while (name[nlen])
            nlen++;
        if (nlen >= TIGFS_NAME_LEN)
            return -1;
    }

    int need_create = (flags & O_CREAT) != 0;
    int acc = flags & 3;
    if (acc > O_RDWR)
        return -1;

    int sz = fs_size(name);
    if (sz < 0) {
        if (!need_create)
            return -1;
        if (fs_create(name) != 0)
            return -1;
        sz = 0;
    }
    if ((flags & O_TRUNC) && acc != O_RDONLY) {
        if (fs_write(name, "", 0) != 0)
            return -1;
        sz = 0;
    }

    int of = of_alloc();
    if (of < 0)
        return -1;
    struct open_file *o = of_get(of);
    if (!o) {
        of_decref(of);
        return -1;
    }
    o->type = OF_FILE;
    o->flags = (int)flags;
    o->offset = (flags & O_APPEND) ? (uint32_t)sz : 0;
    memset(o->name, 0, sizeof(o->name));
    strncpy(o->name, name, TIGFS_NAME_LEN - 1);

    int fd = process_alloc_fd(cur);
    if (fd < 0) {
        of_decref(of);
        return -1;
    }
    cur->fds[fd].of_id = of;
    return fd;
}

static long sys_close(struct process *cur, long fd) {
    if (!cur || fd < 0 || fd >= MAX_FDS)
        return -1;
    if (cur->fds[fd].of_id < 0)
        return -1;
    fd_close(cur, (int)fd);
    return 0;
}

static long sys_dup(struct process *cur, long oldfd) {
    if (!cur || oldfd < 0 || oldfd >= MAX_FDS)
        return -1;
    int oid = cur->fds[oldfd].of_id;
    if (oid < 0)
        return -1;
    int nfd = process_alloc_fd(cur);
    if (nfd < 0)
        return -1;
    cur->fds[nfd].of_id = oid;
    of_incref(oid);
    return nfd;
}

static long sys_dup2(struct process *cur, long oldfd, long newfd) {
    if (!cur || oldfd < 0 || oldfd >= MAX_FDS || newfd < 0 || newfd >= MAX_FDS)
        return -1;
    int oid = cur->fds[oldfd].of_id;
    if (oid < 0)
        return -1;
    if (oldfd == newfd)
        return newfd;
    if (cur->fds[newfd].of_id >= 0)
        fd_close(cur, (int)newfd);
    cur->fds[newfd].of_id = oid;
    of_incref(oid);
    return newfd;
}

static long sys_pipe(struct process *cur, uintptr_t pipefdu) {
    if (!cur || !user_range_writable(pipefdu, 2 * sizeof(int)))
        return -1;
    int rfd = process_alloc_fd(cur);
    if (rfd < 0)
        return -1;
    int of_r = of_alloc();
    if (of_r < 0)
        return -1;
    cur->fds[rfd].of_id = of_r;

    int wfd = process_alloc_fd(cur);
    if (wfd < 0) {
        cur->fds[rfd].of_id = -1;
        of_decref(of_r);
        return -1;
    }
    int of_w = of_alloc();
    if (of_w < 0) {
        cur->fds[rfd].of_id = -1;
        of_decref(of_r);
        return -1;
    }
    cur->fds[wfd].of_id = of_w; /* claim slot early */

    int pid_ = pipe_alloc();
    if (pid_ < 0) {
        cur->fds[rfd].of_id = -1;
        cur->fds[wfd].of_id = -1;
        of_decref(of_r);
        of_decref(of_w);
        return -1;
    }
    struct open_file *or_ = of_get(of_r);
    struct open_file *ow = of_get(of_w);
    if (!or_ || !ow) {
        /* free pipe slot: drop both ends */
        extern void pipe_decref(int pipe_id, int is_write);
        pipe_decref(pid_, 0);
        pipe_decref(pid_, 1);
        cur->fds[rfd].of_id = -1;
        cur->fds[wfd].of_id = -1;
        of_decref(of_r);
        of_decref(of_w);
        return -1;
    }
    or_->type = OF_PIPE;
    or_->flags = 0; /* O_RDONLY */
    or_->pipe_id = pid_;
    ow->type = OF_PIPE;
    ow->flags = 1; /* O_WRONLY */
    ow->pipe_id = pid_;
    int fds[2] = { rfd, wfd };
    if (copy_to_user(pipefdu, fds, sizeof(fds)) != 0) {
        cur->fds[rfd].of_id = -1;
        cur->fds[wfd].of_id = -1;
        of_decref(of_r);
        of_decref(of_w);
        return -1;
    }
    return 0;
}



static long sys_getdents(struct process *cur, uintptr_t buf, size_t len) {
    (void)cur;
    if (!user_range_writable(buf, len))
        return -1;
    if (len > 4096) len = 4096;
    char kbuf[4096];
    int n = fs_getdents(kbuf, (uint32_t)len);
    if (n < 0)
        return -1;
    if (n > 0 && copy_to_user(buf, kbuf, (size_t)n) != 0)
        return -1;
    return n;
}



static long sys_unlink(struct process *cur, uintptr_t pathu) {
    if (!cur)
        return -1;
    char upath[PATH_MAX];
    if (copy_user_path(upath, sizeof(upath), pathu) != 0)
        return -1;
    char resolved[PATH_MAX];
    if (fs_resolve_path(cur->cwd, upath, resolved, sizeof(resolved)) != 0)
        return -1;
    const char *name = resolved;
    if (name[0] == '/' && name[1])
        name++;
    if (!name[0])
        return -1;
    {
        size_t nlen = 0;
        while (name[nlen]) nlen++;
        if (nlen >= TIGFS_NAME_LEN)
            return -1;
    }
    if (of_mark_unlink(name))
        return 0; /* stay until last close */
    return fs_remove(name) == 0 ? 0 : -1;
}

static long sys_stat(struct process *cur, uintptr_t pathu, uintptr_t st_u) {
    if (!cur)
        return -1;
    if (!user_range_writable(st_u, sizeof(struct sys_stat)))
        return -1;
    char upath[PATH_MAX];
    if (copy_user_path(upath, sizeof(upath), pathu) != 0)
        return -1;
    char resolved[PATH_MAX];
    if (fs_resolve_path(cur->cwd, upath, resolved, sizeof(resolved)) != 0)
        return -1;
    const char *name = resolved;
    if (name[0] == '/' && name[1])
        name++;
    if (!name[0])
        return -1;
    {
        size_t nlen = 0;
        while (name[nlen]) nlen++;
        if (nlen >= TIGFS_NAME_LEN)
            return -1;
    }
    int sz = fs_visible_size(name);
    if (sz < 0)
        return -1;
    struct sys_stat st;
    st.size = (uint32_t)sz;
    st.mode = 1;
    if (copy_to_user(st_u, &st, sizeof(st)) != 0)
        return -1;
    return 0;
}


/* Limits for user argv copy (kernel-side staging before setup_user_argv). */
#define SPAWN_MAX_ARGS   16
#define SPAWN_MAX_ARG_LEN 128
#define SPAWN_ARG_BUF    (SPAWN_MAX_ARGS * SPAWN_MAX_ARG_LEN)

/*
 * Copy a NULL-terminated argv vector from user memory into kernel buffers.
 * On success, *out_argv points at a static-duration array of pointers into
 * *string_store (valid until the next sys_spawn). Returns argc, or -1.
 */
static int copy_user_argv(uintptr_t argvu, char *string_store, size_t store_sz,
                          char **out_argv, int max_args)
{
    if (!argvu) {
        out_argv[0] = 0;
        return 0;
    }
    size_t store_used = 0;
    int argc = 0;
    for (;;) {
        if (argc >= max_args)
            return -1;
        /* each argv[i] slot must lie in user range before dereference */
#if defined(__i386__)
        size_t slot_sz = 4;
#else
        size_t slot_sz = 8;
#endif
        uintptr_t slot = argvu + (uintptr_t)argc * slot_sz;
        if (!user_range_ok(slot, slot_sz))
            return -1;
        uintptr_t uptr = 0;
        {
            for (size_t b = 0; b < slot_sz; b++) {
                char c;
                if (user_peek_byte(slot + b, &c) != 0)
                    return -1;
                uptr |= ((uintptr_t)(uint8_t)c) << (8 * b);
            }
        }
        if (uptr == 0)
            break;
        /* copy string byte-by-byte via mapped phys */
        size_t left = store_sz - store_used;
        if (left < 2)
            return -1;
        char *dst = string_store + store_used;
        size_t n = 0;
        while (n + 1 < left && n + 1 < SPAWN_MAX_ARG_LEN) {
            char c;
            if (user_peek_byte(uptr + n, &c) != 0)
                return -1;
            dst[n] = c;
            if (c == 0)
                break;
            n++;
        }
        if (dst[n] != 0) {
            /* stopped without seeing NUL — too long */
            return -1;
        }
        out_argv[argc] = dst;
        store_used += n + 1;
        argc++;
    }
    out_argv[argc] = 0;
    return argc;
}


static long sys_kill(struct process *cur, long pid, long sig) {
    (void)cur;
    if (pid <= 0)
        return -1;
    int s = (int)sig;
    if (s != 2 && s != 9 && s != 15)
        s = 9;
    return process_kill((int)pid, 128 + s);
}


/* SYS_BRK: set process break. arg 0 = new_brk (0 queries current).
 * Returns previous break, or (long)-1 on error.
 * Heap lives at USER_HEAP_BASE .. USER_HEAP_MAX; pages mapped on demand.
 */
static long sys_brk(struct process *cur, uintptr_t new_brk) {
    if (!cur || !cur->as || !cur->is_user)
        return -1;
    if (cur->heap_brk == 0)
        cur->heap_brk = USER_HEAP_BASE;
    uintptr_t old = cur->heap_brk;
    if (new_brk == 0)
        return (long)old;
    if (new_brk < USER_HEAP_BASE || new_brk > USER_HEAP_MAX)
        return -1;
    /* Grow: map any missing pages covering [old, new_brk) */
    if (new_brk > old) {
        uintptr_t v = old & ~(PAGE_SIZE - 1);
        for (; v < new_brk; v += PAGE_SIZE) {
            if (aspace_virt_to_phys(cur->as, v))
                continue;
            void *frame = pmm_alloc_frame();
            if (!frame)
                return -1;
            memset(frame, 0, PAGE_SIZE);
            if (aspace_map(cur->as, v, (uintptr_t)frame, PAGE_WRITE | PAGE_NX) != 0) {
                pmm_free_frame(frame);
                return -1;
            }
        }
    }
    /* Shrink: leave pages mapped (simple); just move brk down */
    cur->heap_brk = new_brk;
    return (long)old;
}

static long sys_chdir(struct process *cur, uintptr_t pathu) {
    if (!cur)
        return -1;
    char upath[PATH_MAX];
    if (copy_user_path(upath, sizeof(upath), pathu) != 0)
        return -1;
    char resolved[PATH_MAX];
    if (fs_resolve_path(cur->cwd, upath, resolved, sizeof(resolved)) != 0)
        return -1;
    /* For flat FS we accept any path string as cwd (no real dir check yet).
     * Keep it absolute and within PATH_MAX. */
    strncpy(cur->cwd, resolved, PATH_MAX - 1);
    cur->cwd[PATH_MAX - 1] = '\0';
    return 0;
}

static long sys_getcwd(struct process *cur, uintptr_t bufu, size_t size) {
    if (!cur || size == 0)
        return -1;
    size_t len = 0;
    while (cur->cwd[len] && len < PATH_MAX)
        len++;
    if (len + 1 > size)
        return -1;
    if (!user_range_writable(bufu, len + 1))
        return -1;
    if (copy_to_user(bufu, cur->cwd, len + 1) != 0)
        return -1;
    return 0;
}

static long sys_setfg(struct process *cur, long pid) {
    (void)cur;
    if (pid == 0) {
        process_fg_clear();
        return 0;
    }
    return process_fg_add((int)pid);
}

static long sys_spawn(struct process *cur, uintptr_t pathu, uintptr_t argvu,
                       uintptr_t envpu) {
    if (!cur)
        return -1;
    char upath[PATH_MAX];
    if (copy_user_path(upath, sizeof(upath), pathu) != 0)
        return -1;
    char resolved[PATH_MAX];
    if (fs_resolve_path(cur->cwd, upath, resolved, sizeof(resolved)) != 0)
        return -1;
    const char *name = resolved;
    if (name[0] == '/' && name[1])
        name++;
    if (!name[0])
        return -1;
    {
        size_t nlen = 0;
        while (name[nlen]) nlen++;
        if (nlen >= TIGFS_NAME_LEN)
            return -1;
    }

    /* Serialize static staging buffers against concurrent spawn */
    __asm__ volatile("cli");
    while (spawn_busy) {
        __asm__ volatile("sti");
        process_yield();
        __asm__ volatile("cli");
    }
    spawn_busy = 1;
    __asm__ volatile("sti");

    static char arg_store[SPAWN_ARG_BUF];
    static char *kargv[SPAWN_MAX_ARGS + 1];
    int argc = copy_user_argv(argvu, arg_store, sizeof(arg_store),
                              kargv, SPAWN_MAX_ARGS);
    if (argc < 0) {
        __asm__ volatile("cli");
        spawn_busy = 0;
        __asm__ volatile("sti");
        return -1;
    }

    static char env_store[SPAWN_ARG_BUF];
    static char *kenvp[SPAWN_MAX_ARGS + 1];
    int envc = 0;
    if (envpu) {
        envc = copy_user_argv(envpu, env_store, sizeof(env_store),
                              kenvp, SPAWN_MAX_ARGS);
        if (envc < 0) {
            __asm__ volatile("cli");
            spawn_busy = 0;
            __asm__ volatile("sti");
            return -1;
        }
    } else {
        kenvp[0] = 0;
    }

    int pid = spawn(name, argc > 0 ? kargv : NULL,
                    envc > 0 ? kenvp : NULL);
    if (pid > 0 && cur)
        process_set_parent(process_lookup_pid(pid), cur->pid);
    __asm__ volatile("cli");
    spawn_busy = 0;
    __asm__ volatile("sti");
    return pid;
}

static void syscall_dispatch(struct interrupt_frame *frame) {
    struct process *cur = process_current();

#if defined(__i386__)
    uint32_t num = frame->eax;
    uint32_t a0  = frame->ebx;
    uint32_t a1  = frame->ecx;
    uint32_t a2  = frame->edx;
    uint32_t ret = (uint32_t)-1;

    switch (num) {
    case SYS_EXIT:
        process_exit((int)a0);
        ret = 0;
        break;
    case SYS_WRITE:
        ret = (uint32_t)sys_write(cur, (long)a0, (uintptr_t)a1, (size_t)a2);
        break;
    case SYS_YIELD:
        process_yield();
        ret = 0;
        break;
    case SYS_GETPID:
        ret = cur ? (uint32_t)cur->pid : (uint32_t)-1;
        break;
    case SYS_SLEEP_MS:
        timer_sleep_ms(a0);
        ret = 0;
        break;
    case SYS_READ:
        ret = (uint32_t)sys_read(cur, (long)a0, (uintptr_t)a1, (size_t)a2);
        break;
    case SYS_OPEN:
        ret = (uint32_t)sys_open(cur, (uintptr_t)a0, (long)a1);
        break;
    case SYS_CLOSE:
        ret = (uint32_t)sys_close(cur, (long)a0);
        break;
    case SYS_GETDENTS:
        ret = (uint32_t)sys_getdents(cur, (uintptr_t)a0, (size_t)a1);
        break;
    case SYS_SPAWN:
        ret = (uint32_t)sys_spawn(cur, (uintptr_t)a0, (uintptr_t)a1, (uintptr_t)a2);
        break;
    case SYS_WAIT:
        if ((int)a1)
            ret = (uint32_t)process_try_wait((int)a0);
        else
            ret = (uint32_t)process_wait((int)a0);
        break;
    case SYS_UNLINK:
        ret = (uint32_t)sys_unlink(cur, (uintptr_t)a0);
        break;
    case SYS_STAT:
        ret = (uint32_t)sys_stat(cur, (uintptr_t)a0, (uintptr_t)a1);
        break;
    case SYS_DUP:
        ret = (uint32_t)sys_dup(cur, (long)a0);
        break;
    case SYS_DUP2:
        ret = (uint32_t)sys_dup2(cur, (long)a0, (long)a1);
        break;
    case SYS_PIPE:
        ret = (uint32_t)sys_pipe(cur, (uintptr_t)a0);
        break;
    case SYS_KILL:
        ret = (uint32_t)sys_kill(cur, (long)a0, (long)a1);
        break;
    case SYS_SETFG:
        ret = (uint32_t)sys_setfg(cur, (long)a0);
        break;
    case SYS_BRK:
        ret = (uint32_t)sys_brk(cur, (uintptr_t)a0);
        break;
    case SYS_CHDIR:
        ret = (uint32_t)sys_chdir(cur, (uintptr_t)a0);
        break;
    case SYS_GETCWD:
        ret = (uint32_t)sys_getcwd(cur, (uintptr_t)a0, (size_t)a1);
        break;
    default:
        kprintf("syscall: unknown %u\n", (unsigned)num);
        ret = (uint32_t)-1;
        break;
    }
    frame->eax = ret;
#else
    uint64_t num = frame->rax;
    uint64_t a0  = frame->rdi;
    uint64_t a1  = frame->rsi;
    uint64_t a2  = frame->rdx;
    uint64_t ret = (uint64_t)-1;

    switch (num) {
    case SYS_EXIT:
        process_exit((int)a0);
        ret = 0;
        break;
    case SYS_WRITE:
        ret = (uint64_t)sys_write(cur, (long)a0, (uintptr_t)a1, (size_t)a2);
        break;
    case SYS_YIELD:
        process_yield();
        ret = 0;
        break;
    case SYS_GETPID:
        ret = cur ? (uint64_t)cur->pid : (uint64_t)-1;
        break;
    case SYS_SLEEP_MS:
        timer_sleep_ms((uint32_t)a0);
        ret = 0;
        break;
    case SYS_READ:
        ret = (uint64_t)sys_read(cur, (long)a0, (uintptr_t)a1, (size_t)a2);
        break;
    case SYS_OPEN:
        ret = (uint64_t)sys_open(cur, (uintptr_t)a0, (long)a1);
        break;
    case SYS_CLOSE:
        ret = (uint64_t)sys_close(cur, (long)a0);
        break;
    case SYS_GETDENTS:
        ret = (uint64_t)sys_getdents(cur, (uintptr_t)a0, (size_t)a1);
        break;
    case SYS_SPAWN:
        ret = (uint64_t)sys_spawn(cur, (uintptr_t)a0, (uintptr_t)a1, (uintptr_t)a2);
        break;
    case SYS_WAIT:
        if ((int)a1)
            ret = (uint64_t)process_try_wait((int)a0);
        else
            ret = (uint64_t)process_wait((int)a0);
        break;
    case SYS_UNLINK:
        ret = (uint64_t)sys_unlink(cur, (uintptr_t)a0);
        break;
    case SYS_STAT:
        ret = (uint64_t)sys_stat(cur, (uintptr_t)a0, (uintptr_t)a1);
        break;
    case SYS_DUP:
        ret = (uint64_t)sys_dup(cur, (long)a0);
        break;
    case SYS_DUP2:
        ret = (uint64_t)sys_dup2(cur, (long)a0, (long)a1);
        break;
    case SYS_PIPE:
        ret = (uint64_t)sys_pipe(cur, (uintptr_t)a0);
        break;
    case SYS_KILL:
        ret = (uint64_t)sys_kill(cur, (long)a0, (long)a1);
        break;
    case SYS_SETFG:
        ret = (uint64_t)sys_setfg(cur, (long)a0);
        break;
    case SYS_BRK:
        ret = (uint64_t)sys_brk(cur, (uintptr_t)a0);
        break;
    case SYS_CHDIR:
        ret = (uint64_t)sys_chdir(cur, (uintptr_t)a0);
        break;
    case SYS_GETCWD:
        ret = (uint64_t)sys_getcwd(cur, (uintptr_t)a0, (size_t)a1);
        break;
    default:
        kprintf("syscall: unknown %u\n", (unsigned)num);
        ret = (uint64_t)-1;
        break;
    }
    frame->rax = ret;
#endif
}

void syscall_init(void) {
    set_idt_gate(SYSCALL_VECTOR, (uintptr_t)syscall_stub,
                 ARCH_KERNEL_CS, 0xEE);
    register_interrupt_handler(SYSCALL_VECTOR, syscall_dispatch);
    kprintf("Syscall: int 0x80 DPL=3 (+PIPE)\n");
}
