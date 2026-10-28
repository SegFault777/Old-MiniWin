# MiniWin 1.0

A real, bootable, from-scratch operating system with a Windows 1.0-style GUI,
mouse support, window management (drag, minimize/restore, maximize, close),
and a working File menu with **real, persistent disk storage**. Written in
x86 assembly (NASM) + C, no libc, no existing OS underneath it.

This is a genuine 32-bit protected-mode kernel — not a simulation, not a web
page pretending to be an OS. It boots on real x86 hardware or in an emulator
like QEMU, and everything you see (the window, the text editor, the mouse
cursor, the taskbar, the saved file) is driven by code in this repo talking
directly to VGA, PS/2, and ATA disk hardware registers.

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
    3-byte packets into cursor movement and left-click edge detection
  - Polls the PS/2 keyboard controller, decodes scancodes (including arrow
    keys, shift, ctrl, and real lowercase letters) into ASCII -- correctly
    distinguishing keyboard bytes from mouse bytes sharing the same PS/2
    status port
  - Renders text with a hand-drawn 8x8 bitmap font that includes real
    lowercase glyphs (proper x-height, descenders on g/j/p/q/y)
  - **Talks directly to the disk controller over ATA PIO (I/O ports
    0x1F0-0x1F7)** to read and write raw sectors -- no BIOS calls, no OS
    underneath, just the kernel driving the hardware -- and implements a
    minimal but genuinely persistent file-storage format on top of it
  - Draws a desktop with a "NOTEPAD.EXE" icon; **double-clicking it** opens
    a Notepad-style window (title bar, menu bar, text area)
  - The window can be **dragged by its title bar**, **maximized** (`□`,
    fills the screen above the taskbar), **minimized** (`_`, to a taskbar
    button with its own restore/close glyphs), and **closed** (`X`)
  - The **File menu** ("File" in the menu bar) is a real working dropdown:
    - **Save** writes the current text to disk as `NEW_TXT_DOC.TXT` and
      shows its icon on the desktop
    - **New** asks "Save changes before New?" with Yes/No buttons -- Yes
      saves then clears the document, No clears without saving
    - **Save As** is a placeholder for now (shows "coming soon")
  - Saved files are **real and persistent**: they survive closing and
    reopening QEMU (as long as you don't re-run `./build.sh`, which
    recreates the disk image from scratch -- see Tuning notes below)
  - Keyboard accelerators: **Ctrl+S** (Save), **Ctrl+N** (New, still shows
    the confirm dialog), and **Y/N** while the confirm dialog is open

## What it is NOT (being upfront about this)

