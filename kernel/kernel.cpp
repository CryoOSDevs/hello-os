// ============================================================================
// kernel.cpp - Minimal freestanding C++ kernel
//
// Entry point kmain() is jumped to directly by the bootloader in 32-bit
// protected mode. There is no C runtime, no libc, no OS underneath us -
// we ARE the OS. We can only use language features that don't require
// runtime support: no exceptions, no RTTI, no heap (no `new`/`delete`
// unless we implement them ourselves), no standard library headers.
//
// Output happens by writing directly to the VGA text-mode framebuffer,
// which lives at physical address 0xB8000. Each on-screen character
// cell is 2 bytes: [ASCII byte][attribute byte (fg/bg color)].
//
// Input comes from the PS/2 keyboard controller. We haven't set up an
// IDT (interrupt descriptor table), so there's no IRQ1 handler here -
// instead we POLL the keyboard controller's status port in a tight
// loop and read scancodes as they arrive. That's simple and reliable
// for a minimal kernel like this, at the cost of burning CPU cycles
// spinning while waiting for a key.
// ============================================================================

#include <stdint.h>

namespace {

// ----------------------------------------------------------------------
// Low-level port I/O helpers
// ----------------------------------------------------------------------
inline void outb(uint16_t port, uint8_t val) {
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// ----------------------------------------------------------------------
// VGA text mode terminal
// ----------------------------------------------------------------------
constexpr uint16_t VGA_WIDTH = 80;
constexpr uint16_t VGA_HEIGHT = 25;
volatile uint16_t* const VGA_BUFFER = reinterpret_cast<uint16_t*>(0xB8000);

enum class VgaColor : uint8_t {
    Black = 0,
    Blue = 1,
    Green = 2,
    Cyan = 3,
    Red = 4,
    Magenta = 5,
    Brown = 6,
    LightGrey = 7,
    DarkGrey = 8,
    LightBlue = 9,
    LightGreen = 10,
    LightCyan = 11,
    LightRed = 12,
    LightMagenta = 13,
    Yellow = 14,
    White = 15,
};

constexpr uint8_t vga_entry_color(VgaColor fg, VgaColor bg) {
    return static_cast<uint8_t>(fg) | (static_cast<uint8_t>(bg) << 4);
}

constexpr uint16_t vga_entry(char c, uint8_t color) {
    return static_cast<uint16_t>(static_cast<uint8_t>(c)) |
           (static_cast<uint16_t>(color) << 8);
}

bool str_equal(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return false;
        ++a;
        ++b;
    }
    return *a == *b;
}

// Compares only the first characters of `prefix` against `s` (used to
// match a command name before a trailing argument, e.g. "echo hi").
bool str_starts_with(const char* s, const char* prefix) {
    while (*prefix) {
        if (*s != *prefix) return false;
        ++s;
        ++prefix;
    }
    return true;
}

class Terminal {
public:
    void clear(uint8_t color) {
        color_ = color;
        for (uint16_t y = 0; y < VGA_HEIGHT; ++y) {
            for (uint16_t x = 0; x < VGA_WIDTH; ++x) {
                VGA_BUFFER[y * VGA_WIDTH + x] = vga_entry(' ', color_);
            }
        }
        row_ = 0;
        col_ = 0;
        update_cursor();
    }

    void set_color(uint8_t color) { color_ = color; }

    void put_char(char c) {
        if (c == '\n') {
            newline();
        } else if (c == '\b') {
            backspace();
        } else {
            VGA_BUFFER[row_ * VGA_WIDTH + col_] = vga_entry(c, color_);
            if (++col_ == VGA_WIDTH) {
                newline();
            }
        }
        update_cursor();
    }

    void write(const char* str) {
        for (const char* p = str; *p != '\0'; ++p) {
            put_char(*p);
        }
    }

private:
    void newline() {
        col_ = 0;
        if (++row_ == VGA_HEIGHT) {
            scroll();
            row_ = VGA_HEIGHT - 1;
        }
    }

    // Erases the character before the cursor and moves the cursor back
    // onto it. Refuses to back up past the start of the current line -
    // this is a tiny shell, not a full line editor, so that's fine.
    void backspace() {
        if (col_ == 0) return;
        --col_;
        VGA_BUFFER[row_ * VGA_WIDTH + col_] = vga_entry(' ', color_);
    }

