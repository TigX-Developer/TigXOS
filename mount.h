/* mount.h — Mount table (TigxOS 1.3.8)
 *
 * Tracks which filesystem is mounted where. VFS will consult this table
 * when resolving paths across multiple volumes (FAT32/NTFS later).
 */
#ifndef TIGX_MOUNT_H
#define TIGX_MOUNT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOUNT_MAX       8
#define MOUNT_PATH_LEN  64

enum mount_fs {
    MOUNT_FS_NONE  = 0,
    MOUNT_FS_TIGFS = 1,
    MOUNT_FS_RAMFS = 2,
    MOUNT_FS_FAT32 = 3,
    MOUNT_FS_NTFS  = 4
};

struct mount_entry {
    int  used;
    char path[MOUNT_PATH_LEN];  /* mount point, e.g. "/" or "/mnt/usb" */
    int  fs_type;               /* MOUNT_FS_* */
    int  dev_id;                /* block device, or -1 for ramfs */
    int  part_index;            /* index into part table, or -1 */
    uint64_t start_lba;         /* partition start (0 for whole disk / ram) */
    uint64_t sectors;
};

void mount_init(void);

/* Register a mount. path must be absolute. Returns 0 on success. */
int  mount_add(const char *path, int fs_type, int dev_id, int part_index,
               uint64_t start_lba, uint64_t sectors);

int  mount_remove(const char *path);

int  mount_count(void);
const struct mount_entry *mount_get(int index);

/* Longest-prefix match for path → mount entry (or NULL). */
const struct mount_entry *mount_lookup(const char *path);

void mount_dump(void);

#ifdef __cplusplus
}
#endif

#endif /* TIGX_MOUNT_H */
