; ============================================================================
; boot.asm - Minimal x86 bootloader
;
; The BIOS loads this sector (512 bytes) to physical address 0x7C00 and
; jumps to it in 16-bit REAL MODE. From here we are responsible for
; everything: setting up a stack, loading the rest of the OS off disk,
; switching the CPU into 32-bit PROTECTED MODE, and handing control to
; the C++ kernel.
;
; Boot process, step by step:
;   1. REAL MODE setup    - segments, stack
;   2. LOAD KERNEL        - BIOS INT 13h reads kernel sectors off disk
;   3. ENABLE A20 LINE    - lets us address memory above 1 MiB
;   4. LOAD GDT           - protected mode needs a Global Descriptor Table
;   5. ENTER PROTECTED MODE - set PE bit in CR0, far jump to flush prefetch
;   6. JUMP TO KERNEL     - 32-bit code now runs kmain()
; ============================================================================

BITS 16
ORG 0x7C00

KERNEL_LOAD_SEGMENT equ 0x0000
KERNEL_LOAD_OFFSET  equ 0x1000      ; kernel is loaded to physical 0x1000
KERNEL_SECTOR_COUNT equ 16          ; how many 512-byte sectors to read (8KB)
KERNEL_START_SECTOR equ 2           ; sector 1 (LBA0) is this bootloader

start:
    ; ------------------------------------------------------------------
    ; STEP 1: Real-mode setup
    ; ------------------------------------------------------------------
    cli                     ; no interrupts while we fiddle with segments
    xor ax, ax
    mov ds, ax              ; DS = 0
    mov es, ax              ; ES = 0 (BIOS disk reads use ES:BX)
    mov ss, ax              ; SS = 0
    mov sp, 0x7C00          ; stack grows down from just below our own code
    sti

    mov [boot_drive], dl    ; BIOS passes boot drive number in DL

    ; ------------------------------------------------------------------
    ; STEP 2: Load the kernel from disk using BIOS INT 13h
    ; ------------------------------------------------------------------
    mov ah, 0x02                    ; BIOS "read sectors" function
    mov al, KERNEL_SECTOR_COUNT     ; number of sectors to read
    mov ch, 0                       ; cylinder 0
    mov dh, 0                       ; head 0
    mov cl, KERNEL_START_SECTOR     ; starting sector (1-indexed; sector 1 = boot sector)
    mov dl, [boot_drive]            ; drive to read from
    mov bx, KERNEL_LOAD_OFFSET      ; ES:BX = destination buffer (ES already 0)
    int 0x13
    jc disk_error                   ; carry flag set on failure

    ; ------------------------------------------------------------------
    ; STEP 3: Enable the A20 line
    ;
    ; On real 8086 hardware, memory addressing wrapped around at 1 MiB
    ; (the "A20 line" was tied low for backwards compatibility). We need
    ; it enabled to access memory beyond 1 MiB in protected mode. The
    ; fast A20 method via I/O port 0x92 works on essentially all modern
    ; hardware and emulators (including QEMU).
    ; ------------------------------------------------------------------
    in al, 0x92
    or al, 2
    out 0x92, al

    ; ------------------------------------------------------------------
    ; STEP 4: Load the Global Descriptor Table (GDT)
    ;
    ; Protected mode has no "segment = address*16" real-mode addressing.
    ; Instead, segment registers hold *selectors* that index into a GDT
    ; describing base/limit/access rights for each segment. We define a
    ; minimal flat GDT: one code segment and one data segment, each
    ; spanning the full 4GB address space.
    ; ------------------------------------------------------------------
    lgdt [gdt_descriptor]

    ; ------------------------------------------------------------------
    ; STEP 5: Enter protected mode
    ;
    ; Set bit 0 (PE - Protection Enable) in CR0. Then perform a far jump
    ; to flush the CPU's instruction prefetch queue (which may contain
    ; real-mode-decoded instructions) and reload CS with our new 32-bit
    ; code selector.
    ; ------------------------------------------------------------------
    mov eax, cr0
    or eax, 1
    mov cr0, eax

    jmp CODE_SEG:protected_mode_entry   ; far jump -> flushes prefetch, loads CS

disk_error:
    mov si, disk_error_msg
    call print_string_rm
    cli
    hlt

; Prints a null-terminated string via BIOS teletype (real mode only,
; used solely for the disk error path above).
print_string_rm:
    mov ah, 0x0E
.loop:
    lodsb
    cmp al, 0
    je .done
    int 0x10
    jmp .loop
.done:
    ret

boot_drive:      db 0
disk_error_msg:  db "Disk read error!", 0

; ============================================================================
; GDT - Global Descriptor Table
; ============================================================================
align 8
gdt_start:
    ; Null descriptor (required, index 0 must be all zero)
    dq 0

    ; Code segment: base=0, limit=0xFFFFF (4GB w/ granularity), executable
gdt_code:
    dw 0xFFFF       ; limit (low 16 bits)
    dw 0x0000       ; base (low 16 bits)
    db 0x00         ; base (mid 8 bits)
    db 10011010b    ; access: present, ring0, code segment, executable, readable
    db 11001111b    ; flags (4-bit granularity, 32-bit) + limit (high 4 bits)
    db 0x00         ; base (high 8 bits)

    ; Data segment: base=0, limit=0xFFFFF (4GB), writable
gdt_data:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 10010010b    ; access: present, ring0, data segment, writable
    db 11001111b
    db 0x00
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1   ; GDT size minus 1
    dd gdt_start                 ; GDT linear address

CODE_SEG equ gdt_code - gdt_start
DATA_SEG equ gdt_data - gdt_start

; ============================================================================
; STEP 6: 32-bit protected mode code - jump to kernel
; ============================================================================
BITS 32
protected_mode_entry:
    ; Reload all data segment registers with our flat data selector.
    ; Real-mode segment registers are meaningless now; everything is
    ; flat 32-bit addressing via the GDT entries above.
    mov ax, DATA_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000        ; fresh 32-bit stack, well away from our code/kernel

    jmp CODE_SEG:KERNEL_LOAD_OFFSET   ; hand off to kmain() in the C++ kernel

; ============================================================================
; Boot sector padding + signature
;
; A boot sector MUST be exactly 512 bytes and end with the magic bytes
; 0x55 0xAA, or BIOS will refuse to treat it as bootable.
; ============================================================================
times 510 - ($ - $$) db 0
dw 0xAA55
