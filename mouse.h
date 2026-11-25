/* mouse.h — PS/2 mouse (TigxOS 1.3.8)
 *
 * Standard 3-byte packet stream over the 8042 controller (IRQ12).
 * Position is tracked in a virtual coordinate space; when a framebuffer
 * is active the bounds follow the FB, otherwise a fixed default is used.
 */
#ifndef TIGX_MOUSE_H
#define TIGX_MOUSE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOUSE_BTN_LEFT   (1u << 0)
#define MOUSE_BTN_RIGHT  (1u << 1)
#define MOUSE_BTN_MIDDLE (1u << 2)

struct mouse_state {
    int32_t  x;
    int32_t  y;
    uint8_t  buttons;       /* MOUSE_BTN_* mask */
    int8_t   dx;            /* last packet delta (signed) */
    int8_t   dy;
    uint32_t packets;       /* total packets received */
};

void mouse_init(void);

/* Snapshot of current position / buttons (IRQ-safe). */
void mouse_get_state(struct mouse_state *out);

/* 1 if at least one packet arrived since last call (clears the flag). */
int  mouse_poll(void);

/* Optional: change the clamp rectangle (e.g. after enabling the FB). */
void mouse_set_bounds(int32_t max_x, int32_t max_y);

/* USB HID mouse (and tests) may push relative motion + buttons. */
void mouse_inject(int8_t dx, int8_t dy, uint8_t buttons);


#ifdef __cplusplus
}
#endif

#endif /* TIGX_MOUSE_H */
