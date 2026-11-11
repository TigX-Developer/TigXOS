// disk.c - ATA PIO primary master (LBA28)
#include "disk.h"
#include "io.h"
#include "console.h"
#include "string.h"
#include "spinlock.h"
#include <stdint.h>
#include <stddef.h>

#define ATA_DATA         0x1F0
#define ATA_ERROR        0x1F1
#define ATA_SECTOR_COUNT 0x1F2
#define ATA_LBA_LOW      0x1F3
#define ATA_LBA_MID      0x1F4
#define ATA_LBA_HIGH     0x1F5
#define ATA_DRIVE        0x1F6
#define ATA_STATUS       0x1F7
#define ATA_COMMAND      0x1F7
#define ATA_ALT_STATUS   0x3F6
#define ATA_DEV_CTRL     0x3F6

#define ATA_SR_ERR  0x01
#define ATA_SR_DRQ  0x08
#define ATA_SR_DF   0x20
#define ATA_SR_DRDY 0x40
#define ATA_SR_BSY  0x80

#define ATA_CMD_READ     0x20
#define ATA_CMD_WRITE    0x30
#define ATA_CMD_FLUSH    0xE7
#define ATA_CMD_IDENTIFY 0xEC

#define ATA_MASTER_LBA  0xE0
#define ATA_TIMEOUT     1000000
#define ATA_LBA28_MAX   0x0FFFFFFFu

static struct ata_disk_info disk_info;

/* ~400 ns: four alt-status reads (spec after drive select). */
static void ata_delay(void) {
    (void)inb(ATA_ALT_STATUS);
    (void)inb(ATA_ALT_STATUS);
    (void)inb(ATA_ALT_STATUS);
    (void)inb(ATA_ALT_STATUS);
}

static int ata_poll(uint8_t want, uint8_t want_mask) {
    for (int i = 0; i < ATA_TIMEOUT; i++) {
        uint8_t st = inb(ATA_STATUS);
        if (st == 0xFF)
            return -1;
        if (st & ATA_SR_ERR)
            return -1;
        if (st & ATA_SR_DF)
            return -1;
        if ((st & want_mask) == want)
            return 0;
    }
    return -1;
}

static int ata_wait_not_busy(void) {
    return ata_poll(0, ATA_SR_BSY);
}

static int ata_wait_drq(void) {
    for (int i = 0; i < ATA_TIMEOUT; i++) {
        uint8_t st = inb(ATA_STATUS);
        if (st == 0xFF)
            return -1;
        if (st & ATA_SR_ERR)
            return -1;
        if (st & ATA_SR_DF)
            return -1;
        if (!(st & ATA_SR_BSY) && (st & ATA_SR_DRQ))
            return 0;
    }
    return -1;
}

static void ata_select(uint32_t lba) {
    outb(ATA_DRIVE, (uint8_t)(ATA_MASTER_LBA | ((lba >> 24) & 0x0F)));
    ata_delay();
}

static void ata_copy_id_string(char *dst, const uint16_t *src, int words, int dstlen) {
    int n = 0;
    for (int i = 0; i < words && n + 1 < dstlen; i++) {
        dst[n++] = (char)(src[i] >> 8);
        if (n + 1 < dstlen)
            dst[n++] = (char)(src[i] & 0xFF);
    }
    dst[n] = 0;
    /* trim trailing spaces */
    int i = n - 1;
    while (i >= 0 && dst[i] == ' ')
        dst[i--] = 0;
    /* trim leading spaces */
    i = 0;
    while (dst[i] == ' ')
        i++;
    if (i > 0)
        memmove(dst, dst + i, (size_t)(n - i + 1));
}

static int ata_range_ok(uint32_t lba, uint8_t count) {
    if (!disk_info.present)
        return 0;
    if (count == 0)
        return 0;
    if (lba > ATA_LBA28_MAX)
        return 0;
    uint64_t end = (uint64_t)lba + count;
    if (end > disk_info.sectors)
        return 0;
    return 1;
}

static void ata_pio_read256(uint16_t *dst) {
    for (int i = 0; i < 256; i++)
        dst[i] = inw(ATA_DATA);
}

static void ata_pio_write256(const uint16_t *src) {
    for (int i = 0; i < 256; i++)
        outw(ATA_DATA, src[i]);
}

int disk_read(uint32_t lba, uint8_t count, void *buf) {
    if (!buf)
        return -1;
    if (!ata_range_ok(lba, count)) {
        kprintf("disk: read rejected (present=%d lba=%u count=%d)\n",
                disk_info.present, lba, count);
        return -1;
    }
    if ((uintptr_t)buf & 1) {
        kprintf("disk: buffer not 16-bit aligned\n");
        return -1;
    }

    unsigned long irq = irq_save();
    int rc = -1;
    if (ata_wait_not_busy() != 0)
        goto out;
    ata_select(lba);
    if (ata_wait_not_busy() != 0)
        goto out;

    outb(ATA_SECTOR_COUNT, count);
    outb(ATA_LBA_LOW,  (uint8_t)(lba));
    outb(ATA_LBA_MID,  (uint8_t)(lba >> 8));
    outb(ATA_LBA_HIGH, (uint8_t)(lba >> 16));
    outb(ATA_COMMAND, ATA_CMD_READ);

    uint16_t *dst = (uint16_t *)buf;
    for (uint8_t s = 0; s < count; s++) {
        if (ata_wait_drq() != 0) {
            kprintf("disk: read DRQ timeout sector %d\n", s);
            goto out;
        }
        ata_pio_read256(dst);
        dst += 256;
        ata_delay();
    }
    rc = 0;
out:
    irq_restore(irq);
    return rc;
}

