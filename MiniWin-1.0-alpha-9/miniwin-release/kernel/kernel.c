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

static inline int clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
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
 * Taskbar (bottom of screen, Windows-95-ish strip)
 * ============================================================ */
#define TASKBAR_H     14
#define TASKBAR_Y     (VGA_HEIGHT - TASKBAR_H)
#define TASKBTN_X     4
#define TASKBTN_Y     (TASKBAR_Y + 2)
#define TASKBTN_W     70
#define TASKBTN_H     10

/* ============================================================
 * Window state + layout
 *
 * Position/size are runtime fields (not compile-time constants) so the
 * window can be dragged by its title bar and maximized/restored. All
 * drawing and hit-testing below reads from `notepad.x/y/w/h` rather than
 * fixed macros.
 * ============================================================ */
#define WIN_DEFAULT_X   40
#define WIN_DEFAULT_Y   25
#define WIN_DEFAULT_W   280
#define WIN_DEFAULT_H   140
#define TITLEBAR_H      10
#define MIN_WIN_H       (TITLEBAR_H + 12 + 20) /* title + menu + a little edit area */

/* Maximized geometry: fill the screen above the taskbar entirely. */
#define MAXIMIZED_X 0
#define MAXIMIZED_Y 0
#define MAXIMIZED_W VGA_WIDTH
#define MAXIMIZED_H (VGA_HEIGHT - TASKBAR_H)

/* title bar control buttons: _  []  X, right-aligned, 9x8 each */
#define BTN_W 9
#define BTN_H 8
#define BTN_GAP 1

typedef struct {
    int open;         /* window exists at all (opened from desktop icon) */
    int minimized;    /* currently minimized to the taskbar */
    int maximized;    /* currently maximized to fill the screen above the taskbar */

    /* Current on-screen geometry (the authoritative rect used for drawing
     * and hit-testing whenever the window is visible). While maximized,
     * this is kept equal to the MAXIMIZED_* rect; restoring copies
     * restore_x/y/w/h back into x/y/w/h. */
    int x, y, w, h;

    /* Geometry to snap back to when un-maximizing. */
    int restore_x, restore_y, restore_w, restore_h;
} window_t;

static window_t notepad = {
    .open = 0, .minimized = 0, .maximized = 0,
    .x = WIN_DEFAULT_X, .y = WIN_DEFAULT_Y, .w = WIN_DEFAULT_W, .h = WIN_DEFAULT_H,
    .restore_x = WIN_DEFAULT_X, .restore_y = WIN_DEFAULT_Y,
    .restore_w = WIN_DEFAULT_W, .restore_h = WIN_DEFAULT_H,
};

static char text_buf[512];
static u32  text_len = 0;

/* Window-relative button positions, computed from current geometry. */
static inline int btn_close_x(void) { return notepad.x + notepad.w - 2 - BTN_W; }
static inline int btn_max_x(void)   { return btn_close_x() - BTN_W - BTN_GAP; }
static inline int btn_min_x(void)   { return btn_max_x() - BTN_W - BTN_GAP; }
static inline int btn_y(void)       { return notepad.y + 1; }

static inline int edit_x(void) { return notepad.x + 4; }
static inline int edit_y(void) { return notepad.y + TITLEBAR_H + 12; }
static inline int edit_w(void) { return notepad.w - 8; }
static inline int edit_h(void) { return notepad.h - TITLEBAR_H - 16; }

/* ============================================================
 * Taskbar drawing + hit-testing
 * ============================================================ */
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
    int y = btn_y();

    /* minimize "_" */
    int mnx = btn_min_x();
    bb_fillrect(mnx, y, BTN_W, BTN_H, COL_LGRAY);
    bb_rect(mnx, y, BTN_W, BTN_H, COL_BLACK);
    for (int i = 2; i < BTN_W - 2; i++) bb_putpixel(mnx + i, y + BTN_H - 3, COL_BLACK);

    /* maximize "[]" -- toggles fullscreen (above the taskbar) */
    int mxx = btn_max_x();
    bb_fillrect(mxx, y, BTN_W, BTN_H, COL_LGRAY);
    bb_rect(mxx, y, BTN_W, BTN_H, COL_BLACK);
    bb_rect(mxx + 2, y + 2, BTN_W - 4, BTN_H - 4, COL_BLACK);

    /* close "X" */
    int clx = btn_close_x();
    bb_fillrect(clx, y, BTN_W, BTN_H, COL_LGRAY);
    bb_rect(clx, y, BTN_W, BTN_H, COL_BLACK);
    for (int i = 2; i < BTN_W - 2; i++) {
        bb_putpixel(clx + i, y + 2 + (i - 2), COL_BLACK);
        bb_putpixel(clx + (BTN_W - 1 - i), y + 2 + (i - 2), COL_BLACK);
    }
}

