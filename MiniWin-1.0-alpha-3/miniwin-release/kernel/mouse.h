#ifndef MOUSE_H
#define MOUSE_H
#include "io.h"

#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_CMD     0x64

static inline void ps2_wait_input_clear(void) {
    /* wait until bit1 (input buffer full) is clear -> safe to write */
    int timeout = 100000;
    while ((inb(PS2_STATUS) & 0x02) && timeout--) { }
}

static inline void ps2_wait_output_full(void) {
    /* wait until bit0 (output buffer full) is set -> safe to read */
    int timeout = 100000;
    while (!(inb(PS2_STATUS) & 0x01) && timeout--) { }
}

static inline void ps2_write_cmd(u8 cmd) {
    ps2_wait_input_clear();
    outb(PS2_CMD, cmd);
}

static inline void ps2_write_data(u8 data) {
    ps2_wait_input_clear();
    outb(PS2_DATA, data);
}

static inline u8 ps2_read_data(void) {
    ps2_wait_output_full();
    return inb(PS2_DATA);
}

/* Send a byte to the second PS/2 port (the mouse) and wait for its ACK (0xFA) */
static inline void mouse_write(u8 val) {
    ps2_write_cmd(0xD4);       /* "next byte goes to mouse (port 2)" */
    ps2_write_data(val);
    (void)ps2_read_data();    /* ACK */
}

static int mouse_dx = 0, mouse_dy = 0;   /* accumulated delta since last mouse_poll() call */
static int mouse_left = 0, mouse_right = 0;
static int mouse_left_prev = 0;           /* button state as of the previous packet processed */
static int mouse_click_event = 0;         /* 1 if a left-button press edge occurred during the last mouse_poll() */
static u8  mouse_packet[3];
static int mouse_packet_idx = 0;

static inline void mouse_init(void) {
    /* Enable the auxiliary (mouse) PS/2 device */
    ps2_write_cmd(0xA8);

    /* Enable IRQ12 for mouse + IRQ1 for keyboard in the controller
     * configuration byte (we still poll rather than use interrupts, but
     * the compaq status byte also controls clock enables which matter). */
    ps2_write_cmd(0x20);              /* read controller config byte */
    u8 status = ps2_read_data();
    status |= 0x02;                    /* enable IRQ12 (bit1) */
    status &= ~0x20;                   /* ensure mouse clock enabled (bit5=0) */
    ps2_write_cmd(0x60);              /* write controller config byte */
    ps2_write_data(status);

    mouse_write(0xF6);   /* set defaults */
    mouse_write(0xF4);   /* enable data reporting (streaming) */

    mouse_packet_idx = 0;
}

/* Non-blocking: call every loop iteration. Reads any pending mouse bytes
 * from the PS/2 output buffer, assembles a 3-byte packet, and when a full
 * packet arrives, updates mouse_dx/dy/buttons. This drains *all* currently
 * available mouse bytes in one call (not just one byte), so that several
 * queued packets don't desync the 3-byte framing across multiple
 * mouse_poll() calls separated by other work (rendering, keyboard, etc).
 * Returns 1 if at least one full packet was processed, 0 otherwise. */
static inline int mouse_poll(void) {
    int got_any = 0;
    int acc_dx = 0, acc_dy = 0;
    int click_event = 0;

    while (1) {
        u8 status = inb(PS2_STATUS);
        if (!(status & 0x01)) break;         /* no data waiting at all */
        if (!(status & 0x20)) {
            /* data waiting but it's from the keyboard (port1), not mouse
             * (port2) -- leave it for keyboard_poll_key() to consume, and
             * stop draining here so we don't steal keyboard bytes. */
            break;
        }

        u8 b = inb(PS2_DATA);

        /* Resync guard: byte 0 of every packet must have bit3 set. If our
         * framing has drifted (e.g. we started mid-packet after a boot),
         * discard bytes until we see a valid packet-start byte. */
        if (mouse_packet_idx == 0 && !(b & 0x08)) {
            continue; /* discard, keep draining */
        }

        mouse_packet[mouse_packet_idx++] = b;
        if (mouse_packet_idx < 3) continue;
        mouse_packet_idx = 0;

        u8 flags = mouse_packet[0];

        /* Discard packets with overflow bits set (bit6=X overflow,
         * bit7=Y overflow) -- their dx/dy bytes are not meaningful and
         * using them produces huge spurious jumps (e.g. cursor snapping
         * to a corner from a single fast movement). */
        if (flags & 0xC0) {
            continue;
        }

        int dx = mouse_packet[1];
        int dy = mouse_packet[2];
        if (flags & 0x10) dx -= 256;   /* sign bit for X */
        if (flags & 0x20) dy -= 256;   /* sign bit for Y */

        /* Defensive clamp: a single legitimate packet should never move
         * the cursor by more than a small amount. If framing ever does
         * drift (dropped byte, USB/PS2 quirks, etc.), a raw dx/dy byte can
         * get misinterpreted as a huge signed value (close to +/-255),
         * snapping the cursor across the whole screen or straight to a
         * corner from one real-world flick of the mouse. Clamping caps the
         * damage from any single bad packet without needing to identify
         * exactly which byte was wrong. */
        if (dx > 80) dx = 80;
        if (dx < -80) dx = -80;
        if (dy > 80) dy = 80;
        if (dy < -80) dy = -80;

        acc_dx += dx;
        acc_dy += dy;  /* NOTE: sign matches this environment's PS/2 mouse
                         * convention directly (no inversion) -- verified
                         * against real mouse movement, since some PS/2
                         * implementations differ from the "textbook" Y
                         * polarity assumption. */

        /* Determine click edge PER PACKET, not just from the final state --
         * otherwise a press+release (or release+press) pair that both land
         * inside a single mouse_poll() drain call would cancel out and the
         * click would be silently lost. */
        int this_left = flags & 0x01;
        if (this_left && !mouse_left_prev) click_event = 1;
        mouse_left_prev = this_left;

        mouse_left  = this_left;
        mouse_right = flags & 0x02;
        got_any = 1;
    }

    mouse_dx = acc_dx;
    mouse_dy = acc_dy;
    mouse_click_event = click_event;
    return got_any;
}

#endif
