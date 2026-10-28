#include "io.h"
#include "vga.h"
#include "font.h"
#include "keyboard.h"
#include "mouse.h"
#include "ata.h"
#include "fs.h"
#include "font_ko.h"
#include "hangul_ime.h"

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

/* SETTING.EXE -- a dummy/placeholder icon, sits next to NOTEPAD.EXE in the
 * same top row. Double-clicking it just shows a "not implemented yet"
 * status message; there's no actual settings window behind it. */
#define ICON2_X  64
#define ICON2_Y  6
#define ICON2_W  28
#define ICON2_H  26

static void draw_desktop_icon2(void) {
    /* simple gear-ish glyph so it reads as a distinct app, not another
     * document -- a filled circle-ish square with a few notches */
    bb_fillrect(ICON2_X + 4, ICON2_Y, 16, 12, COL_LGRAY);
    bb_rect(ICON2_X + 4, ICON2_Y, 16, 12, COL_BLACK);
    bb_fillrect(ICON2_X + 8, ICON2_Y + 3, 8, 6, COL_WHITE);
    bb_rect(ICON2_X + 8, ICON2_Y + 3, 8, 6, COL_BLACK);
    bb_putpixel(ICON2_X + 6, ICON2_Y + 1, COL_BLACK);
    bb_putpixel(ICON2_X + 17, ICON2_Y + 1, COL_BLACK);
    bb_putpixel(ICON2_X + 6, ICON2_Y + 10, COL_BLACK);
    bb_putpixel(ICON2_X + 17, ICON2_Y + 10, COL_BLACK);
    font_draw_string(ICON2_X - 4, ICON2_Y + 14, "SETTING", COL_BLACK);
    font_draw_string(ICON2_X + 6, ICON2_Y + 22, ".EXE", COL_BLACK);
}

/* ============================================================
 * Taskbar (bottom of screen, Windows-95-ish strip)
 * ============================================================ */
#define TASKBAR_H     14
#define TASKBAR_Y     (VGA_HEIGHT - TASKBAR_H)

/* The Start button -- bottom-left corner, obviously. Every desktop OS
 * since 1995 has agreed on this location without ever holding a
 * meeting about it. Sized to fit "AM" plus a proper raised bevel. */
#define STARTBTN_X    2
#define STARTBTN_Y    (TASKBAR_Y + 2)
#define STARTBTN_W    24
#define STARTBTN_H    10

/* The minimized-Notepad pill used to hug the taskbar's left edge; now
 * it scoots over to make room for its new neighbor. */
#define TASKBTN_X     (STARTBTN_X + STARTBTN_W + 4)
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

static char text_buf[FS_MAX_FILE_BYTES];
static u32  text_len = 0;

/* ============================================================
 * File menu (dropdown from the "File" label in the menu bar)
 * and the Yes/No confirm dialog used by "New" and the close (X) button.
 * ============================================================ */
static int file_menu_open = 0;

/* Whether the Windows-95-style Start Menu is currently popped up. Unlike
 * the File dropdown above (which only exists while Notepad's window is
 * open), this one lives entirely in the taskbar and couldn't care less
 * what Notepad is doing. */
static int start_menu_open = 0;

#define CONFIRM_NONE  0
#define CONFIRM_NEW   1  /* "Save changes before New?" */
#define CONFIRM_CLOSE 2  /* "Save changes before closing?" */
static int confirm_mode = CONFIRM_NONE;

/* Desktop file icons: which of the FS_MAX_FILES slots currently hold a
 * saved file (either saved earlier this session, or found already present
 * at boot -- persistence is real, backed by fs.h/ata.h), and their sizes. */
static int desktop_file_exists[FS_MAX_FILES];
static u32 desktop_file_len[FS_MAX_FILES];

/* Which slot the currently-open document is "bound" to: -1 means the
 * document is new/unbound (never saved, or created via "New"), so the
 * next Save picks the first empty slot and binds to it. Once bound,
 * subsequent saves update that same slot in place, like a normal editor's
 * "Save" (not creating a new file every time you press Ctrl+S). */
static int bound_slot = -1;

/* Shared status-line message, file scope so helper functions below (save
 * logic, confirm dialog actions) can set it directly. */
