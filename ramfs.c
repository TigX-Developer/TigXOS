/* ramfs.c — simple RAM disk for /tmp (TigxOS 1.2.1)
 *
 * Flat-ish hierarchy under a fixed root inode. Files hold up to
 * RAMFS_MAX_FILE bytes from a shared data pool. Directories keep a
 * short child-inode list. No persistence across reboot.
 */
#include "ramfs.h"
#include "string.h"
#include "console.h"
#include "spinlock.h"

#define RAMFS_FILE 1
#define RAMFS_DIR  2
#define RAMFS_ROOT 0

struct ramfs_inode {
    uint8_t  used;
    uint8_t  type;
    int16_t  parent;
    uint32_t size;
    char     name[RAMFS_NAME_LEN];
    int16_t  children[RAMFS_MAX_CHILDREN];
    uint8_t  nchildren;
    uint8_t *data;
};

static struct ramfs_inode nodes[RAMFS_MAX_INODES];
static uint8_t pool[RAMFS_DATA_POOL];
static uint32_t pool_used;
static spinlock_t ram_lock = SPINLOCK_INIT;
static int ready;

static int alloc_data(uint32_t n) {
    if (n > RAMFS_MAX_FILE)
        return -1;
    if (pool_used + n > RAMFS_DATA_POOL)
        return -1;
    uint32_t off = pool_used;
    pool_used += n;
    /* keep pool_used aligned to 16 for neatness */
    pool_used = (pool_used + 15u) & ~15u;
    return (int)off;
}

void ramfs_init(void) {
    memset(nodes, 0, sizeof(nodes));
    memset(pool, 0, sizeof(pool));
    pool_used = 0;
    nodes[RAMFS_ROOT].used = 1;
    nodes[RAMFS_ROOT].type = RAMFS_DIR;
    nodes[RAMFS_ROOT].parent = -1;
    strcpy(nodes[RAMFS_ROOT].name, "tmp");
    ready = 1;
    kprintf("ramfs: /tmp ready (%d inodes, %u KB pool)\n",
            RAMFS_MAX_INODES, (unsigned)(RAMFS_DATA_POOL / 1024));
}

/* Strip leading /tmp or /tmp/ → relative path (may be empty for root). */
static const char *strip_tmp(const char *path) {
    if (!path)
        return 0;
    if (path[0] == '/' && path[1] == 't' && path[2] == 'm' && path[3] == 'p') {
        if (path[4] == 0)
            return "";
        if (path[4] == '/')
            return path + 5;
    }
    /* already relative */
    if (path[0] == '/')
        return 0;
    return path;
}

static int find_child(int pino, const char *name) {
    struct ramfs_inode *p = &nodes[pino];
    for (int i = 0; i < p->nchildren; i++) {
        int c = p->children[i];
        if (c >= 0 && nodes[c].used && strcmp(nodes[c].name, name) == 0)
            return c;
    }
    return -1;
}

static int path_walk(const char *rel) {
    if (!ready)
        return -1;
    if (!rel || rel[0] == 0)
        return RAMFS_ROOT;
    int cur = RAMFS_ROOT;
    const char *p = rel;
    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        char name[RAMFS_NAME_LEN];
        int n = 0;
        while (*p && *p != '/' && n < RAMFS_NAME_LEN - 1)
            name[n++] = *p++;
        name[n] = 0;
        if (n == 0)
            continue;
        if (strcmp(name, ".") == 0)
            continue;
        if (strcmp(name, "..") == 0) {
            if (nodes[cur].parent >= 0)
                cur = nodes[cur].parent;
            continue;
        }
        int c = find_child(cur, name);
        if (c < 0)
            return -1;
        cur = c;
    }
    return cur;
}

static int alloc_inode(void) {
    for (int i = 1; i < RAMFS_MAX_INODES; i++) {
        if (!nodes[i].used) {
            memset(&nodes[i], 0, sizeof(nodes[i]));
            nodes[i].used = 1;
            nodes[i].parent = -1;
            return i;
        }
    }
    return -1;
}

