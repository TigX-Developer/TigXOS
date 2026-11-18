/* fs.c — TigFS v6 hierarchical (directories + path walk) for TigxOS 1.2.1
 *
 * On-disk layout (unchanged geometry from v5/v6 flat):
 *   LBA 512      superblock
 *   LBA 513      bitmap (1 sector, up to 4096 data blocks)
 *   LBA 514–577  64 inode sectors (512 B each)
 *   LBA 578+     data blocks
 *
 * Hierarchical change: inode 0 is always the root directory. Directory data
 * blocks hold packed tigfs_dentry records. File lookup walks path components.
 */

#include "version.h"
#include "fs.h"
#include "disk.h"
#include "console.h"
#include "string.h"
#include "spinlock.h"
#include <stdint.h>
#include <stddef.h>

#define TIGFS_MAGIC     0x31534654u /* 'TFS1' */
#define TIGFS_VERSION   6
#define TIGFS_RESERVED  512u
#define TIGFS_MAX_INODES 64
#define TIGFS_INODE_BYTES 512
#define TIGFS_ROOT_INO  0

struct tigfs_super {
    uint32_t magic;
    uint32_t version;
    uint32_t disk_sectors;
    uint32_t reserved;
    uint32_t inode_lba;
    uint32_t inode_count;
    uint32_t bitmap_lba;
    uint32_t data_lba;
    uint32_t data_count;
    uint32_t pad[7];
};

struct tigfs_inode {
    uint8_t  used;
    uint8_t  type;
    uint16_t nlink;
    uint32_t size;
    uint32_t block[TIGFS_DIRECT];
    char     name[TIGFS_NAME_LEN]; /* basename cache */
    uint8_t  extra[28]; /* pad to 512 */
};

_Static_assert(sizeof(struct tigfs_inode) == TIGFS_INODE_BYTES, "inode size");

static struct tigfs_super super;
static struct tigfs_inode inodes[TIGFS_MAX_INODES];
static uint8_t bitmap[512];
static int fs_ok = 0;
static uint8_t sector[512] __attribute__((aligned(2)));
static spinlock_t fs_lock = SPINLOCK_INIT;

static int read_lba(uint32_t lba, void *buf) { return disk_read(lba, 1, buf); }
static int write_lba(uint32_t lba, const void *buf) { return disk_write(lba, 1, buf); }

static int bittest(uint32_t i) { return (bitmap[i / 8] >> (i % 8)) & 1; }
static void bitset(uint32_t i) { bitmap[i / 8] |= (uint8_t)(1u << (i % 8)); }
static void bitclr(uint32_t i) { bitmap[i / 8] &= (uint8_t)~(1u << (i % 8)); }

static int alloc_block(void) {
    for (uint32_t i = 0; i < super.data_count; i++) {
        if (!bittest(i)) {
            bitset(i);
            return (int)i;
        }
    }
    return -1;
}

static void free_inode_blocks(struct tigfs_inode *in) {
    for (int i = 0; i < TIGFS_DIRECT; i++) {
        if (in->block[i]) {
            uint32_t idx = in->block[i] - super.data_lba;
            if (idx < super.data_count)
                bitclr(idx);
            in->block[i] = 0;
        }
    }
}

static int flush_super(void) {
    memset(sector, 0, 512);
    memcpy(sector, &super, sizeof(super));
    return write_lba(TIGFS_RESERVED, sector);
}

static int flush_bitmap(void) { return write_lba(super.bitmap_lba, bitmap); }

static int flush_inodes(void) {
    const uint8_t *p = (const uint8_t *)inodes;
    for (int s = 0; s < 64; s++) {
        memcpy(sector, p + s * 512, 512);
        if (write_lba(super.inode_lba + (uint32_t)s, sector) != 0)
            return -1;
    }
    return 0;
}

