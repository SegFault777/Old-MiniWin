# MiniWin 1.0

A real, bootable, from-scratch operating system with a Windows 1.0-style GUI,
mouse support, window management (drag, minimize/restore, maximize, close),
and a working File menu with **real, persistent disk storage** across
multiple auto-numbered files. Written in x86 assembly (NASM) + C, no libc,
no existing OS underneath it.

This is a genuine 32-bit protected-mode kernel — not a simulation, not a web
page pretending to be an OS. It boots on real x86 hardware or in an emulator
like QEMU, and everything you see (the window, the text editor, the mouse
cursor, the taskbar, the saved files) is driven by code in this repo
talking directly to VGA, PS/2, and ATA disk hardware registers.

## What it actually does

- A 512-byte boot sector (`boot/boot.asm`) that the BIOS loads and runs
- Reads the kernel off disk via BIOS `int 0x13`
- Sets up a GDT and switches the CPU from 16-bit real mode into 32-bit
  protected mode
- Jumps into a C kernel (`kernel/kernel.c`) that:
  - Programs the VGA controller directly into Mode 13h (320x200, 256-color)
    — no BIOS video calls, since BIOS interrupts aren't usable once you're
    in protected mode
  - Renders every frame into an off-screen back buffer and blits it to
    video memory in one shot (double buffering — no flicker)
  - Drives a PS/2 mouse directly (no interrupts, polled), decoding raw
    3-byte packets into cursor movement and left-click edge detection,
    tuned for a natural 1:1 feel (full hardware-range per-packet movement,
    a fast main-loop poll rate)
  - Polls the PS/2 keyboard controller, decodes scancodes (arrow keys,
    shift, ctrl, real lowercase letters) into ASCII -- correctly
    distinguishing keyboard bytes from mouse bytes sharing the same PS/2
    status port
  - Renders text with a hand-drawn 8x8 bitmap font (upper+lowercase Latin)
  - **Talks directly to the disk controller over ATA PIO (I/O ports
    0x1F0-0x1F7)** to read and write raw sectors -- no BIOS calls, no OS
    underneath -- with a minimal but genuinely persistent multi-file
    storage format on top of it
  - Draws a desktop with a "NOTEPAD.EXE" icon (double-click opens a
    Notepad-style window) and a **"SETTING.EXE" dummy icon** next to it
    (double-click just shows a "not implemented yet" message -- there's no
    real settings behind it, it's a placeholder)
  - The window can be **dragged by its title bar**, **maximized** (`□`,
    fills the screen above the taskbar), **minimized** (`_`, to a taskbar
    button with its own restore/close glyphs)
  - **Closing the window (`X`, or the taskbar's close glyph, or Ctrl+W)**
    asks "Save changes before closing?" with Yes/No, just like "New" does
  - The **File menu** ("File" in the menu bar) is a real working dropdown:
    - **Save** writes the current text to disk. The first save picks the
      first free slot (`NEW_TXT_DOC.TXT`); once a document is saved (or
      opened from a desktop icon), further saves of that same document
      update the same file rather than creating a new one every time
    - **New** asks "Save changes before New?" -- Yes saves then starts a
      blank document, No discards and starts blank
    - **Save As** is a placeholder for now (shows "coming soon")
  - **Saving never silently overwrites a different, already-saved file**:
    if the current document isn't already bound to a specific saved file,
    Save picks the next free slot automatically --
    `NEW_TXT_DOC.TXT` → `NEW_TXT_DOC_2.TXT` → `_3.TXT` → `_4.TXT` (up to 4
    files; see "About the file storage" below)
  - Each saved file gets its own small desktop icon (labeled "DOC",
    "DOC2", "DOC3", "DOC4" to fit four side by side on this tiny screen);
    **double-clicking one opens Notepad and loads that file's contents**
  - **Closing the app now properly starts fresh next time**: closing
    Notepad (Yes or No) clears the in-memory buffer, so double-clicking
    NOTEPAD.EXE again always starts a blank document instead of showing
    the previous session's leftover text
  - Saved files are **real and persistent**: they survive closing and
    reopening QEMU (as long as you don't re-run `./build.sh`, which
    recreates the disk image from scratch -- see Tuning notes below)
  - Keyboard accelerators: **Ctrl+S** (Save), **Ctrl+N** (New), **Ctrl+W**
    (close, same as the title bar's X), and **Y/N** while any confirm
    dialog is open

## What it is NOT (being upfront about this)

This is a minimal educational kernel, not Windows 3.1. There is no:
- A real filesystem (FAT, ext, etc.) -- see "About the file storage" below
- "Save As" with a custom filename yet (planned next)
- Korean/Hangul text input yet -- a real, legible 8x8 Hangul bitmap font
  (달무리/Dalmoori, see `third_party/dalmoori-font/`, Apache 2.0) has been
  sourced and verified to render beautifully at 8x8, but the actual
  2-beolsik input method (jamo composition into syllables) and a curated
  glyph table are a separate, dedicated piece of work still to come
- Multiple/overlapping windows (only one window, Notepad, exists, holding
  one document at a time)
- Manual window resizing by dragging an edge/corner (only move + maximize)
- Real Paint, Explorer, or a browser — those would each be their own
  multi-month project; SETTING.EXE is an intentionally empty placeholder
- Multitasking, memory management beyond a flat address space, or
  networking
- Right-click or scroll-wheel support (left-click only)
- Any warning if you double-click a file icon while you have unsaved
  changes in the currently-open document -- opening a saved file
  overwrites the editor's contents immediately, no confirm dialog (unlike
  New/Close, which do ask). Worth knowing if you're mid-edit.

Building a real Windows-3.1-equivalent OS is a multi-year undertaking even
for a full team. This project is the genuine first rung of that ladder —
the same one OS-development hobbyists start on — done for real rather than
faked.

## About the file storage (important to understand)

This is **not** a real filesystem like FAT32 or ext4. It's a small, fixed
number of file slots on disk:

- 4 slots total, each with a fixed, pre-assigned filename:
  `NEW_TXT_DOC.TXT`, `NEW_TXT_DOC_2.TXT`, `NEW_TXT_DOC_3.TXT`,
  `NEW_TXT_DOC_4.TXT`
- Each slot: 1 header sector (magic number + length + filename) + 8 data
  sectors (up to 4096 bytes of text)
- Saving with no document bound to a slot picks the first empty one;
  saving again on the same still-open document updates that same slot
  (normal "Save" behavior, not "Save As" every time)
- If all 4 slots are full, Save reports "STORAGE FULL" and doesn't save --
  there's no delete UI yet to free one up

A real multi-file filesystem (arbitrary names, more files, delete support)
would be a substantial follow-up project — noted under "Extending it"
below.

**Verified, not assumed:** the write+persistence path was tested by typing
text, saving, fully terminating QEMU (equivalent to powering off), and
relaunching the same `os-image.img` without rebuilding -- the desktop file
icon was still there. The close→reopen buffer-clearing fix and the
auto-numbered multi-slot saving were both verified end-to-end (including
reading the raw disk image from the host side to confirm exactly which
slot got which filename and content).

## Tuning notes

**Rebuilding wipes saved files.** `./build.sh` recreates `os-image.img`
from scratch every time. Saved documents only persist across
`qemu-system-i386` runs that reuse the same already-built image.

**Mouse feel.** Per-packet movement is clamped to ±127 (the true PS/2
hardware range) rather than an artificially tighter cap, and the main
loop's delay was lowered for snappier response -- if you ever tune
`delay()` in `kmain()`'s loop, scale `double_click_window` proportionally
(comment right above it explains the ratio).

**ATA disk assumption.** The disk driver (`ata.h`) talks to the primary
ATA bus's master drive over the legacy I/O ports (0x1F0-0x1F7), matching
QEMU's default `-drive format=raw,file=...` (no explicit `if=`). Custom
QEMU flags that change the disk interface (e.g. AHCI/virtio) will break
this driver.

## Running it

You need `nasm`, `gcc` (with 32-bit support), `ld`/`objcopy`, and
`qemu-system-i386`.

```bash
./build.sh
qemu-system-i386 -drive format=raw,file=build/os-image.img
```

A QEMU window should open, show the boot messages briefly, then switch into
graphics mode and show the desktop:
- Move the mouse to move the cursor
- **Double-click** "NOTEPAD.EXE" to open the app; double-click "SETTING.EXE"
  for a placeholder message
- Click inside the window and type to enter text (lowercase works!)
- **Drag the title bar** to move the window; `□` to maximize/restore,
  `_` to minimize, `X` to close (asks to save first)
- **File → Save** to write your text to disk -- its icon appears on the
  desktop; saving again updates the same file, but a *different* new
  document saves as the next numbered file instead of overwriting
- **Double-click any saved file's desktop icon** any time afterward (even
  after closing and reopening the whole OS) to load it back into Notepad
- **File → New** starts a fresh document -- you'll be asked whether to
  save first
- Keyboard shortcuts: Ctrl+S save, Ctrl+N new, Ctrl+W close, Y/N to answer
  a confirm dialog

If you don't have these tools installed (Ubuntu/Debian/WSL):

```bash
sudo apt-get install nasm qemu-system-x86 gcc
```

## Project layout

```
boot/boot.asm      - Stage-1 bootloader: real mode -> protected mode
kernel/kentry.asm  - Asm stub: zeroes .bss, then calls into C's kmain()
kernel/kernel.c    - Main kernel logic: desktop, window state, File menu, input loop
kernel/vga.h       - Direct VGA register programming (Mode 13h) + back buffer
kernel/font.h      - 8x8 bitmap font (upper+lowercase Latin) + text rendering
kernel/keyboard.h  - PS/2 keyboard scancode polling + ASCII decode (+ ctrl tracking)
kernel/mouse.h     - PS/2 mouse polling + packet decode + click-edge detection
kernel/ata.h       - ATA PIO disk driver (raw sector read/write, no BIOS)
kernel/fs.h        - Minimal multi-slot persistent storage format, built on ata.h
kernel/io.h        - inb/outb/inw/outw port I/O helpers
kernel/link.ld     - Linker script (kernel loads at physical 0x10000)
build.sh           - Builds everything into build/os-image.img
third_party/dalmoori-font/ - 8x8 Hangul bitmap font (Apache 2.0), sourced for
                     the upcoming Korean input support; not wired in yet
```

## Extending it

Some natural next steps, roughly in order of difficulty:
1. **Korean input**: a real 2-beolsik jamo-composition input method plus a
   curated set of common Hangul syllables rendered from the bundled
   Dalmoori font (verified legible at 8x8) -- a dedicated, separate piece
   of work given the size of a real IME implementation
2. A confirm-before-overwrite prompt when double-clicking a file icon
   while there are unsaved changes in the editor
3. **Save As** with a real filename prompt (a simple on-screen text input)
4. More file slots / a real directory structure (arbitrary filenames, a
   delete option to free up slots once 4 are full)
5. Window dragging by edge/corner to resize, not just move
6. A second real app (not just the SETTING.EXE placeholder) to prove out
   multi-window taskbar management
7. Line wrapping and scrolling in the text area
8. Interrupt-driven keyboard/mouse/disk I/O (IDT + PIC remapping) instead
   of polling, the real prerequisite for eventually adding multitasking
