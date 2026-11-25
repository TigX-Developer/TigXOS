/* vfs.h — Virtual filesystem layer (TigxOS 1.3.8)
 *
 * One open()/read()/write() path for TigFS and future backends (ramfs, FAT32).
 */
#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <stddef.h>
#include "fs.h"

enum vfs_fsid {
    VFS_FS_TIGFS = 0,
    VFS_FS_RAMFS = 1
};

struct vfs_stat {
    uint32_t size;
    uint32_t mode; /* 1=file, 2=dir */
    int      fsid;
    int      ino;
};

void vfs_init(void);

/* Path is absolute (start with /) after resolve. */
int  vfs_open(const char *path, int flags, int *out_ino, int *out_fsid);
int  vfs_create(const char *path, int is_dir);
int  vfs_unlink(const char *path);
int  vfs_mkdir(const char *path);
int  vfs_stat(const char *path, struct vfs_stat *st);
int  vfs_read(int fsid, int ino, uint32_t offset, void *buf, uint32_t len);
int  vfs_write_replace(int fsid, int ino, const void *data, uint32_t len);
int  vfs_getdents(const char *path, void *buf, uint32_t maxlen);
int  vfs_is_dir(const char *path);

#endif
