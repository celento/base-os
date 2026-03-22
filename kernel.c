/*
 * BaseOS Kernel
 * VGA Mode 13h (320x200, 256 Colors)
 */

#include "font.h"

#define VGA_WIDTH 320
#define VGA_HEIGHT 200
#define VGA_ADDR ((volatile uint8_t *)0xA0000)

// Layout constants
#define CHAR_WIDTH 6
#define CHAR_HEIGHT 8
#define TITLE_BAR_HEIGHT 12
#define WINDOW_PADDING 10
#define MENU_ITEM_SPACING 10

// Colors
#define COLOR_BLACK 0
#define COLOR_BLUE 1
#define COLOR_GREEN 2
#define COLOR_CYAN 3
#define COLOR_RED 4
#define COLOR_MAGENTA 5
#define COLOR_BROWN 6
#define COLOR_LIGHT_GREY 7
#define COLOR_DARK_GREY 8
#define COLOR_LIGHT_BLUE 9
#define COLOR_LIGHT_GREEN 10
#define COLOR_LIGHT_CYAN 11
#define COLOR_LIGHT_RED 12
#define COLOR_LIGHT_MAGENTA 13
#define COLOR_YELLOW 14
#define COLOR_WHITE 15

// Hardware Ports
#define COM1_PORT 0x3f8
#define KEYBOARD_STATUS_PORT 0x64
#define KEYBOARD_DATA_PORT 0x60
#define QEMU_SHUTDOWN_PORT 0x604
#define VBOX_SHUTDOWN_PORT 0xB004

// Keys
#define KEY_ESC 0x01
#define KEY_BACKSPACE 0x0E
#define KEY_ENTER 0x1C
#define KEY_UP 0x48
#define KEY_DOWN 0x50
#define KEY_LEFT 0x4B
#define KEY_RIGHT 0x4D

