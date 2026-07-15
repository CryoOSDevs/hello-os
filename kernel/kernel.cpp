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

    void set_position(uint16_t row, uint16_t col) {
        row_ = (row < VGA_HEIGHT) ? row : VGA_HEIGHT - 1;
        col_ = (col < VGA_WIDTH) ? col : VGA_WIDTH - 1;
        update_cursor();
    }

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

char read_char_nonblocking() {
    if ((inb(KBD_STATUS_PORT) & KBD_OUTPUT_FULL) == 0) {
        return 0;
    }
    uint8_t scancode = inb(KBD_DATA_PORT);

    if (scancode == 0xE0) {
        return 0;
    }
    if (scancode == SCANCODE_LSHIFT || scancode == SCANCODE_RSHIFT) {
        g_shift_held = true;
        return 0;
    }
    if (scancode == SCANCODE_LSHIFT_RELEASE || scancode == SCANCODE_RSHIFT_RELEASE) {
        g_shift_held = false;
        return 0;
    }
    if (scancode & SCANCODE_RELEASE_BIT) {
        return 0;
    }
    if (scancode >= sizeof(SCANCODE_TO_ASCII)) {
        return 0;
    }

    return g_shift_held ? SCANCODE_TO_ASCII_SHIFTED[scancode]
                        : SCANCODE_TO_ASCII[scancode];
}

constexpr uint16_t SNAKE_BOARD_COL = 2;
constexpr uint16_t SNAKE_BOARD_ROW = 3;
constexpr uint16_t SNAKE_BOARD_WIDTH = 36;
constexpr uint16_t SNAKE_BOARD_HEIGHT = 14;
constexpr uint16_t SNAKE_PLAY_WIDTH = SNAKE_BOARD_WIDTH - 2;
constexpr uint16_t SNAKE_PLAY_HEIGHT = SNAKE_BOARD_HEIGHT - 2;
constexpr int MAX_SNAKE_LENGTH = 128;
constexpr uint32_t GAME_DELAY = 1800000u;

enum class SnakeMode : uint8_t {
    Human,
    Greedy,
    Random,
    Spiral,
};

struct Point {
    int8_t x;
    int8_t y;
};

struct Snake {
    Point body[MAX_SNAKE_LENGTH];
    int length;
    int8_t dx;
    int8_t dy;
    bool alive;
};

static uint32_t g_random_seed = 0xABCD1234u;

uint32_t random_u32() {
    g_random_seed = g_random_seed * 1103515245u + 12345u;
    return g_random_seed;
}

int random_int(int max) {
    return static_cast<int>(random_u32() % static_cast<uint32_t>(max));
}

bool point_equal(Point a, Point b) {
    return a.x == b.x && a.y == b.y;
}

bool snake_intersects(const Snake& snake, Point p, bool ignore_head = false) {
    for (int i = ignore_head ? 1 : 0; i < snake.length; ++i) {
        if (point_equal(snake.body[i], p)) {
            return true;
        }
    }
    return false;
}

bool snake_can_move(const Snake& snake, int8_t dx, int8_t dy) {
    Point next = {
        static_cast<int8_t>(snake.body[0].x + dx),
        static_cast<int8_t>(snake.body[0].y + dy),
    };
    if (next.x < 0 || next.x >= static_cast<int8_t>(SNAKE_PLAY_WIDTH) ||
        next.y < 0 || next.y >= static_cast<int8_t>(SNAKE_PLAY_HEIGHT)) {
        return false;
    }
    return !snake_intersects(snake, next, true);
}

void snake_set_direction(Snake& snake, int8_t dx, int8_t dy) {
    if (dx == -snake.dx && dy == -snake.dy) {
        return;
    }
    snake.dx = dx;
    snake.dy = dy;
}

bool snake_step(Snake& snake, const Point& food) {
    Point next = {
        static_cast<int8_t>(snake.body[0].x + snake.dx),
        static_cast<int8_t>(snake.body[0].y + snake.dy),
    };

    if (next.x < 0 || next.x >= static_cast<int8_t>(SNAKE_PLAY_WIDTH) ||
        next.y < 0 || next.y >= static_cast<int8_t>(SNAKE_PLAY_HEIGHT) ||
        snake_intersects(snake, next, true)) {
        snake.alive = false;
        return false;
    }

    bool grow = point_equal(next, food);
    if (grow && snake.length < MAX_SNAKE_LENGTH) {
        ++snake.length;
    }

    for (int i = snake.length - 1; i > 0; --i) {
        snake.body[i] = snake.body[i - 1];
    }
    snake.body[0] = next;
    return grow;
}