static int load_inodes(void) {
    uint8_t *p = (uint8_t *)inodes;
    for (int s = 0; s < 64; s++) {
        if (read_lba(super.inode_lba + (uint32_t)s, sector) != 0)
            return -1;
        memcpy(p + s * 512, sector, 512);
    }
    return 0;
}

/* ---- low-level inode data R/W ---- */

static int inode_read_bytes(struct tigfs_inode *in, uint32_t offset,
                            void *buf, uint32_t len) {
    if (!in || !buf)
        return -1;
    if (offset >= in->size)
        return 0;
    if (offset + len > in->size)
        len = in->size - offset;
    uint8_t *dst = (uint8_t *)buf;
    uint32_t done = 0;
    while (done < len) {
        uint32_t pos = offset + done;
        uint32_t b = pos / 512;
        uint32_t off = pos % 512;
        if (b >= (uint32_t)TIGFS_DIRECT || !in->block[b])
            break;
        if (read_lba(in->block[b], sector) != 0)
            return -1;
        uint32_t chunk = 512 - off;
        if (chunk > len - done)
            chunk = len - done;
        memcpy(dst + done, sector + off, chunk);
        done += chunk;
    }
    return (int)done;
}

static int inode_replace_data(struct tigfs_inode *in, const void *data,
                              uint32_t len) {
    if (len > (uint32_t)TIGFS_DIRECT * 512u)
        return -1;
    uint32_t new_blocks[TIGFS_DIRECT];
    int nnew = 0;
    const uint8_t *src = (const uint8_t *)data;
    uint32_t left = len;
    /* Always allocate at least one block when len>0; len==0 clears all blocks. */
    while (left > 0) {
        if (nnew >= TIGFS_DIRECT)
            return -1;
        int bi = alloc_block();
        if (bi < 0) {
            for (int i = 0; i < nnew; i++) {
                uint32_t idx = (uint32_t)new_blocks[i] - super.data_lba;
                if (idx < super.data_count)
                    bitclr(idx);
            }
            return -1;
        }
        uint32_t lba = super.data_lba + (uint32_t)bi;
        memset(sector, 0, 512);
        uint32_t chunk = left > 512 ? 512 : left;
        if (chunk && src)
            memcpy(sector, src, chunk);
        if (write_lba(lba, sector) != 0) {
            bitclr((uint32_t)bi);
            for (int i = 0; i < nnew; i++) {
                uint32_t idx = (uint32_t)new_blocks[i] - super.data_lba;
                if (idx < super.data_count)
                    bitclr(idx);
            }
            return -1;
        }
        new_blocks[nnew++] = lba;
        if (src)
            src += chunk;
        left -= chunk;
    }
    free_inode_blocks(in);
    for (int i = 0; i < TIGFS_DIRECT; i++)
        in->block[i] = (i < nnew) ? new_blocks[i] : 0;
    in->size = len;
    return (flush_bitmap() != 0 || flush_inodes() != 0) ? -1 : 0;
}

/* ---- directory helpers ---- */

static int dir_find_name(struct tigfs_inode *dir, const char *name,
                         struct tigfs_dentry *out, uint32_t *out_off) {
    if (!dir || dir->type != TIGFS_DIR || !name || !name[0])
        return -1;
    uint32_t off = 0;
    while (off + sizeof(struct tigfs_dentry) <= dir->size) {
        struct tigfs_dentry de;
        if (inode_read_bytes(dir, off, &de, sizeof(de)) != (int)sizeof(de))
            return -1;
        if (de.name[0] && strcmp(de.name, name) == 0) {
            if (out)
                *out = de;
            if (out_off)
                *out_off = off;
            return 0;
        }
        off += sizeof(de);
    }
    return -1;
}

