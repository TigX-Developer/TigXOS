/* part.c — MBR + GPT partition scanning (TigxOS 1.3.8) */

#include "part.h"
#include "block.h"
#include "console.h"
#include "string.h"
#include "log.h"
#include <stdint.h>

static struct partition g_parts[PART_MAX];
static int             g_count;

/* ------------------------------------------------------------------ */
/* MBR                                                                */
/* ------------------------------------------------------------------ */

struct mbr_entry {
    uint8_t  status;
    uint8_t  chs_first[3];
    uint8_t  type;
    uint8_t  chs_last[3];
    uint32_t lba_start;
    uint32_t lba_count;
} __attribute__((packed));

struct mbr {
    uint8_t  bootstrap[446];
    struct mbr_entry entry[4];
    uint16_t signature;
} __attribute__((packed));

_Static_assert(sizeof(struct mbr) == 512, "mbr size");

static int mbr_type_to_part(uint8_t t)
{
    switch (t) {
    case 0x0B: case 0x0C: case 0x1B: case 0x1C:
        return PART_TYPE_FAT32;
    case 0x07:
        return PART_TYPE_NTFS;
    case 0x83:
        return PART_TYPE_LINUX;
    case 0xEF:
        return PART_TYPE_EFI_SYS;
    case 0x00:
        return PART_TYPE_UNKNOWN;
    default:
        return PART_TYPE_RAW;
    }
}

