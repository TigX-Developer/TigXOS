/* audio.c — AC97 PCM + HDA verbs + PC speaker (TigxOS 1.3.8-update1) */
#include "audio.h"
#include "pci.h"
#include "log.h"
#include "console.h"
#include "string.h"
#include "timer.h"
#include "io.h"
#include "memory.h"

static int g_present;
static char g_name[32];
static uint16_t g_nam, g_nabm;
static uint8_t *g_pcm_buf;
static uint32_t *g_bdl;

/* HDA MMIO */
static volatile uint8_t *g_hda;
static uint32_t *g_corb;
static uint32_t *g_rirb;
static int g_hda_ok;

#define HDA_GCTL     0x08
#define HDA_STATESTS 0x0E
#define HDA_CORBLBASE 0x40
#define HDA_CORBUBASE 0x44
#define HDA_CORBWP   0x48
#define HDA_CORBRP   0x4A
#define HDA_CORBCTL  0x4C
#define HDA_CORBSIZE 0x4E
#define HDA_RIRBLBASE 0x50
#define HDA_RIRBUBASE 0x54
#define HDA_RIRBWP   0x58
#define HDA_RINTCNT  0x5A
#define HDA_RIRBCTL  0x5C
#define HDA_RIRBSIZE 0x5E

static inline uint32_t hda_r32(uint32_t off) {
    return *(volatile uint32_t *)(g_hda + off);
}
static inline void hda_w32(uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(g_hda + off) = v;
}
static inline uint16_t hda_r16(uint32_t off) {
    return *(volatile uint16_t *)(g_hda + off);
}
static inline void hda_w16(uint32_t off, uint16_t v) {
    *(volatile uint16_t *)(g_hda + off) = v;
}
static inline uint8_t hda_r8(uint32_t off) {
    return *(volatile uint8_t *)(g_hda + off);
}
static inline void hda_w8(uint32_t off, uint8_t v) {
    *(volatile uint8_t *)(g_hda + off) = v;
}

/* codec << 28 | node << 20 | verb << 8 | payload */
static int hda_verb(uint32_t codec, uint32_t node, uint32_t verb, uint32_t payload)
{
    if (!g_hda_ok || !g_corb) return -1;
    uint32_t cmd = (codec << 28) | (node << 20) | (verb << 8) | (payload & 0xFF);
    uint16_t wp = hda_r16(HDA_CORBWP) & 0xFF;
    wp = (wp + 1) & 0xFF;
    g_corb[wp] = cmd;
    hda_w16(HDA_CORBWP, wp);
    /* wait RIRB */
    for (int i = 0; i < 100000; i++) {
        uint16_t rwp = hda_r16(HDA_RIRBWP) & 0xFF;
        if (rwp != 0 || i > 1000)
            break;
        __asm__ volatile("pause");
    }
    return 0;
}

static int hda_init_controller(struct pci_device *dev)
{
    if (!dev->bars[0].address || dev->bars[0].is_io)
        return -1;
    uintptr_t bar = (uintptr_t)dev->bars[0].address;
#if defined(__x86_64__)
    (void)vmm_map_mmio(bar, bar, 0x2000);
#endif
    g_hda = (volatile uint8_t *)bar;

    /* CRST */
    hda_w32(HDA_GCTL, 0);
    for (int i = 0; i < 10000; i++)
        if ((hda_r32(HDA_GCTL) & 1) == 0) break;
    hda_w32(HDA_GCTL, 1);
    for (int i = 0; i < 100000; i++)
        if (hda_r32(HDA_GCTL) & 1) break;

    uint16_t ss = hda_r16(HDA_STATESTS);
    pr_info("audio: HDA GCTL ok STATESTS=0x%x\n", ss);

    g_corb = (uint32_t *)dma_alloc_pages(1, 0);
    g_rirb = (uint32_t *)dma_alloc_pages(1, 0);
    if (!g_corb || !g_rirb) return -1;
    memset(g_corb, 0, 4096);
    memset(g_rirb, 0, 4096);

    /* CORB size 256, RIRB 256 */
    hda_w8(HDA_CORBCTL, 0);
    hda_w8(HDA_RIRBCTL, 0);
    hda_w32(HDA_CORBLBASE, (uint32_t)(uintptr_t)g_corb);
    hda_w32(HDA_CORBUBASE, (uint32_t)((uint64_t)(uintptr_t)g_corb >> 32));
    hda_w32(HDA_RIRBLBASE, (uint32_t)(uintptr_t)g_rirb);
    hda_w32(HDA_RIRBUBASE, (uint32_t)((uint64_t)(uintptr_t)g_rirb >> 32));
    hda_w8(HDA_CORBSIZE, 0x02); /* 256 entries if supported */
    hda_w8(HDA_RIRBSIZE, 0x02);
    hda_w16(HDA_CORBRP, 0x8000); /* reset RP */
    hda_w16(HDA_CORBRP, 0);
    hda_w16(HDA_CORBWP, 0);
    hda_w16(HDA_RIRBWP, 0x8000);
    hda_w16(HDA_RINTCNT, 255);
    hda_w8(HDA_CORBCTL, 0x02); /* run */
    hda_w8(HDA_RIRBCTL, 0x02);

    g_hda_ok = 1;

    /* Codec 0: AFG node 0x01 power D0, unmute DAC-ish nodes (best-effort) */
    hda_verb(0, 0x01, 0x705, 0x00); /* SET_POWER_STATE D0 */
    hda_verb(0, 0x01, 0x7FF, 0x00); /* GET_PARAM vendor — warm pipe */
    /* SET_AMPLIFIER_GAIN mute clear on common output widgets 0x02-0x0A */
    for (uint32_t n = 0x02; n <= 0x0A; n++) {
        hda_verb(0, n, 0x300, 0xB0); /* output amp, unmute, 0dB-ish */
        hda_verb(0, n, 0x705, 0x00); /* power D0 */
    }
    pr_info("audio: HDA CORB/RIRB running, codec verbs issued\n");
    return 0;
}