static int dir_add_entry(struct tigfs_inode *dir, const char *name,
                         uint16_t ino, uint16_t type) {
    if (!dir || dir->type != TIGFS_DIR || !name || !name[0])
        return -1;
    if (dir_find_name(dir, name, 0, 0) == 0)
        return -1; /* exists */
    /* load full dir into temp (max 56KB is too big for stack — append in place) */
    uint32_t old = dir->size;
    uint32_t need = old + sizeof(struct tigfs_dentry);
    if (need > (uint32_t)TIGFS_DIRECT * 512u)
        return -1;

    /* Read existing data */
    static uint8_t dirbuf[512 * 16]; /* up to 8KB dirs for hobby scale */
    if (old > sizeof(dirbuf))
        return -1;
    if (old > 0) {
        if (inode_read_bytes(dir, 0, dirbuf, old) != (int)old)
            return -1;
    }
    struct tigfs_dentry *de = (struct tigfs_dentry *)(dirbuf + old);
    memset(de, 0, sizeof(*de));
    strncpy(de->name, name, TIGFS_NAME_LEN - 1);
    de->ino = ino;
    de->type = type;
    return inode_replace_data(dir, dirbuf, need);
}

static int dir_remove_entry(struct tigfs_inode *dir, const char *name) {
    uint32_t off = 0;
    struct tigfs_dentry de;
    if (dir_find_name(dir, name, &de, &off) != 0)
        return -1;
    static uint8_t dirbuf[512 * 16];
    if (dir->size > sizeof(dirbuf))
        return -1;
    if (inode_read_bytes(dir, 0, dirbuf, dir->size) != (int)dir->size)
        return -1;
    uint32_t tail = dir->size - off - sizeof(de);
    if (tail)
        memmove(dirbuf + off, dirbuf + off + sizeof(de), tail);
    return inode_replace_data(dir, dirbuf, dir->size - sizeof(de));
}

static int alloc_inode_slot(void) {
    for (int i = 1; i < TIGFS_MAX_INODES; i++) { /* 0 reserved for root */
        if (!inodes[i].used)
            return i;
    }
    return -1;
}

/* Walk absolute path; returns inode index or -1. */
static char norm_path_buf[PATH_MAX];
static const char *normalize_abs(const char *path) {
    if (!path || !path[0])
        return "/";
    if (path[0] == '/')
        return path;
    /* bare name → /name */
    if (strlen(path) + 2 > sizeof(norm_path_buf))
        return 0;
    norm_path_buf[0] = '/';
    strcpy(norm_path_buf + 1, path);
    return norm_path_buf;
}

static int path_walk(const char *abspath) {
    abspath = normalize_abs(abspath);
    if (!abspath || abspath[0] != '/')
        return -1;
    if (!inodes[TIGFS_ROOT_INO].used ||
        inodes[TIGFS_ROOT_INO].type != TIGFS_DIR)
        return -1;
    if (abspath[1] == 0)
        return TIGFS_ROOT_INO;

    int cur = TIGFS_ROOT_INO;
    const char *p = abspath + 1;
    char comp[TIGFS_NAME_LEN];
    while (*p) {
        int n = 0;
        while (*p && *p != '/' && n < TIGFS_NAME_LEN - 1)
            comp[n++] = *p++;
        comp[n] = 0;
        while (*p == '/')
            p++;
        if (n == 0)
            continue;
        if (inodes[cur].type != TIGFS_DIR)
            return -1;
        struct tigfs_dentry de;
        if (dir_find_name(&inodes[cur], comp, &de, 0) != 0)
            return -1;
        if (de.ino >= TIGFS_MAX_INODES || !inodes[de.ino].used)
            return -1;
        cur = de.ino;
    }
    return cur;
}