static int parse_mbr(int dev_id, const uint8_t *sector)
{
    const struct mbr *m = (const struct mbr *)sector;
    if (m->signature != 0xAA55)
        return 0;

    /* Protective MBR for GPT: one entry type 0xEE spanning the disk */
    int protective = 0;
    for (int i = 0; i < 4; i++)
        if (m->entry[i].type == 0xEE)
            protective = 1;

    if (protective)
        return 0;   /* real layout is GPT */

    int n = 0;
    for (int i = 0; i < 4 && g_count < PART_MAX; i++) {
        if (m->entry[i].type == 0 || m->entry[i].lba_count == 0)
            continue;
        struct partition *p = &g_parts[g_count];
        memset(p, 0, sizeof(*p));
        p->present      = 1;
        p->dev_id       = dev_id;
        p->index        = i;
        p->scheme       = PART_SCHEME_MBR;
        p->type         = mbr_type_to_part(m->entry[i].type);
        p->start_lba    = m->entry[i].lba_start;
        p->sector_count = m->entry[i].lba_count;
        p->mbr_type     = m->entry[i].type;
        /* ata0p1 style name */
        {
            struct block_dev *d = block_get(dev_id);
            char tmp[PART_NAME_LEN];
            const char *dn = d ? d->name : "disk";
            int k = 0;
            while (dn[k] && k < 20) { tmp[k] = dn[k]; k++; }
            tmp[k++] = 'p';
            tmp[k++] = (char)('1' + i);
            tmp[k] = 0;
            strncpy(p->name, tmp, PART_NAME_LEN - 1);
        }
        g_count++;
        n++;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* GPT                                                                */
/* ------------------------------------------------------------------ */

struct gpt_header {
    char     signature[8];      /* "EFI PART" */
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc;
    uint32_t reserved;
    uint64_t current_lba;
    uint64_t backup_lba;
    uint64_t first_usable;
    uint64_t last_usable;
    uint8_t  disk_guid[16];
    uint64_t part_entry_lba;
    uint32_t num_entries;
    uint32_t entry_size;
    uint32_t entries_crc;
} __attribute__((packed));

struct gpt_entry {
    uint8_t  type_guid[16];
    uint8_t  unique_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attrs;
    uint16_t name_utf16[36];
} __attribute__((packed));

/* Microsoft basic data */
static const uint8_t guid_ms_basic[16] = {
    0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
    0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7
};
/* EFI system partition */
static const uint8_t guid_efi_sys[16] = {
    0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
    0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
};
/* Linux filesystem data */
static const uint8_t guid_linux[16] = {
    0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47,
    0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4
};

static int guid_eq(const uint8_t *a, const uint8_t *b)
{
    for (int i = 0; i < 16; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

static int gpt_type(const uint8_t *guid)
{
    if (guid_eq(guid, guid_efi_sys))
        return PART_TYPE_EFI_SYS;
    if (guid_eq(guid, guid_ms_basic))
        return PART_TYPE_FAT32;  /* often FAT/NTFS; refine later */
    if (guid_eq(guid, guid_linux))
        return PART_TYPE_LINUX;
    /* all-zero = unused */
    int z = 1;
    for (int i = 0; i < 16; i++)
        if (guid[i]) { z = 0; break; }
    if (z)
        return PART_TYPE_UNKNOWN;
    return PART_TYPE_RAW;
}

static void utf16_to_ascii(const uint16_t *u, char *out, size_t n)
{
    size_t j = 0;
    for (size_t i = 0; i < 36 && j + 1 < n; i++) {
        uint16_t c = u[i];
        if (c == 0)
            break;
        out[j++] = (c < 128) ? (char)c : '?';
    }
    out[j] = 0;
}

static int parse_gpt(int dev_id)
{
    uint8_t sector[512];
    if (blk_read(dev_id, 1, 1, sector) != 0)
        return 0;

    const struct gpt_header *h = (const struct gpt_header *)sector;
    if (memcmp(h->signature, "EFI PART", 8) != 0)
        return 0;
    if (h->entry_size < sizeof(struct gpt_entry) || h->num_entries == 0)
        return 0;

    uint32_t nent = h->num_entries;
    if (nent > 128)
        nent = 128;
    uint32_t esz = h->entry_size;
    uint64_t elba = h->part_entry_lba;

    int n = 0;
    uint32_t entries_per_sector = 512 / esz;
    if (entries_per_sector == 0)
        return 0;

    for (uint32_t i = 0; i < nent && g_count < PART_MAX; i++) {
        uint64_t lba = elba + (i / entries_per_sector);
        uint32_t off = (i % entries_per_sector) * esz;
        if (blk_read(dev_id, lba, 1, sector) != 0)
            break;
        const struct gpt_entry *e = (const struct gpt_entry *)(sector + off);
        int t = gpt_type(e->type_guid);
        if (t == PART_TYPE_UNKNOWN)
            continue;
        if (e->last_lba < e->first_lba)
            continue;

        struct partition *p = &g_parts[g_count];
        memset(p, 0, sizeof(*p));
        p->present      = 1;
        p->dev_id       = dev_id;
        p->index        = (int)i;
        p->scheme       = PART_SCHEME_GPT;
        p->type         = t;
        p->start_lba    = e->first_lba;
        p->sector_count = e->last_lba - e->first_lba + 1;
        {
            /* copy out of packed struct to avoid -Waddress-of-packed-member */
            uint16_t name_local[36];
            for (int ni = 0; ni < 36; ni++)
                name_local[ni] = e->name_utf16[ni];
            utf16_to_ascii(name_local, p->name, PART_NAME_LEN);
        }
        if (p->name[0] == 0) {
            struct block_dev *d = block_get(dev_id);
            const char *dn = d ? d->name : "disk";
            int k = 0;
            while (dn[k] && k < 20) { p->name[k] = dn[k]; k++; }
            p->name[k++] = 'p';
            if (i < 9)
                p->name[k++] = (char)('1' + i);
            else {
                p->name[k++] = '1';
                p->name[k++] = (char)('0' + (i - 9));
            }
            p->name[k] = 0;
        }
        g_count++;
        n++;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Scan                                                               */
/* ------------------------------------------------------------------ */

int part_scan_dev(int dev_id)
{
    struct block_dev *d = block_get(dev_id);
    if (!d || !d->present)
        return -1;

    uint8_t sector[512];
    if (blk_read(dev_id, 0, 1, sector) != 0)
        return -1;

    /* Try GPT first (protective MBR is common) */
    int n = parse_gpt(dev_id);
    if (n > 0)
        return n;

    n = parse_mbr(dev_id, sector);
    if (n > 0)
        return n;

    /* No table: expose the whole disk as one raw partition (TigFS image style) */
    if (g_count < PART_MAX) {
        struct partition *p = &g_parts[g_count];
        memset(p, 0, sizeof(*p));
        p->present      = 1;
        p->dev_id       = dev_id;
        p->index        = 0;
        p->scheme       = PART_SCHEME_NONE;
        p->type         = PART_TYPE_RAW;
        p->start_lba    = 0;
        p->sector_count = d->sectors;
        strncpy(p->name, d->name, PART_NAME_LEN - 1);
        g_count++;
        return 1;
    }
    return 0;
}

void part_init(void)
{
    memset(g_parts, 0, sizeof(g_parts));
    g_count = 0;

    for (int i = 0; i < block_dev_count(); i++)
        part_scan_dev(i);

    pr_info("part: %d partition(s)\n", g_count);
}

int part_count(void)
{
    return g_count;
}

const struct partition *part_get(int index)
{
    if (index < 0 || index >= g_count)
        return NULL;
    return &g_parts[index];
}

const struct partition *part_find_by_name(const char *name)
{
    if (!name)
        return NULL;
    for (int i = 0; i < g_count; i++)
        if (strcmp(g_parts[i].name, name) == 0)
            return &g_parts[i];
    return NULL;
}

static const char *type_str(int t)
{
    switch (t) {
    case PART_TYPE_FAT32:   return "FAT32";
    case PART_TYPE_NTFS:    return "NTFS";
    case PART_TYPE_TIGFS:   return "TigFS";
    case PART_TYPE_EFI_SYS: return "EFI";
    case PART_TYPE_LINUX:   return "Linux";
    case PART_TYPE_RAW:     return "raw";
    default:                return "?";
    }
}

static const char *scheme_str(int s)
{
    switch (s) {
    case PART_SCHEME_MBR: return "MBR";
    case PART_SCHEME_GPT: return "GPT";
    default:              return "none";
    }
}

void part_dump(void)
{
    kprintf("=== partitions (%d) ===\n", g_count);
    for (int i = 0; i < g_count; i++) {
        const struct partition *p = &g_parts[i];
        kprintf("  [%d] %-12s  %s/%s  LBA %llu + %llu\n",
                i, p->name, scheme_str(p->scheme), type_str(p->type),
                (unsigned long long)p->start_lba,
                (unsigned long long)p->sector_count);
    }
}
