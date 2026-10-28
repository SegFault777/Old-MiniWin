; ============================================================
; MiniWin Bootloader (Stage 1)
; - Loaded by BIOS at 0x7C00 in 16-bit real mode
; - Loads the kernel from disk (sectors 2..N) to 0x10000
; - Switches to 32-bit protected mode
; - Jumps into the C kernel
; ============================================================
BITS 16
ORG 0x7C00

KERNEL_LOAD_SEG   equ 0x1000      ; kernel loaded at 0x10000 (seg 0x1000, off 0)
KERNEL_SECTORS    equ 64          ; how many 512-byte sectors to read (32KB, plenty for now)

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti

    mov [boot_drive], dl        ; BIOS passes boot drive number in dl

    ; Print a boot message using BIOS teletype
    mov si, msg_boot
    call print_string

    ; ---- Load kernel from disk using BIOS INT 13h ----
    mov ax, KERNEL_LOAD_SEG
    mov es, ax
    xor bx, bx                  ; ES:BX = 0x1000:0x0000 = 0x10000

    mov ah, 0x02                ; BIOS read sectors function
    mov al, KERNEL_SECTORS      ; number of sectors to read
    mov ch, 0                   ; cylinder 0
    mov cl, 2                   ; start at sector 2 (sector 1 = this bootloader)
    mov dh, 0                   ; head 0
    mov dl, [boot_drive]
    int 0x13
    jc disk_error

    mov si, msg_loaded
    call print_string

    ; ---- Enable A20 line (fast method via port 0x92) ----
    in al, 0x92
    or al, 2
    out 0x92, al

    ; ---- Load GDT and switch to protected mode ----
    cli
    lgdt [gdt_descriptor]

    mov eax, cr0
    or eax, 1
    mov cr0, eax

    jmp CODE_SEG:protected_mode_entry

disk_error:
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
; Global Descriptor Table (flat memory model)
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
; 32-bit protected mode entry point
; ============================================================
BITS 32
protected_mode_entry:
    mov ax, DATA_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000             ; set up a fresh 32-bit stack

    ; Jump to the C kernel entry point, loaded at 0x10000
    jmp CODE_SEG:0x10000

; ============================================================
; Boot sector padding + signature
; ============================================================
times 510-($-$$) db 0
dw 0xAA55