/* Split absolute path into parent dir path + basename. */
static int split_parent(const char *abspath, char *parent, size_t plen,
                        char *base, size_t blen) {
    if (!abspath || abspath[0] != '/' || !parent || !base)
        return -1;
    size_t len = strlen(abspath);
    if (len == 0 || len >= PATH_MAX)
        return -1;
    if (len == 1) { /* "/" has no parent for create */
        return -1;
    }
    /* find last slash */
    size_t last = len - 1;
    while (last > 0 && abspath[last] != '/')
        last--;
    if (last == 0) {
        if (plen < 2 || blen < len)
            return -1;
        parent[0] = '/';
        parent[1] = 0;
        size_t i = 0;
        while (abspath[1 + i] && i + 1 < blen) {
            base[i] = abspath[1 + i];
            i++;
        }
        base[i] = 0;
        return 0;
    }
    if (last >= plen)
        return -1;
    memcpy(parent, abspath, last);
    parent[last] = 0;
    size_t i = 0;
    while (abspath[last + 1 + i] && i + 1 < blen) {
        base[i] = abspath[last + 1 + i];
        i++;
    }
    base[i] = 0;
    return base[0] ? 0 : -1;
}

static int ensure_root(void) {
    if (inodes[TIGFS_ROOT_INO].used &&
        inodes[TIGFS_ROOT_INO].type == TIGFS_DIR)
        return 0;
    memset(&inodes[TIGFS_ROOT_INO], 0, sizeof(inodes[0]));
    inodes[TIGFS_ROOT_INO].used = 1;
    inodes[TIGFS_ROOT_INO].type = TIGFS_DIR;
    inodes[TIGFS_ROOT_INO].nlink = 1;
    inodes[TIGFS_ROOT_INO].size = 0;
    strncpy(inodes[TIGFS_ROOT_INO].name, "/", TIGFS_NAME_LEN - 1);
    return flush_inodes();
}

/* Migrate flat v5-style images: names like "bin/foo.elf" become hierarchy. */
static int migrate_flat_names(void) {
    int migrated = 0;
    for (int i = 1; i < TIGFS_MAX_INODES; i++) {
        if (!inodes[i].used || inodes[i].type == TIGFS_UNLINKED)
            continue;
        if (inodes[i].type == TIGFS_DIR)
            continue;
        const char *nm = inodes[i].name;
        if (!nm[0])
            continue;
        /* Already linked if path_walk finds it */
        char full[PATH_MAX];
        full[0] = '/';
        strncpy(full + 1, nm, PATH_MAX - 2);
        if (path_walk(full) == i)
            continue;
        /* Create intermediate dirs and link */
        char path[PATH_MAX];
        path[0] = '/';
        path[1] = 0;
        const char *p = nm;
        int ino = i;
        while (*p) {
            char comp[TIGFS_NAME_LEN];
            int n = 0;
            while (*p && *p != '/' && n < TIGFS_NAME_LEN - 1)
                comp[n++] = *p++;
            comp[n] = 0;
            while (*p == '/')
                p++;
            int is_last = (*p == 0);
            char next[PATH_MAX];
            size_t pl = strlen(path);
            if (pl + 1 + strlen(comp) >= PATH_MAX)
                return -1;
            if (pl > 1)
                next[0] = 0;
            strncpy(next, path, PATH_MAX - 1);
            if (pl > 1 || path[1]) {
                if (next[pl - 1] != '/')
                    strncat(next, "/", PATH_MAX - strlen(next) - 1);
            }
            strncat(next, comp, PATH_MAX - strlen(next) - 1);

            int existing = path_walk(next);
            if (existing >= 0) {
                strncpy(path, next, PATH_MAX - 1);
                continue;
            }
            if (is_last) {
                /* link this inode into parent */
                int parent = path_walk(path);
                if (parent < 0)
                    return -1;
                strncpy(inodes[ino].name, comp, TIGFS_NAME_LEN - 1);
                if (dir_add_entry(&inodes[parent], comp, (uint16_t)ino,
                                  TIGFS_FILE) != 0)
                    return -1;
                migrated++;
            } else {
                int slot = alloc_inode_slot();
                if (slot < 0)
                    return -1;
                memset(&inodes[slot], 0, sizeof(inodes[0]));
                inodes[slot].used = 1;
                inodes[slot].type = TIGFS_DIR;
                inodes[slot].nlink = 1;
                strncpy(inodes[slot].name, comp, TIGFS_NAME_LEN - 1);
                int parent = path_walk(path);
                if (parent < 0)
                    return -1;
                if (dir_add_entry(&inodes[parent], comp, (uint16_t)slot,
                                  TIGFS_DIR) != 0)
                    return -1;
                strncpy(path, next, PATH_MAX - 1);
                migrated++;
            }
        }
    }
    if (migrated)
        flush_inodes();
    return 0;
}

