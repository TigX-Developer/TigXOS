/* block.h — Block device layer (TigxOS 1.3.8)
 *
 * Thin abstraction over disk backends (ATA today, AHCI later).
 * Optional small sector cache to cut repeated superblock/inode traffic.
 */
#ifndef TIGX_BLOCK_H
#define TIGX_BLOCK_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLOCK_SECTOR_SIZE  512
#define BLOCK_CACHE_SLOTS  32

enum block_dev_type {
    BLOCK_DEV_NONE = 0,
    BLOCK_DEV_ATA  = 1,
    BLOCK_DEV_AHCI = 2,     /* reserved — probe only for 1.3.3 */
    BLOCK_DEV_RAM  = 3
};

struct block_dev {
    int          id;
    int          type;
    int          present;
    uint64_t     sectors;       /* total 512-byte sectors */
    char         name[32];
    /* Ops: return 0 on success, -1 on error */
    int        (*read)(struct block_dev *dev, uint64_t lba, uint32_t count, void *buf);
    int        (*write)(struct block_dev *dev, uint64_t lba, uint32_t count, const void *buf);
    void        *priv;
};

#define BLOCK_MAX_DEVS  8

void block_init(void);

int  block_register(struct block_dev *dev);     /* copies; assigns id; returns id or -1 */
int  block_dev_count(void);
struct block_dev *block_get(int id);
struct block_dev *block_get_by_name(const char *name);

/* Sector I/O with optional cache (count is in sectors). */
int  blk_read(int dev_id, uint64_t lba, uint32_t count, void *buf);
int  blk_write(int dev_id, uint64_t lba, uint32_t count, const void *buf);

/* Invalidate cache entries for a device (e.g. after format). */
void blk_cache_invalidate(int dev_id);

void block_dump(void);
void block_set_default(int id);
int  block_default_id(void);
int  block_read_default(uint64_t lba, uint32_t count, void *buf);
int  block_write_default(uint64_t lba, uint32_t count, const void *buf);

#ifdef __cplusplus
}
#endif

#endif /* TIGX_BLOCK_H */