static inline void outb(uint16_t port, uint8_t val) {
  asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline void outw(uint16_t port, uint16_t val) {
  asm volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
  uint8_t ret;
  asm volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
  return ret;
}

// Serial
void serial_init() {
  outb(COM1_PORT + 1, 0x00);
  outb(COM1_PORT + 3, 0x80);
  outb(COM1_PORT + 0, 0x03);
  outb(COM1_PORT + 1, 0x00);
  outb(COM1_PORT + 3, 0x03);
  outb(COM1_PORT + 2, 0xC7);
  outb(COM1_PORT + 4, 0x0B);
}

int serial_is_transmit_empty() { return inb(COM1_PORT + 5) & 0x20; }

void serial_write(char a) {
  while (serial_is_transmit_empty() == 0)
    ;
  outb(COM1_PORT, a);
}

void kprint_debug(const char *str) {
  while (*str) {
    serial_write(*str++);
  }
}

int kstrlen(const char *str) {
  int len = 0;
  while (str[len])
    len++;
  return len;
}

// Graphics
void put_pixel(int x, int y, uint8_t color) {
  if (x < 0 || x >= VGA_WIDTH || y < 0 || y >= VGA_HEIGHT)
    return;
  VGA_ADDR[y * VGA_WIDTH + x] = color;
}

void draw_rect(int x, int y, int w, int h, uint8_t color) {
  for (int i = 0; i < h; i++) {
    for (int j = 0; j < w; j++) {
      put_pixel(x + j, y + i, color);
    }
  }
}

void draw_line(int x1, int y1, int x2, int y2, uint8_t color) {
  if (y1 == y2) {
    for (int x = x1; x <= x2; x++)
      put_pixel(x, y1, color);
  } else if (x1 == x2) {
    for (int y = y1; y <= y2; y++)
      put_pixel(x1, y, color);
  }
}

void draw_char(char c, int x, int y, uint8_t color) {
  if (c == ' ')
    return;

  const uint8_t(*font)[5] = 0;
  int index = 0;

  if (c >= 'a' && c <= 'z') {
    font = font_lower;
    index = c - 'a';
  } else if (c >= 'A' && c <= 'Z') {
    font = font_alpha;
    index = c - 'A';
  } else if (c >= '0' && c <= '9') {
    font = font_digits;
    index = c - '0';
  }

  if (font) {
    for (int i = 0; i < 5; i++) {
      uint8_t col = font[index][i];
      for (int j = 0; j < 8; j++) {
        if (col & (1 << j)) {
          put_pixel(x + i, y + j, color);
        }
      }
    }
    return;
  }

  // Symbol lookup
  for (int s = 0; s < (int)FONT_SYMBOL_COUNT; s++) {
    if (font_symbols[s].c == c) {
      for (int i = 0; i < 5; i++) {
        uint8_t col = font_symbols[s].glyph[i];
        for (int j = 0; j < 8; j++) {
          if (col & (1 << j)) {
            put_pixel(x + i, y + j, color);
          }
        }
      }
      return;
    }
  }
}

void draw_string(const char *str, int x, int y, uint8_t color) {
  while (*str) {
    draw_char(*str++, x, y, color);
    x += CHAR_WIDTH;
  }
}

void draw_string_centered(const char *str, int y, uint8_t color) {
  int len = kstrlen(str);
  int x = (VGA_WIDTH - (len * CHAR_WIDTH)) / 2;
  draw_string(str, x, y, color);
}

// GUI
void gui_draw_window(int x, int y, int w, int h, const char *title) {
  uint8_t border = COLOR_BLACK;
  uint8_t bg = COLOR_WHITE;

  // Shadow
  draw_rect(x + 2, y + 2, w, h, COLOR_BLACK);

  // Window
  draw_rect(x, y, w, h, bg);

  // Border
  draw_rect(x, y, w, 1, border);
  draw_rect(x, y + h - 1, w, 1, border);
  draw_rect(x, y, 1, h, border);
  draw_rect(x + w - 1, y, 1, h, border);

  if (title) {
    draw_line(x, y + TITLE_BAR_HEIGHT, x + w, y + TITLE_BAR_HEIGHT, border);

    int text_len = kstrlen(title) * CHAR_WIDTH;
    int text_x = x + (w - text_len) / 2;
    draw_string(title, text_x, y + 3, border);
  }
}

uint8_t keyboard_read_scancode() {
  while ((inb(KEYBOARD_STATUS_PORT) & 1) == 0)
    ;
  return inb(KEYBOARD_DATA_PORT);
}

// PS/2 Scancode Set 1 to ASCII lookup (unshifted)
static const char scancode_to_ascii[128] = {
    0,    0,   '1', '2', '3', '4', '5', '6',  // 0x00-0x07
    '7',  '8', '9', '0', '-', '=', 0,   0,    // 0x08-0x0F
    'q',  'w', 'e', 'r', 't', 'y', 'u', 'i',  // 0x10-0x17
    'o',  'p', '[', ']', 0,   0,   'a', 's',   // 0x18-0x1F
    'd',  'f', 'g', 'h', 'j', 'k', 'l', ';',  // 0x20-0x27
    '\'', 0,   0,   '\\', 'z', 'x', 'c', 'v', // 0x28-0x2F
    'b',  'n', 'm', ',', '.', '/', 0,   '*',   // 0x30-0x37
    0,    ' ', 0,   0,   0,   0,   0,   0,     // 0x38-0x3F
    0,    0,   0,   0,   0,   0,   0,   0,     // 0x40-0x47
    0,    0,   0,   0,   0,   0,   0,   0,     // 0x48-0x4F
    0,    0,   0,   0,   0,   0,   0,   0,     // 0x50-0x57
    0,    0,   0,   0,   0,   0,   0,   0,     // 0x58-0x5F
    0,    0,   0,   0,   0,   0,   0,   0,     // 0x60-0x67
    0,    0,   0,   0,   0,   0,   0,   0,     // 0x68-0x6F
    0,    0,   0,   0,   0,   0,   0,   0,     // 0x70-0x77
    0,    0,   0,   0,   0,   0,   0,   0,     // 0x78-0x7F
};

// App states
enum AppState {
  STATE_MENU = 0,
  STATE_HELLO,
  STATE_HELP,
  STATE_ABOUT,
  STATE_SETTINGS,
  STATE_EDITOR
};

const char *MENU_ITEMS[] = {"HELP", "HELLO", "ABOUT", "SETTINGS", "EDITOR",
                            "SHUTDOWN"};
const int MENU_COUNT = 6;

struct Color {
  const char *name;
  uint8_t value;
};

const struct Color COLORS[] = {
    {"WHITE", COLOR_WHITE},   {"BLUE", COLOR_BLUE},
    {"GREEN", COLOR_GREEN},   {"CYAN", COLOR_CYAN},
    {"RED", COLOR_RED},       {"MAGENTA", COLOR_MAGENTA},
    {"BROWN", COLOR_BROWN},   {"L.GREY", COLOR_LIGHT_GREY},
    {"YELLOW", COLOR_YELLOW}, {"L.BLUE", COLOR_LIGHT_BLUE}};
const int COLOR_COUNT = 10;
int current_bg_color = 0;

void draw_menu_items(int x, int y, int selected) {
  for (int i = 0; i < MENU_COUNT; i++) {
    int item_y = y + (i * MENU_ITEM_SPACING);
    if (i == selected)
      draw_string(">", x, item_y, COLOR_BLACK);
    draw_string(MENU_ITEMS[i], x + WINDOW_PADDING, item_y, COLOR_BLACK);
  }
}

void draw_settings_menu(int x, int y, int selected) {
  for (int i = 0; i < COLOR_COUNT; i++) {
    int item_y = y + (i * MENU_ITEM_SPACING);

    if (i == selected)
      draw_string(">", x, item_y, COLOR_BLACK);

    // Color preview box
    draw_rect(x + 10, item_y, 8, 8, COLORS[i].value);
    draw_rect(x + 10, item_y, 8, 1, COLOR_BLACK);
    draw_rect(x + 10, item_y + 7, 8, 1, COLOR_BLACK);
    draw_rect(x + 10, item_y, 1, 8, COLOR_BLACK);
    draw_rect(x + 17, item_y, 1, 8, COLOR_BLACK);

    draw_string(COLORS[i].name, x + 25, item_y, COLOR_BLACK);

    if (i == current_bg_color) {
      draw_string("*", x + 80, item_y, COLOR_BLACK);
    }
  }
}

// Editor
#define EDITOR_ROWS 16
#define EDITOR_COLS 43
#define EDITOR_WIN_X 15
#define EDITOR_WIN_Y 10
#define EDITOR_WIN_W 290
#define EDITOR_WIN_H 180
#define EDITOR_TEXT_X (EDITOR_WIN_X + 6)
#define EDITOR_TEXT_Y (EDITOR_WIN_Y + TITLE_BAR_HEIGHT + 4)

static char editor_buf[EDITOR_ROWS][EDITOR_COLS + 1];
static int editor_cursor_row = 0;
static int editor_cursor_col = 0;
static int editor_initialized = 0;

void editor_init() {
  for (int r = 0; r < EDITOR_ROWS; r++) {
    for (int c = 0; c <= EDITOR_COLS; c++) {
      editor_buf[r][c] = 0;
    }
  }
  editor_cursor_row = 0;
  editor_cursor_col = 0;
  editor_initialized = 1;
}

int editor_line_len(int row) {
  int len = 0;
  while (len < EDITOR_COLS && editor_buf[row][len])
    len++;
  return len;
}

void draw_editor() {
  gui_draw_window(EDITOR_WIN_X, EDITOR_WIN_Y, EDITOR_WIN_W, EDITOR_WIN_H,
                  "Editor");

  // Draw text content
  for (int r = 0; r < EDITOR_ROWS; r++) {
    int y = EDITOR_TEXT_Y + r * (CHAR_HEIGHT + 1);
    if (y + CHAR_HEIGHT > EDITOR_WIN_Y + EDITOR_WIN_H - 2)
      break;

    for (int c = 0; c < EDITOR_COLS; c++) {
      char ch = editor_buf[r][c];
      if (ch == 0)
        break;
      int x = EDITOR_TEXT_X + c * CHAR_WIDTH;
      if (x + CHAR_WIDTH > EDITOR_WIN_X + EDITOR_WIN_W - 2)
        break;
      draw_char(ch, x, y, COLOR_BLACK);
    }
  }

  // Draw cursor (solid block)
  int cur_x = EDITOR_TEXT_X + editor_cursor_col * CHAR_WIDTH;
  int cur_y = EDITOR_TEXT_Y + editor_cursor_row * (CHAR_HEIGHT + 1);
  draw_rect(cur_x, cur_y + CHAR_HEIGHT - 1, CHAR_WIDTH - 1, 1, COLOR_BLACK);

  // Status line
  draw_string("ESC:MENU", EDITOR_WIN_X + 6,
              EDITOR_WIN_Y + EDITOR_WIN_H - 10, COLOR_DARK_GREY);
}

void editor_insert_char(char c) {
  int len = editor_line_len(editor_cursor_row);
  if (len >= EDITOR_COLS)
    return;

  // Shift characters right from cursor
  for (int i = len; i > editor_cursor_col; i--) {
    editor_buf[editor_cursor_row][i] = editor_buf[editor_cursor_row][i - 1];
  }
  editor_buf[editor_cursor_row][editor_cursor_col] = c;
  editor_cursor_col++;
}

void editor_backspace() {
  if (editor_cursor_col > 0) {
    // Delete char before cursor
    int len = editor_line_len(editor_cursor_row);
    for (int i = editor_cursor_col - 1; i < len - 1; i++) {
      editor_buf[editor_cursor_row][i] =
          editor_buf[editor_cursor_row][i + 1];
    }
    editor_buf[editor_cursor_row][len - 1] = 0;
    editor_cursor_col--;
  } else if (editor_cursor_row > 0) {
    // Join with previous line
    int prev_len = editor_line_len(editor_cursor_row - 1);
    int cur_len = editor_line_len(editor_cursor_row);
    if (prev_len + cur_len <= EDITOR_COLS) {
      // Append current line to previous
      for (int i = 0; i < cur_len; i++) {
        editor_buf[editor_cursor_row - 1][prev_len + i] =
            editor_buf[editor_cursor_row][i];
      }
      editor_buf[editor_cursor_row - 1][prev_len + cur_len] = 0;
      // Shift all lines up
      for (int r = editor_cursor_row; r < EDITOR_ROWS - 1; r++) {
        for (int c = 0; c <= EDITOR_COLS; c++) {
          editor_buf[r][c] = editor_buf[r + 1][c];
        }
      }
      // Clear last line
      for (int c = 0; c <= EDITOR_COLS; c++) {
        editor_buf[EDITOR_ROWS - 1][c] = 0;
      }
      editor_cursor_row--;
      editor_cursor_col = prev_len;
    }
  }
}

void editor_newline() {
  if (editor_cursor_row >= EDITOR_ROWS - 1)
    return;

  // Shift lines down to make room
  for (int r = EDITOR_ROWS - 1; r > editor_cursor_row + 1; r--) {
    for (int c = 0; c <= EDITOR_COLS; c++) {
      editor_buf[r][c] = editor_buf[r - 1][c];
    }
  }

  // Split current line at cursor
  int len = editor_line_len(editor_cursor_row);
  for (int c = 0; c <= EDITOR_COLS; c++) {
    editor_buf[editor_cursor_row + 1][c] = 0;
  }
  for (int c = editor_cursor_col; c < len; c++) {
    editor_buf[editor_cursor_row + 1][c - editor_cursor_col] =
        editor_buf[editor_cursor_row][c];
    editor_buf[editor_cursor_row][c] = 0;
  }

  editor_cursor_row++;
  editor_cursor_col = 0;
}

void kmain() {
  serial_init();
  kprint_debug("Kernel started\n");

  int state = STATE_MENU;
  int selected = 0;
  int settings_selected = 0;
  int dirty = 1;
  int full_redraw = 1;

  while (1) {
    if (dirty) {
      if (full_redraw) {
        uint8_t bg_col = COLORS[current_bg_color].value;
        for (int y = 0; y < VGA_HEIGHT; y++) {
          for (int x = 0; x < VGA_WIDTH; x++) {
            put_pixel(x, y, (x + y) % 2 == 0 ? bg_col : COLOR_BLACK);
          }
        }
        full_redraw = 0;
      }

      if (state == STATE_MENU) {
        int w = 100, h = 80;
        int x = (VGA_WIDTH - w) / 2;
        int y = (VGA_HEIGHT - h) / 2;
        gui_draw_window(x, y, w, h, "Start");
        draw_menu_items(x + WINDOW_PADDING, y + TITLE_BAR_HEIGHT + 6,
                        selected);
      } else if (state == STATE_HELLO) {
        int w = 120, h = 50;
        int x = (VGA_WIDTH - w) / 2;
        int y = (VGA_HEIGHT - h) / 2;
        gui_draw_window(x, y, w, h, "HELLO");
        draw_string_centered("HELLO WORLD", y + 20, COLOR_BLACK);
        draw_string_centered("PRESS ESC", y + 35, COLOR_DARK_GREY);
      } else if (state == STATE_HELP) {
        int w = 160, h = 60;
        int x = (VGA_WIDTH - w) / 2;
        int y = (VGA_HEIGHT - h) / 2;
        gui_draw_window(x, y, w, h, "HELP");
        draw_string("USE ARROWS TO MOVE", x + WINDOW_PADDING, y + 20,
                     COLOR_BLACK);
        draw_string("ENTER TO SELECT", x + WINDOW_PADDING, y + 30,
                     COLOR_BLACK);
        draw_string("ESC TO RETURN", x + WINDOW_PADDING, y + 40,
                     COLOR_BLACK);
      } else if (state == STATE_ABOUT) {
        int w = 120, h = 60;
        int x = (VGA_WIDTH - w) / 2;
        int y = (VGA_HEIGHT - h) / 2;
        gui_draw_window(x, y, w, h, "ABOUT");
        draw_string_centered("BASEOS KERNEL", y + 20, COLOR_BLACK);
        draw_string_centered("VER 0.1.0", y + 30, COLOR_DARK_GREY);
        draw_string_centered("2025 CCG", y + 40, COLOR_BLACK);
      } else if (state == STATE_SETTINGS) {
        int w = 140, h = 130;
        int x = (VGA_WIDTH - w) / 2;
        int y = (VGA_HEIGHT - h) / 2;
        gui_draw_window(x, y, w, h, "Settings");
        draw_settings_menu(x + WINDOW_PADDING,
                           y + TITLE_BAR_HEIGHT + 6, settings_selected);
      } else if (state == STATE_EDITOR) {
        draw_editor();
      }
      dirty = 0;
    }

    uint8_t sc = keyboard_read_scancode();
    if (sc & 0x80)
      continue;

    if (state == STATE_MENU) {
      if (sc == KEY_UP) {
        selected = (selected - 1 + MENU_COUNT) % MENU_COUNT;
        dirty = 1;
      } else if (sc == KEY_DOWN) {
        selected = (selected + 1) % MENU_COUNT;
        dirty = 1;
      } else if (sc == KEY_ENTER) {
        if (selected == 5) {
          // Shutdown
          outw(QEMU_SHUTDOWN_PORT, 0x2000);
          outw(VBOX_SHUTDOWN_PORT, 0x2000);
          asm volatile("hlt");
        } else {
          if (selected == 0)
            state = STATE_HELP;
          else if (selected == 1)
            state = STATE_HELLO;
          else if (selected == 2)
            state = STATE_ABOUT;
          else if (selected == 3)
            state = STATE_SETTINGS;
          else if (selected == 4) {
            if (!editor_initialized)
              editor_init();
            state = STATE_EDITOR;
          }
          dirty = 1;
          full_redraw = 1;
        }
      }
    } else if (state == STATE_SETTINGS) {
      if (sc == KEY_UP) {
        settings_selected =
            (settings_selected - 1 + COLOR_COUNT) % COLOR_COUNT;
        dirty = 1;
      } else if (sc == KEY_DOWN) {
        settings_selected = (settings_selected + 1) % COLOR_COUNT;
        dirty = 1;
      } else if (sc == KEY_ENTER) {
        current_bg_color = settings_selected;
        dirty = 1;
        full_redraw = 1;
      } else if (sc == KEY_ESC) {
        state = STATE_MENU;
        dirty = 1;
        full_redraw = 1;
      }
    } else if (state == STATE_EDITOR) {
      if (sc == KEY_ESC) {
        state = STATE_MENU;
        dirty = 1;
        full_redraw = 1;
      } else if (sc == KEY_BACKSPACE) {
        editor_backspace();
        dirty = 1;
        full_redraw = 1;
      } else if (sc == KEY_ENTER) {
        editor_newline();
        dirty = 1;
        full_redraw = 1;
      } else if (sc == KEY_UP) {
        if (editor_cursor_row > 0) {
          editor_cursor_row--;
          int len = editor_line_len(editor_cursor_row);
          if (editor_cursor_col > len)
            editor_cursor_col = len;
          dirty = 1;
          full_redraw = 1;
        }
      } else if (sc == KEY_DOWN) {
        if (editor_cursor_row < EDITOR_ROWS - 1) {
          editor_cursor_row++;
          int len = editor_line_len(editor_cursor_row);
          if (editor_cursor_col > len)
            editor_cursor_col = len;
          dirty = 1;
          full_redraw = 1;
        }
      } else if (sc == KEY_LEFT) {
        if (editor_cursor_col > 0) {
          editor_cursor_col--;
          dirty = 1;
          full_redraw = 1;
        }
      } else if (sc == KEY_RIGHT) {
        int len = editor_line_len(editor_cursor_row);
        if (editor_cursor_col < len) {
          editor_cursor_col++;
          dirty = 1;
          full_redraw = 1;
        }
      } else {
        // Printable character
        char c = (sc < 128) ? scancode_to_ascii[sc] : 0;
        if (c) {
          editor_insert_char(c);
          dirty = 1;
          full_redraw = 1;
        }
      }
    } else {
      if (sc == KEY_ESC) {
        state = STATE_MENU;
        dirty = 1;
        full_redraw = 1;
      }
    }
  }
}
