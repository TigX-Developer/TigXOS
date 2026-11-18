// keyboard.c - PS/2 keyboard: shift symbols, Caps Lock, Ctrl, blocking read



#include "keyboard.h"
#include "process.h"
#include "interrupt.h"
#include "console.h"
#include "io.h"
#include "wait.h"
#include <stdint.h>

#define KEYBOARD_DATA_PORT   0x60
#define KEYBOARD_STATUS_PORT 0x64

static const char scancode_normal[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t',
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\',
    'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ',
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    '7', '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.', 0, 0, 0
};

static const char scancode_shift[128] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|',
    'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ',
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    '7', '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.', 0, 0, 0
};

static volatile char keyboard_buffer[256];
static volatile int  buffer_head = 0;
static volatile int  buffer_tail = 0;

static volatile int shift_pressed = 0;
static volatile int caps_lock_on  = 0;
static volatile int alt_pressed   = 0;
static volatile int ctrl_pressed  = 0;
static volatile int extended      = 0;

static volatile int leds_pending = 0;

static struct wait_queue kbd_waitq;

static void kbd_push(char c) {
    int next = (buffer_head + 1) % 256;
    if (next != buffer_tail) {
        keyboard_buffer[buffer_head] = c;
        buffer_head = next;
    }
}

/* Never spin forever — LED updates from IRQ used to hang the kernel. */
static int kbd_wait_write(void) {
    for (int i = 0; i < 100000; i++)
        if (!(inb(KEYBOARD_STATUS_PORT) & 2))
            return 0;
    return -1;
}

static int kbd_wait_read(void) {
    for (int i = 0; i < 100000; i++)
        if (inb(KEYBOARD_STATUS_PORT) & 1)
            return 0;
    return -1;
}

static void update_leds(void) {
    if (kbd_wait_write() != 0) return;
    outb(KEYBOARD_DATA_PORT, 0xED);
    if (kbd_wait_read() != 0) return;
    (void)inb(KEYBOARD_DATA_PORT); /* ACK or garbage */
    if (kbd_wait_write() != 0) return;
    outb(KEYBOARD_DATA_PORT, caps_lock_on ? 0x04 : 0x00);
    if (kbd_wait_read() == 0)
        (void)inb(KEYBOARD_DATA_PORT);
}

static int is_letter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static void keyboard_handler(struct interrupt_frame *frame) {
    (void)frame;
    uint8_t scancode = inb(KEYBOARD_DATA_PORT);

    if (scancode == 0xE0) {
        extended = 1;
        return;
    }

    int ext = extended;
    extended = 0;

    if (!(scancode & 0x80)) {
        uint8_t key = scancode;

        if (key == 0x2A || key == 0x36) { shift_pressed = 1; return; }
        if (key == 0x1D) { ctrl_pressed = 1; (void)ext; return; }
        if (key == 0x38) { alt_pressed  = 1; return; }
        if (key == 0x3A) {
            caps_lock_on = !caps_lock_on;
            leds_pending = 1;
            return;
        }

        /* Extended navigation keys (E0 prefix) → special single-byte codes */
        if (ext) {
            char spec = 0;
            switch (key) {
            case 0x4B: spec = (char)0x80; break; /* Left  */
            case 0x4D: spec = (char)0x81; break; /* Right */
            case 0x48: spec = (char)0x82; break; /* Up    */
            case 0x50: spec = (char)0x83; break; /* Down  */
            case 0x47: spec = (char)0x84; break; /* Home  */
            case 0x4F: spec = (char)0x85; break; /* End   */
            case 0x53: spec = (char)0x86; break; /* Delete */
            case 0x1D: /* right Ctrl */
            case 0x38: /* right Alt */
                /* treat like left modifiers already handled above */
                if (key == 0x1D) ctrl_pressed = 1;
                if (key == 0x38) alt_pressed = 1;
                return;
            default:
                return; /* ignore other extended keys */
            }
            if (spec) {
                kbd_push(spec);
                wait_queue_wake_one(&kbd_waitq);
            }
            return;
        }

        char normal  = (key < 128) ? scancode_normal[key] : 0;
        char shifted = (key < 128) ? scancode_shift[key]  : 0;
        char c;

        if (is_letter(normal) || is_letter(shifted)) {
            int upper = caps_lock_on ^ shift_pressed;
            c = upper ? shifted : normal;
        } else {
            c = shift_pressed ? shifted : normal;
        }

        if (ctrl_pressed && c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 1);
        else if (ctrl_pressed && c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 1);

        /* Ctrl-C → SIGINT the foreground job; do not feed ^C to the shell */
        if (c == 3) {
            process_fg_interrupt();
            wait_queue_wake_one(&kbd_waitq); /* unblock shell waiters if any */
            return;
        }

        if (c) {
            kbd_push(c);
            /* Deferred wake: IRQ marks the waiter READY; the switch
               happens at the next schedule point (timer IRQ or yield). */
            wait_queue_wake_one(&kbd_waitq);
        }
    } else {
        uint8_t key = scancode & 0x7F;
        if (key == 0x2A || key == 0x36) shift_pressed = 0;
        if (key == 0x1D) ctrl_pressed = 0;
        if (key == 0x38) alt_pressed  = 0;
    }
}

void keyboard_init(void) {
    wait_queue_init(&kbd_waitq, "kbd");
    register_interrupt_handler(33, keyboard_handler);
    pic_enable_irq(1);
    caps_lock_on = 0;
    shift_pressed = ctrl_pressed = alt_pressed = extended = 0;
    buffer_head = buffer_tail = 0;
    leds_pending = 0;
    update_leds();
    kprintf("Keyboard: arrows/home/end/del, Caps, Shift, Ctrl, blocking\n");
}

static void keyboard_flush_leds(void) {
    if (!leds_pending) return;
    leds_pending = 0;
    update_leds();
}

/*
 * Blocking read.  The condition (buffer non-empty) and the enqueue are
 * performed atomically with IRQs off, so no wakeup can be lost.
 */
char keyboard_getchar(void) {
    for (;;) {
        __asm__ volatile("cli");
        if (buffer_head != buffer_tail) {
            char c = keyboard_buffer[buffer_tail];
            buffer_tail = (buffer_tail + 1) % 256;
            __asm__ volatile("sti");
            return c;
        }
        /* wait_queue_block returns with IF=0; allow IRQs between polls */
        wait_queue_block(&kbd_waitq, 0);
        __asm__ volatile("sti");
        /* Buffer may still be empty if woken spuriously; loop. */
        keyboard_flush_leds();
    }
}

int keyboard_has_key(void) {
    keyboard_flush_leds();
    return buffer_head != buffer_tail;
}

int keyboard_get_caps_state(void) {
    return caps_lock_on;
}