void place_food(Point& food, const Snake& snake) {
    for (;;) {
        food.x = static_cast<int8_t>(random_int(SNAKE_PLAY_WIDTH));
        food.y = static_cast<int8_t>(random_int(SNAKE_PLAY_HEIGHT));
        if (!snake_intersects(snake, food)) {
            return;
        }
    }
}

void write_cell(uint16_t row, uint16_t col, char c, uint8_t color) {
    if (row >= VGA_HEIGHT || col >= VGA_WIDTH) {
        return;
    }
    VGA_BUFFER[row * VGA_WIDTH + col] = vga_entry(c, color);
}

void write_at(uint16_t row, uint16_t col, const char* str, uint8_t color) {
    while (*str) {
        write_cell(row, col++, *str++, color);
        if (col == VGA_WIDTH) {
            col = 0;
            ++row;
        }
    }
}

void delay(uint32_t count) {
    while (count--) {
        asm volatile("nop");
    }
}

void clear_area(uint16_t row, uint16_t col, uint16_t width, uint16_t height, uint8_t color) {
    for (uint16_t y = row; y < row + height && y < VGA_HEIGHT; ++y) {
        for (uint16_t x = col; x < col + width && x < VGA_WIDTH; ++x) {
            write_cell(y, x, ' ', color);
        }
    }
}

void draw_snake_board(const Snake& snake, const Point& food, SnakeMode mode, int score) {
    for (uint16_t y = 0; y < SNAKE_BOARD_HEIGHT; ++y) {
        for (uint16_t x = 0; x < SNAKE_BOARD_WIDTH; ++x) {
            char ch = ' ';
            uint8_t color = vga_entry_color(VgaColor::LightGrey, VgaColor::Black);
            if (y == 0 || y + 1 == SNAKE_BOARD_HEIGHT) {
                ch = '-';
                color = vga_entry_color(VgaColor::White, VgaColor::Black);
            } else if (x == 0 || x + 1 == SNAKE_BOARD_WIDTH) {
                ch = '|';
                color = vga_entry_color(VgaColor::White, VgaColor::Black);
            }
            if ((x == 0 || x + 1 == SNAKE_BOARD_WIDTH) && (y == 0 || y + 1 == SNAKE_BOARD_HEIGHT)) {
                ch = '+';
            }
            write_cell(SNAKE_BOARD_ROW + y, SNAKE_BOARD_COL + x, ch, color);
        }
    }

    for (uint16_t y = 1; y + 1 < SNAKE_BOARD_HEIGHT; ++y) {
        for (uint16_t x = 1; x + 1 < SNAKE_BOARD_WIDTH; ++x) {
            write_cell(SNAKE_BOARD_ROW + y, SNAKE_BOARD_COL + x, ' ',
                       vga_entry_color(VgaColor::Black, VgaColor::Black));
        }
    }

    write_cell(SNAKE_BOARD_ROW + 1 + food.y, SNAKE_BOARD_COL + 1 + food.x,
               'X', vga_entry_color(VgaColor::LightRed, VgaColor::Black));

    for (int i = 0; i < snake.length; ++i) {
        char ch = (i == 0) ? 'O' : 'o';
        uint8_t color = (i == 0)
                            ? vga_entry_color(VgaColor::LightGreen, VgaColor::Black)
                            : vga_entry_color(VgaColor::Green, VgaColor::Black);
        write_cell(SNAKE_BOARD_ROW + 1 + snake.body[i].y,
                   SNAKE_BOARD_COL + 1 + snake.body[i].x,
                   ch, color);
    }

    const char* mode_name = "human";
    if (mode == SnakeMode::Greedy) {
        mode_name = "greedy";
    } else if (mode == SnakeMode::Random) {
        mode_name = "random";
    } else if (mode == SnakeMode::Spiral) {
        mode_name = "spiral";
    }
    write_at(SNAKE_BOARD_ROW + SNAKE_BOARD_HEIGHT + 0, SNAKE_BOARD_COL,
             "Snake mode: ", vga_entry_color(VgaColor::White, VgaColor::Black));
    write_at(SNAKE_BOARD_ROW + SNAKE_BOARD_HEIGHT + 0, SNAKE_BOARD_COL + 12,
             mode_name, vga_entry_color(VgaColor::Yellow, VgaColor::Black));

    char score_text[32];
    int len = 0;
    int value = score;
    if (value == 0) {
        score_text[len++] = '0';
    } else {
        char tmp[16];
        int tmp_len = 0;
        while (value > 0 && tmp_len < static_cast<int>(sizeof(tmp))) {
            tmp[tmp_len++] = static_cast<char>('0' + (value % 10));
            value /= 10;
        }
        while (tmp_len > 0) {
            score_text[len++] = tmp[--tmp_len];
        }
    }
    score_text[len] = '\0';

    write_at(SNAKE_BOARD_ROW + SNAKE_BOARD_HEIGHT + 1, SNAKE_BOARD_COL,
             "Score: ", vga_entry_color(VgaColor::White, VgaColor::Black));
    write_at(SNAKE_BOARD_ROW + SNAKE_BOARD_HEIGHT + 1, SNAKE_BOARD_COL + 7,
             score_text, vga_entry_color(VgaColor::LightCyan, VgaColor::Black));
    write_at(SNAKE_BOARD_ROW + SNAKE_BOARD_HEIGHT + 2, SNAKE_BOARD_COL,
             "Controls: WASD = move, Esc = quit", vga_entry_color(VgaColor::LightGrey, VgaColor::Black));
}