static const char *status = "MINIWIN 1.0 - DOUBLE-CLICK NOTEPAD.EXE TO OPEN";
static char status_buf[48]; /* scratch space for status messages that embed a filename */

#define MENU_FILE_LABEL_W 34   /* clickable width for the "File" label */
#define FILE_MENU_ITEM_H  10
#define FILE_MENU_W       74

static inline int menu_y_pos(void) { return notepad.y + TITLEBAR_H + 1; }
static inline int file_menu_x(void) { return notepad.x + 4; }
static inline int file_menu_top_y(void) { return menu_y_pos() + 9; }

static int file_label_hit(int px, int py) {
    return in_rect(px, py, notepad.x + 4, menu_y_pos(), MENU_FILE_LABEL_W, 9);
}

static int file_menu_item_hit(int px, int py, int idx) {
    /* idx: 0=Save As, 1=Save, 2=New */
    int x = file_menu_x();
    int y = file_menu_top_y() + idx * FILE_MENU_ITEM_H;
    return in_rect(px, py, x, y, FILE_MENU_W, FILE_MENU_ITEM_H);
}

#define CONFIRM_W 150
#define CONFIRM_H 56
#define CONFIRM_BTN_W 34
#define CONFIRM_BTN_H 12

static inline int confirm_x(void) { return notepad.x + (notepad.w - CONFIRM_W) / 2; }
static inline int confirm_y(void) { return notepad.y + (notepad.h - CONFIRM_H) / 2; }
static inline int confirm_btn_y(void) { return confirm_y() + CONFIRM_H - CONFIRM_BTN_H - 8; }
static inline int confirm_yes_x(void) { return confirm_x() + 16; }
static inline int confirm_no_x(void)  { return confirm_x() + CONFIRM_W - 16 - CONFIRM_BTN_W; }

static int confirm_yes_hit(int px, int py) {
    return in_rect(px, py, confirm_yes_x(), confirm_btn_y(), CONFIRM_BTN_W, CONFIRM_BTN_H);
}
static int confirm_no_hit(int px, int py) {
    return in_rect(px, py, confirm_no_x(), confirm_btn_y(), CONFIRM_BTN_W, CONFIRM_BTN_H);
}

/* ============================================================
 * Shared save logic -- used by File>Save, Ctrl+S, and the Yes actions of
 * both the "New" and "close window" confirm dialogs, so saving behaves
 * identically no matter which UI path triggered it.
 * ============================================================ */

/* Builds "SAVED: <filename>" into status_buf and returns it. */
static const char *format_saved_status(int slot) {
    u32 i = 0;
    const char *prefix = "SAVED: ";
    while (*prefix) status_buf[i++] = *prefix++;
    const char *name = fs_slot_names[slot];
    while (*name && i < sizeof(status_buf) - 1) status_buf[i++] = *name++;
    status_buf[i] = 0;
    return status_buf;
}

/* Saves text_buf/text_len to the slot the current document is bound to,
 * or to the first empty slot if unbound (and binds to it, so subsequent
 * saves of the same still-open document update that slot instead of
 * creating a new file each time). Returns the slot saved to, or -1 if
 * all FS_MAX_FILES slots are already occupied ("disk full"). */
static int save_current_document(void) {
    int slot = bound_slot;
    if (slot < 0) {
        slot = fs_find_empty_slot();
        if (slot < 0) return -1;
    }
    if (!fs_save_slot(slot, text_buf, text_len)) return -1;
    desktop_file_exists[slot] = 1;
    desktop_file_len[slot] = text_len;
    bound_slot = slot;
    return slot;
}

/* Shared Yes/No handling for the confirm dialog, used by both mouse
 * clicks and the Y/N keyboard shortcuts. What "Yes"/"No" actually do
 * depends on why the dialog was opened (confirm_mode). */