    void scroll() {
        for (uint16_t y = 1; y < VGA_HEIGHT; ++y) {
            for (uint16_t x = 0; x < VGA_WIDTH; ++x) {
                VGA_BUFFER[(y - 1) * VGA_WIDTH + x] = VGA_BUFFER[y * VGA_WIDTH + x];
            }
        }
        for (uint16_t x = 0; x < VGA_WIDTH; ++x) {
            VGA_BUFFER[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = vga_entry(' ', color_);
        }
    }

    // Moves the blinking hardware text cursor to match our (row_, col_),
    // by programming the VGA CRT controller's cursor-position registers.
    void update_cursor() {
        uint16_t pos = row_ * VGA_WIDTH + col_;
        outb(0x3D4, 0x0F);
        outb(0x3D5, static_cast<uint8_t>(pos & 0xFF));
        outb(0x3D4, 0x0E);
        outb(0x3D5, static_cast<uint8_t>((pos >> 8) & 0xFF));
    }

    uint16_t row_ = 0;
    uint16_t col_ = 0;
    uint8_t color_ = 0x0F;
};

Terminal g_terminal;

// ----------------------------------------------------------------------
// PS/2 keyboard driver (polling, scancode set 1, US QWERTY)
//
// Port 0x64 (status register): bit 0 set means there's a byte waiting
// to be read from port 0x60 (data register). Scancodes below 0x80 are
// "make" codes (key pressed); the same code + 0x80 is the matching
// "break" code (key released). We only care about a handful of keys.
// ----------------------------------------------------------------------
constexpr uint8_t KBD_STATUS_PORT = 0x64;
constexpr uint8_t KBD_DATA_PORT = 0x60;
constexpr uint8_t KBD_OUTPUT_FULL = 0x01;

constexpr uint8_t SCANCODE_LSHIFT = 0x2A;
constexpr uint8_t SCANCODE_RSHIFT = 0x36;
constexpr uint8_t SCANCODE_LSHIFT_RELEASE = 0xAA;
constexpr uint8_t SCANCODE_RSHIFT_RELEASE = 0xB6;
constexpr uint8_t SCANCODE_RELEASE_BIT = 0x80;

// Index = scancode, value = ASCII for unshifted key (0 = no mapping).
const char SCANCODE_TO_ASCII[128] = {
    0,   27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t',
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,   'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,   '\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/', 0,   '*', 0,   ' ', 0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
};

const char SCANCODE_TO_ASCII_SHIFTED[128] = {
    0,   27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,   'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,   '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?', 0,   '*', 0,   ' ', 0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
};

bool g_shift_held = false;

// Blocks (spinning, polling) until a key is pressed, then returns its
// ASCII value. Keys we don't map to a character (arrow keys, function
// keys, etc.) are silently skipped - we just keep polling.
char read_char_blocking() {
    for (;;) {
        if ((inb(KBD_STATUS_PORT) & KBD_OUTPUT_FULL) == 0) {
            continue; // no scancode waiting yet - keep spinning
        }
        uint8_t scancode = inb(KBD_DATA_PORT);

        if (scancode == SCANCODE_LSHIFT || scancode == SCANCODE_RSHIFT) {
            g_shift_held = true;
            continue;
        }
        if (scancode == SCANCODE_LSHIFT_RELEASE || scancode == SCANCODE_RSHIFT_RELEASE) {
            g_shift_held = false;
            continue;
        }
        if (scancode & SCANCODE_RELEASE_BIT) {
            continue; // ignore all other key-release events
        }

        char c = g_shift_held ? SCANCODE_TO_ASCII_SHIFTED[scancode]
                               : SCANCODE_TO_ASCII[scancode];
        if (c != 0) {
            return c;
        }
        // Unmapped key (arrows, F-keys, etc.) - keep waiting for something usable.
    }
}

// ----------------------------------------------------------------------
// Tiny built-in shell
//
// Reads one line at a time into a fixed-size buffer (no heap available)
// and dispatches to a handful of hardcoded commands. This is deliberately
// small - just enough to prove keyboard input works end to end and give
// something interactive to type into.
// ----------------------------------------------------------------------
constexpr int LINE_BUFFER_SIZE = 79;

void print_prompt() {
    g_terminal.write("> ");
}

void run_command(const char* line) {
    if (line[0] == '\0') {
        return;
    } else if (str_equal(line, "help")) {
        g_terminal.write("Available commands:\n");
        g_terminal.write("  help        - show this message\n");
        g_terminal.write("  clear       - clear the screen\n");
        g_terminal.write("  echo <text> - print <text> back\n");
        g_terminal.write("  about       - about this OS\n");
    } else if (str_equal(line, "clear")) {
        g_terminal.clear(0x0F);
    } else if (str_equal(line, "about")) {
        g_terminal.write("hello-os: a minimal hand-written x86 kernel.\n");
        g_terminal.write("Boot chain: bootloader -> protected mode -> kmain().\n");
    } else if (str_starts_with(line, "echo")) {
        const char* rest = line + 4;
        if (*rest == ' ') ++rest; // skip the single space after "echo"
        g_terminal.write(rest);
        g_terminal.write("\n");
    } else {
        g_terminal.write("Unknown command: ");
        g_terminal.write(line);
        g_terminal.write("\n");
        g_terminal.write("Type 'help' for a list of commands.\n");
    }
}

void shell_loop() {
    char line[LINE_BUFFER_SIZE + 1];
    for (;;) {
        print_prompt();

        int len = 0;
        for (;;) {
            char c = read_char_blocking();

            if (c == '\n') {
                line[len] = '\0';
                g_terminal.put_char('\n');
                break;
            } else if (c == '\b') {
                if (len > 0) {
                    --len;
                    g_terminal.put_char('\b');
                }
            } else if (len < LINE_BUFFER_SIZE) {
                line[len++] = c;
                g_terminal.put_char(c);
            }
            // if the buffer is full, silently drop further characters
            // until Enter or Backspace - keeps this simple.
        }

        run_command(line);
    }
}

} // namespace

extern "C" void kmain() {
    // We never set up an IDT (interrupt descriptor table) - this kernel
    // polls hardware instead of using interrupts. The bootloader leaves
    // interrupts enabled from real mode, so without this `cli`, the very
    // first hardware IRQ (e.g. a keypress) would fault with no handler
    // to catch it and triple-fault the CPU back to a BIOS reset.
    asm volatile("cli");

    const uint8_t color = vga_entry_color(VgaColor::LightGreen, VgaColor::Black);
    g_terminal.clear(color);
    g_terminal.write("Hello, OS!\n");
    g_terminal.write("Boot chain OK: bootloader -> protected mode -> kmain()\n");
    g_terminal.write("Type 'help' for a list of commands.\n");

    shell_loop(); // never returns
}
