// fs.c - TigFS v1: superblock + inodes + bitmap, LBA 512+
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
#define TIGFS_RESERVED  512u        /* LBA 0–511: MBR + kernel */
#define TIGFS_MAX_INODES 64
#define TIGFS_INODE_BYTES 512

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
    uint16_t pad;
    uint32_t size;
    uint32_t block[TIGFS_DIRECT];
    char     name[TIGFS_NAME_LEN];
    uint8_t  extra[28]; /* pad inode to 512 (DIRECT=112) */
};

_Static_assert(sizeof(struct tigfs_inode) == TIGFS_INODE_BYTES, "inode size");
_Static_assert(sizeof(struct tigfs_super) <= 512, "super fits in a sector");

static struct tigfs_super super;
static struct tigfs_inode inodes[TIGFS_MAX_INODES];
static uint8_t bitmap[512];
static int fs_ok = 0;

static uint8_t sector[512] __attribute__((aligned(2)));

/* Serialize all FS ops: in-memory inodes/bitmap + shared sector[] scratch. */
static spinlock_t fs_lock = SPINLOCK_INIT;

static int read_lba(uint32_t lba, void *buf) {
    return disk_read(lba, 1, buf);
}

static int write_lba(uint32_t lba, const void *buf) {
    return disk_write(lba, 1, buf);
}

static int bittest(uint32_t i) {
    return (bitmap[i / 8] >> (i % 8)) & 1;
}

static void bitset(uint32_t i) {
    bitmap[i / 8] |= (uint8_t)(1u << (i % 8));
}

static void bitclr(uint32_t i) {
    bitmap[i / 8] &= (uint8_t)~(1u << (i % 8));
}

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

static int flush_bitmap(void) {
    return write_lba(super.bitmap_lba, bitmap);
}

