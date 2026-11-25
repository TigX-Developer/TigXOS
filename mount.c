/* mount.c — Mount table (TigxOS 1.3.8) */

#include "mount.h"
#include "console.h"
#include "string.h"
#include "log.h"
#include "part.h"
#include "block.h"
#include <stdint.h>

static struct mount_entry g_mounts[MOUNT_MAX];
static int                g_count;

void mount_init(void)
{
    memset(g_mounts, 0, sizeof(g_mounts));
    g_count = 0;

    /* Root: TigFS on the first block device (whole-disk / first partition).
     * Matches existing behaviour (TigFS at LBA 512 on the image). */
    int dev = 0;
    uint64_t start = 0;
    uint64_t sects = 0;
    int part_idx = -1;

    if (block_dev_count() > 0) {
        struct block_dev *d = block_get(0);
        if (d)
            sects = d->sectors;
    }
    if (part_count() > 0) {
        const struct partition *p = part_get(0);
        if (p) {
            dev = p->dev_id;
            start = p->start_lba;
            sects = p->sector_count;
            part_idx = 0;
        }
    }

    mount_add("/", MOUNT_FS_TIGFS, dev, part_idx, start, sects);
    mount_add("/tmp", MOUNT_FS_RAMFS, -1, -1, 0, 0);

    pr_info("mount: %d mount(s)\n", g_count);
}

int mount_add(const char *path, int fs_type, int dev_id, int part_index,
              uint64_t start_lba, uint64_t sectors)
{
    if (!path || path[0] != '/' || g_count >= MOUNT_MAX)
        return -1;

    /* Replace existing mount at the same path */
    for (int i = 0; i < g_count; i++) {
        if (g_mounts[i].used && strcmp(g_mounts[i].path, path) == 0) {
            g_mounts[i].fs_type    = fs_type;
            g_mounts[i].dev_id     = dev_id;
            g_mounts[i].part_index = part_index;
            g_mounts[i].start_lba  = start_lba;
            g_mounts[i].sectors    = sectors;
            return 0;
        }
    }

    struct mount_entry *m = &g_mounts[g_count];
    memset(m, 0, sizeof(*m));
    m->used = 1;
    strncpy(m->path, path, MOUNT_PATH_LEN - 1);
    m->fs_type    = fs_type;
    m->dev_id     = dev_id;
    m->part_index = part_index;
    m->start_lba  = start_lba;
    m->sectors    = sectors;
    g_count++;
    return 0;
}

int mount_remove(const char *path)
{
    if (!path)
        return -1;
    for (int i = 0; i < g_count; i++) {
        if (g_mounts[i].used && strcmp(g_mounts[i].path, path) == 0) {
            if (strcmp(path, "/") == 0)
                return -1;  /* never unmount root */
            g_mounts[i].used = 0;
            /* compact */
            for (int j = i; j < g_count - 1; j++)
                g_mounts[j] = g_mounts[j + 1];
            g_count--;
            memset(&g_mounts[g_count], 0, sizeof(g_mounts[0]));
            return 0;
        }
    }
    return -1;
}

int mount_count(void)
{
    return g_count;
}

const struct mount_entry *mount_get(int index)
{
    if (index < 0 || index >= g_count)
        return NULL;
    return g_mounts[index].used ? &g_mounts[index] : NULL;
}

const struct mount_entry *mount_lookup(const char *path)
{
    if (!path || path[0] != '/')
        return NULL;

    const struct mount_entry *best = NULL;
    size_t best_len = 0;

    for (int i = 0; i < g_count; i++) {
        if (!g_mounts[i].used)
            continue;
        const char *mp = g_mounts[i].path;
        size_t len = strlen(mp);
        if (len > best_len && strncmp(path, mp, len) == 0) {
            /* Match only on boundary: "/mnt" must not match "/mntfoo" */
            if (path[len] == '\0' || path[len] == '/' || len == 1) {
                best = &g_mounts[i];
                best_len = len;
            }
        }
    }
    return best;
}

static const char *fs_str(int t)
{
    switch (t) {
    case MOUNT_FS_TIGFS: return "tigfs";
    case MOUNT_FS_RAMFS: return "ramfs";
    case MOUNT_FS_FAT32: return "fat32";
    case MOUNT_FS_NTFS:  return "ntfs";
    default:             return "?";
    }
}

void mount_dump(void)
{
    kprintf("=== mounts (%d) ===\n", g_count);
    for (int i = 0; i < g_count; i++) {
        if (!g_mounts[i].used)
            continue;
        const struct mount_entry *m = &g_mounts[i];
        kprintf("  %s  %s  dev=%d part=%d LBA %llu + %llu\n",
                m->path, fs_str(m->fs_type), m->dev_id, m->part_index,
                (unsigned long long)m->start_lba,
                (unsigned long long)m->sectors);
    }
}