This is a minimal educational kernel, not Windows 3.1. There is no:
- A real filesystem (FAT, ext, etc.) -- see "About the file storage" below
- "Save As" with a custom filename yet (planned next)
- Opening/loading the saved file back into Notepad from its desktop icon
  (the icon shows it exists; double-clicking it doesn't load it yet)
- Multiple documents/tabs, or multiple/overlapping windows (only one
  window, Notepad, exists)
- Manual window resizing by dragging an edge/corner (only move + maximize)
- Real Paint, Explorer, or a browser — those would each be their own
  multi-month project
- Multitasking, memory management beyond a flat address space, or
  networking
- Right-click or scroll-wheel support (left-click only)

Building a real Windows-3.1-equivalent OS is a multi-year undertaking even
for a full team. This project is the genuine first rung of that ladder —
the same one OS-development hobbyists start on — done for real rather than
faked.

## About the file storage (important to understand)

This is **not** a real filesystem like FAT32 or ext4. It's the simplest
possible thing that is still genuinely persistent: one fixed slot on disk.

- **LBA 100**: a header sector (magic number + file length + filename)
- **LBA 101-108**: up to 4096 bytes of file data (8 sectors)

There's only room for one file (always named `NEW_TXT_DOC.TXT`), and saving
always overwrites that same slot. This matches exactly what was asked for
(Save creates/overwrites `NEW_TXT_DOC.TXT`), while keeping the on-disk
format simple enough to implement and verify correctly in this session. A
real multi-file filesystem (directory entries, free space tracking, etc.)
would be a substantial follow-up project — noted under "Extending it"
below.

**Verified, not assumed:** I tested this by typing text, saving with
Ctrl+S, fully terminating the QEMU process (equivalent to powering off the
machine), and relaunching the *same* `os-image.img` without rebuilding --
the desktop file icon was still there, proving the save round-trips through
real ATA disk writes and survives a full power cycle.

## Tuning notes

**Rebuilding wipes saved files.** `./build.sh` recreates `os-image.img`
from scratch every time (bootloader + kernel + a zeroed file-storage area).
This means saved documents only persist across `qemu-system-i386` runs
that reuse the same already-built image -- if you rebuild, you're starting
fresh, the same way reinstalling an OS would wipe its disk.

**ATA disk assumption.** The disk driver (`ata.h`) talks to the primary
ATA bus's master drive over the legacy I/O ports (0x1F0-0x1F7). This
matches QEMU's default `-drive format=raw,file=...` (no explicit `if=`),
which is the same command used throughout this project and the same
underlying disk the bootloader's BIOS calls already read from to boot in
the first place. If you customize the QEMU command line to use a different
disk interface (e.g. `-M q35` with AHCI, or `if=virtio`), this driver
won't find the disk -- stick to the plain command below.

**Mouse Y-axis direction / cursor snapping / random characters while
mousing / double-click timing.** See the comments in `mouse.h`,
`keyboard.h`, and the top of `kmain()` in `kernel.c` -- these were tuned
and fixed during development against real WSLg/host hardware; the code
comments explain what each constant controls if you ever need to adjust
them for different hardware.

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
- **Double-click** the "NOTEPAD.EXE" icon (top-left) to open the app
- Click inside the window and type to enter text (lowercase works!)
- **Drag the title bar** to move the window anywhere on screen
- Click `□` to maximize / restore, `_` to minimize, `X` to close
- Click **File** in the menu bar, then **Save** to write your text to disk
  as `NEW_TXT_DOC.TXT` -- its icon appears on the desktop
- Click **File → New** to start a fresh document -- you'll be asked
  whether to save first
- Keyboard shortcuts: Ctrl+S to save, Ctrl+N for New, Y/N to answer the
  save-changes dialog

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
kernel/font.h      - 8x8 bitmap font (upper+lowercase) + text rendering
kernel/keyboard.h  - PS/2 keyboard scancode polling + ASCII decode (+ ctrl tracking)
kernel/mouse.h     - PS/2 mouse polling + packet decode + click-edge detection
kernel/ata.h       - ATA PIO disk driver (raw sector read/write, no BIOS)
kernel/fs.h        - Minimal single-file persistent storage format, built on ata.h
kernel/io.h        - inb/outb/inw/outw port I/O helpers
kernel/link.ld     - Linker script (kernel loads at physical 0x10000)
build.sh           - Builds everything into build/os-image.img
```

## Extending it

Some natural next steps, roughly in order of difficulty:
1. **Save As** with a real filename prompt (a simple on-screen text input)
2. Opening the saved file back into Notepad by double-clicking its desktop
   icon (the read path, `fs_load_file()`, already exists in `fs.h` --
   just needs UI wiring)
3. Multiple files: turn the single fixed slot into a small directory table
   (e.g. 8-16 fixed-size entries) so more than one document can be saved
4. Window dragging by edge/corner to resize, not just move
5. A second app (e.g. a fake "Paint" window) to prove out multi-window
   taskbar management
6. Line wrapping and scrolling in the text area
7. Interrupt-driven keyboard/mouse/disk I/O (IDT + PIC remapping) instead
   of polling, which is the real prerequisite for eventually adding
   multitasking
