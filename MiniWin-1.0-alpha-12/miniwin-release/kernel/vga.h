#ifndef VGA_H
#define VGA_H
#include "io.h"

#define VGA_WIDTH  320
#define VGA_HEIGHT 200
#define VGA_MEMORY ((u8*)0xA0000)

/* Set VGA controller into Mode 13h (320x200, 256 color, linear framebuffer)
 * by writing the standard register sequence directly, since we can't
 * call BIOS int 0x10 from 32-bit protected mode. */
static inline void vga_set_mode13h(void) {
    /* Misc output register */
    outb(0x3C2, 0x63);

    /* Sequencer registers */
    static const u8 seq[5] = {0x03, 0x01, 0x0F, 0x00, 0x0E};
    outb(0x3C4, 0x00); outb(0x3C5, seq[0]);
    outb(0x3C4, 0x01); outb(0x3C5, seq[1]);
    outb(0x3C4, 0x02); outb(0x3C5, seq[2]);
    outb(0x3C4, 0x03); outb(0x3C5, seq[3]);
    outb(0x3C4, 0x04); outb(0x3C5, seq[4]);

    /* Unlock CRTC registers */
    outb(0x3D4, 0x11); outb(0x3D5, inb(0x3D5) & 0x7F);

    static const u8 crtc[25] = {
        0x5F,0x4F,0x50,0x82,0x54,0x80,0xBF,0x1F,
        0x00,0x41,0x00,0x00,0x00,0x00,0x00,0x00,
        0x9C,0x0E,0x8F,0x28,0x40,0x96,0xB9,0xA3,
        0xFF
    };
    for (u8 i = 0; i < 25; i++) {
        outb(0x3D4, i);
        outb(0x3D5, crtc[i]);
    }

    /* Graphics controller registers */
    static const u8 gfx[9] = {0x00,0x00,0x00,0x00,0x00,0x40,0x05,0x0F,0xFF};
    for (u8 i = 0; i < 9; i++) {
        outb(0x3CE, i);
        outb(0x3CF, gfx[i]);
    }

    /* Attribute controller registers */
    static const u8 att[21] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
        0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,
        0x41,0x00,0x0F,0x00,0x00
    };
    (void)inb(0x3DA); /* reset flip-flop */
    for (u8 i = 0; i < 21; i++) {
        outb(0x3C0, i);
        outb(0x3C0, att[i]);
    }
    (void)inb(0x3DA);
    outb(0x3C0, 0x20); /* enable video output */
}

/* Explicitly program the first 16 DAC palette entries to standard VGA
 * colors, since we can't rely on BIOS defaults being sane after our
 * manual mode-13h register programming. VGA DAC values are 0-63 (6-bit). */
static inline void vga_set_standard_palette(void) {
    static const u8 pal[16][3] = {
        {0,0,0},    {0,0,42},   {0,42,0},   {0,42,42},
        {42,0,0},   {42,0,42},  {42,21,0},  {42,42,42},
        {21,21,21}, {21,21,63}, {21,63,21}, {21,63,63},
        {63,21,21}, {63,21,63}, {63,63,21}, {63,63,63},
    };
    outb(0x3C8, 0); /* start writing at palette index 0 */
    for (int i = 0; i < 16; i++) {
        outb(0x3C9, pal[i][0]);
        outb(0x3C9, pal[i][1]);
        outb(0x3C9, pal[i][2]);
    }
}

static inline void vga_putpixel(int x, int y, u8 color) {
    if (x < 0 || y < 0 || x >= VGA_WIDTH || y >= VGA_HEIGHT) return;
    VGA_MEMORY[y * VGA_WIDTH + x] = color;
}

/* ---------------------------------------------------------------------
 * Off-screen back buffer + double buffering.
 *
 * Redrawing the whole 320x200 desktop directly into VGA memory every
 * frame (which we now do, since window state -- open/closed/minimized --
 * can change at any time from mouse clicks) is visibly flickery if drawn
 * straight into the visible framebuffer. So all drawing functions target
 * `backbuf` instead, and vga_present() blits the finished frame to the
 * real VGA memory in one shot at the end of each loop iteration.
 * ------------------------------------------------------------------- */
static u8 backbuf[VGA_WIDTH * VGA_HEIGHT];

static inline void bb_putpixel(int x, int y, u8 color) {
    if ((unsigned)x >= VGA_WIDTH || (unsigned)y >= VGA_HEIGHT) return;
    backbuf[y * VGA_WIDTH + x] = color;
}

static inline void bb_fillrect(int x, int y, int w, int h, u8 color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > VGA_WIDTH)  w = VGA_WIDTH - x;
    if (y + h > VGA_HEIGHT) h = VGA_HEIGHT - y;
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        u8 *row = &backbuf[(y + j) * VGA_WIDTH + x];
        for (int i = 0; i < w; i++) row[i] = color;
    }
}

static inline void bb_rect(int x, int y, int w, int h, u8 color) {
    for (int i = 0; i < w; i++) { bb_putpixel(x+i, y, color); bb_putpixel(x+i, y+h-1, color); }
    for (int j = 0; j < h; j++) { bb_putpixel(x, y+j, color); bb_putpixel(x+w-1, y+j, color); }
}

static inline void vga_present(void) {
    /* single tight copy loop -- fast, no per-pixel bounds checks needed
     * since backbuf and VGA_MEMORY are both exactly VGA_WIDTH*VGA_HEIGHT */
    u32 *src = (u32*)backbuf;
    u32 *dst = (u32*)VGA_MEMORY;
    for (int i = 0; i < (VGA_WIDTH * VGA_HEIGHT) / 4; i++) dst[i] = src[i];
}

static inline void vga_fillrect(int x, int y, int w, int h, u8 color) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            vga_putpixel(x + i, y + j, color);
}

static inline void vga_rect(int x, int y, int w, int h, u8 color) {
    for (int i = 0; i < w; i++) { vga_putpixel(x+i, y, color); vga_putpixel(x+i, y+h-1, color); }
    for (int j = 0; j < h; j++) { vga_putpixel(x, y+j, color); vga_putpixel(x+w-1, y+j, color); }
}

static inline void vga_clear(u8 color) {
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) VGA_MEMORY[i] = color;
}

/* Standard 16-color VGA palette indices (works fine even in 256-color mode
 * since the default DAC palette's first 16 entries match EGA colors) */
#define COL_BLACK       0
#define COL_BLUE        1
#define COL_GREEN       2
#define COL_CYAN        3
#define COL_RED         4
#define COL_MAGENTA     5
#define COL_BROWN       6
#define COL_LGRAY       7
#define COL_DGRAY       8
#define COL_LBLUE       9
#define COL_LGREEN      10
#define COL_LCYAN       11
#define COL_LRED        12
#define COL_LMAGENTA    13
#define COL_YELLOW      14
#define COL_WHITE       15

#endif
