

/* syscall.c — int 0x80 dispatcher (TigxOS 1.3.8, ABI v1) */
#include "syscall.h"
#include "io.h"
#include "interrupt.h"
#include "console.h"
#include "process.h"
#include "filetable.h"
#include "timer.h"
#include "keyboard.h"
#include "fs.h"
#include "vfs.h"
#include "pipe.h"
#include "aspace.h"
#include "futex.h"
#include "string.h"
#include "memory.h"
#include "rtc.h"
#include "version.h"
#include "log.h"
#include "spinlock.h"
#include "acpi.h"
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
static spinlock_t spawn_lock = SPINLOCK_INIT;

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
    if (!cur)
        return SY_EINVAL;
    if (fd < 0 || fd >= MAX_FDS)
        return SY_EBADF;
    if (len > (1u << 20))  /* 1 MiB hard cap per call */
        return SY_EINVAL;
    if (len && !user_range_ok(buf, len))
        return SY_EFAULT;
    struct open_file *o = of_get(process_fds(cur)[fd].of_id);
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
        int sz = fs_ino_size(o->ino);
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
        if (final_len > (uint32_t)TIGFS_DIRECT * 512u)
            return -1;
        char *tmp = kmalloc(final_len ? final_len : 1);
        if (!tmp)
            return -1;
        if (sz > 0) {
            if (vfs_read(o->fsid, o->ino, 0, tmp, (uint32_t)sz) != sz) {
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
        if (vfs_write_replace(o->fsid, o->ino, tmp, final_len) != 0) {
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
    struct open_file *o = of_get(process_fds(cur)[fd].of_id);
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
        int sz = fs_ino_size(o->ino);
        if (sz < 0)
            return 0;
        if (o->offset >= (uint32_t)sz)
            return 0;
        uint32_t remain = (uint32_t)sz - o->offset;
        uint32_t take = remain < (uint32_t)len ? remain : (uint32_t)len;
        if ((uint32_t)sz > (uint32_t)TIGFS_DIRECT * 512u)
            return -1;
        char *tmp = kmalloc((uint32_t)sz ? (uint32_t)sz : 1);
        if (!tmp)
            return -1;
        if (vfs_read(o->fsid, o->ino, 0, tmp, (uint32_t)sz) != sz) {
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

    int need_create = (flags & O_CREAT) != 0;
    int acc = flags & 3;
    if (acc > O_RDWR)
        return -1;

    int ino = -1, fsid = 0;
    if (vfs_open(resolved, (int)flags, &ino, &fsid) != 0) {
        if (!need_create)
            return -1;
        if (vfs_create(resolved, 0) != 0)
            return -1;
        if (vfs_open(resolved, (int)flags, &ino, &fsid) != 0)
            return -1;
    }
    if ((flags & O_TRUNC) && acc != O_RDONLY) {
        if (vfs_write_replace(fsid, ino, "", 0) != 0)
            return -1;
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
    o->ino = ino;
    o->fsid = fsid;
    o->offset = 0;
    if (flags & O_APPEND) {
        int sz = fs_ino_size(ino);
        o->offset = sz > 0 ? (uint32_t)sz : 0;
    }
    memset(o->name, 0, sizeof(o->name));
    fs_ino_name(ino, o->name, sizeof(o->name));

    int fd = process_alloc_fd(cur);
    if (fd < 0) {
        of_decref(of);
        return -1;
    }
    process_fds(cur)[fd].of_id = of;
    return fd;
}

static long sys_close(struct process *cur, long fd) {
    if (!cur)
        return SY_EINVAL;
    if (fd < 0 || fd >= MAX_FDS)
        return SY_EBADF;
    if (process_fds(cur)[fd].of_id < 0)
        return -1;
    fd_close(cur, (int)fd);
    return 0;
}

static long sys_dup(struct process *cur, long oldfd) {
    if (!cur || oldfd < 0 || oldfd >= MAX_FDS)
        return -1;
    int oid = process_fds(cur)[oldfd].of_id;
    if (oid < 0)
        return -1;
    int nfd = process_alloc_fd(cur);
    if (nfd < 0)
        return -1;
    process_fds(cur)[nfd].of_id = oid;
    of_incref(oid);
    return nfd;
}

static long sys_dup2(struct process *cur, long oldfd, long newfd) {
    if (!cur || oldfd < 0 || oldfd >= MAX_FDS || newfd < 0 || newfd >= MAX_FDS)
        return -1;
    int oid = process_fds(cur)[oldfd].of_id;
    if (oid < 0)
        return -1;
    if (oldfd == newfd)
        return newfd;
    if (process_fds(cur)[newfd].of_id >= 0)
        fd_close(cur, (int)newfd);
    process_fds(cur)[newfd].of_id = oid;
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
    process_fds(cur)[rfd].of_id = of_r;

    int wfd = process_alloc_fd(cur);
    if (wfd < 0) {
        process_fds(cur)[rfd].of_id = -1;
        of_decref(of_r);
        return -1;
    }
    int of_w = of_alloc();
    if (of_w < 0) {
        process_fds(cur)[rfd].of_id = -1;
        of_decref(of_r);
        return -1;
    }
    process_fds(cur)[wfd].of_id = of_w; /* claim slot early */

    int pid_ = pipe_alloc();
    if (pid_ < 0) {
        process_fds(cur)[rfd].of_id = -1;
        process_fds(cur)[wfd].of_id = -1;
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
        process_fds(cur)[rfd].of_id = -1;
        process_fds(cur)[wfd].of_id = -1;
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
        process_fds(cur)[rfd].of_id = -1;
        process_fds(cur)[wfd].of_id = -1;
        of_decref(of_r);
        of_decref(of_w);
        return -1;
    }
    return 0;
}



static long sys_getdents(struct process *cur, uintptr_t buf, size_t len) {
    if (!cur || len == 0 || len > 4096)
        return -1;
    if (!user_range_writable(buf, len))
        return -1;
    char kbuf[4096];
    const char *path = (cur->cwd[0]) ? cur->cwd : "/";
    int n = vfs_getdents(path, kbuf, (uint32_t)len);
    if (n < 0)
        return -1;
    if (copy_to_user(buf, kbuf, (size_t)n) != 0)
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
    if (vfs_is_dir(resolved) == 1)
        return -1;
    if (of_mark_unlink(resolved))
        return 0;
    return vfs_unlink(resolved) == 0 ? 0 : -1;
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
    struct vfs_stat vst;
    if (vfs_stat(resolved, &vst) != 0)
        return -1;
    struct sys_stat st;
    st.size = vst.size;
    st.mode = vst.mode;
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
    /* signal 0: existence probe only (POSIX-style), do not deliver */
    if (s == 0) {
        struct process *p = process_lookup_pid((int)pid);
        if (!p || p->state == PROC_UNUSED || p->state == PROC_DEAD)
            return -1;
        return 0;
    }
    /* only SIGINT(2), SIGKILL(9), SIGTERM(15) — unknown → error */
    if (s != 2 && s != 9 && s != 15)
        return -1;
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
    if (cur->heap_limit == 0)
        cur->heap_limit = USER_HEAP_LIMIT;
    uintptr_t old = cur->heap_brk;
    if (new_brk == 0)
        return (long)old;
    if (new_brk < USER_HEAP_BASE || new_brk > USER_HEAP_MAX)
        return -1;
    /* Per-process soft limit (default USER_HEAP_LIMIT) */
    if (new_brk > USER_HEAP_BASE + cur->heap_limit)
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


static long sys_mkdir(struct process *cur, uintptr_t pathu) {
    if (!cur)
        return -1;
    char upath[PATH_MAX];
    if (copy_user_path(upath, sizeof(upath), pathu) != 0)
        return -1;
    char resolved[PATH_MAX];
    if (fs_resolve_path(cur->cwd, upath, resolved, sizeof(resolved)) != 0)
        return -1;
    return vfs_mkdir(resolved) == 0 ? 0 : -1;
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
    if (vfs_is_dir(resolved) != 1)
        return -1;
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


static long sys_sysinfo(struct process *cur, uintptr_t uaddr) {
    (void)cur;
    if (!user_range_writable(uaddr, sizeof(struct sys_sysinfo)))
        return -1;
    struct sys_sysinfo info;
    memset(&info, 0, sizeof(info));
    uint32_t hz = timer_get_hz();
    if (!hz) hz = 100;
    uint32_t ticks = timer_get_ticks();
    info.ticks = ticks;
    info.hz = hz;
    info.uptime_sec = ticks / hz;
    /* usable RAM, not the sum of every E820 region (that can include huge reserved holes) */
    {
        uint64_t usable = memory_get_usable();
        uint64_t freeb  = memory_get_free();
        /* PMM tracks frames only up to MAX_PHYS_ADDR; report a consistent view */
        if (usable > MAX_PHYS_ADDR)
            usable = MAX_PHYS_ADDR;
        if (freeb > usable)
            freeb = usable;
        info.mem_total_kb = (uint32_t)(usable / 1024ull);
        info.mem_free_kb  = (uint32_t)(freeb / 1024ull);
    }
    info.fs_files = (uint32_t)fs_count();
    info.fs_blocks = fs_data_blocks();
    info.fs_free_blocks = fs_free_blocks();
    struct rtc_time rt;
    if (rtc_read(&rt) == 0) {
        info.rtc_year = rt.year;
        info.rtc_mon  = rt.mon;
        info.rtc_day  = rt.day;
        info.rtc_hour = rt.hour;
        info.rtc_min  = rt.min;
        info.rtc_sec  = rt.sec;
    }
#if defined(__x86_64__)
    const char *arch = "x86_64";
#else
    const char *arch = "i386";
#endif
    for (int i = 0; arch[i] && i < 15; i++)
        info.arch[i] = arch[i];
    const char *ver = TIGX_VERSION;
    for (int i = 0; ver[i] && i < 23; i++)
        info.version[i] = ver[i];
    if (copy_to_user(uaddr, &info, sizeof(info)) != 0)
        return -1;
    return 0;
}



static long sys_psinfo(struct process *cur, uintptr_t uaddr) {
    (void)cur;
    if (!user_range_writable(uaddr, sizeof(struct sys_psinfo)))
        return -1;
    struct sys_psinfo info;
    info.count = 0;
    for (int i = 0; i < SYS_PSINFO_MAX; i++) {
        info.procs[i].pid = 0;
        info.procs[i].ppid = 0;
        info.procs[i].state = 0;
        info.procs[i].is_user = 0;
        info.procs[i].exit_status = 0;
        info.procs[i].name[0] = 0;
    }
    int n = process_snapshot(info.procs, SYS_PSINFO_MAX);
    info.count = n;
    if (copy_to_user(uaddr, &info, sizeof(info)) != 0)
        return -1;
    return n;
}


static long sys_halt(struct process *cur) {
    /* Policy: only the shell may halt the machine (Foundations). */
    if (!cur || !cur->is_user)
        return -1;
    if (strcmp(cur->name, "shell") != 0 && strcmp(cur->name, "shell.elf") != 0)
        return -1;
    process_request_halt();
    /* Prefer ACPI poweroff; falls back to QEMU ports. */
    acpi_poweroff();
    return 0; /* not reached */
}

static long sys_poweroff(struct process *cur) {
    if (!cur || !cur->is_user) return -1;
    if (strcmp(cur->name, "shell") != 0 && strcmp(cur->name, "shell.elf") != 0)
        return -1;
    acpi_poweroff();
    return 0;
}

static long sys_reboot(struct process *cur) {
    if (!cur || !cur->is_user) return -1;
    if (strcmp(cur->name, "shell") != 0 && strcmp(cur->name, "shell.elf") != 0)
        return -1;
    acpi_reboot();
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
    /* Hierarchical TigFS: keep absolute path for elf_load_file / fs_lookup. */
    if (!resolved[0])
        return -1;

    /* Serialize static staging buffers against concurrent spawn.
     * 1.2.7: proper spinlock instead of home-made spawn_busy + raw cli/sti. */
    spin_lock(&spawn_lock);

    static char arg_store[SPAWN_ARG_BUF];
    static char *kargv[SPAWN_MAX_ARGS + 1];
    int argc = copy_user_argv(argvu, arg_store, sizeof(arg_store),
                              kargv, SPAWN_MAX_ARGS);
    if (argc < 0) {
        spin_unlock(&spawn_lock);
        return -1;
    }

    static char env_store[SPAWN_ARG_BUF];
    static char *kenvp[SPAWN_MAX_ARGS + 1];
    int envc = 0;
    if (envpu) {
        envc = copy_user_argv(envpu, env_store, sizeof(env_store),
                              kenvp, SPAWN_MAX_ARGS);
        if (envc < 0) {
            spin_unlock(&spawn_lock);
            return -1;
        }
    } else {
        kenvp[0] = 0;
    }

    int pid = spawn(resolved, argc > 0 ? kargv : NULL,
                    envc > 0 ? kenvp : NULL);
    if (pid > 0 && cur)
        process_set_parent(process_lookup_pid(pid), cur->pid);
    spin_unlock(&spawn_lock);
    return pid;
}


/* SYS_DMESG: copy kernel log ring to user buffer.
 * args: buf, len, offset  →  bytes copied (0 = EOF / empty)
 */
static long sys_dmesg(struct process *cur, uintptr_t bufu, size_t len, size_t off) {
    (void)cur;
    if (len == 0)
        return 0;
    if (len > 65536)
        return SY_EINVAL;
    if (!user_range_writable(bufu, len))
        return SY_EFAULT;
    /* Stage through a kernel temporary buffer (page-sized chunks). */
    char kbuf[512];
    size_t total = 0;
    while (total < len) {
        size_t chunk = len - total;
        if (chunk > sizeof(kbuf))
            chunk = sizeof(kbuf);
        size_t got = log_read(kbuf, chunk, off + total);
        if (got == 0)
            break;
        if (copy_to_user(bufu + total, kbuf, got) != 0)
            return SY_EFAULT;
        total += got;
        if (got < chunk)
            break;
    }
    return (long)total;
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
    case SYS_SYSINFO:
        ret = (uint32_t)sys_sysinfo(cur, (uintptr_t)a0);
        break;
    case SYS_PSINFO:
        ret = (uint32_t)sys_psinfo(cur, (uintptr_t)a0);
        break;
    case SYS_MKDIR:
        ret = (uint32_t)sys_mkdir(cur, (uintptr_t)a0);
        break;
    case SYS_HALT:
        ret = (uint32_t)sys_halt(cur);
        break;
    case SYS_ABI_VERSION:
        ret = (uint32_t)TIGX_SYSCALL_ABI;
        break;
    case SYS_DMESG:
        ret = (uint32_t)sys_dmesg(cur, (uintptr_t)a0, (size_t)a1, (size_t)a2);
        break;
    case SYS_POWEROFF:
        ret = (uint32_t)sys_poweroff(cur);
        break;
    case SYS_REBOOT:
        ret = (uint32_t)sys_reboot(cur);
        break;
    case SYS_CLONE: {
        uintptr_t entry = (uintptr_t)a0;
        uintptr_t stack = (uintptr_t)a1;
        if (!entry || entry < ASPACE_USER_MIN || entry >= ASPACE_USER_MAX)
            ret = (uint32_t)SY_EINVAL;
        else if (stack != 0 && (stack < ASPACE_USER_MIN || stack > ASPACE_USER_MAX))
            ret = (uint32_t)SY_EINVAL;
        else {
            int tid = process_thread_create((void (*)(void))entry, stack);
            ret = (uint32_t)(tid > 0 ? tid : SY_ERR);
        }
        break;
    }
    case SYS_THREAD_EXIT:
        process_thread_exit((int)a0);
        ret = 0;
        break;
    case SYS_THREAD_JOIN: {
        int tid = (int)a0;
        int status = 0;
        if (a1) {
            if (!cur || !cur->as ||
                !aspace_user_range_writable(cur->as, (uintptr_t)a1, sizeof(int))) {
                ret = (uint32_t)SY_EFAULT;
                break;
            }
        }
        int r = process_thread_join(tid, a1 ? &status : NULL);
        if (r == 0) {
            if (a1)
                *(int *)(uintptr_t)a1 = status;
            ret = 0;
        } else
            ret = (uint32_t)SY_ERR;
        break;
    }
    case SYS_SET_TLS: {
        if (!cur)
            ret = (uint32_t)SY_ERR;
        else {
            cur->tls_base = (uintptr_t)a0;
            process_load_tls(cur);
            ret = 0;
        }
        break;
    }
    case SYS_GETTID:
        ret = cur ? (uint32_t)cur->pid : (uint32_t)SY_ERR;
        break;
    case SYS_FUTEX: {
        /* a0=uaddr a1=op a2=val  (timeout = 0 / infinite in 1.2.12) */
        ret = (uint32_t)futex_do((uintptr_t)a0, (int)a1, (int)a2, 0);
        break;
    }

    default:
        ret = (uint32_t)SY_ENOSYS;
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
    case SYS_SYSINFO:
        ret = (uint64_t)sys_sysinfo(cur, (uintptr_t)a0);
        break;
    case SYS_PSINFO:
        ret = (uint64_t)sys_psinfo(cur, (uintptr_t)a0);
        break;
    case SYS_MKDIR:
        ret = (uint64_t)sys_mkdir(cur, (uintptr_t)a0);
        break;
    case SYS_HALT:
        ret = (uint64_t)sys_halt(cur);
        break;
    case SYS_ABI_VERSION:
        ret = (uint64_t)TIGX_SYSCALL_ABI;
        break;
    case SYS_DMESG:
        ret = (uint64_t)sys_dmesg(cur, (uintptr_t)a0, (size_t)a1, (size_t)a2);
        break;
    case SYS_POWEROFF:
        ret = (uint64_t)sys_poweroff(cur);
        break;
    case SYS_REBOOT:
        ret = (uint64_t)sys_reboot(cur);
        break;
    case SYS_CLONE: {
        uintptr_t entry = (uintptr_t)a0;
        uintptr_t stack = (uintptr_t)a1;
        if (!entry || entry < ASPACE_USER_MIN || entry >= ASPACE_USER_MAX)
            ret = (uint64_t)SY_EINVAL;
        else if (stack != 0 && (stack < ASPACE_USER_MIN || stack > ASPACE_USER_MAX))
            ret = (uint64_t)SY_EINVAL;
        else {
            int tid = process_thread_create((void (*)(void))entry, stack);
            ret = (uint64_t)(tid > 0 ? tid : SY_ERR);
        }
        break;
    }
    case SYS_THREAD_EXIT:
        process_thread_exit((int)a0);
        ret = 0;
        break;
    case SYS_THREAD_JOIN: {
        int tid = (int)a0;
        int status = 0;
        if (a1) {
            if (!cur || !cur->as ||
                !aspace_user_range_writable(cur->as, (uintptr_t)a1, sizeof(int))) {
                ret = (uint64_t)SY_EFAULT;
                break;
            }
        }
        int r = process_thread_join(tid, a1 ? &status : NULL);
        if (r == 0) {
            if (a1)
                *(int *)(uintptr_t)a1 = status;
            ret = 0;
        } else
            ret = (uint64_t)SY_ERR;
        break;
    }
    case SYS_SET_TLS: {
        if (!cur)
            ret = (uint64_t)SY_ERR;
        else {
            cur->tls_base = (uintptr_t)a0;
            process_load_tls(cur);
            ret = 0;
        }
        break;
    }
    case SYS_GETTID:
        ret = cur ? (uint64_t)cur->pid : (uint64_t)SY_ERR;
        break;
    case SYS_FUTEX: {
        ret = (uint64_t)futex_do((uintptr_t)a0, (int)a1, (int)a2, 0);
        break;
    }

    default:
        ret = (uint64_t)SY_ENOSYS;
        break;
    }
    frame->rax = ret;
#endif
}

void syscall_init(void) {
    set_idt_gate(SYSCALL_VECTOR, (uintptr_t)syscall_stub,
                 ARCH_KERNEL_CS, 0xEE);
    register_interrupt_handler(SYSCALL_VECTOR, syscall_dispatch);
    kprintf_color(TIG_OK, "Syscall: int 0x80 DPL=3  ABI v%d\n", TIGX_SYSCALL_ABI);
}