static void confirm_yes_action(void) {
    int saved_slot = save_current_document();
    if (confirm_mode == CONFIRM_NEW) {
        text_len = 0; text_buf[0] = 0; bound_slot = -1; ko_ime_reset();
        status = saved_slot >= 0 ? format_saved_status(saved_slot) : "STORAGE FULL - NOT SAVED";
    } else if (confirm_mode == CONFIRM_CLOSE) {
        notepad.open = 0;
        notepad.minimized = 0;
        /* Closing ends this editing session -- clear the buffer so the
         * next time the app icon is double-clicked, it starts blank
         * rather than showing this document's leftover text again. */
        text_len = 0; text_buf[0] = 0; bound_slot = -1; ko_ime_reset();
        status = saved_slot >= 0 ? "SAVED AND CLOSED" : "STORAGE FULL - CLOSED, NOT SAVED";
    }
    confirm_mode = CONFIRM_NONE;
}

static void confirm_no_action(void) {
    if (confirm_mode == CONFIRM_NEW) {
        text_len = 0; text_buf[0] = 0; bound_slot = -1; ko_ime_reset();
        status = "NEW DOCUMENT (NOT SAVED)";
    } else if (confirm_mode == CONFIRM_CLOSE) {
        notepad.open = 0;
        notepad.minimized = 0;
        /* Same as above: discard the in-memory buffer on close so a
         * fresh app-icon open doesn't resurrect this session's text. */
        text_len = 0; text_buf[0] = 0; bound_slot = -1; ko_ime_reset();
        status = "CLOSED (NOT SAVED)";
    }
    confirm_mode = CONFIRM_NONE;
}

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

    /* Start button. The classic "raised" 3D bevel is just a light stripe
     * on the top/left edges and a dark stripe on the bottom/right --
     * and flipping which stripe goes where makes it look "pressed in"
     * while the menu is open. This exact four-line trick single-handedly
     * carried the entire aesthetic of 16-bit UI toolkits. We salute it. */
    bb_fillrect(STARTBTN_X, STARTBTN_Y, STARTBTN_W, STARTBTN_H, COL_LGRAY);
    u8 hi = start_menu_open ? COL_DGRAY : COL_WHITE;
    u8 lo = start_menu_open ? COL_WHITE : COL_DGRAY;
    for (int i = 0; i < STARTBTN_W - 1; i++) bb_putpixel(STARTBTN_X + i, STARTBTN_Y, hi);
    for (int j = 0; j < STARTBTN_H - 1; j++) bb_putpixel(STARTBTN_X, STARTBTN_Y + j, hi);
    for (int i = 0; i < STARTBTN_W; i++) bb_putpixel(STARTBTN_X + i, STARTBTN_Y + STARTBTN_H - 1, lo);
    for (int j = 0; j < STARTBTN_H; j++) bb_putpixel(STARTBTN_X + STARTBTN_W - 1, STARTBTN_Y + j, lo);
    /* nudge the label a pixel down-right while "pressed", like it's
     * physically sinking into the taskbar under the weight of your click */
    int press = start_menu_open ? 1 : 0;
    font_draw_string(STARTBTN_X + 3 + press, STARTBTN_Y + 1 + press, "AM", COL_BLACK);

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

