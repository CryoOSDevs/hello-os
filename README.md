# hello-os

A minimal, hand-written x86 "Hello World" operating system. Boots via a
16-bit real-mode bootloader written in NASM assembly, switches the CPU
into 32-bit protected mode, and jumps into a freestanding C++ kernel that
prints text directly to the VGA text buffer.

This is deliberately tiny — just enough to prove the full boot chain
(BIOS → bootloader → protected mode → C++ kernel) works end to end. No
GRUB, no libc, no third-party bootloader.

## Required tools

| Tool | Purpose | Debian/Ubuntu install |
|---|---|---|
| `nasm` | Assembles boot.asm and kernel_entry.asm | `sudo apt install nasm` |
| `i686-elf-g++` / `i686-elf-gcc` | Freestanding cross-compiler (bare metal, no host OS assumptions) | see below — usually not in apt |
| `qemu-system-i386` | Runs the OS image | `sudo apt install qemu-system-x86` |
| `gdb` | Source-level kernel debugging | `sudo apt install gdb` |
| `cmake` | Build orchestration | `sudo apt install cmake` |

### Building the i686-elf cross-compiler

Using your host's regular `g++` to compile OS code is unsafe — it
assumes a hosted environment (Linux ABI, stack protector runtime,
possibly PIE) that doesn't exist when there's no OS underneath you. The
standard OS-dev solution is a cross-compiler that targets bare-metal
`i686-elf` with no libc.

Quick options:
- **Pre-built**: search for `i686-elf-gcc` packages for your distro (e.g.
  some distros have `i686-elf-binutils` / `i686-elf-gcc` in community
  repos; macOS has `brew install i686-elf-gcc` via some taps).
- **Build from source**: follow the OSDev wiki's "GCC Cross-Compiler"
  guide (https://wiki.osdev.org/GCC_Cross-Compiler) — builds binutils
  and gcc targeting `i686-elf`, takes ~15-30 minutes.

If no cross-compiler is found, `CMakeLists.txt` falls back to host
`g++ -m32 -ffreestanding`, which works on many x86 Linux setups but is
not a true cross-compiler (no guarantee on other host OSes/archs) — treat
it as a convenience fallback, not the recommended path.

## How to build

**Via VS Code**: `Ctrl+Shift+B` (runs the default build task, which
configures + builds via CMake) → produces `build/os-image.bin` and
`build/kernel.elf`.

**Manually**:
```bash
cmake -S . -B build
cmake --build build
```

## How to run

```bash
qemu-system-i386 -drive format=raw,file=build/os-image.bin
```

You should see:
```
Hello, OS!
Boot chain OK: bootloader -> protected mode -> kmain()
Type 'help' for a list of commands.
> _
```
in light green text on a black screen in the QEMU window, with a blinking
cursor at the prompt. Type a command and press Enter — try `help`.

## Keyboard input / built-in shell

The kernel includes a small polling-based PS/2 keyboard driver and a tiny
built-in shell. There's no IDT (interrupt descriptor table) set up, so
input is read by spinning on the keyboard controller's status port rather
than via IRQ1 — simple and reliable for a kernel this size, at the cost of
burning CPU while idle.

Supported keys: letters, digits, punctuation, Space, Backspace (single-line
only), Enter, and Shift (both held-shift symbols and shifted letters).

Built-in commands:
- `help` — list commands
- `clear` — clear the screen
- `echo <text>` — print `<text>` back
- `about` — short description of the OS

**Important gotcha if you modify the boot code**: `boot.asm`'s real-mode
setup re-enables interrupts (`sti`) before the switch to protected mode,
and that IF state carries through. Since this kernel never installs an
IDT, `kmain()` explicitly issues `cli` as its first instruction — without
it, the very first hardware interrupt (e.g. a keypress firing IRQ1) has
no handler to catch it and triple-faults the CPU back to a BIOS reset.
If you add interrupt-driven features later, you'll need a real IDT and
PIC remapping instead of this blanket `cli`.

## How to debug

Press **F5** in VS Code. This:
1. Builds the OS image (via the task dependency chain).
2. Launches QEMU paused at the first instruction, with its GDB stub
   listening on `localhost:1234` (`-s -S` flags).
3. Attaches GDB, loading debug symbols from `build/kernel.elf`, and sets
   an initial breakpoint at `kmain`.

From there you can set breakpoints directly in `kernel/kernel.cpp`, step
through the C++ kernel, inspect the VGA buffer contents, etc.

To debug manually instead:
```bash
qemu-system-i386 -drive format=raw,file=build/os-image.bin -s -S &
gdb build/kernel.elf -ex "target remote localhost:1234"
```

## Project layout

```
boot/boot.asm          16-bit real-mode bootloader (exactly 512 bytes, 0xAA55 signature)
kernel/kernel_entry.asm  tiny asm stub guaranteed to be the first kernel byte at 0x1000
kernel/kernel.cpp       freestanding C++ kernel, kmain() writes to VGA text buffer at 0xB8000
linker.ld               places the kernel at 0x1000, matching the bootloader's jump target
CMakeLists.txt          nasm -> cross-g++ -> link -> objcopy -> concatenate into os-image.bin
.vscode/tasks.json      build + QEMU-run tasks
.vscode/launch.json     GDB-over-QEMU debug configuration
```

## How the boot chain works (short version)

1. BIOS loads sector 1 of the disk (`boot.asm`, 512 bytes) to `0x7C00`
   and jumps to it in 16-bit real mode.
2. The bootloader sets up a stack, then uses BIOS `INT 13h` to read the
   next several sectors (the kernel) into memory at `0x1000`.
3. It enables the A20 line (so addresses above 1MB work), loads a
   minimal GDT, sets the PE bit in `CR0`, and far-jumps into 32-bit
   protected mode.
4. In protected mode, it reloads segment registers from the GDT, sets
   up a new stack, and jumps to `0x1000` — where `kernel_entry.asm`'s
   stub immediately calls `kmain()`.
5. `kmain()` writes "Hello, OS!" directly into the VGA text buffer at
   `0xB8000` and halts the CPU in an infinite loop.

See the comments in `boot/boot.asm` for the detailed step-by-step
walkthrough.
