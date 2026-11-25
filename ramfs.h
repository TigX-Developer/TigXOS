/* ramfs.h — in-memory filesystem mounted at /tmp (TigxOS 1.3.8) */
#ifndef RAMFS_H
#define RAMFS_H

#include <stdint.h>
#include <stddef.h>

#define RAMFS_MAX_INODES  32
#define RAMFS_NAME_LEN    28
#define RAMFS_MAX_FILE    4096
#define RAMFS_DATA_POOL   (64 * 1024)
#define RAMFS_MAX_CHILDREN 16

void ramfs_init(void);

/* Paths may be "/tmp", "/tmp/foo", or relative names under /tmp. */
int  ramfs_lookup(const char *path);          /* inode >=0 or -1 */
int  ramfs_create(const char *path, int is_dir);
int  ramfs_remove(const char *path);
int  ramfs_stat(const char *path, uint32_t *size, int *is_dir, int *ino);
int  ramfs_is_dir(const char *path);

int  ramfs_ino_type(int ino);                 /* 1=file, 2=dir, -1=bad */
int  ramfs_ino_size(int ino);
int  ramfs_read(int ino, uint32_t offset, void *buf, uint32_t len);
int  ramfs_write_replace(int ino, const void *data, uint32_t len);
int  ramfs_getdents(const char *path, void *buf, uint32_t maxlen);

#endif
