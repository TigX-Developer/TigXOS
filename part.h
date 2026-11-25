/* part.h — Partition table (MBR + GPT) (TigxOS 1.3.8) */

#ifndef TIGX_PART_H
#define TIGX_PART_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PART_MAX           16
#define PART_NAME_LEN      36

enum part_scheme {
    PART_SCHEME_NONE = 0,
    PART_SCHEME_MBR  = 1,
    PART_SCHEME_GPT  = 2
};

enum part_type {
    PART_TYPE_UNKNOWN  = 0,
    PART_TYPE_FAT32    = 1,
    PART_TYPE_NTFS     = 2,
    PART_TYPE_TIGFS    = 3,
    PART_TYPE_EFI_SYS  = 4,
    PART_TYPE_LINUX    = 5,
    PART_TYPE_RAW      = 6
};

struct partition {
    int      present;
    int      dev_id;            /* block device */
    int      index;             /* 0-based on that disk */
    int      scheme;            /* PART_SCHEME_* */
    int      type;              /* PART_TYPE_* */
    uint64_t start_lba;
    uint64_t sector_count;
    char     name[PART_NAME_LEN];
    uint8_t  mbr_type;          /* raw MBR type byte when scheme=MBR */
};

void part_init(void);           /* scan all block devices */
int  part_count(void);
const struct partition *part_get(int index);
const struct partition *part_find_by_name(const char *name);

/* Rescan one device (e.g. after writing a partition table). */
int  part_scan_dev(int dev_id);

void part_dump(void);

#ifdef __cplusplus
}
#endif

#endif /* TIGX_PART_H */