static int start_button_hit(int px, int py) {
    return in_rect(px, py, STARTBTN_X, STARTBTN_Y, STARTBTN_W, STARTBTN_H);
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
 * Start Menu -- pops up above the AM button, Windows-95-style: a dark
 * vertical banner strip down the left side, a short list of items on
 * the right, and "Shut Down..." pinned to the bottom behind a divider
 * because even a two-app OS should let you pretend to turn it off.
 * ============================================================ */
#define STARTMENU_BANNER_W  14
#define STARTMENU_W         120
#define STARTMENU_H         74
#define STARTMENU_ITEM_H    12
#define STARTMENU_ITEMS     3   /* 0=Notepad.exe, 1=Setting.exe, 2=Shut Down... */

static inline int start_menu_x(void) { return STARTBTN_X; }
static inline int start_menu_y(void) { return TASKBAR_Y - STARTMENU_H; }

/* Notepad/Setting are grouped near the top; Shut Down sits glued to the
 * bottom edge behind its own divider, exactly like the real thing --
 * no matter how few programs you have installed, Shut Down never moves. */
static inline int start_menu_item_y(int idx) {
    if (idx == STARTMENU_ITEMS - 1)
        return start_menu_y() + STARTMENU_H - STARTMENU_ITEM_H - 3;
    return start_menu_y() + 4 + idx * STARTMENU_ITEM_H;
}

static int start_menu_item_hit(int px, int py, int idx) {
    int x = start_menu_x() + STARTMENU_BANNER_W + 2;
    int w = STARTMENU_W - STARTMENU_BANNER_W - 4;
    return in_rect(px, py, x, start_menu_item_y(idx), w, STARTMENU_ITEM_H);
}

static void draw_start_menu(int mx, int my) {
    int x = start_menu_x(), y = start_menu_y();
    int w = STARTMENU_W, h = STARTMENU_H;

    /* same drop-shadow-plus-outline recipe as the File dropdown and the
     * confirm dialog -- three popups, one visual language, zero effort
     * spent reinventing it each time */
    bb_fillrect(x + 2, y + 2, w, h, COL_DGRAY);
    bb_fillrect(x, y, w, h, COL_LGRAY);
    bb_rect(x, y, w, h, COL_BLACK);

    /* left banner: solid navy strip with the product name climbing up
     * it bottom-to-top. This is the single most 1995 thing in this
     * entire codebase and we regret nothing. */
    bb_fillrect(x + 1, y + 1, STARTMENU_BANNER_W - 1, h - 2, COL_BLUE);
    font_draw_string_vertical(x + 3, y + h - 9, "MINIWIN", COL_WHITE);

    const char *labels[STARTMENU_ITEMS] = {"Notepad.exe", "Setting.exe", "Shut Down..."};
    int ix = x + STARTMENU_BANNER_W + 2;
    int iw = w - STARTMENU_BANNER_W - 4;
    for (int i = 0; i < STARTMENU_ITEMS; i++) {
        int iy = start_menu_item_y(i);
        int hover = in_rect(mx, my, ix, iy, iw, STARTMENU_ITEM_H);
        u8 bg = hover ? COL_BLUE : COL_LGRAY;
        u8 fg = hover ? COL_WHITE : COL_BLACK;
        bb_fillrect(ix, iy, iw, STARTMENU_ITEM_H, bg);
        font_draw_string(ix + 3, iy + 2, labels[i], fg);
    }

    /* divider directly above Shut Down, the traditional "heads up,
     * something drastic is about to happen" line */
    int sep_y = start_menu_item_y(STARTMENU_ITEMS - 1) - 2;
    for (int i = 0; i < iw; i++) bb_putpixel(ix + i, sep_y, COL_DGRAY);
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
    if (file_menu_open) {
        /* highlight the File label while its dropdown is open, like a
         * pressed menu button */
        bb_fillrect(wx + 3, menu_y, MENU_FILE_LABEL_W - 2, 9, COL_BLUE);
        font_draw_string(wx + 4,  menu_y, "File", COL_WHITE);
    } else {
        font_draw_string(wx + 4,  menu_y, "File", COL_BLACK);
    }
    font_draw_string(wx + 40, menu_y, "Edit", COL_BLACK);
    font_draw_string(wx + 76, menu_y, "Help", COL_BLACK);
    for (int i = 0; i < ww - 2; i++)
        bb_putpixel(wx + 1 + i, menu_y + 9, COL_DGRAY);

    /* text edit area (white, sunken border) */
    int ex = edit_x(), ey = edit_y(), ew = edit_w(), eh = edit_h();
    bb_fillrect(ex, ey, ew, eh, COL_WHITE);
    bb_rect(ex - 1, ey - 1, ew + 2, eh + 2, COL_DGRAY);

    /* text content -- UTF-8 aware: most bytes are 1 ASCII char, but a
     * 3-byte sequence (0xE0 lead byte) is one Hangul syllable/jamo drawn
     * via the Korean font instead of the Latin one. Both render at the
     * same 8px advance since the Hangul glyphs were extracted at 8x8 to
     * match. */
    int cx = ex + 2, cy = ey + 2;
    for (u32 i = 0; i < text_len; ) {
        int clen = ko_utf8_char_len((unsigned char)text_buf[i]);
        if (clen == 3 && i + 3 <= text_len) {
            if (cx > ex + ew - 10) { cx = ex + 2; cy += 9; }
            if (cy > ey + eh - 8) break;
            int cp = ko_utf8_decode3(&text_buf[i]);
            ko_font_draw_codepoint(cx, cy, cp, COL_BLACK);
            cx += 8;
            i += 3;
            continue;
        }
        char c = text_buf[i];
        if (c == '\n' || cx > ex + ew - 10) {
            cx = ex + 2;
            cy += 9;
            if (c == '\n') { i++; continue; }
        }
        if (cy > ey + eh - 8) break;
        font_draw_char(cx, cy, c, COL_BLACK);
        cx += 8;
        i++;
    }

    /* Live preview of the syllable currently being composed (if Hangul
     * mode is on and something's mid-composition), shown right at the
     * cursor position before it's actually committed to text_buf. */
    if (ko_ime_enabled && ko_ime_is_composing()) {
        if (cx > ex + ew - 10) { cx = ex + 2; cy += 9; }
        int preview_cp = ko_ime_preview_codepoint();
        if (preview_cp >= 0 && cy <= ey + eh - 8) {
            ko_font_draw_codepoint(cx, cy, preview_cp, COL_BLACK);
            cx += 8; /* advance past the preview glyph so the cursor bar
                      * below is drawn after it, not overlapping it */
        }
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
 * File menu dropdown + Save/New confirm dialog drawing
 * ============================================================ */
static void draw_file_menu(int mx, int my) {
    int x = file_menu_x();
    int y = file_menu_top_y();
    int w = FILE_MENU_W;
    int h = FILE_MENU_ITEM_H * 3;

    bb_fillrect(x + 2, y + 2, w, h, COL_DGRAY); /* drop shadow */
    bb_fillrect(x, y, w, h, COL_LGRAY);
    bb_rect(x, y, w, h, COL_BLACK);

    const char *labels[3] = {"Save As", "Save", "New"};
    for (int i = 0; i < 3; i++) {
        int iy = y + i * FILE_MENU_ITEM_H;
        int hover = in_rect(mx, my, x, iy, w, FILE_MENU_ITEM_H);
        u8 bg = hover ? COL_BLUE : COL_LGRAY;
        u8 fg = hover ? COL_WHITE : COL_BLACK;
        bb_fillrect(x + 1, iy, w - 2, FILE_MENU_ITEM_H, bg);
        font_draw_string(x + 4, iy + 1, labels[i], fg);
    }
}

static void draw_confirm_dialog(int mx, int my) {
    int x = confirm_x(), y = confirm_y();

    bb_fillrect(x + 3, y + 3, CONFIRM_W, CONFIRM_H, COL_DGRAY);
    bb_fillrect(x, y, CONFIRM_W, CONFIRM_H, COL_LGRAY);
    bb_rect(x, y, CONFIRM_W, CONFIRM_H, COL_BLACK);

    font_draw_string(x + 8, y + 8,  "Save changes", COL_BLACK);
    if (confirm_mode == CONFIRM_CLOSE) {
        font_draw_string(x + 8, y + 18, "before closing?", COL_BLACK);
    } else {
        font_draw_string(x + 8, y + 18, "before New?", COL_BLACK);
    }

    int yb = confirm_btn_y();
    int yesx = confirm_yes_x(), nox = confirm_no_x();
    int yes_hover = in_rect(mx, my, yesx, yb, CONFIRM_BTN_W, CONFIRM_BTN_H);
    int no_hover  = in_rect(mx, my, nox,  yb, CONFIRM_BTN_W, CONFIRM_BTN_H);

    bb_fillrect(yesx, yb, CONFIRM_BTN_W, CONFIRM_BTN_H, yes_hover ? COL_BLUE : COL_WHITE);
    bb_rect(yesx, yb, CONFIRM_BTN_W, CONFIRM_BTN_H, COL_BLACK);
    font_draw_string(yesx + 8, yb + 2, "Yes", yes_hover ? COL_WHITE : COL_BLACK);

    bb_fillrect(nox, yb, CONFIRM_BTN_W, CONFIRM_BTN_H, no_hover ? COL_BLUE : COL_WHITE);
    bb_rect(nox, yb, CONFIRM_BTN_W, CONFIRM_BTN_H, COL_BLACK);
    font_draw_string(nox + 10, yb + 2, "No", no_hover ? COL_WHITE : COL_BLACK);
}

/* ============================================================
 * Desktop file icons -- one per occupied slot (up to FS_MAX_FILES),
 * arranged in a row below the app icon since the 320x200 screen has much
 * more spare width than height. Labels are short ("DOC", "DOC2", ...)
 * rather than the full filename, since there isn't room for 4 full
 * "NEW_TXT_DOC_N.TXT" labels side by side -- the real saved filename is
 * still the proper one on disk and in status messages, this is just a
 * compact on-screen label.
 * ============================================================ */
#define FILEICON_ROW_Y      (ICON_Y + ICON_H + 6)
#define FILEICON_SPACING_X  34

static const char *fileicon_labels[FS_MAX_FILES] = {"DOC", "DOC2", "DOC3", "DOC4"};

static inline int fileicon_x(int slot) { return ICON_X + slot * FILEICON_SPACING_X; }
static inline int fileicon_y(int slot) { (void)slot; return FILEICON_ROW_Y; }

static void draw_desktop_file_icons(void) {
    for (int slot = 0; slot < FS_MAX_FILES; slot++) {
        if (!desktop_file_exists[slot]) continue;
        int x = fileicon_x(slot), y = fileicon_y(slot);
        bb_fillrect(x + 4, y, 16, 12, COL_WHITE);
        bb_rect(x + 4, y, 16, 12, COL_BLACK);
        bb_fillrect(x + 4 + 11, y, 5, 4, DESKTOP_COLOR_ICON_BG);
        bb_rect(x + 4 + 11, y, 5, 4, COL_BLACK);
        /* a couple of horizontal "text lines" inside so it reads as a
         * document icon, visually distinct from the app icon above it */
        bb_fillrect(x + 7, y + 4, 8, 1, COL_LGRAY);
        bb_fillrect(x + 7, y + 7, 8, 1, COL_LGRAY);
        font_draw_string(x, y + 14, fileicon_labels[slot], COL_BLACK);
    }
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
    draw_desktop_icon2();
    draw_desktop_file_icons();

    if (status_msg) draw_status_line(status_msg);

    if (notepad.open && !notepad.minimized) {
        draw_window();
        if (file_menu_open) draw_file_menu(mouse_x, mouse_y);
        if (confirm_mode != CONFIRM_NONE) draw_confirm_dialog(mouse_x, mouse_y);
    }

    draw_taskbar();
    /* drawn after the taskbar (and after Notepad's window above) so it
     * sits on top of both -- popups always win the z-order argument */
    if (start_menu_open) draw_start_menu(mouse_x, mouse_y);
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

    /* Check for previously-saved files on disk (real, persistent storage
     * via the ATA driver -- this survives across QEMU runs as long as the
     * disk image itself isn't rebuilt from scratch). */
    for (int slot = 0; slot < FS_MAX_FILES; slot++) {
        u32 loaded_len = 0;
        if (fs_check_slot(slot, &loaded_len)) {
            desktop_file_exists[slot] = 1;
            desktop_file_len[slot] = loaded_len;
        }
    }

    int mx = VGA_WIDTH / 2, my = VGA_HEIGHT / 2;
    int awaiting_second_click = 0;   /* 1 = one icon click seen, waiting for a 2nd within the window */
    u32 last_icon_click_tick = 0;
    int awaiting_second_click_setting = 0;   /* same idea, but for the dummy SETTING.EXE icon */
    u32 last_setting_click_tick = 0;
    int awaiting_second_click_file_slot = -1;   /* which file icon (if any) saw a first click */
    u32 last_file_icon_click_tick = 0;
    u32 tick = 0;
    const u32 double_click_window = 60000; /* tuned for delay(2000) per frame;
                                             * adjust proportionally if delay() changes */

    /* Window-drag state: while dragging, we track the offset from the
     * window's top-left corner to the point the user grabbed, so the
     * window follows the cursor without "snapping" its corner to it. */
    int dragging = 0;
    int drag_offset_x = 0, drag_offset_y = 0;

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
                if (notepad.open && !notepad.minimized && confirm_mode != CONFIRM_NONE) {
                    /* Modal: while a Save-changes confirm dialog is open,
                     * it takes total precedence -- the user must explicitly
                     * choose Yes or No. Clicks elsewhere are ignored. */
                    if (confirm_yes_hit(mx, my)) {
                        confirm_yes_action();
                    } else if (confirm_no_hit(mx, my)) {
                        confirm_no_action();
                    }
                } else if (notepad.open && !notepad.minimized && file_menu_open) {
                    /* Modal-ish: while the File dropdown is open, a click
                     * either picks an item or dismisses the menu -- it
                     * doesn't fall through to icon/titlebar/etc handling
                     * this same frame. */
                    if (file_menu_item_hit(mx, my, 0)) {
                        status = "SAVE AS - COMING SOON";
                        file_menu_open = 0;
                    } else if (file_menu_item_hit(mx, my, 1)) {
                        int saved_slot = save_current_document();
                        status = saved_slot >= 0 ? format_saved_status(saved_slot) : "STORAGE FULL - NOT SAVED";
                        file_menu_open = 0;
                    } else if (file_menu_item_hit(mx, my, 2)) {
                        confirm_mode = CONFIRM_NEW;
                        file_menu_open = 0;
                    } else {
                        file_menu_open = 0; /* click outside just dismisses it */
                    }
                } else if (start_menu_open) {
                    /* Same "pick an item or dismiss" contract as the File
                     * dropdown just above: whatever this click was for,
                     * the menu closes afterward, so one shared reset at
                     * the bottom covers every branch here. */
                    if (start_menu_item_hit(mx, my, 0)) {
                        notepad.open = 1;
                        notepad.minimized = 0;
                        ko_ime_reset();
                        status = "NOTEPAD.EXE OPENED (F7/RALT: HANGUL)";
                    } else if (start_menu_item_hit(mx, my, 1)) {
                        status = "SETTING.EXE - NOT IMPLEMENTED YET";
                    } else if (start_menu_item_hit(mx, my, 2)) {
                        /* no real power management on a machine that's
                         * just a QEMU process, so this is where the
                         * honesty of SETTING.EXE's placeholder rubs off
                         * on Shut Down too */
                        status = "SHUT DOWN - NOT IMPLEMENTED YET";
                    }
                    start_menu_open = 0;
                } else if (start_button_hit(mx, my)) {
                    start_menu_open = 1;
                } else if (in_rect(mx, my, ICON_X, ICON_Y, ICON_W, ICON_H)) {
                    if (awaiting_second_click && (tick - last_icon_click_tick) < double_click_window) {
                        /* this is the 2nd click of a double-click: open it */
                        notepad.open = 1;
                        notepad.minimized = 0;
                        ko_ime_reset();
                        status = "NOTEPAD.EXE OPENED (F7/RALT: HANGUL)";
                        awaiting_second_click = 0;
                    } else {
                        /* this is a 1st click: arm the double-click window */
                        awaiting_second_click = 1;
                        last_icon_click_tick = tick;
                    }
                } else if (in_rect(mx, my, ICON2_X, ICON2_Y, ICON2_W, ICON2_H)) {
                    /* SETTING.EXE is a dummy icon -- double-clicking it just
                     * shows a placeholder status message, no window opens. */
                    if (awaiting_second_click_setting && (tick - last_setting_click_tick) < double_click_window) {
                        status = "SETTING.EXE - NOT IMPLEMENTED YET";
                        awaiting_second_click_setting = 0;
                    } else {
                        awaiting_second_click_setting = 1;
                        last_setting_click_tick = tick;
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
                        /* Ask before closing, same Yes/No pattern as New. */
                        confirm_mode = CONFIRM_CLOSE;
                    } else if (file_label_hit(mx, my)) {
                        file_menu_open = 1;
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
                    /* Minimized taskbar close also asks first, for
                     * consistency with the window's own X button. */
                    confirm_mode = CONFIRM_CLOSE;
                    notepad.minimized = 0; /* bring it back on-screen so the dialog is visible */
                } else {
                    /* Check desktop file icons last (any slot). */
                    for (int slot = 0; slot < FS_MAX_FILES; slot++) {
                        if (!desktop_file_exists[slot]) continue;
                        if (!in_rect(mx, my, fileicon_x(slot), fileicon_y(slot), ICON_W, ICON_H)) continue;

                        if (awaiting_second_click_file_slot == slot &&
                            (tick - last_file_icon_click_tick) < double_click_window) {
                            /* 2nd click: open Notepad (if needed) and load
                             * this slot's saved contents into the text
                             * buffer, replacing whatever was there
                             * unsaved, and bind the document to this slot
                             * so a subsequent Save updates it in place. */
                            u32 loaded = fs_load_slot(slot, text_buf, sizeof(text_buf) - 1);
                            text_buf[loaded] = 0;
                            text_len = loaded;
                            bound_slot = slot;
                            notepad.open = 1;
                            notepad.minimized = 0;
                            ko_ime_reset();
                            status = format_saved_status(slot); /* reuse "SAVED: name" wording to show which file opened */
                            awaiting_second_click_file_slot = -1;
                        } else {
                            awaiting_second_click_file_slot = slot;
                            last_file_icon_click_tick = tick;
                        }
                        break;
                    }
                }
            }
        }

        /* ---- keyboard: only affects notepad text when it's open+visible ---- */
        int k = keyboard_poll_key();

        if (confirm_mode != CONFIRM_NONE) {
            /* keyboard shortcuts for the confirm dialog, so it can be
             * driven without the mouse too */
            if (k == 'y' || k == 'Y') {
                confirm_yes_action();
            } else if (k == 'n' || k == 'N') {
                confirm_no_action();
            }
        } else if ((k == KEY_F7 || k == KEY_RALT) && notepad.open && !notepad.minimized && !file_menu_open) {
            /* F7 or Right Alt toggles Hangul input mode (Right Alt matches
             * the 한/영 key position on real Korean keyboards; F7 is kept
             * as an easy-to-reach alternative). Switching modes
             * mid-syllable commits whatever was in progress first, so
             * nothing is silently lost. */
            if (ko_ime_is_composing()) ko_ime_commit(text_buf, &text_len, sizeof(text_buf));
            ko_ime_enabled = !ko_ime_enabled;
            status = ko_ime_enabled ? "HANGUL MODE ON (F7/RALT TO SWITCH BACK)" : "ENGLISH MODE ON (F7/RALT FOR HANGUL)";
        } else if (k > 0 && notepad.open && !notepad.minimized && !file_menu_open) {
            char c = (char)k;
            if (keyboard_ctrl_held()) {
                /* Ctrl+S / Ctrl+N / Ctrl+W accelerators, mirroring the
                 * File menu and the title bar's close button */
                if (ko_ime_is_composing()) ko_ime_commit(text_buf, &text_len, sizeof(text_buf));
                if (c == 's' || c == 'S') {
                    int saved_slot = save_current_document();
                    status = saved_slot >= 0 ? format_saved_status(saved_slot) : "STORAGE FULL - NOT SAVED";
                } else if (c == 'n' || c == 'N') {
                    confirm_mode = CONFIRM_NEW;
                } else if (c == 'w' || c == 'W') {
                    confirm_mode = CONFIRM_CLOSE;
                }
            } else if (c == '\b') {
                if (ko_ime_enabled && ko_ime_backspace()) {
                    /* consumed by the IME: undid one step of the syllable
                     * currently being composed, nothing else to do */
                } else if (text_len > 0) {
                    int del = ko_utf8_last_char_len(text_buf, text_len);
                    text_len -= del;
                    text_buf[text_len] = 0;
                }
            } else if (ko_ime_enabled && ko_ime_feed_key(c, text_buf, &text_len, sizeof(text_buf))) {
                /* consumed as a jamo keystroke -- composition state
                 * updated (and/or a completed syllable was appended to
                 * text_buf) inside ko_ime_feed_key() itself */
            } else if (c == '\n' || c == ' ' || (c >= 32 && c < 127)) {
                if (ko_ime_enabled && ko_ime_is_composing()) {
                    /* a non-jamo key (space, enter, punctuation) always
                     * flushes an in-progress syllable first, matching how
                     * every real Hangul IME behaves */
                    ko_ime_commit(text_buf, &text_len, sizeof(text_buf));
                }
                kstrcpy_append(text_buf, &text_len, sizeof(text_buf), c);
            }
        }

        render_frame(mx, my, status);
        delay(2000); /* lowered further from 8000 -- mouse felt sluggish/
                      * capped at 8000; this loop's real-world speed varies
                      * a lot by CPU, so double_click_window above is
                      * scaled proportionally to keep the same real-time
                      * double-click window. */
    }
}
