#include "io.h"
#include "vga.h"
#include "font.h"
#include "keyboard.h"
#include "mouse.h"

#define DESKTOP_COLOR_BG      COL_LCYAN
#define DESKTOP_COLOR_ICON_BG COL_LCYAN

/* ---------- tiny freestanding helpers (no libc available) ---------- */
static void kstrcpy_append(char *buf, u32 *len, u32 maxlen, char c) {
    if (*len < maxlen - 1) {
        buf[*len] = c;
        (*len)++;
        buf[*len] = 0;
    }
}

/* simple busy-wait delay, calibrated roughly for typical QEMU/CPU speed */
static void delay(volatile u32 loops) {
    while (loops--) { __asm__ volatile ("nop"); }
}

static inline int in_rect(int px, int py, int x, int y, int w, int h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

/* ============================================================
 * Desktop icon
 * ============================================================ */
#define ICON_X   6
#define ICON_Y   6
#define ICON_W   28
#define ICON_H   26   /* box + label combined hit area */

static void draw_desktop_icon(void) {
    bb_fillrect(ICON_X + 4, ICON_Y, 16, 12, COL_WHITE);
    bb_rect(ICON_X + 4, ICON_Y, 16, 12, COL_BLACK);
    /* little folded-corner notch to look like a document/app icon */
    bb_fillrect(ICON_X + 4 + 11, ICON_Y, 5, 4, DESKTOP_COLOR_ICON_BG);
    bb_rect(ICON_X + 4 + 11, ICON_Y, 5, 4, COL_BLACK);
    font_draw_string(ICON_X - 4, ICON_Y + 14, "NOTEPAD", COL_BLACK);
    font_draw_string(ICON_X + 6, ICON_Y + 22, ".EXE", COL_BLACK);
}

/* ============================================================
 * Window state + layout
 * ============================================================ */
#define WIN_X   40
#define WIN_Y   25
#define WIN_W   280
#define WIN_H   140
#define TITLEBAR_H 10

#define EDIT_X  (WIN_X + 4)
#define EDIT_Y  (WIN_Y + TITLEBAR_H + 12)
#define EDIT_W  (WIN_W - 8)
#define EDIT_H  (WIN_H - TITLEBAR_H - 16)

/* title bar control buttons: _  []  X, right-aligned, 9x8 each */
#define BTN_W 9
#define BTN_H 8
#define BTN_GAP 1
#define BTN_CLOSE_X (WIN_X + WIN_W - 2 - BTN_W)
#define BTN_MAX_X   (BTN_CLOSE_X - BTN_W - BTN_GAP)
#define BTN_MIN_X   (BTN_MAX_X   - BTN_W - BTN_GAP)
#define BTN_Y       (WIN_Y + 1)

typedef struct {
    int open;        /* window exists at all (opened from desktop icon) */
    int minimized;    /* currently minimized to the taskbar */
} window_t;

static window_t notepad = {0, 0};

static char text_buf[512];
static u32  text_len = 0;

/* ============================================================
 * Taskbar (bottom of screen, Windows-95-ish strip)
 * ============================================================ */
#define TASKBAR_H     14
#define TASKBAR_Y     (VGA_HEIGHT - TASKBAR_H)
#define TASKBTN_X     4
#define TASKBTN_Y     (TASKBAR_Y + 2)
#define TASKBTN_W     70
#define TASKBTN_H     10

static void draw_taskbar(void) {
    bb_fillrect(0, TASKBAR_Y, VGA_WIDTH, TASKBAR_H, COL_LGRAY);
    for (int i = 0; i < VGA_WIDTH; i++) bb_putpixel(i, TASKBAR_Y, COL_WHITE);

    if (notepad.open && notepad.minimized) {
        /* taskbar button showing the minimized app, with its own mini
         * restore/close glyphs drawn right inside the taskbar button. */
        bb_fillrect(TASKBTN_X, TASKBTN_Y, TASKBTN_W, TASKBTN_H, COL_WHITE);
        bb_rect(TASKBTN_X, TASKBTN_Y, TASKBTN_W, TASKBTN_H, COL_DGRAY);
        font_draw_string(TASKBTN_X + 2, TASKBTN_Y + 1, "N...", COL_BLACK);

        /* restore glyph: two overlapping squares */
        int rx = TASKBTN_X + TASKBTN_W - 20, ry = TASKBTN_Y + 1;
        bb_rect(rx + 2, ry, 6, 6, COL_BLACK);
        bb_rect(rx, ry + 2, 6, 6, COL_BLACK);
        bb_fillrect(rx + 1, ry + 3, 4, 4, COL_WHITE);

        /* close glyph: X */
        int cx = TASKBTN_X + TASKBTN_W - 9, cy = TASKBTN_Y + 1;
        for (int i = 0; i < 7; i++) {
            bb_putpixel(cx + i, cy + i, COL_BLACK);
            bb_putpixel(cx + i, cy + 6 - i, COL_BLACK);
        }
    }
}

/* Hit-test helpers for the taskbar's restore/close glyphs, matching the
 * geometry drawn above. */
static int taskbar_restore_hit(int px, int py) {
    if (!(notepad.open && notepad.minimized)) return 0;
    int rx = TASKBTN_X + TASKBTN_W - 20, ry = TASKBTN_Y + 1;
    return in_rect(px, py, rx, ry, 8, 8);
}
static int taskbar_close_hit(int px, int py) {
    if (!(notepad.open && notepad.minimized)) return 0;
    int cx = TASKBTN_X + TASKBTN_W - 9, cy = TASKBTN_Y + 1;
    return in_rect(px, py, cx, cy, 8, 8);
}

/* ============================================================
 * Notepad window drawing
 * ============================================================ */
static void draw_titlebar_buttons(void) {
    /* minimize "_" */
    bb_fillrect(BTN_MIN_X, BTN_Y, BTN_W, BTN_H, COL_LGRAY);
    bb_rect(BTN_MIN_X, BTN_Y, BTN_W, BTN_H, COL_BLACK);
    for (int i = 2; i < BTN_W - 2; i++) bb_putpixel(BTN_MIN_X + i, BTN_Y + BTN_H - 3, COL_BLACK);

    /* maximize "[]" (visual only, non-functional -- not requested to do anything) */
    bb_fillrect(BTN_MAX_X, BTN_Y, BTN_W, BTN_H, COL_LGRAY);
    bb_rect(BTN_MAX_X, BTN_Y, BTN_W, BTN_H, COL_BLACK);
    bb_rect(BTN_MAX_X + 2, BTN_Y + 2, BTN_W - 4, BTN_H - 4, COL_BLACK);

    /* close "X" */
    bb_fillrect(BTN_CLOSE_X, BTN_Y, BTN_W, BTN_H, COL_LGRAY);
    bb_rect(BTN_CLOSE_X, BTN_Y, BTN_W, BTN_H, COL_BLACK);
    for (int i = 2; i < BTN_W - 2; i++) {
        bb_putpixel(BTN_CLOSE_X + i, BTN_Y + 2 + (i - 2), COL_BLACK);
        bb_putpixel(BTN_CLOSE_X + (BTN_W - 1 - i), BTN_Y + 2 + (i - 2), COL_BLACK);
    }
}

static void draw_window(void) {
    /* drop shadow */
    bb_fillrect(WIN_X + 3, WIN_Y + 3, WIN_W, WIN_H, COL_DGRAY);

    /* window body */
    bb_fillrect(WIN_X, WIN_Y, WIN_W, WIN_H, COL_LGRAY);
    bb_rect(WIN_X, WIN_Y, WIN_W, WIN_H, COL_BLACK);

    /* title bar */
    bb_fillrect(WIN_X + 1, WIN_Y + 1, WIN_W - 2, TITLEBAR_H, COL_BLUE);
    font_draw_string(WIN_X + 3, WIN_Y + 1, "NOTEPAD.EXE", COL_WHITE);

    draw_titlebar_buttons();

    /* menu bar */
    int menu_y = WIN_Y + TITLEBAR_H + 1;
    bb_fillrect(WIN_X + 1, menu_y, WIN_W - 2, 9, COL_LGRAY);
    font_draw_string(WIN_X + 4,  menu_y, "FILE", COL_BLACK);
    font_draw_string(WIN_X + 40, menu_y, "EDIT", COL_BLACK);
    font_draw_string(WIN_X + 76, menu_y, "HELP", COL_BLACK);
    for (int i = 0; i < WIN_W - 2; i++)
        bb_putpixel(WIN_X + 1 + i, menu_y + 9, COL_DGRAY);

    /* text edit area (white, sunken border) */
    bb_fillrect(EDIT_X, EDIT_Y, EDIT_W, EDIT_H, COL_WHITE);
    bb_rect(EDIT_X - 1, EDIT_Y - 1, EDIT_W + 2, EDIT_H + 2, COL_DGRAY);

    /* text content */
    int cx = EDIT_X + 2, cy = EDIT_Y + 2;
    for (u32 i = 0; i < text_len; i++) {
        char c = text_buf[i];
        if (c == '\n' || cx > EDIT_X + EDIT_W - 10) {
            cx = EDIT_X + 2;
            cy += 9;
            if (c == '\n') continue;
        }
        if (cy > EDIT_Y + EDIT_H - 8) break;
        font_draw_char(cx, cy, c, COL_BLACK);
        cx += 8;
    }
    bb_fillrect(cx, cy, 2, 8, COL_BLACK); /* text cursor */
}

/* ============================================================
 * Mouse cursor
 * ============================================================ */
static const char cursor_shape[11][7] = {
    "X......","XX.....","X.X....","X..X...","X...X..",
    "X....X.","X.....X","X....XX","X..X.X.","X.X..X.","XX...X.",
};

static void draw_cursor(int x, int y) {
    for (int j = 0; j < 11; j++)
        for (int i = 0; i < 7; i++)
            if (cursor_shape[j][i] == 'X')
                bb_putpixel(x + i, y + j, COL_BLACK);
}

/* ============================================================
 * Frame composition
 * ============================================================ */
static void draw_status_line(const char *msg) {
    font_draw_string(4, VGA_HEIGHT - TASKBAR_H - 9, msg, COL_BLACK);
}

static void render_frame(int mouse_x, int mouse_y, const char *status_msg) {
    bb_fillrect(0, 0, VGA_WIDTH, VGA_HEIGHT, DESKTOP_COLOR_BG);

    draw_desktop_icon();

    if (status_msg) draw_status_line(status_msg);

    if (notepad.open && !notepad.minimized) {
        draw_window();
    }

    draw_taskbar();
    draw_cursor(mouse_x, mouse_y);

    vga_present();
}

/* ============================================================
 * Kernel entry
 * ============================================================ */
void kmain(void) {
    vga_set_mode13h();
    vga_set_standard_palette();
    mouse_init();

    int mx = VGA_WIDTH / 2, my = VGA_HEIGHT / 2;
    int awaiting_second_click = 0;   /* 1 = one icon click seen, waiting for a 2nd within the window */
    u32 last_icon_click_tick = 0;
    u32 tick = 0;
    const u32 double_click_window = 60; /* loop iterations counted as "close together";
                                          * tune this if double-click feels too strict/loose */

    const char *status = "MINIWIN 1.0 - DOUBLE-CLICK NOTEPAD.EXE TO OPEN";

    render_frame(mx, my, status);

    while (1) {
        tick++;

        /* ---- mouse ---- */
        if (mouse_poll()) {
            mx += mouse_dx;
            my += mouse_dy;
            if (mx < 0) mx = 0;
            if (my < 0) my = 0;
            if (mx > VGA_WIDTH - 1)  mx = VGA_WIDTH - 1;
            if (my > VGA_HEIGHT - 1) my = VGA_HEIGHT - 1;

            int clicked = mouse_click_event; /* per-packet edge detection from the driver */

            if (clicked) {
                if (in_rect(mx, my, ICON_X, ICON_Y, ICON_W, ICON_H)) {
                    if (awaiting_second_click && (tick - last_icon_click_tick) < double_click_window) {
                        /* this is the 2nd click of a double-click: open it */
                        notepad.open = 1;
                        notepad.minimized = 0;
                        status = "NOTEPAD.EXE OPENED";
                        awaiting_second_click = 0;
                    } else {
                        /* this is a 1st click: arm the double-click window */
                        awaiting_second_click = 1;
                        last_icon_click_tick = tick;
                        status = "CLICK 1 REGISTERED - CLICK AGAIN";
                    }
                } else if (notepad.open && !notepad.minimized) {
                    if (in_rect(mx, my, BTN_MIN_X, BTN_Y, BTN_W, BTN_H)) {
                        notepad.minimized = 1;
                        status = "NOTEPAD.EXE MINIMIZED";
                    } else if (in_rect(mx, my, BTN_CLOSE_X, BTN_Y, BTN_W, BTN_H)) {
                        notepad.open = 0;
                        notepad.minimized = 0;
                        status = "NOTEPAD.EXE CLOSED";
                    }
                    /* BTN_MAX_X (maximize) intentionally not wired to any
                     * behavior -- not part of the requested feature set. */
                } else if (taskbar_restore_hit(mx, my)) {
                    notepad.minimized = 0;
                    status = "NOTEPAD.EXE RESTORED";
                } else if (taskbar_close_hit(mx, my)) {
                    notepad.open = 0;
                    notepad.minimized = 0;
                    status = "NOTEPAD.EXE CLOSED";
                }
            }
        }

        /* ---- keyboard: only affects notepad text when it's open+visible ---- */
        int k = keyboard_poll_key();
        if (k > 0 && notepad.open && !notepad.minimized) {
            char c = (char)k;
            if (c == '\b') {
                if (text_len > 0) { text_len--; text_buf[text_len] = 0; }
            } else if (c == '\n' || c == ' ' || (c >= 32 && c < 127)) {
                kstrcpy_append(text_buf, &text_len, sizeof(text_buf), c);
            }
        }

        render_frame(mx, my, status);
        delay(80000);
    }
}
