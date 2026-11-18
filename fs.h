/* fs.h — TigFS v6 hierarchical filesystem (TigxOS 1.2.1) */
#ifndef FS_H
#define FS_H

#include <stdint.h>
#include <stddef.h>

#define TIGFS_NAME_LEN 28
#define TIGFS_DIRECT   112  /* 112*512 = 56KB max file / dir */
#ifndef PATH_MAX
#define PATH_MAX 64
#endif

enum tigfs_type {
    TIGFS_FILE     = 1,
    TIGFS_UNLINKED = 2,
    TIGFS_DIR      = 3
};

/* Packed directory entry in directory data blocks (32 bytes). */
struct tigfs_dentry {
    char     name[TIGFS_NAME_LEN];
    uint16_t ino;
    uint16_t type;
} __attribute__((packed));

struct tigfs_dirent {
    char     name[TIGFS_NAME_LEN];
    uint32_t size;
    uint8_t  used;
    uint8_t  type;
};

void fs_init(void);
int  fs_ready(void);
int  fs_format(void);

int  fs_create(const char *path);
int  fs_mkdir(const char *path);
int  fs_remove(const char *path);
int  fs_write(const char *path, const void *data, uint32_t len);
int  fs_read(const char *path, void *buf, uint32_t maxlen);
int  fs_size(const char *path);
int  fs_visible_size(const char *path);
int  fs_is_dir(const char *path);
void fs_list(void);
int  fs_count(void);
uint32_t fs_data_blocks(void);
uint32_t fs_free_blocks(void);

int  fs_getdents_path(const char *path, void *buf, uint32_t maxlen);
int  fs_getdents(void *buf, uint32_t maxlen);

int  fs_detach_name(const char *path);
int  fs_resolve_path(const char *cwd, const char *path, char *out, size_t outlen);

int  fs_lookup_ino(const char *path);
int  fs_ino_size(int ino);
int  fs_ino_type(int ino);
int  fs_ino_read(int ino, uint32_t offset, void *buf, uint32_t len);
int  fs_ino_write(int ino, const void *data, uint32_t len);
int  fs_ino_name(int ino, char *out, size_t outlen);

#endif
