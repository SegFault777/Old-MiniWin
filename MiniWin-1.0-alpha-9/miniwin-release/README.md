# MiniWin 1.0

A real, bootable, from-scratch operating system with a Windows 1.0-style GUI,
mouse support, and window management (drag, minimize/restore, maximize,
close). Written in x86 assembly (NASM) + C, no libc, no existing OS
underneath it.

This is a genuine 32-bit protected-mode kernel — not a simulation, not a web
page pretending to be an OS. It boots on real x86 hardware or in an emulator
like QEMU, and everything you see (the window, the text editor, the mouse
cursor, the taskbar) is driven by code in this repo talking directly to VGA
and PS/2 hardware registers.

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
    video memory in one shot (double buffering — no flicker as windows
    open/close/move)
  - Drives a PS/2 mouse directly (no interrupts, polled), decoding raw
    3-byte packets into cursor movement and left-click edge detection
  - Polls the PS/2 keyboard controller, decodes scancodes (including arrow
    keys, shift, and real lowercase letters) into ASCII -- and correctly
    distinguishes keyboard bytes from mouse bytes sharing the same PS/2
    status port
  - Renders text with a genuine hand-drawn 8x8 bitmap font that includes
    real lowercase glyphs (with proper x-height and descenders on g/j/p/q/y),
    not just upper-cased fallbacks
  - Draws a desktop with a "NOTEPAD.EXE" icon; **double-clicking it** opens
    a Notepad-style window (title bar, menu bar, text area)
  - The window can be **dragged by its title bar** to anywhere on screen
  - The window's title bar has working `_` (minimize), `□` (maximize —
    fills the screen above the taskbar, click again to restore), and `X`
    (close) buttons
  - Minimizing sends the window to a taskbar strip at the bottom of the
    screen, showing a small button with its own restore (two overlapping
    squares) and close (X) glyphs
  - Clicking restore brings the window back exactly as it was (including
    its dragged position/size before minimizing); clicking close ends the
    app and clears the taskbar button

## What it is NOT (being upfront about this)

This is a minimal educational kernel, not Windows 3.1. There is no:
- File system, disk driver beyond raw sector reads, or way to save files
- Multiple/overlapping windows or manual resizing (only one window,
  Notepad, exists; it can be moved and maximized but not resized by
  dragging an edge/corner)
- Real Paint, Explorer, or a browser — those would each be their own
  multi-month project
- Multitasking, memory management beyond a flat address space, or
  networking
- Right-click or scroll-wheel support (left-click only)

Building a real Windows-3.1-equivalent OS is a multi-year undertaking even
for a full team. This project is the genuine first rung of that ladder —
the same one OS-development hobbyists start on — done for real rather than
faked.

## Tuning notes

**Mouse Y-axis direction.** PS/2 mice can report their Y-axis polarity
differently depending on the host/emulator. This build's `mouse.h` flips
the raw packet's Y delta (`acc_dy += -dy;`) so that moving the mouse up
moves the cursor up on screen — confirmed against real WSLg/host mouse
hardware. If you ever port this to different hardware and see the cursor
moving opposite to your hand, flip that sign back (`acc_dy += dy;`) in
`mouse_poll()`.

**Cursor "snapping" to a corner on a single movement.** PS/2 packets can
occasionally arrive with the overflow bits set, or (more rarely) framing
can briefly drift by a byte, which turns a normal-sized movement into a
huge misread delta. `mouse_poll()` discards the *movement* from packets
with the overflow bits set and clamps every packet's dx/dy to +/-80 before
applying it. Button/click state is still processed from every packet
(including overflowed ones), so a click landing on the same packet as a
big movement is never silently dropped.

**Random characters appearing while moving the mouse.** The keyboard and
mouse share the same PS/2 status/data ports (0x60/0x64) on real hardware.
`keyboard_poll_key()` checks bit 5 of the status register and skips any
byte that belongs to the mouse, so mouse packets can no longer be misread
as keyboard scancodes.

**Double-click timing / overall responsiveness.** The busy-wait `delay()`
call in the main loop and the `double_click_window` constant near the top
of `kmain()` in `kernel.c` are tuned together, since the delay loop's
real-world speed varies a lot by CPU. If the whole UI (mouse response,
typing, double-click) ever feels sluggish or too twitchy, adjust the
`delay()` argument in the main loop, and scale `double_click_window`
proportionally so double-clicks still cover roughly the same amount of
real time.

**Window dragging.** The window is dragged by tracking the offset between
the cursor and the window's top-left corner at the moment you click the
title bar (anywhere except the `_`/`□`/`X` buttons), so it doesn't jump to
re-center under the cursor when you start dragging. Dragging is disabled
while maximized (drag the title bar after restoring first). The window is
clamped so at least a sliver of its title bar always stays on-screen,
so it can never be dragged somewhere you can't grab it again.

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
- Click `□` in the title bar to maximize (fills the screen above the
  taskbar); click it again to restore
- Click `_` in the title bar to minimize — a taskbar button appears at the
  bottom-left
- Click the overlapping-squares glyph on that taskbar button to restore the
  window
- Click the X glyph (either on the taskbar button or the window's own
  title bar) to close the app

If you don't have these tools installed (Ubuntu/Debian/WSL):

```bash
sudo apt-get install nasm qemu-system-x86 gcc
```

## Project layout

```
boot/boot.asm      - Stage-1 bootloader: real mode -> protected mode
kernel/kentry.asm  - Asm stub: zeroes .bss, then calls into C's kmain()
kernel/kernel.c    - Main kernel logic: desktop, window state, input loop
kernel/vga.h       - Direct VGA register programming (Mode 13h) + back buffer
kernel/font.h      - 8x8 bitmap font (upper+lowercase) + text rendering
kernel/keyboard.h  - PS/2 keyboard scancode polling + ASCII decode
kernel/mouse.h     - PS/2 mouse polling + packet decode + click-edge detection
kernel/io.h        - inb/outb port I/O helpers
kernel/link.ld     - Linker script (kernel loads at physical 0x10000)
build.sh           - Builds everything into build/os-image.img
```

## Extending it

Some natural next steps, roughly in order of difficulty:
1. Resizing the window by dragging an edge/corner (not just moving it)
2. A second app (e.g. a fake "Paint" window) to prove out multi-window
   taskbar management
3. A simple in-memory "file system" (just named byte buffers) so
   Notepad can have more than one document
4. Line wrapping and scrolling in the text area
5. Interrupt-driven keyboard/mouse input (IDT + PIC remapping) instead of
   polling, which is the real prerequisite for eventually adding
   multitasking