/* ---- public API ---- */

int fs_format(void) {
    struct ata_disk_info di;
    disk_get_info(&di);
    if (!di.present || di.sectors <= TIGFS_RESERVED + 80)
        return -1;

    memset(&super, 0, sizeof(super));
    super.magic = TIGFS_MAGIC;
    super.version = TIGFS_VERSION;
    super.disk_sectors = (uint32_t)di.sectors;
    super.reserved = TIGFS_RESERVED;
    super.bitmap_lba = TIGFS_RESERVED + 1;
    super.inode_lba = TIGFS_RESERVED + 2;
    super.inode_count = TIGFS_MAX_INODES;
    super.data_lba = TIGFS_RESERVED + 2 + 64;
    if (super.disk_sectors <= super.data_lba)
        return -1;
    super.data_count = super.disk_sectors - super.data_lba;
    if (super.data_count > 4096)
        super.data_count = 4096;

    memset(inodes, 0, sizeof(inodes));
    memset(bitmap, 0, sizeof(bitmap));
    inodes[TIGFS_ROOT_INO].used = 1;
    inodes[TIGFS_ROOT_INO].type = TIGFS_DIR;
    inodes[TIGFS_ROOT_INO].nlink = 1;
    strncpy(inodes[TIGFS_ROOT_INO].name, "/", TIGFS_NAME_LEN - 1);

    if (flush_super() || flush_bitmap() || flush_inodes())
        return -1;
    fs_ok = 1;
    kprintf("fs: formatted TigFS v6 hierarchical (%u data blocks)\n",
            super.data_count);
    return 0;
}

static int fs_mount(void) {
    if (read_lba(TIGFS_RESERVED, sector) != 0)
        return -1;
    memcpy(&super, sector, sizeof(super));
    if (super.magic != TIGFS_MAGIC || super.version != TIGFS_VERSION)
        return -1;
    if (super.inode_count != TIGFS_MAX_INODES)
        return -1;
    if (super.reserved != TIGFS_RESERVED)
        return -1;
    if (super.bitmap_lba != TIGFS_RESERVED + 1)
        return -1;
    if (super.inode_lba != TIGFS_RESERVED + 2)
        return -1;
    if (super.data_lba != TIGFS_RESERVED + 66)
        return -1;
    if (super.data_count == 0 || super.data_count > 4096)
        return -1;
    if (read_lba(super.bitmap_lba, bitmap) != 0)
        return -1;
    if (load_inodes() != 0)
        return -1;
    fs_ok = 1;
    if (ensure_root() != 0)
        return -1;
    /* Only migrate legacy flat images (root empty but files have path-like names). */
    if (inodes[TIGFS_ROOT_INO].size == 0)
        migrate_flat_names();
    return 0;
}

void fs_init(void) {
    spin_lock_init(&fs_lock);
    if (fs_mount() == 0) {
        kprintf("fs: mounted TigFS v6 (%u inodes, %u data blocks)\n",
                super.inode_count, super.data_count);
        return;
    }
    kprintf("fs: no valid image, formatting...\n");
    if (fs_format() != 0)
        kprintf("fs: format failed\n");
}

int fs_ready(void) { return fs_ok; }