static int flush_inodes(void) {
    /* 64 inodes × 512 bytes = 32768 = 64 sectors */
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

/* Validate every used inode: size and block LBAs must lie in the data region. */
static int validate_inodes(void) {
    uint32_t data_lo = super.data_lba;
    uint32_t data_hi = super.data_lba + super.data_count; /* exclusive */
    uint32_t max_size = (uint32_t)TIGFS_DIRECT * 512u;

    for (int i = 0; i < TIGFS_MAX_INODES; i++) {
        struct tigfs_inode *in = &inodes[i];
        if (!in->used)
            continue;
        if (in->size > max_size)
            return -1;
        /* Blocks covering the size must be non-zero and in range;
         * trailing zeros after the last used block are fine. */
        uint32_t need = (in->size + 511u) / 512u;
        if (need > (uint32_t)TIGFS_DIRECT)
            return -1;
        for (uint32_t b = 0; b < (uint32_t)TIGFS_DIRECT; b++) {
            uint32_t lba = in->block[b];
            if (b < need) {
                if (lba < data_lo || lba >= data_hi)
                    return -1;
            } else {
                /* spare slots should be zero; tolerate non-zero only if in range */
                if (lba != 0 && (lba < data_lo || lba >= data_hi))
                    return -1;
            }
        }
    }
    return 0;
}

static struct tigfs_inode *find_inode(const char *name) {
    if (!name || !name[0]) return NULL;
    for (int i = 0; i < TIGFS_MAX_INODES; i++) {
        if (inodes[i].used && inodes[i].type != TIGFS_UNLINKED &&
            strcmp(inodes[i].name, name) == 0)
            return &inodes[i];
    }
    return NULL;
}

/* Like find_inode but includes UNLINKED (for final fs_remove after detach). */
static struct tigfs_inode *find_inode_any(const char *name) {
    if (!name || !name[0]) return NULL;
    for (int i = 0; i < TIGFS_MAX_INODES; i++) {
        if (inodes[i].used && strcmp(inodes[i].name, name) == 0)
            return &inodes[i];
    }
    return NULL;
}


static struct tigfs_inode *alloc_inode(void) {
    for (int i = 0; i < TIGFS_MAX_INODES; i++) {
        if (!inodes[i].used)
            return &inodes[i];
    }
    return NULL;
}

int fs_format(void) {
    struct ata_disk_info di;
    disk_get_info(&di);
    if (!di.present || di.sectors <= TIGFS_RESERVED + 80) {
        kprintf("fs: disk too small to format\n");
        return -1;
    }

    memset(&super, 0, sizeof(super));
    super.magic = TIGFS_MAGIC;
    super.version = TIGFS_VERSION;
    super.disk_sectors = (uint32_t)di.sectors;
    super.reserved = TIGFS_RESERVED;
    super.bitmap_lba = TIGFS_RESERVED + 1;
    super.inode_lba = TIGFS_RESERVED + 2;
    super.inode_count = TIGFS_MAX_INODES;
    super.data_lba = TIGFS_RESERVED + 2 + 64; /* 64 inode sectors @ 512B */
    /* Reject disks too small to host metadata + at least 1 data block */
    if (super.disk_sectors <= super.data_lba) {
        kprintf("fs: disk too small (sectors=%u data_lba=%u)\n",
                super.disk_sectors, super.data_lba);
        return -1;
    }
    super.data_count = super.disk_sectors - super.data_lba;
    if (super.data_count > 4096)
        super.data_count = 4096; /* one bitmap sector */
    if (super.data_count == 0)
        return -1;

    memset(inodes, 0, sizeof(inodes));
    memset(bitmap, 0, sizeof(bitmap));

    if (flush_super() != 0 || flush_bitmap() != 0 || flush_inodes() != 0) {
        kprintf("fs: format write failed\n");
        fs_ok = 0;
        return -1;
    }
    fs_ok = 1;
    kprintf("fs: formatted TigFS (%u data blocks)\n", super.data_count);

    const char *msg =
        "TigxOS V" TIGX_VERSION " TigFS\n"
        "Simple flat filesystem. Boot+kernel occupy LBA 0-511.\n"
        "Keys: f=list  r=read README  w=write sample\n";
    fs_write("README", msg, (uint32_t)strlen(msg));
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
    /* Geometry must fit on the physical disk */
    {
        struct ata_disk_info di;
        disk_get_info(&di);
        if (!di.present || di.sectors == 0)
            return -1;
        if ((uint64_t)super.disk_sectors > di.sectors)
            return -1;
        if ((uint64_t)super.data_lba >= di.sectors)
            return -1;
        if ((uint64_t)super.data_lba + (uint64_t)super.data_count > di.sectors)
            return -1;
        if ((uint64_t)super.bitmap_lba >= di.sectors ||
            (uint64_t)super.inode_lba >= di.sectors)
            return -1;
    }
    if (read_lba(super.bitmap_lba, bitmap) != 0)
        return -1;
    if (load_inodes() != 0)
        return -1;
    if (validate_inodes() != 0) {
        kprintf("fs: corrupt inode metadata — mount refused\n");
        return -1;
    }
    fs_ok = 1;
    return 0;
}

void fs_init(void) {
    fs_ok = 0;
    memset(&super, 0, sizeof(super));
    memset(inodes, 0, sizeof(inodes));
    memset(bitmap, 0, sizeof(bitmap));

    if (!disk_present()) {
        kprintf("fs: no disk, skipped\n");
        return;
    }
    kprintf("fs: mounting TigFS at LBA %u...\n", TIGFS_RESERVED);
    if (fs_mount() == 0) {
        kprintf("fs: mounted (%d files, %u data blocks)\n",
                fs_count(), super.data_count);
        return;
    }

    /*
     * Mount failed. Only auto-format a *blank* superblock sector
     * (all zeros). Refuse to destroy data on I/O error, wrong magic,
     * version mismatch, or any non-zero content.
     */
    uint8_t probe[512];
    if (read_lba(TIGFS_RESERVED, probe) != 0) {
        kprintf("fs: superblock read failed — not formatting\n");
        return;
    }
    int blank = 1;
    for (int i = 0; i < 512; i++) {
        if (probe[i] != 0) {
            blank = 0;
            break;
        }
    }
    if (!blank) {
        uint32_t magic = 0;
        memcpy(&magic, probe, 4);
        kprintf("fs: unrecognized FS (magic=0x%x) — not auto-formatting\n",
                (unsigned)magic);
        return;
    }
    kprintf("fs: blank disk, formatting...\n");
    if (fs_format() != 0)
        return;
}



int fs_detach_name(const char *name) {
    spin_lock(&fs_lock);
    if (!fs_ok || !name || !name[0]) {
        spin_unlock(&fs_lock);
        return -1;
    }
    struct tigfs_inode *in = find_inode(name);
    if (!in) {
        spin_unlock(&fs_lock);
        return -1;
    }
    /* Keep name for deferred fs_remove; hide from lookup/dirents. */
    in->type = TIGFS_UNLINKED;
    int rc = flush_inodes();
    spin_unlock(&fs_lock);
    return rc;
}


int fs_ready(void) {
    return fs_ok;
}

int fs_count(void) {
    spin_lock(&fs_lock);
    int n = 0;
    if (fs_ok) {
        for (int i = 0; i < TIGFS_MAX_INODES; i++)
            if (inodes[i].used && inodes[i].type != TIGFS_UNLINKED) n++;
    }
    spin_unlock(&fs_lock);
    return n;
}

void fs_list(void) {
    spin_lock(&fs_lock);
    if (!fs_ok) {
        spin_unlock(&fs_lock);
        kprintf("fs: not mounted\n");
        return;
    }
    kprintf("\nNAME                          SIZE\n");
    int n = 0;
    for (int i = 0; i < TIGFS_MAX_INODES; i++) {
        if (!inodes[i].used || inodes[i].type == TIGFS_UNLINKED) continue;
        kprintf("%s", inodes[i].name);
        int pad = TIGFS_NAME_LEN - (int)strlen(inodes[i].name);
        while (pad-- > 0)
            console_putchar(' ');
        kprintf("  %u\n", inodes[i].size);
        n++;
    }
    if (!n)
        kprintf("(empty)\n");
    kprintf("(%d files)\n\n", n);
    spin_unlock(&fs_lock);
}

int fs_create(const char *name) {
    spin_lock(&fs_lock);
    if (!fs_ok || !name || !name[0] || strlen(name) >= TIGFS_NAME_LEN) {
        spin_unlock(&fs_lock);
        return -1;
    }
    if (find_inode(name)) {
        spin_unlock(&fs_lock);
        return -1;
    }
    struct tigfs_inode *in = alloc_inode();
    if (!in) {
        spin_unlock(&fs_lock);
        return -1;
    }
    memset(in, 0, sizeof(*in));
    in->used = 1;
    in->type = TIGFS_FILE;
    strncpy(in->name, name, TIGFS_NAME_LEN - 1);
    int rc = flush_inodes();
    spin_unlock(&fs_lock);
    return rc;
}

int fs_remove(const char *name) {
    spin_lock(&fs_lock);
    if (!fs_ok) {
        spin_unlock(&fs_lock);
        return -1;
    }
    struct tigfs_inode *in = find_inode_any(name);
    if (!in) {
        spin_unlock(&fs_lock);
        return -1;
    }
    free_inode_blocks(in);
    memset(in, 0, sizeof(*in));
    int rc = (flush_bitmap() != 0 || flush_inodes() != 0) ? -1 : 0;
    spin_unlock(&fs_lock);
    return rc;
}

int fs_write(const char *name, const void *data, uint32_t len) {
    spin_lock(&fs_lock);
    if (!fs_ok || !data) {
        spin_unlock(&fs_lock);
        return -1;
    }
    if (len > TIGFS_DIRECT * 512) {
        spin_unlock(&fs_lock);
        return -1;
    }
    /* find_inode_any: keep writing through unlink-while-open (UNLINKED). */
    struct tigfs_inode *in = find_inode_any(name);
    if (!in) {
        /* create under same lock (avoid nested public fs_create) */
        if (!name || !name[0] || strlen(name) >= TIGFS_NAME_LEN) {
            spin_unlock(&fs_lock);
            return -1;
        }
        in = alloc_inode();
        if (!in) {
            spin_unlock(&fs_lock);
            return -1;
        }
        memset(in, 0, sizeof(*in));
        in->used = 1;
        in->type = TIGFS_FILE;
        strncpy(in->name, name, TIGFS_NAME_LEN - 1);
        if (flush_inodes() != 0) {
            spin_unlock(&fs_lock);
            return -1;
        }
    }
    /* Allocate + write into NEW blocks first. Only free the old blocks
     * after the new image is fully on disk — avoids empty-file data loss
     * when we run out of space mid-write. */
    uint32_t new_blocks[TIGFS_DIRECT];
    int nnew = 0;
    for (int i = 0; i < TIGFS_DIRECT; i++)
        new_blocks[i] = 0;

    const uint8_t *src = (const uint8_t *)data;
    uint32_t left = len;
    while (left) {
        int idx = alloc_block();
        if (idx < 0) {
            for (int i = 0; i < nnew; i++) {
                if (new_blocks[i]) {
                    uint32_t bi = new_blocks[i] - super.data_lba;
                    if (bi < super.data_count)
                        bitmap[bi / 8] &= (uint8_t)~(1u << (bi % 8));
                    new_blocks[i] = 0;
                }
            }
            flush_bitmap();
            kprintf("fs: out of space (write aborted, old data kept)\n");
            spin_unlock(&fs_lock);
            return -1;
        }
        uint32_t lba = super.data_lba + (uint32_t)idx;
        memset(sector, 0, 512);
        uint32_t chunk = left > 512 ? 512 : left;
        memcpy(sector, src, chunk);
        if (write_lba(lba, sector) != 0) {
            for (int i = 0; i < nnew; i++) {
                if (new_blocks[i]) {
                    uint32_t bi = new_blocks[i] - super.data_lba;
                    if (bi < super.data_count)
                        bitmap[bi / 8] &= (uint8_t)~(1u << (bi % 8));
                }
            }
            {
                uint32_t bi = lba - super.data_lba;
                if (bi < super.data_count)
                    bitmap[bi / 8] &= (uint8_t)~(1u << (bi % 8));
            }
            flush_bitmap();
            spin_unlock(&fs_lock);
            return -1;
        }
        new_blocks[nnew++] = lba;
        src += chunk;
        left -= chunk;
    }
    free_inode_blocks(in);
    for (int i = 0; i < TIGFS_DIRECT; i++)
        in->block[i] = (i < nnew) ? new_blocks[i] : 0;
    in->size = len;
    int rc = (flush_bitmap() != 0 || flush_inodes() != 0) ? -1 : 0;
    spin_unlock(&fs_lock);
    return rc;
}

int fs_read(const char *name, void *buf, uint32_t maxlen) {
    spin_lock(&fs_lock);
    if (!fs_ok || !buf) {
        spin_unlock(&fs_lock);
        return -1;
    }
    struct tigfs_inode *in = find_inode_any(name);
    if (!in) {
        spin_unlock(&fs_lock);
        return -1;
    }
    uint32_t want = in->size;
    if (want > maxlen) want = maxlen;
    uint8_t *dst = (uint8_t *)buf;
    uint32_t done = 0;
    for (int b = 0; b < TIGFS_DIRECT && done < want; b++) {
        uint32_t lba = in->block[b];
        if (!lba) break;
        /* Defensive: reject LBAs outside the data region */
        if (lba < super.data_lba ||
            lba >= super.data_lba + super.data_count) {
            spin_unlock(&fs_lock);
            return -1;
        }
        if (read_lba(lba, sector) != 0) {
            spin_unlock(&fs_lock);
            return -1;
        }
        uint32_t chunk = want - done;
        if (chunk > 512) chunk = 512;
        memcpy(dst + done, sector, chunk);
        done += chunk;
    }
    spin_unlock(&fs_lock);
    return (int)done;
}

int fs_size(const char *name) {
    spin_lock(&fs_lock);
    if (!fs_ok) {
        spin_unlock(&fs_lock);
        return -1;
    }
    struct tigfs_inode *in = find_inode_any(name);
    if (!in) {
        spin_unlock(&fs_lock);
        return -1;
    }
    int sz = (int)in->size;
    spin_unlock(&fs_lock);
    return sz;
}

int fs_visible_size(const char *name) {
    spin_lock(&fs_lock);
    if (!fs_ok) {
        spin_unlock(&fs_lock);
        return -1;
    }
    struct tigfs_inode *in = find_inode(name);
    if (!in) {
        spin_unlock(&fs_lock);
        return -1;
    }
    int sz = (int)in->size;
    spin_unlock(&fs_lock);
    return sz;
}

int fs_getdents(void *buf, uint32_t maxlen) {
    spin_lock(&fs_lock);
    if (!fs_ok || !buf) {
        spin_unlock(&fs_lock);
        return -1;
    }
    struct {
        char     name[28];
        uint32_t size;
    } __attribute__((packed)) *out = buf;
    uint32_t max_n = maxlen / sizeof(*out);
    uint32_t n = 0;
    for (int i = 0; i < TIGFS_MAX_INODES && n < max_n; i++) {
        if (!inodes[i].used || inodes[i].type == TIGFS_UNLINKED)
            continue;
        memset(out[n].name, 0, 28);
        strncpy(out[n].name, inodes[i].name, 27);
        out[n].size = inodes[i].size;
        n++;
    }
    spin_unlock(&fs_lock);
    return (int)(n * sizeof(*out));
}

/* Path resolve for flat TigFS (v1.1.2/1.1.3).
 * Returns a normalized absolute path (always starts with /).
 * Handles ., .., and // . Relative paths are joined to cwd. */
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
        while (cwd[cl] && cl < sizeof(tmp) - 1) {
            tmp[cl] = cwd[cl];
            cl++;
        }
        /* cwd itself must fit */
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
                return -1; /* would truncate — reject */
            tmp[n++] = *path++;
        }
        tmp[n] = '\0';
    } else {
        while (*path) {
            if (n >= sizeof(tmp) - 1)
                return -1; /* would truncate — reject */
            tmp[n++] = *path++;
        }
        tmp[n] = '\0';
    }

    /* Split into components, apply . and .. (max 16) */
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
        if (len == 0) {
            /* skip */
        } else if (len == 1 && tmp[start] == '.') {
            /* skip . */
        } else if (len == 2 && tmp[start] == '.' && tmp[start + 1] == '.') {
            if (nc > 0)
                nc--;
        } else {
            if (nc >= 16)
                return -1; /* too many components */
            if (len >= PATH_MAX)
                return -1; /* component too long */
            for (size_t k = 0; k < len; k++)
                comps[nc][k] = tmp[start + k];
            comps[nc][len] = '\0';
            nc++;
        }
        while (tmp[i] == '/')
            i++;
    }

    /* Rebuild absolute path — reject if it would not fit outlen */
    char norm[PATH_MAX];
    size_t j = 0;
    if (nc == 0) {
        norm[0] = '/';
        norm[1] = '\0';
        j = 1;
    } else {
        for (int c = 0; c < nc; c++) {
            if (j >= sizeof(norm) - 1)
                return -1;
            norm[j++] = '/';
            for (size_t k = 0; comps[c][k]; k++) {
                if (j >= sizeof(norm) - 1)
                    return -1;
                norm[j++] = comps[c][k];
            }
        }
        norm[j] = '\0';
    }

    if (j >= outlen)
        return -1;
    for (size_t k = 0; k <= j; k++)
        out[k] = norm[k];
    return 0;
}
