; kernel_entry.asm - The bootloader jumps to physical address 0x1000 and
; expects to start executing immediately. We can't rely on the C++
; compiler placing kmain() as the very first bytes of the .text section,
; so this tiny stub is guaranteed (via linker.ld's .text.entry) to be
; first, and simply jumps into the real C++ entry point.

BITS 32

global _entry
extern kmain

section .text.entry
_entry:
    call kmain
.hang:
    hlt
    jmp .hang
