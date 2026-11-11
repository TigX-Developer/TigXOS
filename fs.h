// fs.h - TigFS: flat on-disk filesystem (after boot+kernel, LBA 512+)
#ifndef FS_H
#define FS_H

#include <stdint.h>
#include <stddef.h>

#define TIGFS_NAME_LEN 28
#define TIGFS_DIRECT   112  /* 112*512 = 56KB max file */
#ifndef PATH_MAX
#define PATH_MAX 64
#endif

enum tigfs_type { TIGFS_FILE = 1, TIGFS_UNLINKED = 2 };

struct tigfs_dirent {
    char     name[TIGFS_NAME_LEN];
    uint32_t size;
    uint8_t  used;
};

void fs_init(void);
int  fs_ready(void);
int  fs_format(void);
int  fs_create(const char *name);
int  fs_remove(const char *name);
int  fs_write(const char *name, const void *data, uint32_t len);
int  fs_read(const char *name, void *buf, uint32_t maxlen);
int  fs_size(const char *name);
/* Path lookup only — ignores UNLINKED (unlink-while-open). */
int  fs_visible_size(const char *name);
void fs_list(void);
int  fs_count(void);
/* Pack dirents into buf; returns bytes written (multiple of sizeof sys-compatible dirent). */
int  fs_getdents(void *buf, uint32_t maxlen);

/* Unlink-while-open: hide from lookup/dirents; blocks freed on last of_decref. */
int  fs_detach_name(const char *name);

/* Resolve relative path against cwd into absolute name (for flat TigFS).
 * out receives the full path string used as the FS name. Returns 0 or -1. */
int  fs_resolve_path(const char *cwd, const char *path, char *out, size_t outlen);

#endif
