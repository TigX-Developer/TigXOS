/* block.c — Block device registry + sector cache (TigxOS 1.3.8) */

#include "block.h"
#include "disk.h"
#include "console.h"
#include "string.h"
#include "log.h"
#include "spinlock.h"
#include <stdint.h>

static struct block_dev g_devs[BLOCK_MAX_DEVS];
static int              g_count;
static spinlock_t       g_lock = SPINLOCK_INIT;

/* Simple direct-mapped cache: key = (dev_id, lba) */
struct cache_slot {
    int      valid;
    int      dirty;
    int      dev_id;
    uint64_t lba;
    uint8_t  data[BLOCK_SECTOR_SIZE];
};
static struct cache_slot g_cache[BLOCK_CACHE_SLOTS];

static struct cache_slot *cache_lookup(int dev_id, uint64_t lba)
{
    for (int i = 0; i < BLOCK_CACHE_SLOTS; i++)
        if (g_cache[i].valid && g_cache[i].dev_id == dev_id && g_cache[i].lba == lba)
            return &g_cache[i];
    return NULL;
}

static struct cache_slot *cache_victim(void)
{
    static int next;
    for (int n = 0; n < BLOCK_CACHE_SLOTS; n++) {
        int i = (next + n) % BLOCK_CACHE_SLOTS;
        if (!g_cache[i].valid || !g_cache[i].dirty) {
            next = (i + 1) % BLOCK_CACHE_SLOTS;
            return &g_cache[i];
        }
    }
    /* All dirty — flush first slot */
    next = (next + 1) % BLOCK_CACHE_SLOTS;
    return &g_cache[next];
}