int fs_resolve_path(const char *cwd, const char *path, char *out, size_t outlen) {
    if (!out || outlen < 2)
        return -1;
    if (!cwd || !cwd[0])
        cwd = "/";
    if (!path)
        path = "";

    int abs = (path[0] == '/');
    char tmp[PATH_MAX];
    size_t n = 0;

    if (!abs) {
        size_t cl = 0;
        while (cwd[cl] && cl < sizeof(tmp) - 1)
            tmp[cl] = cwd[cl], cl++;
        if (cwd[cl])
            return -1;
        if (cl == 0 || tmp[cl - 1] != '/') {
            if (cl >= sizeof(tmp) - 1)
                return -1;
            tmp[cl++] = '/';
        }
        n = cl;
        while (*path) {
            if (n >= sizeof(tmp) - 1)
                return -1;
            tmp[n++] = *path++;
        }
        tmp[n] = 0;
    } else {
        while (*path) {
            if (n >= sizeof(tmp) - 1)
                return -1;
            tmp[n++] = *path++;
        }
        tmp[n] = 0;
    }

    char comps[16][PATH_MAX];
    int nc = 0;
    size_t i = 0;
    while (tmp[i] == '/')
        i++;
    while (tmp[i]) {
        size_t start = i;
        while (tmp[i] && tmp[i] != '/')
            i++;
        size_t len = i - start;
        if (len == 1 && tmp[start] == '.') {
            /* skip */
        } else if (len == 2 && tmp[start] == '.' && tmp[start + 1] == '.') {
            if (nc > 0)
                nc--;
        } else if (len > 0) {
            if (nc >= 16 || len >= PATH_MAX)
                return -1;
            for (size_t k = 0; k < len; k++)
                comps[nc][k] = tmp[start + k];
            comps[nc][len] = 0;
            nc++;
        }
        while (tmp[i] == '/')
            i++;
    }

    out[0] = '/';
    size_t o = 1;
    for (int c = 0; c < nc; c++) {
        size_t L = strlen(comps[c]);
        if (o + L + (c ? 1 : 0) >= outlen)
            return -1;
        if (c && o > 1)
            out[o++] = '/';
        for (size_t k = 0; k < L; k++)
            out[o++] = comps[c][k];
    }
    out[o] = 0;
    return 0;
}

int fs_lookup_ino(const char *path) {
    spin_lock(&fs_lock);
    if (!fs_ok || !path) {
        spin_unlock(&fs_lock);
        return -1;
    }
    int ino = path_walk(path);
    spin_unlock(&fs_lock);
    return ino;
}

int fs_ino_size(int ino) {
    if (ino < 0 || ino >= TIGFS_MAX_INODES || !inodes[ino].used)
        return -1;
    return (int)inodes[ino].size;
}

int fs_ino_type(int ino) {
    if (ino < 0 || ino >= TIGFS_MAX_INODES || !inodes[ino].used)
        return -1;
    return inodes[ino].type;
}

int fs_ino_name(int ino, char *out, size_t outlen) {
    if (ino < 0 || ino >= TIGFS_MAX_INODES || !inodes[ino].used || !out || outlen < 2)
        return -1;
    strncpy(out, inodes[ino].name, outlen - 1);
    out[outlen - 1] = 0;
    return 0;
}

int fs_ino_read(int ino, uint32_t offset, void *buf, uint32_t len) {
    spin_lock(&fs_lock);
    if (!fs_ok || ino < 0 || ino >= TIGFS_MAX_INODES || !inodes[ino].used) {
        spin_unlock(&fs_lock);
        return -1;
    }
    int n = inode_read_bytes(&inodes[ino], offset, buf, len);
    spin_unlock(&fs_lock);
    return n;
}

int fs_ino_write(int ino, const void *data, uint32_t len) {
    spin_lock(&fs_lock);
    if (!fs_ok || ino < 0 || ino >= TIGFS_MAX_INODES || !inodes[ino].used ||
        inodes[ino].type != TIGFS_FILE) {
        spin_unlock(&fs_lock);
        return -1;
    }
    int rc = inode_replace_data(&inodes[ino], data, len);
    spin_unlock(&fs_lock);
    return rc;
}

int fs_is_dir(const char *path) {
    int ino = fs_lookup_ino(path);
    if (ino < 0)
        return -1;
    return fs_ino_type(ino) == TIGFS_DIR ? 1 : 0;
}

