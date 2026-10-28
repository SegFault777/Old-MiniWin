#ifndef KEYBOARD_H
#define KEYBOARD_H
#include "io.h"

#define KBD_DATA_PORT   0x60
#define KBD_STATUS_PORT 0x64

/* Extended-key scancodes (arrow keys) arrive as a two-byte sequence:
 * 0xE0 prefix, then one of these */
#define SC_EXT_PREFIX  0xE0
#define SC_UP          0x48
#define SC_DOWN        0x50
#define SC_LEFT        0x4B
#define SC_RIGHT       0x4D

/* Special return values from keyboard_poll_key() for non-ASCII keys */
#define KEY_NONE   0
#define KEY_UP     -1
#define KEY_DOWN   -2
#define KEY_LEFT   -3
#define KEY_RIGHT  -4

/* US QWERTY scancode set 1 -> ASCII (unshifted), index = scancode */
static const char scancode_ascii[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0, /* ctrl */
    'a','s','d','f','g','h','j','k','l',';','\'','`',
    0, /* left shift */
    '\\','z','x','c','v','b','n','m',',','.','/',
    0, /* right shift */
    '*',
    0, /* alt */
    ' ',
    0, /* caps lock */
    0,0,0,0,0,0,0,0,0,0, /* F1-F10 */
    0, /* num lock */
    0, /* scroll lock */
    0,0,0,0,0,0,0,0,0,0,0,0,0, /* keypad etc */
};

static const char scancode_ascii_shift[128] = {
    0,  27, '!','@','#','$','%','^','&','*','(',')','_','+','\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,
    'A','S','D','F','G','H','J','K','L',':','"','~',
    0,
    '|','Z','X','C','V','B','N','M','<','>','?',
    0,
    '*',
    0,
    ' ',
    0,
    0,0,0,0,0,0,0,0,0,0,
    0,
    0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,
};

static int shift_held = 0;
static int ctrl_held = 0;
static int ext_prefix_pending = 0;

static inline int keyboard_ctrl_held(void) { return ctrl_held; }

/* Returns:
 *   > 0   : ASCII character from a newly pressed key
 *   KEY_UP/DOWN/LEFT/RIGHT (negative) : arrow key pressed
 *   0     : nothing new (no data, a release, or a modifier-only press)
 * Non-blocking: call this every frame from the main loop. */
static inline int keyboard_poll_key(void) {
    u8 status = inb(KBD_STATUS_PORT);
    if (!(status & 0x01)) return KEY_NONE; /* no data waiting at all */
    if (status & 0x20) {
        /* This byte belongs to the mouse (AUX device), not the keyboard --
         * bit 5 of the status register distinguishes the two, since both
         * devices share the same output buffer/IRQ line on port 0x60/0x64.
         * Leave it alone for mouse_poll() to consume; reading it here would
         * misinterpret raw PS/2 mouse packet bytes as keyboard scancodes
         * (this is what caused random characters like '7', '0', ''' to
         * appear while moving the mouse). */
        return KEY_NONE;
    }

    u8 sc = inb(KBD_DATA_PORT);

    if (sc == SC_EXT_PREFIX) { ext_prefix_pending = 1; return KEY_NONE; }

    if (ext_prefix_pending) {
        ext_prefix_pending = 0;
        if (sc & 0x80) return KEY_NONE; /* extended key release */
        switch (sc) {
            case SC_UP:    return KEY_UP;
            case SC_DOWN:  return KEY_DOWN;
            case SC_LEFT:  return KEY_LEFT;
            case SC_RIGHT: return KEY_RIGHT;
            default:       return KEY_NONE;
        }
    }

    if (sc == 0x2A || sc == 0x36) { shift_held = 1; return KEY_NONE; }  /* shift down */
    if (sc == 0xAA || sc == 0xB6) { shift_held = 0; return KEY_NONE; }  /* shift up */
    if (sc == 0x1D) { ctrl_held = 1; return KEY_NONE; }                 /* (left) ctrl down */
    if (sc == 0x9D) { ctrl_held = 0; return KEY_NONE; }                 /* (left) ctrl up */

    if (sc & 0x80) return KEY_NONE; /* key release, ignore */
    if (sc >= 128) return KEY_NONE;

    char c = shift_held ? scancode_ascii_shift[sc] : scancode_ascii[sc];
    return (int)(unsigned char)c;
}

/* Legacy simple wrapper kept for compatibility: ASCII only, arrows ignored. */
static inline char keyboard_poll(void) {
    int k = keyboard_poll_key();
    if (k > 0) return (char)k;
    return 0;
}

#endif