void audio_init(void)
{
    g_present = 0;
    g_nam = g_nabm = 0;
    g_pcm_buf = 0;
    g_bdl = 0;
    g_hda = 0;
    g_hda_ok = 0;
    strncpy(g_name, "none", sizeof(g_name) - 1);

    size_t n = pci_device_count();
    for (size_t i = 0; i < n; i++) {
        const struct pci_device *d = pci_device_at(i);
        if (!d) continue;
        if (d->class_code != 0x04) continue;

        if (d->subclass == 0x03) {
            g_present = 1;
            strncpy(g_name, "HDA", sizeof(g_name) - 1);
            struct pci_device mut = *d;
            pci_enable_device(&mut);
            if (hda_init_controller(&mut) != 0)
                pr_warn("audio: HDA init incomplete — speaker fallback\n");
            return;
        }
        if (d->subclass == 0x01) {
            g_present = 1;
            strncpy(g_name, "AC97", sizeof(g_name) - 1);
            if (d->bars[0].is_io) g_nam = (uint16_t)d->bars[0].address;
            if (d->bars[1].is_io) g_nabm = (uint16_t)d->bars[1].address;
            if (g_nam) {
                outw(g_nam + 0x02, 0x0000);
                outw(g_nam + 0x18, 0x0000);
            }
            {
                uintptr_t phys = 0;
                g_pcm_buf = (uint8_t *)dma_alloc_pages(1, &phys);
                g_bdl = (uint32_t *)dma_alloc_pages(1, 0);
            }
            if (g_pcm_buf && g_bdl && g_nabm) {
                memset(g_pcm_buf, 0, 4096);
                g_bdl[0] = (uint32_t)(uintptr_t)g_pcm_buf;
                g_bdl[1] = 0;
                g_bdl[2] = 0x80000000u | 1024;
                g_bdl[3] = 0;
                pr_info("audio: AC97 PCM ready nam=0x%x nabm=0x%x\n", g_nam, g_nabm);
            }
            return;
        }
    }
    pr_info("audio: no AC97/HDA; PC speaker available\n");
}

int audio_present(void) { return g_present; }
const char *audio_backend_name(void) { return g_name; }

void audio_beep(uint32_t freq_hz, uint32_t ms)
{
    if (freq_hz < 20) freq_hz = 20;
    if (freq_hz > 20000) freq_hz = 20000;
    uint32_t div = 1193182u / freq_hz;
    outb(0x43, 0xB6);
    outb(0x42, (uint8_t)div);
    outb(0x42, (uint8_t)(div >> 8));
    uint8_t t = inb(0x61);
    outb(0x61, t | 3);
    timer_sleep_ms(ms ? ms : 100);
    outb(0x61, t & 0xFC);
}

int audio_play_tone(uint32_t freq_hz, uint32_t ms)
{
    if (g_nabm && g_pcm_buf && g_bdl) {
        uint32_t rate = 48000;
        uint32_t samples = 1024;
        uint32_t half = rate / (freq_hz ? freq_hz : 440) / 2;
        if (half < 1) half = 1;
        int16_t *s = (int16_t *)g_pcm_buf;
        for (uint32_t i = 0; i < samples; i++)
            s[i] = ((i / half) & 1) ? 2000 : -2000;
        outl(g_nabm + 0x10, (uint32_t)(uintptr_t)g_bdl);
        outb(g_nabm + 0x15, 0);
        outb(g_nabm + 0x1B, 0x01);
        timer_sleep_ms(ms ? ms : 50);
        outb(g_nabm + 0x1B, 0x00);
        return 0;
    }
    if (g_hda_ok) {
        /* Power path already programmed; audible path still uses speaker until
           stream descriptors + DMA buffers are fully wired for QEMU HDA. */
        audio_beep(freq_hz, ms);
        return 0;
    }
    audio_beep(freq_hz, ms);
    return 0;
}
