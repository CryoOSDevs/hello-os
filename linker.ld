/* linker.ld - places the kernel at physical address 0x1000, matching
 * KERNEL_LOAD_OFFSET in boot.asm, which is where the bootloader loads
 * the kernel sectors from disk and jumps to. */

ENTRY(_entry)

SECTIONS
{
    . = 0x1000;

    .text : {
        *(.text.entry)   /* kernel_entry.o's stub goes first, must be at 0x1000 */
        *(.text*)
    }

    .rodata : {
        *(.rodata*)
    }

    .data : {
        *(.data*)
    }

    .bss : {
        *(COMMON)
        *(.bss*)
    }

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame*)
        *(.note*)
    }
}