void run_snake_ai(Snake& snake, const Point& food, SnakeMode mode, int tick) {
    if (mode == SnakeMode::Greedy) {
        int8_t dx = 0;
        int8_t dy = 0;
        if (snake.body[0].x < food.x) dx = 1;
        else if (snake.body[0].x > food.x) dx = -1;
        else if (snake.body[0].y < food.y) dy = 1;
        else if (snake.body[0].y > food.y) dy = -1;
        if (dx != 0 && snake_can_move(snake, dx, 0)) {
            snake_set_direction(snake, dx, 0);
        } else if (dy != 0 && snake_can_move(snake, 0, dy)) {
            snake_set_direction(snake, 0, dy);
        } else if (snake_can_move(snake, snake.dx, snake.dy)) {
        } else if (snake_can_move(snake, -snake.dy, snake.dx)) {
            snake_set_direction(snake, -snake.dy, snake.dx);
        } else if (snake_can_move(snake, snake.dy, -snake.dx)) {
            snake_set_direction(snake, snake.dy, -snake.dx);
        }
    } else if (mode == SnakeMode::Random) {
        if ((tick % 5) == 0) {
            int tries = 0;
            while (tries < 10) {
                int choice = random_int(4);
                int8_t dx = 0;
                int8_t dy = 0;
                if (choice == 0) { dx = 1; }
                else if (choice == 1) { dx = -1; }
                else if (choice == 2) { dy = 1; }
                else { dy = -1; }
                if (!(dx == -snake.dx && dy == -snake.dy) &&
                    snake_can_move(snake, dx, dy)) {
                    snake_set_direction(snake, dx, dy);
                    break;
                }
                ++tries;
            }
        }
        if (!snake_can_move(snake, snake.dx, snake.dy)) {
            if (snake_can_move(snake, -snake.dy, snake.dx)) {
                snake_set_direction(snake, -snake.dy, snake.dx);
            } else if (snake_can_move(snake, snake.dy, -snake.dx)) {
                snake_set_direction(snake, snake.dy, -snake.dx);
            }
        }
    } else if (mode == SnakeMode::Spiral) {
        int8_t next_dx = snake.dy;
        int8_t next_dy = -snake.dx;
        if (!snake_can_move(snake, next_dx, next_dy)) {
            if (!snake_can_move(snake, snake.dx, snake.dy)) {
                if (snake_can_move(snake, -snake.dy, snake.dx)) {
                    next_dx = -snake.dy;
                    next_dy = snake.dx;
                } else if (snake_can_move(snake, 0, -1)) {
                    next_dx = 0;
                    next_dy = -1;
                } else if (snake_can_move(snake, -1, 0)) {
                    next_dx = -1;
                    next_dy = 0;
                } else if (snake_can_move(snake, 1, 0)) {
                    next_dx = 1;
                    next_dy = 0;
                } else if (snake_can_move(snake, 0, 1)) {
                    next_dx = 0;
                    next_dy = 1;
                }
            } else {
                next_dx = snake.dx;
                next_dy = snake.dy;
            }
        }
        snake_set_direction(snake, next_dx, next_dy);
    }
}

