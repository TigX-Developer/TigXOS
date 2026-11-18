// disk.h - ATA PIO (primary master, LBA28)


#ifndef DISK_H
#define DISK_H

#include <stdint.h>
#include <stddef.h>

struct ata_disk_info {
    uint8_t  present;
    char     model[41];
    char     serial[21];
    char     firmware[9];
    uint64_t sectors;
    uint32_t lba28_sectors;
    uint8_t  lba48_supported;
};

void disk_init(void);
int  disk_present(void);
int  disk_read(uint32_t lba, uint8_t count, void *buf);
int  disk_write(uint32_t lba, uint8_t count, const void *buf);
void disk_get_info(struct ata_disk_info *info);
void disk_print_info(void);

#endif
