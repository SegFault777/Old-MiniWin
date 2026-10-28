; ============================================================
; Kernel entry stub - this MUST be the very first code at 0x10000
; It calls the C function kmain()
; ============================================================
BITS 32
[extern kmain]
[extern _bss_start]
[extern _bss_end]

section .text
global _start
_start:
    ; Zero the .bss section before calling into C. The bootloader only
    ; copies raw file bytes off disk -- it has no notion of BSS -- so any
    ; statically-allocated buffer (like the video back buffer) would
    ; otherwise start out full of garbage/undefined memory contents.
    mov edi, _bss_start
    mov ecx, _bss_end
    sub ecx, edi
    xor eax, eax
    cld
    rep stosb

    call kmain
.hang:
    cli
    hlt
    jmp .hang
