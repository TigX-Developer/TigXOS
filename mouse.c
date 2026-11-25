/* mouse.c — PS/2 mouse driver (TigxOS 1.3.8)
 *
 * Uses the same 8042 ports as the keyboard (0x60/0x64). Commands to the
 * mouse go through the controller auxiliary device path (0xD4 prefix).
 * IRQ12 delivers stream-mode 3-byte packets.
 */

#include "mouse.h"
#include "interrupt.h"
#include "console.h"
#include "io.h"
#include "log.h"
#include "spinlock.h"
#include "fb.h"
#include <stdint.h>

#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_CMD     0x64

#define STATUS_OBF  0x01    /* output buffer full */
#define STATUS_IBF  0x02    /* input buffer full */
#define STATUS_AUX  0x20    /* data is from mouse */

/* Default virtual desktop when no FB is active */
#define MOUSE_DEFAULT_W  1024
#define MOUSE_DEFAULT_H  768

static struct mouse_state g_state;
static int32_t g_max_x = MOUSE_DEFAULT_W - 1;
static int32_t g_max_y = MOUSE_DEFAULT_H - 1;
static volatile int g_packet_flag;
static uint8_t g_cycle;
static uint8_t g_bytes[3];
static int g_ok;

/* ------------------------------------------------------------------ */
/* 8042 helpers                                                       */
/* ------------------------------------------------------------------ */

static void ps2_wait_write(void)
{
    for (int i = 0; i < 100000; i++)
        if (!(inb(PS2_STATUS) & STATUS_IBF))
            return;
}

static void ps2_wait_read(void)
{
    for (int i = 0; i < 100000; i++)
        if (inb(PS2_STATUS) & STATUS_OBF)
            return;
}

static void ps2_flush(void)
{
    for (int i = 0; i < 100; i++) {
        if (!(inb(PS2_STATUS) & STATUS_OBF))
            break;
        (void)inb(PS2_DATA);
    }
}

static void ps2_write_cmd(uint8_t cmd)
{
    ps2_wait_write();
    outb(PS2_CMD, cmd);
}

static void ps2_write_data(uint8_t data)
{
    ps2_wait_write();
    outb(PS2_DATA, data);
}

static uint8_t ps2_read_data(void)
{
    ps2_wait_read();
    return inb(PS2_DATA);
}

/* Send a byte to the mouse (prefix with 0xD4) and wait for ACK (0xFA). */
static int mouse_cmd(uint8_t cmd)
{
    ps2_write_cmd(0xD4);
    ps2_write_data(cmd);
    uint8_t ack = ps2_read_data();
    return ack == 0xFA;
}

/* ------------------------------------------------------------------ */
/* IRQ12 handler                                                      */
/* ------------------------------------------------------------------ */