void run_snake_game(SnakeMode mode) {
    Snake snake;
    snake.alive = true;
    snake.length = 3;
    snake.dx = 1;
    snake.dy = 0;
    snake.body[0] = {static_cast<int8_t>(SNAKE_PLAY_WIDTH / 2),
                     static_cast<int8_t>(SNAKE_PLAY_HEIGHT / 2)};
    snake.body[1] = {static_cast<int8_t>(snake.body[0].x - 1), snake.body[0].y};
    snake.body[2] = {static_cast<int8_t>(snake.body[0].x - 2), snake.body[0].y};

    Point food;
    place_food(food, snake);

    int score = 0;
    int tick = 0;

    clear_area(0, 0, VGA_WIDTH, VGA_HEIGHT, vga_entry_color(VgaColor::Black, VgaColor::Black));
    g_terminal.set_color(vga_entry_color(VgaColor::LightGreen, VgaColor::Black));
    g_terminal.set_position(SNAKE_BOARD_ROW + SNAKE_BOARD_HEIGHT + 4, 0);
    g_terminal.write("Press 'Esc' to exit the current snake game.\n");

    for (;;) {
        if (mode == SnakeMode::Human) {
            char c = read_char_nonblocking();
            if (c == 'w' || c == 'W') { snake_set_direction(snake, 0, -1); }
            else if (c == 's' || c == 'S') { snake_set_direction(snake, 0, 1); }
            else if (c == 'a' || c == 'A') { snake_set_direction(snake, -1, 0); }
            else if (c == 'd' || c == 'D') { snake_set_direction(snake, 1, 0); }
            else if (c == 27) {
                break;
            }
        } else {
            run_snake_ai(snake, food, mode, tick);
        }

        if (!snake_step(snake, food)) {
            if (!snake.alive) {
                break;
            }
        } else {
            ++score;
            place_food(food, snake);
        }

        draw_snake_board(snake, food, mode, score);
        g_terminal.set_position(SNAKE_BOARD_ROW + SNAKE_BOARD_HEIGHT + 4, 0);

        if (!snake.alive) {
            break;
        }

        delay(GAME_DELAY);
        ++tick;
    }

    g_terminal.set_position(SNAKE_BOARD_ROW + SNAKE_BOARD_HEIGHT + 5, 0);
    if (snake.alive) {
        g_terminal.write("Game exited. ");
    } else {
        g_terminal.write("Game over. ");
    }
    g_terminal.write("Final score: ");
    char score_text[16];
    int len = 0;
    int value = score;
    if (value == 0) {
        score_text[len++] = '0';
    } else {
        char tmp[16];
        int tmp_len = 0;
        while (value > 0) {
            tmp[tmp_len++] = static_cast<char>('0' + (value % 10));
            value /= 10;
        }
        while (tmp_len > 0) {
            score_text[len++] = tmp[--tmp_len];
        }
    }
    score_text[len] = '\0';
    g_terminal.write(score_text);
    g_terminal.write("\n\n");
}

bool str_equal_ignore_case(const char* a, const char* b) {
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? static_cast<char>(*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? static_cast<char>(*b + 32) : *b;
        if (ca != cb) return false;
        ++a;
        ++b;
    }
    return *a == *b;
}

const char* skip_spaces(const char* s) {
    while (*s == ' ') { ++s; }
    return s;
}

int parse_int(const char*& s, int& out) {
    s = skip_spaces(s);
    if (*s == '\0') {
        return 0;
    }
    bool negative = false;
    if (*s == '+' || *s == '-') {
        negative = (*s == '-');
        ++s;
    }
    if (*s < '0' || *s > '9') {
        return 0;
    }
    int value = 0;
    while (*s >= '0' && *s <= '9') {
        value = value * 10 + (*s - '0');
        ++s;
    }
    out = negative ? -value : value;
    return 1;
}