static int add_child(int pino, int cino) {
    struct ramfs_inode *p = &nodes[pino];
    if (p->nchildren >= RAMFS_MAX_CHILDREN)
        return -1;
    p->children[p->nchildren++] = (int16_t)cino;
    nodes[cino].parent = (int16_t)pino;
    return 0;
}

static void remove_child(int pino, int cino) {
    struct ramfs_inode *p = &nodes[pino];
    for (int i = 0; i < p->nchildren; i++) {
        if (p->children[i] == cino) {
            for (int j = i; j < p->nchildren - 1; j++)
                p->children[j] = p->children[j + 1];
            p->nchildren--;
            break;
        }
    }
}

/* Split "/tmp/a/b" or "a/b" into parent path + basename. */
static int split(const char *path, char *parent, size_t psz, char *base, size_t bsz) {
    const char *rel = strip_tmp(path);
    if (!rel)
        return -1;
    /* find last component */
    const char *last = rel; (void)last;
    const char *slash = 0;
    for (const char *p = rel; *p; p++)
        if (*p == '/')
            slash = p;
    if (!slash) {
        if (psz)
            parent[0] = 0;
        if (strlen(rel) >= bsz)
            return -1;
        strcpy(base, rel);
        return 0;
    }
    size_t plen = (size_t)(slash - rel);
    if (plen >= psz || strlen(slash + 1) >= bsz || slash[1] == 0)
        return -1;
    memcpy(parent, rel, plen);
    parent[plen] = 0;
    strcpy(base, slash + 1);
    return 0;
}

int ramfs_lookup(const char *path) {
    const char *rel = strip_tmp(path);
    if (!rel)
        return -1;
    spin_lock(&ram_lock);
    int ino = path_walk(rel);
    spin_unlock(&ram_lock);
    return ino;
}

int ramfs_create(const char *path, int is_dir) {
    char parent[64], base[RAMFS_NAME_LEN];
    if (split(path, parent, sizeof(parent), base, sizeof(base)) != 0)
        return -1;
    if (base[0] == 0 || strcmp(base, ".") == 0 || strcmp(base, "..") == 0)
        return -1;
    if (strlen(base) >= RAMFS_NAME_LEN)
        return -1;

    spin_lock(&ram_lock);
    int pino = path_walk(parent);
    if (pino < 0 || nodes[pino].type != RAMFS_DIR) {
        spin_unlock(&ram_lock);
        return -1;
    }
    if (find_child(pino, base) >= 0) {
        spin_unlock(&ram_lock);
        return -1; /* exists */
    }
    int ino = alloc_inode();
    if (ino < 0) {
        spin_unlock(&ram_lock);
        return -1;
    }
    nodes[ino].type = is_dir ? RAMFS_DIR : RAMFS_FILE;
    nodes[ino].size = 0;
    nodes[ino].data = 0;
    strcpy(nodes[ino].name, base);
    if (add_child(pino, ino) != 0) {
        nodes[ino].used = 0;
        spin_unlock(&ram_lock);
        return -1;
    }
    spin_unlock(&ram_lock);
    return ino;
}

int ramfs_remove(const char *path) {
    const char *rel = strip_tmp(path);
    if (!rel || rel[0] == 0)
        return -1; /* cannot remove /tmp root */
    spin_lock(&ram_lock);
    int ino = path_walk(rel);
    if (ino < 0 || ino == RAMFS_ROOT) {
        spin_unlock(&ram_lock);
        return -1;
    }
    if (nodes[ino].type == RAMFS_DIR && nodes[ino].nchildren > 0) {
        spin_unlock(&ram_lock);
        return -1; /* non-empty */
    }
    int pino = nodes[ino].parent;
    if (pino >= 0)
        remove_child(pino, ino);
    nodes[ino].used = 0;
    nodes[ino].data = 0;
    nodes[ino].size = 0;
    spin_unlock(&ram_lock);
    return 0;
}