int fs_create(const char *path) {
    spin_lock(&fs_lock);
    path = normalize_abs(path);
    if (!fs_ok || !path) {
        spin_unlock(&fs_lock);
        return -1;
    }
    if (path_walk(path) >= 0) {
        spin_unlock(&fs_lock);
        return -1; /* exists */
    }
    char parent[PATH_MAX], base[TIGFS_NAME_LEN];
    if (split_parent(path, parent, sizeof(parent), base, sizeof(base)) != 0) {
        spin_unlock(&fs_lock);
        return -1;
    }
    int pino = path_walk(parent);
    if (pino < 0 || inodes[pino].type != TIGFS_DIR) {
        spin_unlock(&fs_lock);
        return -1;
    }
    int slot = alloc_inode_slot();
    if (slot < 0) {
        spin_unlock(&fs_lock);
        return -1;
    }
    memset(&inodes[slot], 0, sizeof(inodes[0]));
    inodes[slot].used = 1;
    inodes[slot].type = TIGFS_FILE;
    inodes[slot].nlink = 1;
    strncpy(inodes[slot].name, base, TIGFS_NAME_LEN - 1);
    if (dir_add_entry(&inodes[pino], base, (uint16_t)slot, TIGFS_FILE) != 0) {
        inodes[slot].used = 0;
        spin_unlock(&fs_lock);
        return -1;
    }
    flush_inodes();
    spin_unlock(&fs_lock);
    return 0;
}

int fs_mkdir(const char *path) {
    spin_lock(&fs_lock);
    path = normalize_abs(path);
    if (!fs_ok || !path) {
        spin_unlock(&fs_lock);
        return -1;
    }
    if (path_walk(path) >= 0) {
        spin_unlock(&fs_lock);
        return -1;
    }
    char parent[PATH_MAX], base[TIGFS_NAME_LEN];
    if (split_parent(path, parent, sizeof(parent), base, sizeof(base)) != 0) {
        spin_unlock(&fs_lock);
        return -1;
    }
    int pino = path_walk(parent);
    if (pino < 0 || inodes[pino].type != TIGFS_DIR) {
        spin_unlock(&fs_lock);
        return -1;
    }
    int slot = alloc_inode_slot();
    if (slot < 0) {
        spin_unlock(&fs_lock);
        return -1;
    }
    memset(&inodes[slot], 0, sizeof(inodes[0]));
    inodes[slot].used = 1;
    inodes[slot].type = TIGFS_DIR;
    inodes[slot].nlink = 1;
    strncpy(inodes[slot].name, base, TIGFS_NAME_LEN - 1);
    if (dir_add_entry(&inodes[pino], base, (uint16_t)slot, TIGFS_DIR) != 0) {
        inodes[slot].used = 0;
        spin_unlock(&fs_lock);
        return -1;
    }
    flush_inodes();
    spin_unlock(&fs_lock);
    return 0;
}

int fs_remove(const char *path) {
    spin_lock(&fs_lock);
    if (!fs_ok || !path || path[0] != '/' || strcmp(path, "/") == 0) {
        spin_unlock(&fs_lock);
        return -1;
    }
    int ino = path_walk(path);
    if (ino < 0) {
        spin_unlock(&fs_lock);
        return -1;
    }
    if (inodes[ino].type == TIGFS_DIR && inodes[ino].size > 0) {
        spin_unlock(&fs_lock);
        return -1; /* non-empty */
    }
    char parent[PATH_MAX], base[TIGFS_NAME_LEN];
    if (split_parent(path, parent, sizeof(parent), base, sizeof(base)) != 0) {
        spin_unlock(&fs_lock);
        return -1;
    }
    int pino = path_walk(parent);
    if (pino < 0) {
        spin_unlock(&fs_lock);
        return -1;
    }
    dir_remove_entry(&inodes[pino], base);
    free_inode_blocks(&inodes[ino]);
    memset(&inodes[ino], 0, sizeof(inodes[0]));
    flush_bitmap();
    flush_inodes();
    spin_unlock(&fs_lock);
    return 0;
}