static void draw_window(void) {
    int wx = notepad.x, wy = notepad.y, ww = notepad.w, wh = notepad.h;

    /* drop shadow (skip when maximized -- looks wrong flush with the
     * screen edge and the taskbar) */
    if (!notepad.maximized) {
        bb_fillrect(wx + 3, wy + 3, ww, wh, COL_DGRAY);
    }

    /* window body */
    bb_fillrect(wx, wy, ww, wh, COL_LGRAY);
    bb_rect(wx, wy, ww, wh, COL_BLACK);

    /* title bar */
    bb_fillrect(wx + 1, wy + 1, ww - 2, TITLEBAR_H, COL_BLUE);
    font_draw_string(wx + 3, wy + 1, "NOTEPAD.EXE", COL_WHITE);

    draw_titlebar_buttons();

    /* menu bar */
    int menu_y = wy + TITLEBAR_H + 1;
    bb_fillrect(wx + 1, menu_y, ww - 2, 9, COL_LGRAY);
    font_draw_string(wx + 4,  menu_y, "File", COL_BLACK);
    font_draw_string(wx + 40, menu_y, "Edit", COL_BLACK);
    font_draw_string(wx + 76, menu_y, "Help", COL_BLACK);
    for (int i = 0; i < ww - 2; i++)
        bb_putpixel(wx + 1 + i, menu_y + 9, COL_DGRAY);

    /* text edit area (white, sunken border) */
    int ex = edit_x(), ey = edit_y(), ew = edit_w(), eh = edit_h();
    bb_fillrect(ex, ey, ew, eh, COL_WHITE);
    bb_rect(ex - 1, ey - 1, ew + 2, eh + 2, COL_DGRAY);

    /* text content */
    int cx = ex + 2, cy = ey + 2;
    for (u32 i = 0; i < text_len; i++) {
        char c = text_buf[i];
        if (c == '\n' || cx > ex + ew - 10) {
            cx = ex + 2;
            cy += 9;
            if (c == '\n') continue;
        }
        if (cy > ey + eh - 8) break;
        font_draw_char(cx, cy, c, COL_BLACK);
        cx += 8;
    }
    bb_fillrect(cx, cy, 2, 8, COL_BLACK); /* text cursor */
}

/* Is (px,py) over the draggable part of the title bar -- i.e. the title
 * bar itself, but not over any of the three control buttons? */
static int titlebar_drag_hit(int px, int py) {
    if (!in_rect(px, py, notepad.x + 1, notepad.y + 1, notepad.w - 2, TITLEBAR_H)) return 0;
    if (in_rect(px, py, btn_min_x(), btn_y(), BTN_W, BTN_H)) return 0;
    if (in_rect(px, py, btn_max_x(), btn_y(), BTN_W, BTN_H)) return 0;
    if (in_rect(px, py, btn_close_x(), btn_y(), BTN_W, BTN_H)) return 0;
    return 1;
}

static void maximize_window(void) {
    if (notepad.maximized) return;
    /* stash current geometry so restore can bring it back exactly */
    notepad.restore_x = notepad.x;
    notepad.restore_y = notepad.y;
    notepad.restore_w = notepad.w;
    notepad.restore_h = notepad.h;
    notepad.x = MAXIMIZED_X;
    notepad.y = MAXIMIZED_Y;
    notepad.w = MAXIMIZED_W;
    notepad.h = MAXIMIZED_H;
    notepad.maximized = 1;
}

static void unmaximize_window(void) {
    if (!notepad.maximized) return;
    notepad.x = notepad.restore_x;
    notepad.y = notepad.restore_y;
    notepad.w = notepad.restore_w;
    notepad.h = notepad.restore_h;
    notepad.maximized = 0;
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
    const u32 double_click_window = 15000; /* tuned for delay(8000) per frame;
                                             * adjust proportionally if delay() changes */

    /* Window-drag state: while dragging, we track the offset from the
     * window's top-left corner to the point the user grabbed, so the
     * window follows the cursor without "snapping" its corner to it. */
    int dragging = 0;
    int drag_offset_x = 0, drag_offset_y = 0;

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

            int left_now = mouse_left;
            int clicked = mouse_click_event; /* per-packet edge detection from the driver */

            /* ---- drag in progress: move the window with the cursor ---- */
            if (dragging) {
                if (left_now) {
                    if (!notepad.maximized) {
                        int new_x = mx - drag_offset_x;
                        int new_y = my - drag_offset_y;
                        /* keep at least a sliver of the title bar on-screen
                         * so the window can never be dragged somewhere the
                         * user can't grab it again */
                        notepad.x = clampi(new_x, -(notepad.w - 20), VGA_WIDTH - 20);
                        notepad.y = clampi(new_y, 0, VGA_HEIGHT - TASKBAR_H - TITLEBAR_H);
                    }
                } else {
                    dragging = 0; /* button released -> stop dragging */
                }
            }

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
                    }
                } else if (notepad.open && !notepad.minimized) {
                    if (in_rect(mx, my, btn_min_x(), btn_y(), BTN_W, BTN_H)) {
                        notepad.minimized = 1;
                        status = "NOTEPAD.EXE MINIMIZED";
                    } else if (in_rect(mx, my, btn_max_x(), btn_y(), BTN_W, BTN_H)) {
                        if (notepad.maximized) {
                            unmaximize_window();
                            status = "NOTEPAD.EXE RESTORED";
                        } else {
                            maximize_window();
                            status = "NOTEPAD.EXE MAXIMIZED";
                        }
                    } else if (in_rect(mx, my, btn_close_x(), btn_y(), BTN_W, BTN_H)) {
                        notepad.open = 0;
                        notepad.minimized = 0;
                        status = "NOTEPAD.EXE CLOSED";
                    } else if (titlebar_drag_hit(mx, my) && !notepad.maximized) {
                        /* start dragging: remember the grab offset so the
                         * window doesn't jump when the drag begins */
                        dragging = 1;
                        drag_offset_x = mx - notepad.x;
                        drag_offset_y = my - notepad.y;
                    }
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
        delay(8000); /* lowered from 80000 -- was making the whole UI feel
                      * sluggish on real hardware (mouse/keyboard response,
                      * double-click timing) since this loop's real-world
                      * speed varies a lot by CPU. */
    }
}