static void mouse_handler(struct interrupt_frame *frame)
{
    (void)frame;

    uint8_t status = inb(PS2_STATUS);
    if (!(status & STATUS_OBF))
        return;

    uint8_t data = inb(PS2_DATA);

    /* Only process auxiliary (mouse) data. Keyboard data on 0x60 during
     * IRQ12 is rare but possible on some controllers — drop it. */
    if (!(status & STATUS_AUX) && g_cycle == 0) {
        /* Could be a spurious read; ignore non-aux when idle. */
    }

    if (g_cycle == 0) {
        /* Byte 0 must have bit 3 set (always-1 in PS/2 mouse protocol). */
        if (!(data & 0x08))
            return;
        g_bytes[0] = data;
        g_cycle = 1;
        return;
    }

    g_bytes[g_cycle++] = data;
    if (g_cycle < 3)
        return;
    g_cycle = 0;

    uint8_t b0 = g_bytes[0];
    int8_t  dx = (int8_t)g_bytes[1];
    int8_t  dy = (int8_t)g_bytes[2];

    /* Overflow bits — drop the packet */
    if (b0 & 0xC0)
        return;

    /* Sign extend already done by int8_t cast; Y is inverted in PS/2
     * (positive Y = move away from user = up on screen in many UIs).
     * We keep PS/2 native Y and let the UI flip if it wants. */
    g_state.dx = dx;
    g_state.dy = dy;
    g_state.buttons = b0 & 0x07;

    g_state.x += dx;
    g_state.y -= dy;    /* invert so +Y is down on screen (common UI convention) */

    if (g_state.x < 0) g_state.x = 0;
    if (g_state.y < 0) g_state.y = 0;
    if (g_state.x > g_max_x) g_state.x = g_max_x;
    if (g_state.y > g_max_y) g_state.y = g_max_y;

    g_state.packets++;
    g_packet_flag = 1;
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

void mouse_set_bounds(int32_t max_x, int32_t max_y)
{
    if (max_x < 0) max_x = 0;
    if (max_y < 0) max_y = 0;
    g_max_x = max_x;
    g_max_y = max_y;
    if (g_state.x > g_max_x) g_state.x = g_max_x;
    if (g_state.y > g_max_y) g_state.y = g_max_y;
}

void mouse_get_state(struct mouse_state *out)
{
    if (!out)
        return;
    /* Snapshot — single 32-bit fields, good enough without a lock for UI */
    *out = g_state;
}

int mouse_poll(void)
{
    int f = g_packet_flag;
    g_packet_flag = 0;
    return f;
}

void mouse_init(void)
{
    g_ok = 0;
    g_cycle = 0;
    g_packet_flag = 0;
    g_state.x = g_max_x / 2;
    g_state.y = g_max_y / 2;
    g_state.buttons = 0;
    g_state.packets = 0;

    /* If FB is already active, clamp to it */
    const struct fb_info *fb = fb_get();
    if (fb && fb->active) {
        g_max_x = (int32_t)fb->width  - 1;
        g_max_y = (int32_t)fb->height - 1;
        g_state.x = g_max_x / 2;
        g_state.y = g_max_y / 2;
    }

    ps2_flush();

    /* Enable auxiliary device (mouse) on the 8042 */
    ps2_write_cmd(0xA8);            /* enable aux */

    /* Read controller command byte, set interrupts + aux */
    ps2_write_cmd(0x20);
    uint8_t cfg = ps2_read_data();
    cfg |= 0x02;                    /* enable IRQ12 */
    cfg |= 0x01;                    /* keep IRQ1 (keyboard) */
    cfg &= (uint8_t)~0x20;          /* clear mouse clock disable */
    cfg &= (uint8_t)~0x10;          /* clear keyboard clock disable */
    ps2_write_cmd(0x60);
    ps2_write_data(cfg);

    /* Reset mouse — expect ACK then 0xAA self-test, then device ID */
    if (!mouse_cmd(0xFF)) {
        pr_info("mouse: reset failed (no mouse?)\n");
        return;
    }
    /* Self-test result */
    uint8_t st = ps2_read_data();
    if (st != 0xAA) {
        pr_info("mouse: self-test 0x%02x (expected 0xAA)\n", st);
        return;
    }
    (void)ps2_read_data();          /* device ID (usually 0x00) */

    /* Defaults + stream mode + enable reporting */
    if (!mouse_cmd(0xF6)) {         /* set defaults */
        pr_info("mouse: set defaults failed\n");
        return;
    }
    if (!mouse_cmd(0xF4)) {         /* enable data reporting */
        pr_info("mouse: enable reporting failed\n");
        return;
    }

    register_interrupt_handler(32 + 12, mouse_handler);  /* IRQ12 → vector 44 */
    pic_enable_irq(12);
    /* IRQ2 is cascade — must be unmasked for IRQ8–15 */
    pic_enable_irq(2);

    g_ok = 1;
    pr_info("mouse: PS/2 stream ready (IRQ12), pos %d,%d bounds %dx%d\n",
            g_state.x, g_state.y, g_max_x + 1, g_max_y + 1);
}

void mouse_inject(int8_t dx, int8_t dy, uint8_t buttons)
{
    g_state.dx = dx;
    g_state.dy = dy;
    g_state.buttons = buttons & 0x07;
    g_state.x += dx;
    g_state.y -= dy;
    if (g_state.x < 0) g_state.x = 0;
    if (g_state.y < 0) g_state.y = 0;
    if (g_state.x > g_max_x) g_state.x = g_max_x;
    if (g_state.y > g_max_y) g_state.y = g_max_y;
    g_state.packets++;
    g_packet_flag = 1;
}
