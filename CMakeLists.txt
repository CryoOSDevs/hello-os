cmake_minimum_required(VERSION 3.16)
project(hello_os ASM C CXX)

# ----------------------------------------------------------------------------
# Toolchain
#
# We need a cross-compiler that targets bare-metal i686 (no host OS, no
# libc), because compiling freestanding kernel code with your regular
# host g++ would pull in Linux/macOS-specific assumptions (e.g. calling
# convention for a hosted environment, PIE, stack protectors that need
# runtime support). If i686-elf-g++ isn't installed, see README.md.
# ----------------------------------------------------------------------------
find_program(NASM nasm REQUIRED)

find_program(CROSS_CXX i686-elf-g++)
find_program(CROSS_LD i686-elf-ld)
find_program(CROSS_OBJCOPY i686-elf-objcopy)

if(NOT CROSS_CXX)
    message(WARNING
        "i686-elf-g++ not found. Falling back to host g++ with "
        "-m32 -ffreestanding, which works on many Linux setups but is "
        "NOT a true cross-compiler. See README.md to build i686-elf-gcc "
        "for a correct, portable toolchain.")
    set(CROSS_CXX "g++")
    set(CROSS_LD "ld")
    if(NOT CROSS_OBJCOPY)
        find_program(CROSS_OBJCOPY objcopy REQUIRED)
    endif()
endif()

set(CMAKE_VERBOSE_MAKEFILE ON)

set(BOOT_ASM   ${CMAKE_SOURCE_DIR}/boot/boot.asm)
set(KERNEL_CPP ${CMAKE_SOURCE_DIR}/kernel/kernel.cpp)
set(KERNEL_ENTRY_ASM ${CMAKE_SOURCE_DIR}/kernel/kernel_entry.asm)
set(LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/linker.ld)

set(OS_CXX_FLAGS -m32 -ffreestanding -fno-exceptions -fno-rtti -fno-stack-protector -nostdlib -Wall -Wextra -g -Og)
set(OS_LINK_FLAGS -m32 -ffreestanding -nostdlib -g -Og)

set(BOOT_BIN    ${CMAKE_BINARY_DIR}/boot.bin)
set(ENTRY_OBJ   ${CMAKE_BINARY_DIR}/kernel_entry.o)
set(KERNEL_OBJ  ${CMAKE_BINARY_DIR}/kernel.o)
set(KERNEL_ELF  ${CMAKE_BINARY_DIR}/kernel.elf)
set(KERNEL_BIN  ${CMAKE_BINARY_DIR}/kernel.bin)
set(OS_IMAGE    ${CMAKE_BINARY_DIR}/os-image.bin)

# ----------------------------------------------------------------------------
# 1. Assemble the bootloader (raw flat binary, exactly 512 bytes)
# ----------------------------------------------------------------------------
add_custom_command(
    OUTPUT ${BOOT_BIN}
    COMMAND ${NASM} -f bin ${BOOT_ASM} -o ${BOOT_BIN}
    DEPENDS ${BOOT_ASM}
    COMMENT "Assembling bootloader -> boot.bin"
    VERBATIM
)

# ----------------------------------------------------------------------------
# 2. Assemble the kernel entry stub as an ELF object (so it can be linked
#    with the C++ object file)
# ----------------------------------------------------------------------------
add_custom_command(
    OUTPUT ${ENTRY_OBJ}
    COMMAND ${NASM} -f elf32 ${KERNEL_ENTRY_ASM} -o ${ENTRY_OBJ}
    DEPENDS ${KERNEL_ENTRY_ASM}
    COMMENT "Assembling kernel entry stub -> kernel_entry.o"
    VERBATIM
)

# ----------------------------------------------------------------------------
# 3. Compile the freestanding C++ kernel
# ----------------------------------------------------------------------------
add_custom_command(
    OUTPUT ${KERNEL_OBJ}
    COMMAND ${CROSS_CXX}
            ${OS_CXX_FLAGS}
            -c ${KERNEL_CPP} -o ${KERNEL_OBJ}
    DEPENDS ${KERNEL_CPP}
    COMMENT "Compiling kernel.cpp -> kernel.o"
    VERBATIM
)

# ----------------------------------------------------------------------------
# 4. Link entry stub + kernel object with the custom linker script into an
#    ELF file (kept around for GDB symbols during debugging)
# ----------------------------------------------------------------------------
add_custom_command(
    OUTPUT ${KERNEL_ELF}
    COMMAND ${CROSS_CXX}
            ${OS_LINK_FLAGS}
            -T ${LINKER_SCRIPT}
            -o ${KERNEL_ELF}
            ${ENTRY_OBJ} ${KERNEL_OBJ}
            -lgcc
    DEPENDS ${ENTRY_OBJ} ${KERNEL_OBJ} ${LINKER_SCRIPT}
    COMMENT "Linking kernel.elf"
    VERBATIM
)

# ----------------------------------------------------------------------------
# 5. Strip ELF metadata down to a flat binary the bootloader can load
#    directly into memory and jump to.
# ----------------------------------------------------------------------------
add_custom_command(
    OUTPUT ${KERNEL_BIN}
    COMMAND ${CROSS_OBJCOPY} -O binary ${KERNEL_ELF} ${KERNEL_BIN}
    DEPENDS ${KERNEL_ELF}
    COMMENT "Extracting kernel.bin from kernel.elf"
    VERBATIM
)

# ----------------------------------------------------------------------------
# 6. Concatenate boot.bin + kernel.bin into the final disk image.
#    boot.bin is already padded to exactly 512 bytes by boot.asm itself.
#    kernel.bin is padded up to a whole number of 512-byte sectors here,
#    matching KERNEL_SECTOR_COUNT read by the bootloader.
# ----------------------------------------------------------------------------
add_custom_command(
    OUTPUT ${OS_IMAGE}
    COMMAND cat ${BOOT_BIN} ${KERNEL_BIN} > ${OS_IMAGE}
    COMMAND ${CMAKE_COMMAND} -E env bash -c
            "target=$((512 + 16*512)); size=$(stat -c%s ${OS_IMAGE}); \
             pad=$(( target - size )); \
             if [ $pad -gt 0 ]; then dd if=/dev/zero bs=1 count=$pad >> ${OS_IMAGE} 2>/dev/null; fi"
    DEPENDS ${BOOT_BIN} ${KERNEL_BIN}
    COMMENT "Building final os-image.bin (boot sector + kernel, padded to KERNEL_SECTOR_COUNT sectors)"
    VERBATIM
)

add_custom_target(os_image ALL DEPENDS ${OS_IMAGE} ${KERNEL_ELF})
