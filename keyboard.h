// keyboard.h



#ifndef KEYBOARD_H
#define KEYBOARD_H

void keyboard_init(void);
char keyboard_getchar(void);
int keyboard_has_key(void);
int keyboard_get_caps_state(void);

/* Special keys delivered through keyboard_getchar / console read.
 * Values are outside normal printable ASCII so userland can detect them. */
#define KEY_LEFT   ((char)0x80)
#define KEY_RIGHT  ((char)0x81)
#define KEY_UP     ((char)0x82)
#define KEY_DOWN   ((char)0x83)
#define KEY_HOME   ((char)0x84)
#define KEY_END    ((char)0x85)
#define KEY_DELETE ((char)0x86)

#endif // KEYBOARD_H