static int cache_flush_slot(struct cache_slot *s)
{
    if (!s->valid || !s->dirty)
        return 0;
    struct block_dev *d = block_get(s->dev_id);
    if (!d || !d->write)
        return -1;
    if (d->write(d, s->lba, 1, s->data) != 0)
        return -1;
    s->dirty = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* ATA backend wrappers                                               */
/* ------------------------------------------------------------------ */

static int ata_blk_read(struct block_dev *dev, uint64_t lba, uint32_t count, void *buf)
{
    (void)dev;
    if (lba > 0xFFFFFFFFull || count > 255)
        return -1;
    return disk_read((uint32_t)lba, (uint8_t)count, buf);
}

static int ata_blk_write(struct block_dev *dev, uint64_t lba, uint32_t count, const void *buf)
{
    (void)dev;
    if (lba > 0xFFFFFFFFull || count > 255)
        return -1;
    return disk_write((uint32_t)lba, (uint8_t)count, buf);
}

/* ------------------------------------------------------------------ */
/* Public                                                             */
/* ------------------------------------------------------------------ */

void block_init(void)
{
    memset(g_devs, 0, sizeof(g_devs));
    memset(g_cache, 0, sizeof(g_cache));
    g_count = 0;
    spin_lock_init(&g_lock);

    /* Register the existing ATA primary master if present */
    if (disk_present()) {
        struct ata_disk_info info;
        disk_get_info(&info);

        struct block_dev d;
        memset(&d, 0, sizeof(d));
        d.type    = BLOCK_DEV_ATA;
        d.present = 1;
        d.sectors = info.sectors ? info.sectors : info.lba28_sectors;
        d.read    = ata_blk_read;
        d.write   = ata_blk_write;
        strncpy(d.name, "ata0", sizeof(d.name) - 1);
        block_register(&d);
        pr_info("block: registered ata0 (%llu sectors)\n",
                (unsigned long long)d.sectors);
    } else {
        pr_info("block: no ATA disk\n");
    }
}

int block_register(struct block_dev *dev)
{
    if (!dev || g_count >= BLOCK_MAX_DEVS)
        return -1;
    g_devs[g_count] = *dev;
    g_devs[g_count].id = g_count;
    g_devs[g_count].present = 1;
    return g_count++;
}

int block_dev_count(void)
{
    return g_count;
}

struct block_dev *block_get(int id)
{
    if (id < 0 || id >= g_count)
        return NULL;
    return &g_devs[id];
}

struct block_dev *block_get_by_name(const char *name)
{
    if (!name)
        return NULL;
    for (int i = 0; i < g_count; i++)
        if (strcmp(g_devs[i].name, name) == 0)
            return &g_devs[i];
    return NULL;
}

int blk_read(int dev_id, uint64_t lba, uint32_t count, void *buf)
{
    struct block_dev *d = block_get(dev_id);
    if (!d || !d->present || !d->read || !buf || count == 0)
        return -1;

    uint8_t *out = (uint8_t *)buf;

    /* Single-sector path uses the cache */
    if (count == 1) {
        spin_lock(&g_lock);
        struct cache_slot *s = cache_lookup(dev_id, lba);
        if (s) {
            memcpy(out, s->data, BLOCK_SECTOR_SIZE);
            spin_unlock(&g_lock);
            return 0;
        }
        spin_unlock(&g_lock);

        if (d->read(d, lba, 1, out) != 0)
            return -1;

        spin_lock(&g_lock);
        s = cache_victim();
        if (s->dirty)
            cache_flush_slot(s);
        s->valid  = 1;
        s->dirty  = 0;
        s->dev_id = dev_id;
        s->lba    = lba;
        memcpy(s->data, out, BLOCK_SECTOR_SIZE);
        spin_unlock(&g_lock);
        return 0;
    }

    return d->read(d, lba, count, buf);
}

int blk_write(int dev_id, uint64_t lba, uint32_t count, const void *buf)
{
    struct block_dev *d = block_get(dev_id);
    if (!d || !d->present || !d->write || !buf || count == 0)
        return -1;

    const uint8_t *in = (const uint8_t *)buf;

    if (count == 1) {
        if (d->write(d, lba, 1, in) != 0)
            return -1;
        spin_lock(&g_lock);
        struct cache_slot *s = cache_lookup(dev_id, lba);
        if (!s) {
            s = cache_victim();
            if (s->dirty)
                cache_flush_slot(s);
        }
        s->valid  = 1;
        s->dirty  = 0;
        s->dev_id = dev_id;
        s->lba    = lba;
        memcpy(s->data, in, BLOCK_SECTOR_SIZE);
        spin_unlock(&g_lock);
        return 0;
    }

    /* Multi-sector: invalidate overlapping cache lines then write through */
    spin_lock(&g_lock);
    for (uint32_t i = 0; i < count; i++) {
        struct cache_slot *s = cache_lookup(dev_id, lba + i);
        if (s)
            s->valid = 0;
    }
    spin_unlock(&g_lock);
    return d->write(d, lba, count, buf);
}

void blk_cache_invalidate(int dev_id)
{
    spin_lock(&g_lock);
    for (int i = 0; i < BLOCK_CACHE_SLOTS; i++)
        if (g_cache[i].valid && g_cache[i].dev_id == dev_id)
            g_cache[i].valid = 0;
    spin_unlock(&g_lock);
}

void block_dump(void)
{
    kprintf("=== block devices (%d) ===\n", g_count);
    for (int i = 0; i < g_count; i++) {
        struct block_dev *d = &g_devs[i];
        kprintf("  [%d] %s  type=%d  sectors=%llu\n",
                d->id, d->name, d->type,
                (unsigned long long)d->sectors);
    }
}


static int g_default = -1;

void block_set_default(int id)
{
    g_default = id;
}

int block_default_id(void)
{
    if (g_default >= 0)
        return g_default;
    return g_count > 0 ? 0 : -1;
}

int block_read_default(uint64_t lba, uint32_t count, void *buf)
{
    int id = block_default_id();
    if (id < 0) return -1;
    struct block_dev *d = block_get(id);
    if (!d || !d->read) return -1;
    return d->read(d, lba, count, buf);
}

int block_write_default(uint64_t lba, uint32_t count, const void *buf)
{
    int id = block_default_id();
    if (id < 0) return -1;
    struct block_dev *d = block_get(id);
    if (!d || !d->write) return -1;
    return d->write(d, lba, count, buf);
}