void int_to_string(int value, char* buffer, int& length) {
    if (value == 0) {
        buffer[length++] = '0';
        return;
    }
    if (value < 0) {
        buffer[length++] = '-';
        value = -value;
    }
    char temp[16];
    int temp_len = 0;
    while (value > 0) {
        temp[temp_len++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    }
    while (temp_len > 0) {
        buffer[length++] = temp[--temp_len];
    }
}

struct RtcTime {
    int hour;
    int minute;
    int second;
};

uint8_t read_cmos(uint8_t reg) {
    outb(0x70, static_cast<uint8_t>(reg | 0x80));
    return inb(0x71);
}

int bcd_to_binary(uint8_t value) {
    return ((value >> 4) * 10) + (value & 0x0F);
}

RtcTime read_rtc_time() {
    while (read_cmos(0x0A) & 0x80) {
        // wait for update cycle to finish
    }
    uint8_t second = read_cmos(0x00);
    uint8_t minute = read_cmos(0x02);
    uint8_t hour = read_cmos(0x04);
    uint8_t regB = read_cmos(0x0B);
    bool hour_24 = (regB & 0x02) != 0;
    bool binary = (regB & 0x04) != 0;

    if (!binary) {
        second = static_cast<uint8_t>(bcd_to_binary(second));
        minute = static_cast<uint8_t>(bcd_to_binary(minute));
        hour = static_cast<uint8_t>(bcd_to_binary(hour & 0x7F));
    } else {
        hour &= 0x7F;
    }

    if (!hour_24) {
        bool pm = (hour & 0x80) != 0;
        hour &= 0x7F;
        if (pm && hour != 12) {
            hour += 12;
        } else if (!pm && hour == 12) {
            hour = 0;
        }
    }

    return {static_cast<int>(hour), static_cast<int>(minute), static_cast<int>(second)};
}

int normalize_hour(int hour) {
    while (hour < 0) hour += 24;
    while (hour >= 24) hour -= 24;
    return hour;
}

void write_time_zone(const char* name, int offset, const RtcTime& utc) {
    char buffer[64];
    int len = 0;
    const char* prefix = "  ";
    while (*prefix) buffer[len++] = *prefix++;
    const char* text = name;
    while (*text) buffer[len++] = *text++;
    buffer[len++] = ':';
    buffer[len++] = ' ';
    int hour = normalize_hour(utc.hour + offset);
    int minute = utc.minute;
    int second = utc.second;
    if (hour < 10) buffer[len++] = '0';
    int_to_string(hour, buffer, len);
    buffer[len++] = ':';
    if (minute < 10) buffer[len++] = '0';
    int_to_string(minute, buffer, len);
    buffer[len++] = ':';
    if (second < 10) buffer[len++] = '0';
    int_to_string(second, buffer, len);
    buffer[len] = '\0';
    g_terminal.write(buffer);
    g_terminal.write("\n");
}

void run_clock_cli(const char* line) {
    const char* arg = skip_spaces(line + 5);
    if (*arg == '\0' || str_equal(arg, "help") || str_equal(arg, "all")) {
        RtcTime utc = read_rtc_time();
        if (*arg == '\0' || str_equal(arg, "all")) {
            g_terminal.write("World clocks:\n");
            write_time_zone("UTC", 0, utc);
            write_time_zone("London", 0, utc);
            write_time_zone("New York", -5, utc);
            write_time_zone("Moscow", 3, utc);
            write_time_zone("Tokyo", 9, utc);
            write_time_zone("Sydney", 10, utc);
            return;
        }
        g_terminal.write("Usage: clock <zone>\n");
        g_terminal.write("  utc      - Coordinated Universal Time\n");
        g_terminal.write("  london   - London\n");
        g_terminal.write("  newyork  - New York\n");
        g_terminal.write("  moscow   - Moscow\n");
        g_terminal.write("  tokyo    - Tokyo\n");
        g_terminal.write("  sydney   - Sydney\n");
        g_terminal.write("  all      - show all zones\n");
        return;
    }

    RtcTime utc = read_rtc_time();
    if (str_equal_ignore_case(arg, "utc")) {
        write_time_zone("UTC", 0, utc);
    } else if (str_equal_ignore_case(arg, "london")) {
        write_time_zone("London", 0, utc);
    } else if (str_equal_ignore_case(arg, "newyork")) {
        write_time_zone("New York", -5, utc);
    } else if (str_equal_ignore_case(arg, "moscow")) {
        write_time_zone("Moscow", 3, utc);
    } else if (str_equal_ignore_case(arg, "tokyo")) {
        write_time_zone("Tokyo", 9, utc);
    } else if (str_equal_ignore_case(arg, "sydney")) {
        write_time_zone("Sydney", 10, utc);
    } else {
        g_terminal.write("Unknown zone: ");
        g_terminal.write(arg);
        g_terminal.write("\n");
        g_terminal.write("Type 'clock help' for available zones.\n");
    }
}

void run_calc_cli(const char* line) {
    const char* arg = skip_spaces(line + 4);
    if (*arg == '\0' || str_equal(arg, "help")) {
        g_terminal.write("Usage: calc <op> <a> <b>\n");
        g_terminal.write("  add <a> <b> - add two integers\n");
        g_terminal.write("  sub <a> <b> - subtract b from a\n");
        g_terminal.write("  mul <a> <b> - multiply two integers\n");
        g_terminal.write("  div <a> <b> - integer divide a by b\n");
        g_terminal.write("  mod <a> <b> - remainder of a / b\n");
        return;
    }

    char op_name[8];
    int op_len = 0;
    while (*arg && *arg != ' ' && op_len < static_cast<int>(sizeof(op_name) - 1)) {
        op_name[op_len++] = *arg++;
    }
    op_name[op_len] = '\0';
    arg = skip_spaces(arg);

    int a = 0;
    int b = 0;
    if (!parse_int(arg, a)) {
        g_terminal.write("Expected first integer after operation.\n");
        return;
    }
    if (!parse_int(arg, b)) {
        g_terminal.write("Expected second integer after first.\n");
        return;
    }

    if (str_equal_ignore_case(op_name, "add")) {
        int result = a + b;
        char text[32];
        int len = 0;
        int_to_string(result, text, len);
        text[len] = '\0';
        g_terminal.write(text);
        g_terminal.write("\n");
    } else if (str_equal_ignore_case(op_name, "sub")) {
        int result = a - b;
        char text[32];
        int len = 0;
        int_to_string(result, text, len);
        text[len] = '\0';
        g_terminal.write(text);
        g_terminal.write("\n");
    } else if (str_equal_ignore_case(op_name, "mul")) {
        int result = a * b;
        char text[32];
        int len = 0;
        int_to_string(result, text, len);
        text[len] = '\0';
        g_terminal.write(text);
        g_terminal.write("\n");
    } else if (str_equal_ignore_case(op_name, "div")) {
        if (b == 0) {
            g_terminal.write("Division by zero.\n");
            return;
        }
        int result = a / b;
        char text[32];
        int len = 0;
        int_to_string(result, text, len);
        text[len] = '\0';
        g_terminal.write(text);
        g_terminal.write("\n");
    } else if (str_equal_ignore_case(op_name, "mod")) {
        if (b == 0) {
            g_terminal.write("Division by zero.\n");
            return;
        }
        int result = a % b;
        char text[32];
        int len = 0;
        int_to_string(result, text, len);
        text[len] = '\0';
        g_terminal.write(text);
        g_terminal.write("\n");
    } else {
        g_terminal.write("Unknown calc operation: ");
        g_terminal.write(op_name);
        g_terminal.write("\n");
        g_terminal.write("Type 'calc help' for supported operations.\n");
    }
}

void run_snake_cli(const char* line) {
    const char* arg = skip_spaces(line + 5);
    if (*arg == '\0' || str_equal(arg, "help")) {
        g_terminal.write("Usage: snake <mode>\n");
        g_terminal.write("  human   - play with WASD controls\n");
        g_terminal.write("  greedy  - greedy AI like snake.io bots\n");
        g_terminal.write("  random  - random AI with safe turns\n");
        g_terminal.write("  spiral  - spiral motion around the map\n");
        g_terminal.write("  all     - run all AI modes sequentially\n");
        return;
    }

    if (str_equal_ignore_case(arg, "human")) {
        run_snake_game(SnakeMode::Human);
    } else if (str_equal_ignore_case(arg, "greedy")) {
        run_snake_game(SnakeMode::Greedy);
    } else if (str_equal_ignore_case(arg, "random")) {
        run_snake_game(SnakeMode::Random);
    } else if (str_equal_ignore_case(arg, "spiral")) {
        run_snake_game(SnakeMode::Spiral);
    } else if (str_equal_ignore_case(arg, "all")) {
        run_snake_game(SnakeMode::Greedy);
        run_snake_game(SnakeMode::Random);
        run_snake_game(SnakeMode::Spiral);
    } else {
        g_terminal.write("Unknown snake mode: ");
        g_terminal.write(arg);
        g_terminal.write("\n");
        g_terminal.write("Type 'snake help' for available modes.\n");
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
        g_terminal.write("  clock       - show world clocks\n");
        g_terminal.write("  calc        - integer math operations\n");
        g_terminal.write("  snake       - play snake or run AI modes\n");
    } else if (str_equal(line, "clear")) {
        g_terminal.clear(0x0F);
    } else if (str_equal(line, "about")) {
        g_terminal.write("hello-os: a minimal hand-written x86 kernel.\n");
        g_terminal.write("Boot chain: bootloader -> protected mode -> kmain().\n");
    } else if (str_equal(line, "snake") || str_starts_with(line, "snake ")) {
        run_snake_cli(line);
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