int fs_write(const char *path, const void *data, uint32_t len) {
    int ino = fs_lookup_ino(path);
    if (ino < 0) {
        if (fs_create(path) != 0)
            return -1;
        ino = fs_lookup_ino(path);
        if (ino < 0)
            return -1;
    }
    return fs_ino_write(ino, data, len);
}

int fs_read(const char *path, void *buf, uint32_t maxlen) {
    int ino = fs_lookup_ino(path);
    if (ino < 0)
        return -1;
    return fs_ino_read(ino, 0, buf, maxlen);
}

int fs_size(const char *path) {
    int ino = fs_lookup_ino(path);
    if (ino < 0)
        return -1;
    return fs_ino_size(ino);
}

int fs_visible_size(const char *path) {
    return fs_size(path);
}

int fs_detach_name(const char *path) {
    /* hierarchical: same as unlink semantics for open files — mark UNLINKED */
    spin_lock(&fs_lock);
    int ino = path_walk(path);
    if (ino < 0) {
        spin_unlock(&fs_lock);
        return 0;
    }
    inodes[ino].type = TIGFS_UNLINKED;
    flush_inodes();
    spin_unlock(&fs_lock);
    return 1;
}

void fs_list(void) {
    spin_lock(&fs_lock);
    if (!fs_ok) {
        spin_unlock(&fs_lock);
        return;
    }
    kprintf("TigFS root listing:\n");
    for (int i = 0; i < TIGFS_MAX_INODES; i++) {
        if (!inodes[i].used || inodes[i].type == TIGFS_UNLINKED)
            continue;
        const char *t = inodes[i].type == TIGFS_DIR ? "dir " : "file";
        kprintf("  [%2d] %s %6u  %s\n", i, t, inodes[i].size, inodes[i].name);
    }
    spin_unlock(&fs_lock);
}

int fs_count(void) {
    int n = 0;
    for (int i = 0; i < TIGFS_MAX_INODES; i++)
        if (inodes[i].used && inodes[i].type != TIGFS_UNLINKED)
            n++;
    return n;
}

uint32_t fs_data_blocks(void) { return fs_ok ? super.data_count : 0; }
uint32_t fs_free_blocks(void) {
    if (!fs_ok)
        return 0;
    uint32_t free = 0;
    for (uint32_t i = 0; i < super.data_count; i++)
        if (!bittest(i))
            free++;
    return free;
}

int fs_getdents_path(const char *path, void *buf, uint32_t maxlen) {
    spin_lock(&fs_lock);
    if (!fs_ok || !buf) {
        spin_unlock(&fs_lock);
        return -1;
    }
    if (!path || !path[0])
        path = "/";
    int ino = path_walk(path);
    if (ino < 0 || inodes[ino].type != TIGFS_DIR) {
        spin_unlock(&fs_lock);
        return -1;
    }
    struct {
        char     name[28];
        uint32_t size;
    } __attribute__((packed)) *out = buf;
    uint32_t max_n = maxlen / sizeof(*out);
    uint32_t n = 0;
    uint32_t off = 0;
    while (off + sizeof(struct tigfs_dentry) <= inodes[ino].size && n < max_n) {
        struct tigfs_dentry de;
        if (inode_read_bytes(&inodes[ino], off, &de, sizeof(de)) != (int)sizeof(de))
            break;
        off += sizeof(de);
        if (!de.name[0] || de.ino >= TIGFS_MAX_INODES || !inodes[de.ino].used)
            continue;
        memset(out[n].name, 0, 28);
        strncpy(out[n].name, de.name, 27);
        out[n].size = inodes[de.ino].size;
        n++;
    }
    spin_unlock(&fs_lock);
    return (int)(n * sizeof(*out));
}

int fs_getdents(void *buf, uint32_t maxlen) {
    return fs_getdents_path("/", buf, maxlen);
}