int ramfs_stat(const char *path, uint32_t *size, int *is_dir, int *ino_out) {
    int ino = ramfs_lookup(path);
    if (ino < 0)
        return -1;
    spin_lock(&ram_lock);
    if (size)
        *size = nodes[ino].size;
    if (is_dir)
        *is_dir = (nodes[ino].type == RAMFS_DIR);
    if (ino_out)
        *ino_out = ino;
    spin_unlock(&ram_lock);
    return 0;
}

int ramfs_is_dir(const char *path) {
    int isdir = 0;
    if (ramfs_stat(path, 0, &isdir, 0) != 0)
        return 0;
    return isdir;
}

int ramfs_ino_type(int ino) {
    if (ino < 0 || ino >= RAMFS_MAX_INODES || !nodes[ino].used)
        return -1;
    return nodes[ino].type == RAMFS_DIR ? 2 : 1;
}

int ramfs_ino_size(int ino) {
    if (ino < 0 || ino >= RAMFS_MAX_INODES || !nodes[ino].used)
        return -1;
    return (int)nodes[ino].size;
}

int ramfs_read(int ino, uint32_t offset, void *buf, uint32_t len) {
    if (!buf || ino < 0 || ino >= RAMFS_MAX_INODES)
        return -1;
    spin_lock(&ram_lock);
    if (!nodes[ino].used || nodes[ino].type != RAMFS_FILE) {
        spin_unlock(&ram_lock);
        return -1;
    }
    if (offset >= nodes[ino].size) {
        spin_unlock(&ram_lock);
        return 0;
    }
    uint32_t avail = nodes[ino].size - offset;
    if (len > avail)
        len = avail;
    if (len && nodes[ino].data)
        memcpy(buf, nodes[ino].data + offset, len);
    spin_unlock(&ram_lock);
    return (int)len;
}

int ramfs_write_replace(int ino, const void *data, uint32_t len) {
    if (ino < 0 || ino >= RAMFS_MAX_INODES)
        return -1;
    if (len > RAMFS_MAX_FILE)
        return -1;
    spin_lock(&ram_lock);
    if (!nodes[ino].used || nodes[ino].type != RAMFS_FILE) {
        spin_unlock(&ram_lock);
        return -1;
    }
    if (len == 0) {
        nodes[ino].size = 0;
        /* keep old data pointer; content ignored */
        spin_unlock(&ram_lock);
        return 0;
    }
    if (!nodes[ino].data || nodes[ino].size < len) {
        int off = alloc_data(len);
        if (off < 0) {
            spin_unlock(&ram_lock);
            return -1;
        }
        nodes[ino].data = pool + off;
    }
    memcpy(nodes[ino].data, data, len);
    nodes[ino].size = len;
    spin_unlock(&ram_lock);
    return 0;
}

int ramfs_getdents(const char *path, void *buf, uint32_t maxlen) {
    if (!buf || maxlen < 32)
        return -1;
    int ino = ramfs_lookup(path);
    if (ino < 0)
        return -1;
    spin_lock(&ram_lock);
    if (nodes[ino].type != RAMFS_DIR) {
        spin_unlock(&ram_lock);
        return -1;
    }
    /* Pack same layout as TigFS dirent: name[28] + ino u16 + type u16 */
    uint8_t *out = (uint8_t *)buf;
    uint32_t off = 0;
    for (int i = 0; i < nodes[ino].nchildren; i++) {
        int c = nodes[ino].children[i];
        if (c < 0 || !nodes[c].used)
            continue;
        if (off + 32 > maxlen)
            break;
        memset(out + off, 0, 32);
        strncpy((char *)(out + off), nodes[c].name, RAMFS_NAME_LEN - 1);
        uint16_t cino = (uint16_t)c;
        uint16_t typ = (nodes[c].type == RAMFS_DIR) ? 3 : 1;
        memcpy(out + off + 28, &cino, 2);
        memcpy(out + off + 30, &typ, 2);
        off += 32;
    }
    spin_unlock(&ram_lock);
    return (int)off;
}
