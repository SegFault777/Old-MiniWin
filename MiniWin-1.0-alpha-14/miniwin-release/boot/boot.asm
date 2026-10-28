; ============================================================
; MiniWin Bootloader (Stage 1)
; - This is the very first code that runs. Ever. On the whole machine.
;   BIOS drops us here at 0x7C00 in ancient 16-bit real mode and just
;   trusts us to figure the rest out. No safety net, no OS, nothing.
; - Loads the kernel from disk
; - Wrestles the CPU into 32-bit protected mode
; - Jumps into the C kernel and washes its hands of the whole affair
; ============================================================
BITS 16
ORG 0x7C00

KERNEL_LOAD_SEG   equ 0x1000      ; kernel lands at physical 0x10000 (seg 0x1000, off 0)

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti

    mov [boot_drive], dl        ; BIOS hands us the boot drive number in dl, don't lose it

    ; Print a boot message using BIOS teletype
    mov si, msg_boot
    call print_string

    ; ---- Load kernel from disk using BIOS INT13h extended (LBA) reads ----
    ; The kernel got FAT. It's got an entire Hangul font crammed in there
    ; now, so it can blow right past 64KB -- which is the absolute max a
    ; single real-mode segment:offset pair can address in one BIOS call.
    ; The old legacy CHS read (AH=02h) also cheaps out and only gives you
    ; one measly byte for the sector count (255 sectors, tops), so screw
    ; that too. Instead: four extended reads (AH=42h, LBA-addressed,
    ; actual 16-bit sector count) of 64 sectors (32KB) apiece, nudging the
    ; destination segment up by 0x800 each time so all four chunks land
    ; back-to-back in memory starting at 0x10000 -- exactly where the
    ; linker script is expecting to find the kernel. If any of these four
    ; reads fails, we bail loudly instead of limping forward with half a
    ; kernel loaded, because half a kernel is worse than no kernel.
    mov si, dap1
    mov dl, [boot_drive]
    mov ah, 0x42
    int 0x13
    jc disk_error

    mov si, dap2
    mov dl, [boot_drive]
    mov ah, 0x42
    int 0x13
    jc disk_error

    mov si, dap3
    mov dl, [boot_drive]
    mov ah, 0x42
    int 0x13
    jc disk_error

    mov si, dap4
    mov dl, [boot_drive]
    mov ah, 0x42
    int 0x13
    jc disk_error

    mov si, msg_loaded
    call print_string

    ; ---- Enable A20 line (fast method via port 0x92) ----
    ; Without this, memory access wraps around at 1MB like it's still
    ; 1981, and everything above that just silently aliases back to zero.
    ; Flip the bit, move on with our lives.
    in al, 0x92
    or al, 2
    out 0x92, al

    ; ---- Load GDT and shove the CPU into protected mode ----
    cli
    lgdt [gdt_descriptor]

    mov eax, cr0
    or eax, 1
    mov cr0, eax

    jmp CODE_SEG:protected_mode_entry

disk_error:
    ; Something about the disk read went sideways. We don't have the
    ; luxury of a stack trace or a debugger at this point in the universe's
    ; existence, so all we can do is print this and freeze. Sorry.
    mov si, msg_disk_err
    call print_string
    jmp $

; ---- 16-bit helper: print null-terminated string at SI via BIOS teletype ----
print_string:
    pusha
.loop:
    lodsb
    or al, al
    jz .done
    mov ah, 0x0E
    mov bh, 0
    int 0x10
    jmp .loop
.done:
    popa
    ret

boot_drive: db 0
msg_boot:      db "MiniWin: booting...", 13, 10, 0
msg_loaded:    db "MiniWin: kernel loaded, entering 32-bit mode...", 13, 10, 0
msg_disk_err:  db "MiniWin: DISK READ ERROR", 13, 10, 0

; ============================================================
; Disk Address Packets for the four extended-read (INT13h AH=42h) calls.
; Each one reads 64 sectors (32KB) from the given LBA into the given
; real-mode segment (offset 0, every time), together covering kernel
; LBA 1-256 (128KB total) and landing back-to-back at physical
; 0x10000-0x2FFFF like good little chunks of memory should.
; ============================================================
align 4
dap1:
    db 0x10, 0
    dw 64
    dw 0x0000
    dw 0x1000
    dq 1
dap2:
    db 0x10, 0
    dw 64
    dw 0x0000
    dw 0x1800
    dq 65
dap3:
    db 0x10, 0
    dw 64
    dw 0x0000
    dw 0x2000
    dq 129
dap4:
    db 0x10, 0
    dw 64
    dw 0x0000
    dw 0x2800
    dq 193

; ============================================================
; Global Descriptor Table (flat memory model, because life's too short
; for segmented memory models)
; ============================================================
align 8
gdt_start:
gdt_null:
    dq 0x0

gdt_code:                       ; CODE_SEG selector = gdt_code - gdt_start = 0x08
    dw 0xFFFF                   ; limit low
    dw 0x0000                   ; base low
    db 0x00                     ; base middle
    db 10011010b                ; access: present, ring0, code, executable, readable
    db 11001111b                ; flags(4-bit) + limit high(4-bit): 4KB granularity, 32-bit
    db 0x00                     ; base high

gdt_data:                       ; DATA_SEG selector = gdt_data - gdt_start = 0x10
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 10010010b                ; access: present, ring0, data, writable
    db 11001111b
    db 0x00
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1  ; GDT size - 1
    dd gdt_start                ; GDT address

CODE_SEG equ gdt_code - gdt_start
DATA_SEG equ gdt_data - gdt_start

; ============================================================
; 32-bit protected mode entry point -- welcome to the future (1985)
; ============================================================
BITS 32
protected_mode_entry:
    mov ax, DATA_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000             ; brand new 32-bit stack, never been used

    ; Off we go into the C kernel, loaded at 0x10000. This bootloader's
    ; entire job is now done. It was a good run.
    jmp CODE_SEG:0x10000

; ============================================================
; Boot sector padding + the magic signature that tells the BIOS
; "yes, this is actually bootable, I promise"
; ============================================================
times 510-($-$$) db 0
dw 0xAA55