int disk_write(uint32_t lba, uint8_t count, const void *buf) {
    if (!buf)
        return -1;
    if (!ata_range_ok(lba, count)) {
        kprintf("disk: write rejected (present=%d lba=%u count=%d)\n",
                disk_info.present, lba, count);
        return -1;
    }
    if ((uintptr_t)buf & 1) {
        kprintf("disk: buffer not 16-bit aligned\n");
        return -1;
    }

    unsigned long irq = irq_save();
    int rc = -1;
    if (ata_wait_not_busy() != 0)
        goto out;
    ata_select(lba);
    if (ata_wait_not_busy() != 0)
        goto out;

    outb(ATA_SECTOR_COUNT, count);
    outb(ATA_LBA_LOW,  (uint8_t)(lba));
    outb(ATA_LBA_MID,  (uint8_t)(lba >> 8));
    outb(ATA_LBA_HIGH, (uint8_t)(lba >> 16));
    outb(ATA_COMMAND, ATA_CMD_WRITE);

    const uint16_t *src = (const uint16_t *)buf;
    for (uint8_t s = 0; s < count; s++) {
        if (ata_wait_drq() != 0) {
            kprintf("disk: write DRQ timeout sector %d\n", s);
            goto out;
        }
        ata_pio_write256(src);
        src += 256;
        ata_delay();
    }

    if (ata_wait_not_busy() != 0)
        goto out;
    outb(ATA_COMMAND, ATA_CMD_FLUSH);
    if (ata_wait_not_busy() != 0)
        goto out;
    rc = 0;
out:
    irq_restore(irq);
    return rc;
}

static int ata_identify(void) {
    memset(&disk_info, 0, sizeof(disk_info));

    uint8_t st = inb(ATA_STATUS);
    if (st == 0xFF || st == 0x00)
        return -1;

    /* Polling: nIEN=1 so the drive does not raise IRQ 14 */
    outb(ATA_DEV_CTRL, 0x02);
    ata_delay();

    outb(ATA_DRIVE, ATA_MASTER_LBA);
    ata_delay();

    st = inb(ATA_STATUS);
    if (st == 0xFF || st == 0x00)
        return -1;

    outb(ATA_SECTOR_COUNT, 0);
    outb(ATA_LBA_LOW, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HIGH, 0);
    outb(ATA_COMMAND, ATA_CMD_IDENTIFY);
    ata_delay();

    st = inb(ATA_STATUS);
    if (st == 0x00 || st == 0xFF)
        return -1;

    /* ATAPI devices abort IDENTIFY and put signature in LBA mid/high */
    uint8_t mid = inb(ATA_LBA_MID);
    uint8_t hi  = inb(ATA_LBA_HIGH);
    if (mid != 0 || hi != 0)
        return -1; /* ATAPI / not ATA */

    if (ata_wait_drq() != 0)
        return -1;

    uint16_t id[256];
    ata_pio_read256(id);

    ata_copy_id_string(disk_info.model,    &id[27], 20, 41);
    ata_copy_id_string(disk_info.serial,   &id[10], 10, 21);
    ata_copy_id_string(disk_info.firmware, &id[23],  4,  9);

    /* Word 49 bit 9 = LBA supported */
    if (!(id[49] & (1u << 9)))
        return -1;

    disk_info.lba28_sectors = (uint32_t)id[60] | ((uint32_t)id[61] << 16);

    /* Word 83 is valid if bit 14 is 1 and bit 15 is 0; bit 10 = LBA48 */
    if ((id[83] & 0xC000) == 0x4000)
        disk_info.lba48_supported = (id[83] & (1u << 10)) ? 1 : 0;

    if (disk_info.lba48_supported) {
        disk_info.sectors =
            (uint64_t)id[100] |
            ((uint64_t)id[101] << 16) |
            ((uint64_t)id[102] << 32) |
            ((uint64_t)id[103] << 48);
    } else {
        disk_info.sectors = disk_info.lba28_sectors;
    }

    if (disk_info.sectors == 0)
        return -1;

    disk_info.present = 1;
    return 0;
}

void disk_init(void) {
    kprintf("ATA: probing primary master (PIO, LBA28)...\n");
    if (ata_identify() == 0) {
        kprintf("ATA: drive found\n");
        disk_print_info();
    } else {
        kprintf("ATA: no ATA disk on primary master\n");
        memset(&disk_info, 0, sizeof(disk_info));
    }
}

int disk_present(void) {
    return disk_info.present;
}

void disk_get_info(struct ata_disk_info *info) {
    if (info)
        memcpy(info, &disk_info, sizeof(*info));
}

void disk_print_info(void) {
    if (!disk_info.present) {
        kprintf("No disk present\n");
        return;
    }
    uint64_t mb = (disk_info.sectors * 512ull) / (1024ull * 1024ull);
    kprintf("\n=== Disk (primary master) ===\n");
    kprintf("Model:    %s\n", disk_info.model);
    kprintf("Serial:   %s\n", disk_info.serial);
    kprintf("Firmware: %s\n", disk_info.firmware);
    kprintf("Sectors:  %u (%u MB)\n",
            (unsigned)disk_info.sectors, (unsigned)mb);
    kprintf("LBA28:    %u sectors\n", disk_info.lba28_sectors);
    kprintf("LBA48:    %s (size only; I/O is LBA28)\n",
            disk_info.lba48_supported ? "yes" : "no");
    kprintf("=============================\n\n");
}
