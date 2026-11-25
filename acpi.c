/* acpi.c — RSDP/RSDT/XSDT, FADT, MADT, S5 poweroff/reboot (TigxOS 1.3.8) */

#include "acpi.h"
#include "console.h"
#include "io.h"
#include "log.h"
#include "string.h"
#include <stdint.h>

/* ---------- packed ACPI structures ---------- */
struct rsdp {
    char     sig[8];        /* "RSD PTR " */
    uint8_t  checksum;
    char     oem[6];
    uint8_t  revision;      /* 0 = 1.0, 2 = 2.0+ */
    uint32_t rsdt_addr;
    /* v2+ */
    uint32_t length;
    uint64_t xsdt_addr;
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
} __attribute__((packed));

struct sdt_header {
    char     sig[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem[6];
    char     oem_table[8];
    uint32_t oem_rev;
    uint32_t creator_id;
    uint32_t creator_rev;
} __attribute__((packed));

struct fadt {
    struct sdt_header h;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t  reserved;
    uint8_t  preferred_pm_profile;
    uint16_t sci_int;
    uint32_t smi_cmd;
    uint8_t  acpi_enable;
    uint8_t  acpi_disable;
    uint8_t  s4bios_req;
    uint8_t  pstate_cnt;
    uint32_t pm1a_evt_blk;
    uint32_t pm1b_evt_blk;
    uint32_t pm1a_cnt_blk;
    uint32_t pm1b_cnt_blk;
    /* ... many more fields; we only need a few */
    uint8_t  pad[15];       /* skip to reset_reg area carefully */
} __attribute__((packed));

/* FADT offsets we care about (beyond the partial struct) */
#define FADT_PM1A_CNT_OFFSET  64   /* from start of FADT (header+fields) */
#define FADT_PM1B_CNT_OFFSET  68
#define FADT_RESET_REG_OFFSET 116  /* GAS (12 bytes) + reset_value */
#define FADT_RESET_VALUE_OFF  128

struct madt {
    struct sdt_header h;
    uint32_t lapic_addr;
    uint32_t flags;
    /* variable entries follow */
} __attribute__((packed));

struct madt_entry {
    uint8_t type;
    uint8_t length;
} __attribute__((packed));

#define MADT_LAPIC     0
#define MADT_IOAPIC    1
#define MADT_OVERRIDE  2
#define MADT_NMI       3
#define MADT_LAPIC_AO  5   /* address override */

static struct acpi_info info;
static int acpi_ready;

/* ---------- helpers ---------- */
static int checksum_ok(const void *p, size_t n) {
    const uint8_t *b = p;
    uint8_t sum = 0;
    for (size_t i = 0; i < n; i++)
        sum += b[i];
    return sum == 0;
}

static int sig_eq(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

/* Scan for RSDP: EBDA then 0xE0000–0xFFFFF */
static struct rsdp *find_rsdp(void) {
    /* EBDA segment pointer at 0x40E */
    uint16_t ebda_seg = *(volatile uint16_t *)0x40E;
    if (ebda_seg) {
        uintptr_t ebda = (uintptr_t)ebda_seg << 4;
        for (uintptr_t a = ebda; a < ebda + 0x400; a += 16) {
            struct rsdp *r = (struct rsdp *)a;
            if (sig_eq(r->sig, "RSD PTR ", 8) && checksum_ok(r, 20))
                return r;
        }
    }
    /* BIOS ROM area */
    for (uintptr_t a = 0xE0000; a < 0x100000; a += 16) {
        struct rsdp *r = (struct rsdp *)a;
        if (sig_eq(r->sig, "RSD PTR ", 8) && checksum_ok(r, 20))
            return r;
    }
    return NULL;
}

static int table_ok(struct sdt_header *h) {
    if (!h || h->length < sizeof(struct sdt_header) || h->length > 0x100000)
        return 0;
    return checksum_ok(h, h->length);
}

/* Tiny scan of DSDT for the _S5_ package: look for the name and then
 * the two SLP_TYPa/b integer bytes that follow in the AML stream.
 * This is intentionally minimal — no full AML interpreter. */
static void parse_s5(uint32_t dsdt_phys) {
    if (!dsdt_phys) return;
    struct sdt_header *dsdt = (struct sdt_header *)(uintptr_t)dsdt_phys;
    if (!table_ok(dsdt) || !sig_eq(dsdt->sig, "DSDT", 4))
        return;

    const uint8_t *p = (const uint8_t *)dsdt + sizeof(struct sdt_header);
    const uint8_t *end = (const uint8_t *)dsdt + dsdt->length;

    while (p + 8 < end) {
        /* look for "_S5_" */
        if (p[0] == '_' && p[1] == 'S' && p[2] == '5' && p[3] == '_') {
            /* AML Package: after name we usually see 0x12 (PackageOp) then
             * pkg length, then NumElements, then the data. We just hunt for
             * the next two small integers (0x0A = BytePrefix). */
            const uint8_t *q = p + 4;
            int found = 0;
            uint8_t a = 0, b = 0;
            while (q + 2 < end && q < p + 64) {
                if (q[0] == 0x0A) { /* BytePrefix */
                    if (found == 0) { a = q[1]; found = 1; }
                    else { b = q[1]; found = 2; break; }
                    q += 2;
                } else {
                    q++;
                }
            }
            if (found >= 1) {
                info.slp_typa = a;
                info.slp_typb = b;
                info.has_s5 = 1;
                pr_info("acpi: _S5_ SLP_TYPa=%u SLP_TYPb=%u\n", a, b);
            }
            return;
        }
        p++;
    }
}

static void parse_fadt(struct sdt_header *h) {
    if (!table_ok(h) || h->length < 76) return;
    const uint8_t *base = (const uint8_t *)h;

    /* PM1a/b control blocks (32-bit ports in older FADT) */
    info.pm1a_cnt = *(const uint32_t *)(base + 64) & 0xFFFF;
    info.pm1b_cnt = *(const uint32_t *)(base + 68) & 0xFFFF;

    /* DSDT physical address */
    uint32_t dsdt = *(const uint32_t *)(base + 40);
    parse_s5(dsdt);

    /* Reset register (FADT v2+, optional) */
    if (h->length >= 129) {
        /* GAS at offset 116: address_space(1), width, offset, access, addr(8) */
        uint8_t space = base[116];
        uint64_t addr = *(const uint64_t *)(base + 116 + 4);
        info.reset_value = base[128];
        if (space == 1 && addr < 0x10000) { /* SystemIO */
            info.reset_reg_space = 1;
            info.reset_reg = (uint16_t)addr;
        }
    }

    pr_info("acpi: FADT PM1a_CNT=0x%x  reset=%s\n",
            info.pm1a_cnt,
            info.reset_reg_space ? "yes" : "no");
}

static void parse_madt(struct sdt_header *h) {
    if (!table_ok(h) || h->length < sizeof(struct madt)) return;
    struct madt *m = (struct madt *)h;
    info.lapic_addr = m->lapic_addr ? m->lapic_addr : 0xFEE00000;

    const uint8_t *p = (const uint8_t *)(m + 1);
    const uint8_t *end = (const uint8_t *)h + h->length;
    int bsp_seen = 0;

    while (p + 2 <= end) {
        struct madt_entry *e = (struct madt_entry *)p;
        if (e->length < 2 || p + e->length > end)
            break;

        if (e->type == MADT_LAPIC && e->length >= 8) {
            uint8_t apic_id = p[3];
            uint32_t flags  = *(const uint32_t *)(p + 4);
            if ((flags & 1) && info.cpu_count < ACPI_MAX_CPUS) {
                info.cpus[info.cpu_count].apic_id = apic_id;
                info.cpus[info.cpu_count].enabled = 1;
                info.cpus[info.cpu_count].is_bsp  = !bsp_seen;
                bsp_seen = 1;
                info.cpu_count++;
            }
        } else if (e->type == MADT_IOAPIC && e->length >= 12) {
            if (info.ioapic_count < ACPI_MAX_IOAPICS) {
                info.ioapics[info.ioapic_count].id       = p[2];
                info.ioapics[info.ioapic_count].addr     = *(const uint32_t *)(p + 4);
                info.ioapics[info.ioapic_count].gsi_base = *(const uint32_t *)(p + 8);
                info.ioapic_count++;
            }
        } else if (e->type == MADT_OVERRIDE && e->length >= 10) {
            if (info.override_count < ACPI_MAX_OVERRIDES) {
                info.overrides[info.override_count].bus    = p[2];
                info.overrides[info.override_count].source = p[3];
                info.overrides[info.override_count].gsi    = *(const uint32_t *)(p + 4);
                info.overrides[info.override_count].flags  = *(const uint16_t *)(p + 8);
                info.override_count++;
            }
        } else if (e->type == MADT_LAPIC_AO && e->length >= 12) {
            info.lapic_addr = (uint32_t)*(const uint64_t *)(p + 4);
        }
        p += e->length;
    }

    pr_info("acpi: MADT %d CPU(s), %d I/O APIC(s), LAPIC @ 0x%x\n",
            info.cpu_count, info.ioapic_count, info.lapic_addr);
}


/* HPET table (ACPI 2.0+): find MMIO base instead of hardcoding 0xFED00000. */
static void parse_hpet(struct sdt_header *h) {
    /* Layout after header:
     *  event_timer_block_id u32
     *  base_address GAS (12 bytes): space_id, bit_width, bit_offset, access, address u64
     */
    if (h->length < sizeof(struct sdt_header) + 4 + 12)
        return;
    const uint8_t *p = (const uint8_t *)(h + 1);
    p += 4; /* skip event_timer_block_id */
    uint8_t space = p[0];
    uint64_t addr = 0;
    for (int i = 0; i < 8; i++)
        addr |= (uint64_t)p[4 + i] << (8 * i);
    if (space != 0) /* only System Memory */
        return;
    if (addr == 0)
        return;
    info.hpet_addr = addr;
    pr_info("acpi: HPET @ 0x%llx\n", (unsigned long long)addr);
}

static void walk_sdt(uint32_t addr, int is_xsdt) {
    struct sdt_header *root = (struct sdt_header *)(uintptr_t)addr;
    if (!table_ok(root)) return;

    size_t entry_size = is_xsdt ? 8 : 4;
    size_t n = (root->length - sizeof(struct sdt_header)) / entry_size;
    const uint8_t *entries = (const uint8_t *)(root + 1);

    for (size_t i = 0; i < n; i++) {
        uint64_t phys;
        if (is_xsdt)
            phys = *(const uint64_t *)(entries + i * 8);
        else
            phys = *(const uint32_t *)(entries + i * 4);

        if (phys > 0xFFFFFFFFUL) continue; /* we only identity-map low mem */
        struct sdt_header *t = (struct sdt_header *)(uintptr_t)phys;
        if (!table_ok(t)) continue;

        if (sig_eq(t->sig, "FACP", 4))
            parse_fadt(t);
        else if (sig_eq(t->sig, "APIC", 4))
            parse_madt(t);
        else if (sig_eq(t->sig, "HPET", 4))
            parse_hpet(t);
    }
}

void acpi_init(void) {
    memset(&info, 0, sizeof(info));
    info.lapic_addr = 0xFEE00000;

    struct rsdp *rsdp = find_rsdp();
    if (!rsdp) {
        pr_warn("acpi: no RSDP found — power control will use QEMU fallbacks\n");
        return;
    }

    pr_info("acpi: RSDP @ %p  rev=%u  OEM=%.6s\n",
            (void *)rsdp, rsdp->revision, rsdp->oem);

    if (rsdp->revision >= 2 && rsdp->xsdt_addr &&
        (rsdp->xsdt_addr >> 32) == 0) {
        walk_sdt((uint32_t)rsdp->xsdt_addr, 1);
    } else if (rsdp->rsdt_addr) {
        walk_sdt(rsdp->rsdt_addr, 0);
    }

    if (info.cpu_count == 0) {
        /* No MADT or empty — assume single CPU */
        info.cpu_count = 1;
        info.cpus[0].apic_id = 0;
        info.cpus[0].enabled = 1;
        info.cpus[0].is_bsp  = 1;
    }

    info.present = 1;
    acpi_ready = 1;
    pr_info("acpi: %d CPU(s) ready for SMP\n", info.cpu_count);
}

const struct acpi_info *acpi_get(void) {
    return &info;
}

/* ---------- power control ---------- */
void acpi_poweroff(void) {
    pr_info("acpi: poweroff\n");
    __asm__ volatile("cli" ::: "memory");

    if (info.has_s5 && info.pm1a_cnt) {
        /* Enable ACPI if needed via SMI command (best-effort) */
        /* Write SLP_TYPx | SLP_EN (bit 13) into PM1a_CNT */
        uint16_t val = (uint16_t)((info.slp_typa << 10) | (1u << 13));
        outw(info.pm1a_cnt, val);
        if (info.pm1b_cnt) {
            val = (uint16_t)((info.slp_typb << 10) | (1u << 13));
            outw(info.pm1b_cnt, val);
        }
        /* If we get here the write did not power off */
    }

    /* QEMU / Bochs / VirtualBox fallbacks */
    outw(0x604, 0x2000);   /* QEMU */
    outw(0xB004, 0x2000);  /* Bochs */
    outw(0x4004, 0x3400);  /* VirtualBox */

    for (;;)
        __asm__ volatile("hlt");
}

void acpi_reboot(void) {
    pr_info("acpi: reboot\n");
    __asm__ volatile("cli" ::: "memory");

    /* 1. ACPI reset register */
    if (info.reset_reg_space && info.reset_reg)
        outb(info.reset_reg, info.reset_value);

    /* 2. Keyboard controller pulse */
    for (int i = 0; i < 100000; i++) {
        if ((inb(0x64) & 0x02) == 0)
            break;
    }
    outb(0x64, 0xFE);

    /* 3. Triple fault as last resort */
    uint8_t idt_null[6] = {0};
    __asm__ volatile("lidt %0; int $0" :: "m"(idt_null));

    for (;;)
        __asm__ volatile("hlt");
}
