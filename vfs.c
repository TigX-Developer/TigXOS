/* vfs.c — thin VFS over TigFS + ramfs at /tmp (TigxOS 1.2.1) */

#include "vfs.h"
#include "ramfs.h"
#include "console.h"
#include "string.h"

void vfs_init(void) {
    ramfs_init();
    kprintf("VFS: TigFS mounted at /, ramfs at /tmp\n");
}

static int is_tmp_path(const char *path) {
    return path && path[0] == '/' && path[1] == 't' && path[2] == 'm' &&
           path[3] == 'p' && (path[4] == 0 || path[4] == '/');
}

int vfs_open(const char *path, int flags, int *out_ino, int *out_fsid) {
    (void)flags;
    if (!path || !out_ino || !out_fsid)
        return -1;
    if (is_tmp_path(path)) {
        int ino = ramfs_lookup(path);
        if (ino < 0)
            return -1;
        if (ramfs_ino_type(ino) == 2)
            return -1; /* directory */
        *out_ino = ino;
        *out_fsid = VFS_FS_RAMFS;
        return 0;
    }
    int ino = fs_lookup_ino(path);
    if (ino < 0)
        return -1;
    if (fs_ino_type(ino) == TIGFS_DIR)
        return -1;
    *out_ino = ino;
    *out_fsid = VFS_FS_TIGFS;
    return 0;
}

int vfs_create(const char *path, int is_dir) {
    if (!path)
        return -1;
    if (is_tmp_path(path))
        return ramfs_create(path, is_dir) >= 0 ? 0 : -1;
    return is_dir ? fs_mkdir(path) : fs_create(path);
}

int vfs_unlink(const char *path) {
    if (!path)
        return -1;
    if (is_tmp_path(path))
        return ramfs_remove(path);
    return fs_remove(path);
}

int vfs_mkdir(const char *path) {
    return vfs_create(path, 1);
}

int vfs_stat(const char *path, struct vfs_stat *st) {
    if (!path || !st)
        return -1;
    if (is_tmp_path(path)) {
        uint32_t size = 0;
        int isdir = 0, ino = -1;
        if (ramfs_stat(path, &size, &isdir, &ino) != 0)
            return -1;
        st->ino = ino;
        st->fsid = VFS_FS_RAMFS;
        st->size = size;
        st->mode = isdir ? 2 : 1;
        return 0;
    }
    int ino = fs_lookup_ino(path);
    if (ino < 0)
        return -1;
    st->ino = ino;
    st->fsid = VFS_FS_TIGFS;
    st->size = (uint32_t)fs_ino_size(ino);
    int t = fs_ino_type(ino);
    st->mode = (t == TIGFS_DIR) ? 2 : 1;
    return 0;
}

int vfs_read(int fsid, int ino, uint32_t offset, void *buf, uint32_t len) {
    if (fsid == VFS_FS_RAMFS)
        return ramfs_read(ino, offset, buf, len);
    if (fsid == VFS_FS_TIGFS)
        return fs_ino_read(ino, offset, buf, len);
    return -1;
}

int vfs_write_replace(int fsid, int ino, const void *data, uint32_t len) {
    if (fsid == VFS_FS_RAMFS)
        return ramfs_write_replace(ino, data, len);
    if (fsid == VFS_FS_TIGFS)
        return fs_ino_write(ino, data, len);
    return -1;
}

int vfs_getdents(const char *path, void *buf, uint32_t maxlen) {
    if (!path)
        path = "/";
    if (is_tmp_path(path))
        return ramfs_getdents(path, buf, maxlen);
    return fs_getdents_path(path, buf, maxlen);
}

int vfs_is_dir(const char *path) {
    if (!path)
        return -1;
    if (is_tmp_path(path))
        return ramfs_is_dir(path) ? 1 : 0;
    return fs_is_dir(path);
}
