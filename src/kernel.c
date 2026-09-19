/*
 * BaseOS Kernel
 * VBE 1280x720 desktop, PS/2 mouse, RAM filesystem, apps
 */

#include "gfx.h"
#include "platform.h"
#include "history.h"
#include "program.h"
#include "native_example.h"
#include "persist.h"
#include "app.h"
#include "font.h"
#include "clock.h"
#include "calendar.h"
#include "mines.h"
#include "game2048.h"
#include "breakout.h"
#include "sysmon.h"
#include "fs.h"
#include "wordle.h"
#include "term.h"
#include "todo.h"
#include "assets/kilroy_about.h"
#include "assets/cursor_art.h"

#define CHAR_H         UI_FONT_H
#define LINE_H         (CHAR_H + 4)
#define EDIT_LINE_H    (EDIT_FONT_H + 1)

#define MENUBAR_H      36
#define TITLE_H        32
#define INFO_H         28
#define SB             16
#define ROW_H          28
#define ICON_S         32
#define TILE_S         48
#define CURSOR_W       CURSOR_ART_W
#define CURSOR_H       CURSOR_ART_H
#define MENU_ROW       30
#define CLOSE_S        18
#define BTN_H          30
#define TASKBAR_Y      (fb_h - TASKBAR_H)
#define TASKBAR_H      44
#define MAX_WIN        8

#define THEME_N        8

#define KEYBOARD_STATUS_PORT 0x64
#define KEYBOARD_DATA_PORT   0x60
#define QEMU_SHUTDOWN_PORT   0x604
#define VBOX_SHUTDOWN_PORT   0xB004

#define KEY_LCTRL      0x1D
#define KEY_LSHIFT     0x2A
#define KEY_RSHIFT     0x36
#define KEY_M          0x32
#define KEY_N          0x31
#define KEY_S          0x1F
#define KEY_W          0x11
#define KEY_Q          0x10
#define SAVER_DELAY    (70 * 90)

#define EDIT_BUF_SIZE  FS_MAX_SIZE

typedef struct {
    const char *name;
    uint32_t desk_top;
    uint32_t desk_bot;
    uint32_t accent;
} Theme;

static const Theme themes[THEME_N] = {
    {"Aqua",     0x4A7194, 0x14243A, 0x306AC4},
    {"Graphite", 0x667080, 0x242A36, 0x536278},
    {"Forest",   0x567E71, 0x172F2B, 0x28785C},
    {"Sunset",   0xA37366, 0x3B283B, 0xB95336},
    {"Plum",     0x7D739C, 0x2D2544, 0x7956B2},
    {"Ocean",    0x48858C, 0x18343E, 0x237987},
    {"Sand",     0x938572, 0x39342E, 0x91652B},
    {"Rose",     0x9C7389, 0x382536, 0xB5456C},
};

static int theme_id = 0;
static const char *session_status="";

static uint8_t ui_accent;
static uint8_t ui_accent_dk;
static uint32_t ui_accent_rgb;
static uint32_t ui_accent_rgb_lt;
static uint32_t ui_accent_rgb_dk;
static uint8_t ui_border;
static uint8_t ui_chrome;
static uint8_t ui_chrome_dk;
static uint8_t ui_text;
static uint8_t ui_text_dim;
static uint8_t ui_track;
static uint8_t ui_body;
static int desk_cache_theme = -1;
uint8_t app_accent;
uint8_t app_accent_dk;
uint8_t app_text;
uint8_t app_text_dim;
uint8_t app_chrome;
uint8_t app_chrome_dk;
static int saver_enabled = 1;
static int saver_on = 0;
static uint32_t last_input_frame = 0;
static int launcher_on = 0;
static char launch_buf[24];
static int launch_len = 0;
static int launch_sel = 0;
static unsigned redraw_count = 0;

static uint32_t rgb_lighten(uint32_t c, int pct) {
    int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
    r += (255 - r) * pct / 100;
    g += (255 - g) * pct / 100;
    b += (255 - b) * pct / 100;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static uint32_t rgb_darken(uint32_t c, int pct) {
    int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
    return ((uint32_t)(r * pct / 100) << 16) | ((uint32_t)(g * pct / 100) << 8) |
           (uint32_t)(b * pct / 100);
}

static uint32_t rgb_lerp(uint32_t a, uint32_t b, int t, int n) {
    int r0 = (a >> 16) & 255, g0 = (a >> 8) & 255, b0 = a & 255;
    int r1 = (b >> 16) & 255, g1 = (b >> 8) & 255, b1 = b & 255;
    if (n < 1) n = 1;
    return ((uint32_t)(r0 + (r1 - r0) * t / n) << 16) |
           ((uint32_t)(g0 + (g1 - g0) * t / n) << 8) |
           (uint32_t)(b0 + (b1 - b0) * t / n);
}

static uint8_t idx24(uint32_t c) {
    return gfx_rgb((c >> 16) & 255, (c >> 8) & 255, c & 255);
}

static void theme_apply(void) {
    const Theme *t = &themes[theme_id];
    ui_accent_rgb = t->accent;
    ui_accent_rgb_lt = rgb_lighten(t->accent, 45);
    ui_accent_rgb_dk = rgb_darken(t->accent, 52);
    gfx_set_ramp(PAL_DESK, PAL_DESK_N, t->desk_top, t->desk_bot);
    gfx_set_ramp(PAL_ACCENT, 9, ui_accent_rgb_lt, ui_accent_rgb);
    gfx_set_ramp(PAL_ACCENT + 8, 8, ui_accent_rgb, ui_accent_rgb_dk);
    gfx_pal_commit();
    ui_accent = PAL_ACCENT + 8;
    ui_accent_dk = PAL_ACCENT + 14;
    ui_border = gfx_gray(0xB8);
    ui_chrome = gfx_gray(0xF4);
    ui_chrome_dk = gfx_gray(0xDE);
    ui_text = gfx_gray(0x20);
    ui_text_dim = gfx_gray(0x70);
    ui_track = gfx_gray(0xF4);
    ui_body = COLOR_WHITE;
    desk_cache_theme = -1;
    app_accent = ui_accent;
    app_accent_dk = ui_accent_dk;
    app_text = ui_text;
    app_text_dim = ui_text_dim;
    app_chrome = ui_chrome;
    app_chrome_dk = ui_chrome_dk;
}

void drain_8042(void);
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

void drain_8042(void);

/* ---------- Serial ---------- */

static void serial_write(char c) {
    char text[2] = {c, 0};
    platform_log(text);
}

void kprint_debug(const char *str) { platform_log(str); }

static void kprint_uint(unsigned v) {
    char tmp[10];
    int n = 0;
    if (v == 0) {
        serial_write('0');
        return;
    }
    while (v && n < 10) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n)
        serial_write(tmp[--n]);
}

static void kprint_hex32(uint32_t v) {
    static const char *hex = "0123456789ABCDEF";
    kprint_debug("0x");
    for (int i = 7; i >= 0; i--)
        serial_write(hex[(v >> (i * 4)) & 0xF]);
}

/* ---------- Helpers ---------- */

static void utoa(unsigned v, char *out) {
    char tmp[10];
    int n = 0;
    if (v == 0) {
        out[0] = '0';
        out[1] = 0;
        return;
    }
    while (v && n < 10) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    int i = 0;
    while (n)
        out[i++] = tmp[--n];
    out[i] = 0;
}

static int btn_w(const char *label) { return ui_string_w(label) + 24; }

/* ---------- Shared desktop controls ---------- */

#define WIN_INFO 1
#define WIN_SCROLL 2
#define WIN_NOCLOSE 4
#define WIN_INACTIVE 8

static void close_box_pos(int wx, int wy, int *cx, int *cy) {
    *cx = wx + 9;
    *cy = wy + (TITLE_H - CLOSE_S) / 2 + 1;
}

static void min_box_pos(int wx, int wy, int *cx, int *cy) {
    *cx = wx + 9 + CLOSE_S + 8;
    *cy = wy + (TITLE_H - CLOSE_S) / 2 + 1;
}

static void draw_min_box(int x, int y, int active) {
    draw_round_rect(x, y, CLOSE_S, CLOSE_S, 5, gfx_gray(0xE8));
    draw_rect(x + 5, y + 9, 8, 2, active ? ui_text : ui_text_dim);
}

static void draw_close_box(int x, int y, int active) {
    draw_round_rect(x, y, CLOSE_S, CLOSE_S, 5, gfx_gray(0xE8));
    uint8_t ink = active ? ui_text : ui_text_dim;
    draw_line(x + 6, y + 6, x + 11, y + 11, ink);
    draw_line(x + 6, y + 7, x + 10, y + 11, ink);
    draw_line(x + 6, y + 11, x + 11, y + 6, ink);
    draw_line(x + 7, y + 11, x + 11, y + 7, ink);
}

static void draw_sb_arrow(int x, int y, int dir, uint8_t col) {
    int cx = x + SB / 2;
    int cy = y + SB / 2;
    for (int i = 0; i < 4; i++) {
        if (dir == 0)
            draw_line(cx - i, cy - 2 + i, cx + i, cy - 2 + i, col);
        else if (dir == 1)
            draw_line(cx - i, cy + 1 - i, cx + i, cy + 1 - i, col);
        else if (dir == 2)
            draw_line(cx - 2 + i, cy - i, cx - 2 + i, cy + i, col);
        else
            draw_line(cx + 1 - i, cy - i, cx + 1 - i, cy + i, col);
    }
}

static void draw_thumb(int x, int y, int w, int h) {
    draw_round_rect(x + 1, y + 1, w - 2, h - 2, 3, gfx_gray(0xB0));
}

static void draw_scrollbars(int x, int y, int w, int h, int top) {
    uint8_t arrow = gfx_rgb(0x60, 0x64, 0x70);
    int vx = x + w - 1 - SB;
    int vy = y + top + 1;
    int vh = h - top - SB - 2;
    if (vh < SB * 2)
        vh = SB * 2;
    draw_rect(vx, vy, SB, vh, ui_track);
    draw_vline(vx, vy, vh, ui_chrome_dk);
    draw_sb_arrow(vx, vy, 0, arrow);
    draw_sb_arrow(vx, vy + vh - SB, 1, arrow);
    int track_h = vh - 2 * SB;
    if (track_h > 30)
        draw_thumb(vx + 3, vy + SB + 2, SB - 5, 26);

    int hy = y + h - 1 - SB;
    int hw = w - SB - 1;
    draw_rect(x + 1, hy, hw, SB, ui_track);
    draw_hline(x + 1, hy, hw, ui_chrome_dk);
    draw_sb_arrow(x + 1, hy, 2, arrow);
    draw_sb_arrow(x + hw - SB, hy, 3, arrow);
    int track_w = hw - 2 * SB;
    if (track_w > 36)
        draw_thumb(x + 1 + SB + 2, hy + 3, 30, SB - 5);
    draw_rect(vx, hy, SB, SB, ui_track);
}

void gui_draw_window(int x, int y, int w, int h, const char *title,
                     const char *info, int flags) {
    int noclose = flags & WIN_NOCLOSE;
    int inactive = flags & WIN_INACTIVE;
    int r = 7;

    draw_shadow(x, y, w, h);
    draw_round_rect(x - 1, y - 1, w + 2, h + 2, r + 1, ui_border);
    draw_round_rect(x, y, w, h, r, ui_body);
    draw_round_top(x, y, w, TITLE_H, r, inactive ? ui_chrome : COLOR_WHITE);
    draw_hline(x, y + TITLE_H, w, ui_chrome_dk);

    if (!noclose) {
        int cx, cy;
        close_box_pos(x, y, &cx, &cy);
        draw_close_box(cx, cy, !inactive);
        min_box_pos(x, y, &cx, &cy);
        draw_min_box(cx, cy, !inactive);
        draw_round_rect(cx + CLOSE_S + 6, cy, CLOSE_S, CLOSE_S, 7,
                        gfx_gray(0xE8));
        draw_frame(cx + CLOSE_S + 11, cy + 5, 8, 8, inactive ? ui_text_dim : ui_text);
    }

    if(!noclose)for(int d=0;d<3;d++)draw_line(x+w-4-d*3,y+h-3,x+w-3,y+h-4-d*3,ui_chrome_dk);
    int title_y = y + (TITLE_H - CHAR_H) / 2;
    if (title) {
        int tw = uib_string_w(title);
        int left = x + (noclose ? 12 : CLOSE_S * 3 + 44);
        int tx = x + (w - tw) / 2;
        if (tx < left)
            tx = left;
        int xmax = x + w - 12;
        if (inactive)
            draw_string_bold_clip(title, tx, title_y, ui_text_dim, xmax);
        else {
            draw_string_bold_clip(title, tx, title_y, ui_text, xmax);
            draw_round_rect(tx - 10, title_y + CHAR_H / 2 - 2, 4, 4, 2, ui_accent);
        }
    }

    int top = TITLE_H;
    if ((flags & WIN_INFO) && info) {
        draw_rect(x, y + TITLE_H + 1, w, INFO_H - 1, ui_chrome);
        int iy = y + TITLE_H + (INFO_H - CHAR_H) / 2;
        draw_string_clip(info, x + 10, iy, ui_text_dim, x + w - 8);
        draw_hline(x, y + TITLE_H + INFO_H, w, ui_chrome_dk);
        top = TITLE_H + INFO_H;
    }

    if (flags & WIN_SCROLL)
        draw_scrollbars(x, y, w, h, top);
}

static void draw_button_styled(int x, int y, int w, int h, const char *label, int primary) {
    draw_round_rect(x, y, w, h, 6, primary ? ui_accent : ui_chrome_dk);
    if (!primary)
        draw_round_rect(x + 1, y + 1, w - 2, h - 2, 5, COLOR_WHITE);
    int tw = ui_string_w(label);
    int tx = x + (w - tw) / 2;
    int ty = y + (h - CHAR_H) / 2;
    draw_string_clip(label, tx, ty, primary ? COLOR_WHITE : ui_text, x + w - 4);
}

void draw_button(int x, int y, int w, int h, const char *label) {
    draw_button_styled(x, y, w, h, label, 0);
}

static void draw_default_button(int x, int y, int w, int h, const char *label) {
    draw_button_styled(x, y, w, h, label, 1);
}

static void draw_icon_art(int x, int y, const char *art, int scale, uint8_t ink, int fill) {
    for (int j = 0; j < 16; j++) {
        for (int i = 0; i < 16; i++) {
            char c = art[j * 16 + i];
            if (c == ' ')
                continue;
            int on = (c == '#');
            if (!on && fill < 0)
                continue;
            uint8_t col = on ? ink : (uint8_t)fill;
            draw_rect(x + i * scale, y + j * scale, scale, scale, col);
        }
    }
}

static const char icon_trash[] =
    "     ######     "
    "  ############  "
    " ############## "
    "  #..........#  "
    "  #.#..#..#..#  "
    "  #.#..#..#..#  "
    "  #.#..#..#..#  "
    "  #.#..#..#..#  "
    "  #.#..#..#..#  "
    "  #.#..#..#..#  "
    "  #.#..#..#..#  "
    "  #.#..#..#..#  "
    "  #..........#  "
    "   ##########   "
    "    ########    "
    "                ";

static const char icon_folder16[] =
    "                "
    "  ######        "
    " #......#       "
    " #......######  "
    " #.............#"
    " #.............#"
    " ###############"
    " #.............#"
    " #.............#"
    " #.............#"
    " #.............#"
    " #.............#"
    " #.............#"
    " ###############"
    "                "
    "                ";

static const char icon_edit16[] =
    "                "
    "   #########    "
    "   #.......##   "
    "   #.......#.#  "
    "   #.......#### "
    "   #..........# "
    "   #.#######..# "
    "   #..........# "
    "   #.#######..# "
    "   #..........# "
    "   #.#######..# "
    "   #..........# "
    "   #.#####....# "
    "   #..........# "
    "   ############ "
    "                ";

static const char icon_gear16[] =
    "                "
    "      ####      "
    "   #  #..#  #   "
    "  ###########   "
    "  #.........#   "
    "   #..###..#    "
    "  ##.#...#.##   "
    " #...#...#...#  "
    " #...#...#...#  "
    "  ##.#...#.##   "
    "   #..###..#    "
    "  #.........#   "
    "  ###########   "
    "   #  #..#  #   "
    "      ####      "
    "                ";

static const char icon_clock16[] =
    "     ######     "
    "   ##......##   "
    "  #..........#  "
    " #.....#......# "
    " #.....#......# "
    "#......#.......#"
    "#......#.......#"
    "#......####....#"
    "#..............#"
    "#..............#"
    " #............# "
    " #............# "
    "  #..........#  "
    "   ##......##   "
    "     ######     "
    "                ";

static const char icon_cal16[] =
    "   #        #   "
    " ############## "
    " #............# "
    " ############## "
    " #............# "
    " #.#.#.#.#.#..# "
    " #............# "
    " #.#.###.#.#..# "
    " #............# "
    " #.#.#.#.#.#..# "
    " #............# "
    " #.#.#........# "
    " #............# "
    " ############## "
    "                "
    "                ";

static const char icon_mine16[] =
    "       #        "
    "   #   #   #    "
    "    # ### #     "
    "     #####      "
    "    ##.####     "
    "  ###.#####  #  "
    "   ########     "
    "#  ########   # "
    "   ########     "
    "  ##########    "
    "    #######  #  "
    "     #####      "
    "    # ### #     "
    "   #   #   #    "
    "       #        "
    "                ";

static const char icon_2048_16[] =
    "################"
    "#......#.......#"
    "#.###..#..###..#"
    "#...#..#....#..#"
    "#.###..#..###..#"
    "#.#....#..#.#..#"
    "#.###..#..###..#"
    "################"
    "#......#.......#"
    "#.#.#..#..###..#"
    "#.#.#..#..#.#..#"
    "#.###..#..###..#"
    "#...#..#..#.#..#"
    "#...#..#..###..#"
    "#......#.......#"
    "################";

static const char icon_brick16[] =
    "################"
    "#..#..#..#..#..#"
    "################"
    "#.#..#..#..#..##"
    "################"
    "#..#..#..#..#..#"
    "################"
    "                "
    "                "
    "       ##       "
    "       ##       "
    "                "
    "                "
    "   ##########   "
    "   ##########   "
    "                ";

static const char icon_mon16[] =
    "################"
    "#..............#"
    "#..............#"
    "#..........#...#"
    "#.....#....#...#"
    "#.....#...##...#"
    "#..#..#...##...#"
    "#..#.##.#.##.#.#"
    "#.##.####.##.#.#"
    "#.##.####.####.#"
    "#.############.#"
    "#..............#"
    "################"
    "      ####      "
    "    ########    "
    "                ";

typedef struct {
    const char *label;
    const char *art;
    uint32_t color;
    int x, y;
} DeskIcon;

enum {
    ICON_FILES = 0,
    ICON_EDIT,
    ICON_CALC,
    ICON_PAINT,
    ICON_VIEW,
    ICON_SNAKE,
    ICON_WORDLE,
    ICON_TERM,
    ICON_TODO,
    ICON_CLOCK,
    ICON_CAL,
    ICON_MINES,
    ICON_2048,
    ICON_BREAKOUT,
    ICON_SYSMON,
    ICON_SETTINGS,
    ICON_TRASH,
    ICON_COUNT
};
#define ICON_DISK ICON_FILES

static const char icon_calc16[];
static const char icon_paint16[];
static const char icon_view16[];
static const char icon_snake16[];
static const char icon_wordle16[];
static const char icon_term16[];
static const char icon_todo16[];

static const char icon_clock16[];
static const char icon_cal16[];
static const char icon_mine16[];
static const char icon_2048_16[];
static const char icon_brick16[];
static const char icon_mon16[];

static DeskIcon icons[ICON_COUNT];

static void icon_set(int id, const char *label, const char *art, uint32_t color) {
    icons[id].label = label;
    icons[id].art = art;
    icons[id].color = color;
}

static void icons_init(void) {
    icon_set(ICON_FILES, "Files", icon_folder16, 0x3B82F6);
    icon_set(ICON_EDIT, "Editor", icon_edit16, 0x6B7A90);
    icon_set(ICON_CALC, "Calculator", icon_calc16, 0xF59E0B);
    icon_set(ICON_PAINT, "Paint", icon_paint16, 0xEC4899);
    icon_set(ICON_VIEW, "Viewer", icon_view16, 0x8B5CF6);
    icon_set(ICON_SNAKE, "Snake", icon_snake16, 0x22C55E);
    icon_set(ICON_WORDLE, "Wordle", icon_wordle16, 0x14B8A6);
    icon_set(ICON_TERM, "Terminal", icon_term16, 0x2B3140);
    icon_set(ICON_TODO, "Todo", icon_todo16, 0xEF4444);
    icon_set(ICON_CLOCK, "Clock", icon_clock16, 0x0EA5E9);
    icon_set(ICON_CAL, "Calendar", icon_cal16, 0xF97316);
    icon_set(ICON_MINES, "Mines", icon_mine16, 0x475569);
    icon_set(ICON_2048, "2048", icon_2048_16, 0xEDC22E);
    icon_set(ICON_BREAKOUT, "Breakout", icon_brick16, 0xA855F7);
    icon_set(ICON_SYSMON, "Monitor", icon_mon16, 0x10B981);
    icon_set(ICON_SETTINGS, "Settings", icon_gear16, 0x7C8494);
    icon_set(ICON_TRASH, "Trash", icon_trash, 0x9CA3AF);

    int col_w = 96;
    int row_h = 86;
    int x0 = 26;
    int y0 = MENUBAR_H + 16;
    int rows = (TASKBAR_Y - y0 - 12) / row_h;
    if (rows < 1) rows = 1;
    for (int i = 0; i < ICON_TRASH; i++) {
        icons[i].x = x0 + (i / rows) * col_w;
        icons[i].y = y0 + (i % rows) * row_h;
    }
    icons[ICON_TRASH].x = fb_w - 26 - TILE_S;
    icons[ICON_TRASH].y = TASKBAR_Y - 12 - TILE_S - CHAR_H - 8;
}

static void icon_label_pos(int id, int *lx, int *ly, int *lw) {
    int tw = ui_string_w(icons[id].label);
    *lx = icons[id].x + TILE_S / 2 - tw / 2;
    *ly = icons[id].y + TILE_S + 6;
    *lw = tw;
}

static int icon_hit(int id, int px, int py) {
    int lx, ly, lw;
    icon_label_pos(id, &lx, &ly, &lw);
    int x0 = icons[id].x < lx ? icons[id].x : lx;
    int x1 = icons[id].x + TILE_S;
    if (lx + lw > x1)
        x1 = lx + lw;
    return hit(px, py, x0 - 6, icons[id].y - 4, (x1 - x0) + 12, TILE_S + CHAR_H + 16);
}

/* Simple geometric symbols share a 32px canvas and a two-pixel stroke. */
static void symbol_line(int x,int y,int a,int b,int c,int d,uint8_t ink) {
    draw_line(x+a,y+b,x+c,y+d,ink);
    if(c-a > d-b)draw_line(x+a,y+b+1,x+c,y+d+1,ink);
    else draw_line(x+a+1,y+b,x+c+1,y+d,ink);
}
static void symbol_box(int x,int y,int w,int h,int r,uint8_t ink) {
    draw_round_frame(x,y,w,h,r,ink);
    draw_round_frame(x+1,y+1,w-2,h-2,r>1?r-1:0,ink);
}
static void draw_app_symbol(int id,int x,int y,uint8_t ink) {
    switch(id) {
    case ICON_FILES:
        symbol_box(x+2,y+8,28,21,3,ink);
        draw_round_rect(x+3,y+4,12,6,2,ink);
        break;
    case ICON_EDIT:
        symbol_box(x+6,y+2,21,28,3,ink);
        for(int j=0;j<3;j++)draw_rect(x+11,y+10+j*6,j==2?8:11,2,ink);
        break;
    case ICON_TERM:
        symbol_line(x,y,5,8,12,15,ink);symbol_line(x,y,12,15,5,22,ink);
        draw_rect(x+17,y+22,10,2,ink);break;
    case ICON_CALC:
        symbol_box(x+5,y+1,23,30,4,ink);draw_rect(x+10,y+7,13,3,ink);
        for(int j=0;j<2;j++)for(int i=0;i<3;i++)draw_round_rect(x+10+i*5,y+15+j*7,3,3,1,ink);
        break;
    case ICON_PAINT:
        symbol_line(x,y,9,22,23,5,ink);symbol_line(x,y,13,24,27,7,ink);
        symbol_line(x,y,23,5,27,7,ink);symbol_line(x,y,9,22,13,24,ink);
        draw_round_rect(x+5,y+24,7,5,2,ink);break;
    case ICON_VIEW:
        symbol_box(x+2,y+4,28,24,3,ink);draw_round_rect(x+20,y+9,4,4,2,ink);
        symbol_line(x,y,6,23,13,15,ink);symbol_line(x,y,13,15,19,22,ink);
        symbol_line(x,y,19,22,24,17,ink);break;
    case ICON_CLOCK:
        symbol_box(x+2,y+2,28,28,14,ink);
        symbol_line(x,y,16,7,16,16,ink);symbol_line(x,y,16,16,22,19,ink);break;
    case ICON_CAL:
        symbol_box(x+3,y+6,26,24,3,ink);draw_rect(x+4,y+12,24,2,ink);
        draw_rect(x+9,y+2,2,8,ink);draw_rect(x+22,y+2,2,8,ink);
        for(int j=0;j<2;j++)for(int i=0;i<3;i++)draw_rect(x+8+i*7,y+18+j*6,3,2,ink);
        break;
    case ICON_TODO:
        for(int j=0;j<3;j++){
            symbol_line(x,y,3,6+j*9,5,8+j*9,ink);symbol_line(x,y,5,8+j*9,9,3+j*9,ink);
            draw_rect(x+14,y+6+j*9,14,2,ink);
        }break;
    case ICON_SETTINGS:
        symbol_box(x+7,y+7,18,18,9,ink);symbol_box(x+13,y+13,6,6,3,ink);
        for(int j=0;j<2;j++){
            draw_rect(x+14,y+2+j*24,4,5,ink);draw_rect(x+2+j*24,y+14,5,4,ink);
        }
        symbol_line(x,y,5,5,9,9,ink);symbol_line(x,y,23,23,27,27,ink);
        symbol_line(x,y,5,27,9,23,ink);symbol_line(x,y,23,9,27,5,ink);break;
    case ICON_SYSMON:
        draw_rect(x+4,y+19,5,10,ink);draw_rect(x+13,y+11,5,18,ink);draw_rect(x+22,y+3,5,26,ink);break;
    case ICON_WORDLE:case ICON_2048:
        for(int j=0;j<2;j++)for(int i=0;i<2;i++)symbol_box(x+3+i*15,y+3+j*15,11,11,2,ink);
        if(id==ICON_WORDLE)draw_rect(x+6,y+6,5,5,ink);
        else {draw_rect(x+21,y+6,5,2,ink);draw_rect(x+21,y+8,2,3,ink);}
        break;
    case ICON_SNAKE:
        symbol_line(x,y,6,26,6,9,ink);symbol_line(x,y,6,9,16,9,ink);
        symbol_line(x,y,16,9,16,24,ink);symbol_line(x,y,16,24,26,24,ink);
        symbol_line(x,y,26,24,26,5,ink);draw_round_rect(x+23,y+2,7,6,2,ink);break;
    case ICON_MINES:
        draw_round_rect(x+8,y+8,16,16,8,ink);
        symbol_line(x,y,16,2,16,30,ink);symbol_line(x,y,2,16,30,16,ink);
        symbol_line(x,y,5,5,27,27,ink);symbol_line(x,y,5,27,27,5,ink);break;
    case ICON_BREAKOUT:
        for(int j=0;j<2;j++)for(int i=0;i<3;i++)draw_round_rect(x+2+i*10,y+3+j*7,8,4,1,ink);
        draw_round_rect(x+18,y+18,4,4,2,ink);draw_round_rect(x+7,y+27,18,3,1,ink);break;
    case ICON_TRASH:
        symbol_box(x+7,y+9,18,21,3,ink);draw_rect(x+4,y+6,24,2,ink);
        draw_rect(x+12,y+2,8,2,ink);draw_rect(x+12,y+14,2,10,ink);draw_rect(x+19,y+14,2,10,ink);break;
    default: symbol_box(x+5,y+4,22,24,4,ink);break;
    }
}
static void draw_app_tile(int x, int y, const char *art, uint32_t color, int size) {
    draw_round_rect(x, y, size, size, size / 4, idx24(color));
    int id=0;
    for(int i=0;i<ICON_COUNT;i++)if(icons[i].art==art){id=i;break;}
    draw_app_symbol(id,x+(size-32)/2,y+(size-32)/2,COLOR_WHITE);
}

static void draw_desktop_icons(int selected) {
    for (int i = 0; i < ICON_COUNT; i++) {
        int sel = (i == selected);
        int lx, ly, lw;
        icon_label_pos(i, &lx, &ly, &lw);
        if (sel) {
            int x0 = icons[i].x - 10;
            int y0 = icons[i].y - 6;
            int x1 = lx + lw + 10;
            if (x1 < icons[i].x + TILE_S + 10)
                x1 = icons[i].x + TILE_S + 10;
            if (lx - 10 < x0)
                x0 = lx - 10;
            draw_round_rect(x0, y0, x1 - x0, ly + CHAR_H + 6 - y0, 8, gfx_mix(get_pixel(x0, y0), COLOR_WHITE, 60));
            draw_round_frame(x0, y0, x1 - x0, ly + CHAR_H + 6 - y0, 8, idx24(rgb_lighten(themes[theme_id].desk_top, 55)));
        }
        draw_app_tile(icons[i].x, icons[i].y, icons[i].art, icons[i].color, TILE_S);
        draw_string(icons[i].label, lx, ly, COLOR_WHITE);
    }
}

/* The wallpaper is generated only when the theme changes, then copied from RAM.
 * Broad curved bands use the dedicated 64-color ramp, without per-frame blending. */
static void desktop_render(void) {
    for (int x = 0; x < fb_w; x++) {
        int nx = x * 1024 / fb_w;
        int curve = 190 + (nx - 600) * (nx - 600) / 1400;
        for (int y = 0; y < fb_h; y++) {
            int ny = y * 1024 / fb_h;
            int d = ny - curve;
            int band = d < 0 ? -d : d;
            int light = band < 260 ? (260 - band) / 12 : 0;
            int tone = 23 + ny * 27 / 1024 - light;
            if (tone < 0) tone = 0;
            if (tone >= PAL_DESK_N) tone = PAL_DESK_N - 1;
            fb[y * fb_w + x] = PAL_DESK + tone;
        }
    }
}

void draw_desktop(void) {
    uint8_t *cache = (uint8_t *)DESK_CACHE;
    int n = fb_w * fb_h;
    if (desk_cache_theme != theme_id) {
        desktop_render();
        kmemcpy(cache, fb, n);
        desk_cache_theme = theme_id;
        return;
    }
    kmemcpy(fb, cache, n);
}

/* Menu bar */
enum {
    MENU_NONE = -1,
    MENU_BASEOS = 0,
    MENU_FILE,
    MENU_EDITM,
    MENU_SPECIAL,
    MENU_N
};

static const char *bar_name[MENU_N] = {"BaseOS", "File", "Edit", "System"};
static int bar_x[MENU_N];
static int bar_w[MENU_N];

static const char *m_baseos[] = {"About BaseOS", "Search...", "Settings", "Help"};
static const char *m_file[] = {"New", "New Folder", "Open", "Close", "Save", "Duplicate", "Properties"};
static const char *m_edit[] = {"Cut", "Copy", "Paste"};
static const char *m_special[] = {"Empty Trash", "Show Desktop", "Screen Saver", "-", "Shutdown"};

static const char **menu_items[MENU_N] = {m_baseos, m_file, m_edit, m_special};
static const int menu_count[MENU_N] = {4, 7, 3, 5};

static int menu_item_enabled(int m, int item);
static int front_kind(void);
static void launcher_open(void);
static void saver_start(void);
static void show_desktop(void);
static void sysinfo_fill(SysInfo *si);
static void draw_logo(int x, int y, int size);
static void draw_mini_doc(int x, int y, uint8_t fg, uint8_t bg);
static void icon_open(int id);
static void open_fs_file(int id);
static void draw_icon_art(int x, int y, const char *art, int scale, uint8_t ink, int fill);
static int snake_is_front(void);
static int wordle_is_front(void);
static int clip_is_number(void);
static void open_calc(void);
static void calc_copy(void);
static void calc_paste(void);
static void draw_calc(int wx, int wy, int ww, int wh, int inactive);
static void handle_calc_click(int wx, int wy, int ww, int wh);
static void fm_go_up(void);
static int fm_row_id(int row);
static void fm_rename_cancel(void);
static void theme_load(void);
static void draw_about(int wx, int wy, int ww, int wh, int fl);
static void draw_settings(int wx, int wy, int ww, int wh, int fl);
static void handle_settings_click(int wx, int wy, int ww, int wh);
static void theme_set(int id);
static void saver_save(void);
static void boot_splash(void);
static void start_open_dialog(int pics_only);

/* File item 0 reads "New Game" while Snake or Wordle is the front window. */
static const char *menu_label(int m, int i) {
    if (m == MENU_FILE && i == 0 && (snake_is_front() || wordle_is_front()))
        return "New Game";
    return menu_items[m][i];
}

static void menu_bar_init(void) {
    int x = 16; /* 16px left inset */
    for (int i = 0; i < MENU_N; i++) {
        bar_x[i] = x;
        bar_w[i] = (i == 0 ? uib_string_w(bar_name[i]) : ui_string_w(bar_name[i])) + 20;
        x += bar_w[i];
    }
}

static int menu_pulldown_w(int m) {
    int w = 80;
    for (int i = 0; i < menu_count[m]; i++) {
        int tw = ui_string_w(menu_label(m, i)) + 24;
        if (tw > w)
            w = tw;
    }
    return w;
}

static void menu_geom(int m, int *x, int *y, int *w, int *h) {
    *x = bar_x[m];
    *y = MENUBAR_H - 1;
    *w = menu_pulldown_w(m);
    *h = 8;
    for (int i = 0; i < menu_count[m]; i++)
        *h += (menu_label(m, i)[0] == '-') ? 9 : MENU_ROW;
}

static int menu_item_at(int m, int px, int py) {
    int x, y, w, h;
    menu_geom(m, &x, &y, &w, &h);
    if (!hit(px, py, x, y, w, h))
        return -1;
    int iy = y + 4;
    for (int i = 0; i < menu_count[m]; i++) {
        int rh = (menu_label(m, i)[0] == '-') ? 9 : MENU_ROW;
        if (hit(px, py, x, iy, w, rh) && menu_label(m, i)[0] != '-')
            return i;
        iy += rh;
    }
    return -1;
}

static uint32_t frame_count = 0;

static void draw_menubar(int open_menu, int menu_sel) {
    (void)menu_sel;
    draw_rect(0, 0, fb_w, MENUBAR_H, ui_chrome);
    draw_hline(0, MENUBAR_H - 1, fb_w, ui_chrome_dk);

    int label_y = (MENUBAR_H - CHAR_H) / 2;
    for (int i = 0; i < MENU_N; i++) {
        int inv = (open_menu == i);
        if (inv) {
            draw_round_rect(bar_x[i], 3, bar_w[i], MENUBAR_H - 7, 5, ui_accent);
            draw_string(bar_name[i], bar_x[i] + 10, label_y, COLOR_WHITE);
        } else {
            if (i == 0)
                draw_string_bold(bar_name[i], bar_x[i] + 10, label_y, ui_accent_dk);
            else
                draw_string(bar_name[i], bar_x[i] + 10, label_y, ui_text);
        }
    }

    RtcTime now;
    rtc_read(&now);
    char clk[40];
    int n = 0;
    static const char *dshort[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    const char *d = dshort[now.wday];
    while (*d) clk[n++] = *d++;
    clk[n++] = ' ';
    const char *mn = rtc_month_name[now.month - 1];
    for (int i = 0; i < 3 && mn[i]; i++) clk[n++] = mn[i];
    clk[n++] = ' ';
    char t[4];
    fmt_uint(t, (unsigned)now.day);
    for (int i = 0; t[i]; i++) clk[n++] = t[i];
    clk[n++] = ' ';
    clk[n++] = ' ';
    fmt_pad2(t, now.hour);
    clk[n++] = t[0]; clk[n++] = t[1];
    clk[n++] = ':';
    fmt_pad2(t, now.min);
    clk[n++] = t[0]; clk[n++] = t[1];
    clk[n] = 0;
    int clock_x = fb_w - ui_string_w(clk) - 18;
    draw_string(clk, clock_x, label_y, ui_text);
    const char *storage = fs_storage_status();
    if(!storage && session_status[0])storage=session_status;
    if (storage)
        draw_string_clip(storage, bar_x[MENU_N - 1] + bar_w[MENU_N - 1] + 12,
                         label_y, COLOR_RED, clock_x - 10);
}

static void draw_pulldown(int open_menu, int menu_sel) {
    if (open_menu < 0)
        return;

    int x, y, w, h;
    menu_geom(open_menu, &x, &y, &w, &h);
    draw_shadow(x, y, w, h);
    draw_round_rect(x, y, w, h, 6, ui_border);
    draw_round_rect(x + 1, y + 1, w - 2, h - 2, 5, COLOR_WHITE);

    int iy = y + 4;
    for (int i = 0; i < menu_count[open_menu]; i++) {
        const char *it = menu_label(open_menu, i);
        if (it[0] == '-') {
            draw_hline(x + 8, iy + 4, w - 16, ui_chrome_dk);
            iy += 9;
            continue;
        }
        int enabled = menu_item_enabled(open_menu, i);
        int inv = (i == menu_sel) && enabled;
        if (inv)
            draw_round_rect(x + 4, iy, w - 8, MENU_ROW, 4, ui_accent);
        int ty = iy + (MENU_ROW - CHAR_H) / 2;
        uint8_t fg = inv ? COLOR_WHITE : (enabled ? ui_text : ui_text_dim);
        draw_string(it, x + 14, ty, fg);
        iy += MENU_ROW;
    }
}

/* ---------- PS/2 mouse (polled) ---------- */

static int mouse_x;
static int mouse_y;
static int mouse_left = 0;
static int mouse_left_prev = 0;
static int mouse_clicked = 0;
static int mouse_right = 0;
static int mouse_rclicked = 0;
static int mouse_moved = 0;
static int mouse_ok = 0;

static uint8_t mouse_pkt[3];
static int mouse_pkt_n = 0;

static uint8_t cursor_saved[CURSOR_W * CURSOR_H];
static int cursor_sx = -1, cursor_sy = -1;
static int cursor_on = 0;


static int mouse_wait_write(void) {
    for (int i = 0; i < 100000; i++) {
        if ((inb(0x64) & 2) == 0)
            return 1;
    }
    return 0;
}

static int mouse_wait_read(void) {
    for (int i = 0; i < 100000; i++) {
        if (inb(0x64) & 1)
            return 1;
    }
    return 0;
}

static void mouse_flush(void) {
    for (int i = 0; i < 32; i++) {
        if (!(inb(0x64) & 1))
            break;
        (void)inb(0x60);
    }
}

static int mouse_cmd(uint8_t cmd) {
    if (!mouse_wait_write())
        return 0;
    outb(0x64, 0xD4);
    if (!mouse_wait_write())
        return 0;
    outb(0x60, cmd);
    if (!mouse_wait_read())
        return 0;
    uint8_t ack = inb(0x60);
    return ack == 0xFA;
}

void mouse_init(void) {
    mouse_flush();

    if (!mouse_wait_write())
        return;
    outb(0x64, 0xA8);

    if (!mouse_wait_write())
        return;
    outb(0x64, 0x20);
    if (!mouse_wait_read())
        return;
    uint8_t status = inb(0x60);
    status &= (uint8_t)~0x03; /* Both PS/2 IRQs stay masked; input is polled. */
    status &= (uint8_t)~0x20;
    if (!mouse_wait_write())
        return;
    outb(0x64, 0x60);
    if (!mouse_wait_write())
        return;
    outb(0x60, status);

    if (!mouse_cmd(0xF6))
        return;
    if (!mouse_cmd(0xF4))
        return;

    mouse_flush();
    mouse_ok = 1;
    mouse_pkt_n = 0;
}

static void mouse_handle_byte(uint8_t b) {
    if (mouse_pkt_n == 0 && !(b & 0x08))
        return;

    mouse_pkt[mouse_pkt_n++] = b;
    if (mouse_pkt_n < 3)
        return;
    mouse_pkt_n = 0;

    uint8_t flags = mouse_pkt[0];
    if (flags & 0xC0)
        return;

    int16_t dx = mouse_pkt[1];
    int16_t dy = mouse_pkt[2];
    if (flags & 0x10)
        dx |= (int16_t)0xFF00;
    if (flags & 0x20)
        dy |= (int16_t)0xFF00;

    int nx = mouse_x + dx;
    int ny = mouse_y - dy;
    if (nx < 0)
        nx = 0;
    if (nx > fb_w - 1)
        nx = fb_w - 1;
    if (ny < 0)
        ny = 0;
    if (ny > fb_h - 1)
        ny = fb_h - 1;

    if (nx != mouse_x || ny != mouse_y)
        mouse_moved = 1;
    mouse_x = nx;
    mouse_y = ny;

    mouse_left_prev = mouse_left;
    mouse_left = flags & 0x01;
    if (mouse_left && !mouse_left_prev)
        mouse_clicked = 1;
    int right = flags & 0x02;
    if (right && !mouse_right)
        mouse_rclicked = 1;
    mouse_right = right;
}

void cursor_restore(void) {
    if (!cursor_on)
        return;
    for (int j = 0; j < CURSOR_H; j++) {
        for (int i = 0; i < CURSOR_W; i++) {
            put_pixel(cursor_sx + i, cursor_sy + j,
                      cursor_saved[j * CURSOR_W + i]);
        }
    }
    cursor_on = 0;
}

void cursor_save_draw(void) {
    cursor_sx = mouse_x;
    cursor_sy = mouse_y;
    for (int j = 0; j < CURSOR_H; j++) {
        for (int i = 0; i < CURSOR_W; i++) {
            int px = cursor_sx + i, py = cursor_sy + j;
            uint8_t bg = get_pixel(px, py);
            cursor_saved[j * CURSOR_W + i] = bg;
            int oa = cursor_outline_a[j][i];
            int fa = cursor_fill_a[j][i];
            if (!oa && !fa)
                continue;
            uint8_t c = bg;
            if (oa)
                c = gfx_mix(c, COLOR_BLACK, oa * 16 + oa / 2);
            if (fa)
                c = gfx_mix(c, COLOR_WHITE, fa * 16 + fa / 2);
            put_pixel(px, py, c);
        }
    }
    cursor_on = 1;
}

/* ---------- Keyboard (polled) ---------- */

static int shift_down = 0;
static int alt_down;
static int ctrl_down = 0;
static int key_pressed = 0;
static uint8_t key_sc = 0;
static char key_char = 0;

static const char keymap[0x40] = {
    0,   0,   '1', '2', '3', '4', '5', '6',
    '7', '8', '9', '0', '-', '=', 0,   0,
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i',
    'o', 'p', '[', ']', 0,   0,   'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',
    '\'', '`', 0,  '\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/', 0,   '*',
    0,   ' ', 0,   0,   0,   0,   0,   0};

static const char keymap_shift[0x40] = {
    0,   0,   '!', '@', '#', '$', '%', '^',
    '&', '*', '(', ')', '_', '+', 0,   0,
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I',
    'O', 'P', '{', '}', 0,   0,   'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',
    '"', '~', 0,  '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?', 0,   '*',
    0,   ' ', 0,   0,   0,   0,   0,   0};

static void keyboard_handle_byte(uint8_t sc) {
    if (sc == 0xE0)
        return;

    if (sc & 0x80) {
        uint8_t make = sc & 0x7F;
        if (make == KEY_LSHIFT || make == KEY_RSHIFT)
            shift_down = 0;
        if (make == 0x38) alt_down = 0;
        if (make == KEY_LCTRL)
            ctrl_down = 0;
        return;
    }

    if (sc == KEY_LSHIFT || sc == KEY_RSHIFT) {
        shift_down = 1;
        return;
    }
    if (sc == 0x38) { alt_down = 1; return; }
    if (sc == KEY_LCTRL) {
        ctrl_down = 1;
        return;
    }

    key_sc = sc;
    key_pressed = 1;
    if (sc < 0x40) {
        key_char = shift_down ? keymap_shift[sc] : keymap[sc];
    } else {
        key_char = 0;
    }
}

static uint8_t kq[1024];
static int input_ready;
void platform_poll(void){if(input_ready)drain_8042();}
static int kqn;

void drain_8042(void) {
    for (int i = 0; i < 32; i++) {
        uint8_t status = inb(KEYBOARD_STATUS_PORT);
        if ((status & 1) == 0)
            break;
        uint8_t data = inb(KEYBOARD_DATA_PORT);
        if (status & 0x20)
            mouse_handle_byte(data);
        else if (kqn < (int)sizeof kq)
            kq[kqn++] = data;
    }
}

static void poll_time(void) {
    frame_count = timer_ticks();
}

/* ---------- Video init ---------- */
static void video_init(void) {
    const BootInfo *bi = (const BootInfo *)BOOTINFO_ADDR;
    if (!video_info_valid(bi))
        panic("no supported, verified linear framebuffer");
    fb_w = bi->width;
    fb_h = bi->height;
    fb_bpp = bi->bpp;
    fb_pitch = bi->pitch;
    lfb = (uint8_t *)bi->lfb;
    gfx_init((uint8_t *)FB_BASE, lfb, fb_w, fb_h, fb_bpp, fb_pitch);
    gfx_set_flip_hook(drain_8042);

    kprint_debug("Video ");
    kprint_uint((unsigned)fb_w);
    serial_write('x');
    kprint_uint((unsigned)fb_h);
    serial_write('x');
    kprint_uint((unsigned)fb_bpp);
    kprint_debug(" pitch=");
    kprint_uint((unsigned)fb_pitch);
    kprint_debug(" LFB=");
    kprint_hex32((uint32_t)(uintptr_t)lfb);
    kprint_debug(" flags=");
    kprint_uint(bi->magic == BOOTINFO_MAGIC ? bi->flags : 0);
    serial_write('\n');
}

/* ---------- Window manager ---------- */

enum WinKind {
    WK_NONE = -1,
    WK_HELLO = 0,
    WK_HELP,
    WK_ABOUT,
    WK_SETTINGS,
    WK_FILES,
    WK_EDIT,
    WK_CALC,
    WK_PAINT,
    WK_VIEW,
    WK_SNAKE,
    WK_WORDLE,
    WK_TERM,
    WK_TODO,
    WK_CLOCK,
    WK_CAL,
    WK_MINES,
    WK_2048,
    WK_BREAKOUT,
    WK_SYSMON,
    WK_PROPERTIES
};

/* Max 8 windows: kind, x, y, w, h, z. seq is taskbar creation order. */
typedef struct {
    int kind;
    int x, y, w, h;
    int z;
    int open;
    int seq;
    int min;
    int maximized, old_x, old_y, old_w, old_h;
} Win;

static Win wins[MAX_WIN];
static int wm_z;
static int wm_seq;
static int dragging_win = -1;
static int drag_active;
static int drag_from_x;
static int drag_from_y;
static int drag_orig_x;
static int drag_orig_y;

static int tb_x[MAX_WIN];
static int tb_w[MAX_WIN];
static int tb_id[MAX_WIN];
static int tb_n;

static int dirty = 1;
static int open_menu = MENU_NONE;
static int menu_sel = -1;
static int icon_sel = -1;
static int trash_id = -1;
static int prefs_id = -1;
static int properties_id=-1;

typedef struct {
    char buf[EDIT_BUF_SIZE];
    int len;
    int caret;
    int file;
    unsigned identity;
    int saved_ok;
    int scroll;
    int sel_a;
    int sel_b;
    int dragging;
} Document;
typedef struct {
    int cwd;
    int selected;
    int first;
    int ids[FS_MAX_NODES];
    int count;
    uint32_t last_click_frame;
    int last_click_item;
    Document doc, undo[8];
    History history;
} WindowState;
static WindowState *window_state = (WindowState *)APPS_BASE;
static int context_slot;
static void context_set(int slot) { if (slot >= 0 && slot < MAX_WIN) { context_slot = slot; term_select(slot); } }
_Static_assert(sizeof(WindowState) * MAX_WIN + 8 * 16000 < 0x300000, "window arena overflow");
#define fm_cwd (window_state[context_slot].cwd)
#define fm_selected (window_state[context_slot].selected)
#define fm_first (window_state[context_slot].first)
#define fm_ids (window_state[context_slot].ids)
#define fm_count (window_state[context_slot].count)
#define fm_last_click_frame (window_state[context_slot].last_click_frame)
#define fm_last_click_item (window_state[context_slot].last_click_item)
#define edit_buf (window_state[context_slot].doc.buf)
#define edit_len (window_state[context_slot].doc.len)
#define edit_caret (window_state[context_slot].doc.caret)
#define edit_file (window_state[context_slot].doc.file)
#define edit_identity (window_state[context_slot].doc.identity)
#define edit_saved_ok (window_state[context_slot].doc.saved_ok)
#define edit_scroll (window_state[context_slot].doc.scroll)
#define edit_sel_a (window_state[context_slot].doc.sel_a)
#define edit_sel_b (window_state[context_slot].doc.sel_b)
#define edit_dragging (window_state[context_slot].doc.dragging)
static void edit_record(void) { history_record(&window_state[context_slot].history, &window_state[context_slot].doc); }
static void edit_undo(int redo) {
    int file=edit_file;unsigned identity=edit_identity;
    if (history_step(&window_state[context_slot].history, &window_state[context_slot].doc, redo)) {
        edit_file=file;edit_identity=identity;edit_saved_ok = 0; edit_dragging = 0; dirty = 1;
    }
}
static void session_save(void);
static void session_restore(void);
static int resizing_win = -1, resize_start_w, resize_start_h, resize_edges;
static uint32_t title_click_time;
static int title_click_window = -1;

static char clip_buf[EDIT_BUF_SIZE];
static int clip_len = 0;

#define TERM_WIN_W   640
#define TERM_WIN_H   400
#define TERM_PAD     12
#define TERM_FG      gfx_gray(0xE8)

#define CALC_W       280
#define CALC_H       408
#define CALC_MARGIN  16
#define CALC_DISP_H  36
#define CALC_KEY_W   56
#define CALC_KEY_H   50
#define CALC_GAP     8
#define CALC_DIGITS  12
#define CALC_SCALE   1000

static char calc_entry[20];
static int calc_entry_len;
static int calc_has_dot;
static int calc_fresh;
static int calc_error;
static int calc_acc;
static int calc_op;

typedef struct {
    char ch;
    int row;
    int col;
} CalcKey;

static const CalcKey calc_keys[] = {
    {'C', 0, 0},
    {'7', 1, 0}, {'8', 1, 1}, {'9', 1, 2}, {'/', 1, 3},
    {'4', 2, 0}, {'5', 2, 1}, {'6', 2, 2}, {'*', 2, 3},
    {'1', 3, 0}, {'2', 3, 1}, {'3', 3, 2}, {'-', 3, 3},
    {'0', 4, 0}, {'.', 4, 1}, {'=', 4, 2}, {'+', 4, 3},
};
#define CALC_NKEYS ((int)(sizeof(calc_keys) / sizeof(calc_keys[0])))

static int open_dlg = 0;
static int name_dlg = 0;
static int pick_cwd = 0;
static int pick_focus, pick_first;
static int pick_ids[FS_MAX_NODES];
static int pick_count = 0;
static int pick_selected = 0;
static uint32_t pick_last_click_frame = 0;
static int pick_last_click_item = -1;
static int pick_pics_only = 0;

static uint32_t icon_last_frame = 0;
static int icon_last = -1;

static int fm_dragging = 0;
static int fm_drag_active = 0;
static int fm_drag_id = -1;
static int fm_drag_sx = 0;
static int fm_drag_sy = 0;

static int fm_renaming = 0;
static int fm_rename_id = -1;
static char fm_rename_buf[FS_NAME_LEN];
static int fm_rename_len = 0;

static void do_shutdown(void) {
    session_save();
    if(session_status[0]){dirty=1;return;}
    if (fs_sync() < 0) {
        dirty = 1;
        return; /* Keep unsaved work available after a failed flush. */
    }
    outw(QEMU_SHUTDOWN_PORT, 0x2000);
    outw(VBOX_SHUTDOWN_PORT, 0x2000);
    asm volatile("hlt");
}

static int find_open_kind(int kind) {
    if (wins[context_slot].open && wins[context_slot].kind == kind) return context_slot;
    int best = -1;
    for (int i = 0; i < MAX_WIN; i++)
        if (wins[i].open && wins[i].kind == kind && (best < 0 || wins[i].z > wins[best].z)) best = i;
    return best;
}

static int win_front(void) {
    int best = -1;
    int bz = -1;
    for (int i = 0; i < MAX_WIN; i++) {
        if (wins[i].open && !wins[i].min && wins[i].z > bz) {
            bz = wins[i].z;
            best = i;
        }
    }
    return best;
}

static int front_kind(void) {
    int i = win_front();
    return i < 0 ? WK_NONE : wins[i].kind;
}

static int snake_is_front(void) {
    return front_kind() == WK_SNAKE;
}

static int wordle_is_front(void) {
    return front_kind() == WK_WORDLE;
}

static void win_focus(int i) {
    if (i < 0 || i >= MAX_WIN || !wins[i].open)
        return;
    if(context_slot!=i)fm_rename_cancel();
    context_set(i);
    wins[i].min = 0;
    wins[i].z = ++wm_z;
    dirty = 1;
}

static void win_minimize(int i) {
    if (i < 0 || i >= MAX_WIN || !wins[i].open)
        return;
    wins[i].min = 1;
    dirty = 1;
}

static void win_cycle(void) {
    int best = -1;
    int bz = 0x7FFFFFFF;
    for (int i = 0; i < MAX_WIN; i++) {
        if (wins[i].open && wins[i].z < bz) {
            bz = wins[i].z;
            best = i;
        }
    }
    if (best >= 0)
        win_focus(best);
}

static void show_desktop(void) {
    for (int i = 0; i < MAX_WIN; i++)
        if (wins[i].open)
            wins[i].min = 1;
    dirty = 1;
}

static void layout_window(int kind, int *x, int *y, int *w, int *h);

static void win_minimum(Win *w, int *mw, int *mh) {
    if (w->kind == WK_EDIT || w->kind == WK_FILES || w->kind == WK_TERM) { *mw = 360; *mh = 200; }
    else { int x, y; layout_window(w->kind, &x, &y, mw, mh); }
}
static void win_clamp(Win *w) {
    int mw, mh; win_minimum(w, &mw, &mh);
    if (w->w < mw) w->w = mw;
    if (w->h < mh) w->h = mh;
    if (w->w > fb_w - 4) w->w = fb_w - 4;
    if (w->h > TASKBAR_Y - MENUBAR_H - 4) w->h = TASKBAR_Y - MENUBAR_H - 4;
    if (w->x < 2) w->x = 2;
    if (w->y < MENUBAR_H + 2) w->y = MENUBAR_H + 2;
    if (w->x + w->w > fb_w - 2) w->x = fb_w - 2 - w->w;
    if (w->y + w->h > TASKBAR_Y - 2) w->y = TASKBAR_Y - 2 - w->h;
}
static void win_arrange(int i, int mode) {
    if (i < 0) return;
    Win *w = &wins[i];
    if (!w->maximized) { w->old_x=w->x; w->old_y=w->y; w->old_w=w->w; w->old_h=w->h; }
    if (!mode && w->maximized) {
        w->x=w->old_x; w->y=w->old_y; w->w=w->old_w; w->h=w->old_h; w->maximized=0;
    } else {
        w->maximized=1;
        w->x = mode == 2 ? fb_w / 2 : 2;
        w->y = MENUBAR_H + 2;
        w->w = mode ? fb_w / 2 - 4 : fb_w - 4;
        w->h = TASKBAR_Y - MENUBAR_H - 4;
    }
    win_clamp(w); dirty=1;
}

static void win_resize_tick(void) {
        if (resizing_win >= 0 && mouse_left) {
            Win *rw=&wins[resizing_win];
            int dx=mouse_x-drag_from_x, dy=mouse_y-drag_from_y;
            rw->w=resize_start_w+((resize_edges&1)?-dx:(resize_edges&2)?dx:0);
            rw->h=resize_start_h+((resize_edges&4)?-dy:(resize_edges&8)?dy:0);
            int mw,mh;win_minimum(rw,&mw,&mh);
            if(rw->w<mw)rw->w=mw;
            if(rw->h<mh)rw->h=mh;
            rw->x=(resize_edges&1)?drag_orig_x+resize_start_w-rw->w:drag_orig_x;
            rw->y=(resize_edges&4)?drag_orig_y+resize_start_h-rw->h:drag_orig_y;
            win_clamp(rw);dirty=1;
        }
}

static int win_open(int kind) {
    int i = find_open_kind(kind);
    if (i >= 0 && kind != WK_EDIT && kind != WK_FILES && kind != WK_TERM) {
        win_focus(i);
        return i;
    }
    int slot = -1;
    for (int j = 0; j < MAX_WIN; j++) {
        if (!wins[j].open) {
            slot = j;
            break;
        }
    }
    if (slot < 0)
        return -1;
    kmemset(&wins[slot], 0, sizeof(Win));
    kmemset(&window_state[slot], 0, sizeof(WindowState));
    context_set(slot);
    edit_file = -1;
    fm_last_click_item = -1;
    window_state[slot].history = (History){0, 0, 8, sizeof(Document), (unsigned char *)window_state[slot].undo};
    if (kind == WK_TERM) term_reset();
    wins[slot].kind = kind;
    wins[slot].open = 1;
    wins[slot].min = 0;
    wins[slot].seq = ++wm_seq;
    layout_window(kind, &wins[slot].x, &wins[slot].y, &wins[slot].w, &wins[slot].h);
    wins[slot].x += slot * 18; wins[slot].y += slot * 14;
    win_clamp(&wins[slot]);
    win_focus(slot);
    return slot;
}

static void win_close(int i) {
    if (i < 0 || i >= MAX_WIN || !wins[i].open)
        return;
    wins[i].open = 0;
    context_set(win_front());
    if (dragging_win == i) {
        dragging_win = -1;
        drag_active = 0;
    }
    dirty = 1;
}

static void wins_by_z(int *order, int *n, int front_first) {
    *n = 0;
    for (int i = 0; i < MAX_WIN; i++) {
        if (wins[i].open && !wins[i].min)
            order[(*n)++] = i;
    }
    for (int a = 0; a < *n; a++) {
        for (int b = a + 1; b < *n; b++) {
            int za = wins[order[a]].z;
            int zb = wins[order[b]].z;
            int swap = front_first ? (zb > za) : (zb < za);
            if (swap) {
                int t = order[a];
                order[a] = order[b];
                order[b] = t;
            }
        }
    }
}

static int win_geom_kind(int kind, int *x, int *y, int *w, int *h) {
    int i = find_open_kind(kind);
    if (i < 0)
        return 0;
    *x = wins[i].x;
    *y = wins[i].y;
    *w = wins[i].w;
    *h = wins[i].h;
    return 1;
}

static int menu_item_enabled(int m, int item) {
    if (m == MENU_EDITM) {
        /* Files keeps Cut/Copy/Paste dim. Calc: Cut dim, Copy/Paste as below. */
        if (open_dlg)
            return 0;
        if (front_kind() == WK_CALC) {
            if (item == 0) /* Cut */
                return 0;
            if (item == 1) /* Copy */
                return !calc_error && calc_entry_len > 0;
            if (item == 2) /* Paste */
                return clip_is_number();
            return 0;
        }
        if (front_kind() != WK_EDIT)
            return 0;
        if (item == 0 || item == 1) /* Cut, Copy */
            return edit_sel_a != edit_sel_b;
        if (item == 2) /* Paste */
            return clip_len > 0;
        return 0;
    }
    if (m == MENU_FILE) {
        if(item==6)return !open_dlg && front_kind()==WK_FILES && fm_row_id(fm_selected)>=0;
        if (!open_dlg && (snake_is_front() || wordle_is_front()))
            return item == 0 || item == 3; /* New Game, Close */
        if (item == 1) {
            if (open_dlg)
                return 0;
            return front_kind() == WK_FILES;
        }
        if (item == 4) { /* Save */
            int fk = front_kind();
            return !open_dlg && (fk == WK_EDIT || fk == WK_PAINT);
        }
        if (item == 5) { /* Duplicate: Files + selected file/folder, not an app. */
            int id;
            if (open_dlg)
                return 0;
            if (front_kind() != WK_FILES)
                return 0;
            if (fm_count <= 0)
                return 0;
            id = fm_row_id(fm_selected);
            if (id < 0)
                return 0;
            if (fs_is_app(id) || kstrcmp(fs_name(id), "Calculator") == 0)
                return 0;
            return 1;
        }
        return 1;
    }
    if (m == MENU_SPECIAL) {
        if (item == 0) /* Empty Trash */
            return trash_id >= 0 && fs_child_count(trash_id) > 0;
        return 1;
    }
    (void)item;
    return 1;
}

static void close_front(void) {
    int i = win_front();
    if (i >= 0)
        win_close(i);
    open_dlg = 0;
    edit_dragging = 0;
    fm_dragging = 0;
    fm_drag_active = 0;
    fm_drag_id = -1;
    fm_rename_cancel();
    open_menu = MENU_NONE;
    dirty = 1;
}

static int fm_has_parent(void) {
    return fm_cwd != fs_root() && fs_parent(fm_cwd) >= 0;
}

static int fm_vis_count(void) {
    return fm_count + (fm_has_parent() ? 1 : 0);
}

static int fm_row_id(int row) {
    int up = fm_has_parent() ? 1 : 0;
    if (up && row == 0)
        return -1; /* synthetic ".." */
    int i = row - up;
    if (i < 0 || i >= fm_count)
        return -2;
    return fm_ids[i];
}

static void fm_refresh(void) {
    if(!fs_is_dir(fm_cwd))fm_cwd=fs_root();
    int raw[FS_MAX_NODES];
    int n = fs_list(fm_cwd, raw, FS_MAX_NODES);
    fm_count = 0;
    for (int i = 0; i < n; i++) {
        /* Desktop Trash is the trash; hide /trash and /prefs at root. */
        if (fm_cwd == fs_root() && trash_id >= 0 && raw[i] == trash_id)
            continue;
        if (fm_cwd == fs_root() && prefs_id >= 0 && raw[i] == prefs_id)
            continue;
        fm_ids[fm_count++] = raw[i];
    }
    int vis = fm_vis_count();
    if (fm_selected >= vis)
        fm_selected = vis ? vis - 1 : 0;
    if (fm_selected < 0)
        fm_selected = 0;
}

static void edit_sel_collapse(void) {
    edit_sel_a = edit_sel_b = edit_caret;
}

static void edit_delete_sel(void) {
    int lo = edit_sel_a < edit_sel_b ? edit_sel_a : edit_sel_b;
    int hi = edit_sel_a > edit_sel_b ? edit_sel_a : edit_sel_b;
    if (lo == hi)
        return;
    int n = hi - lo;
    for (int i = lo; i <= edit_len - n; i++)
        edit_buf[i] = edit_buf[i + n];
    edit_len -= n;
    edit_buf[edit_len] = 0;
    edit_caret = lo;
    edit_sel_collapse();
    edit_saved_ok = 0;
}

static void edit_copy(void) {
    int lo = edit_sel_a < edit_sel_b ? edit_sel_a : edit_sel_b;
    int hi = edit_sel_a > edit_sel_b ? edit_sel_a : edit_sel_b;
    if (lo == hi)
        return;
    clip_len = hi - lo;
    if (clip_len > EDIT_BUF_SIZE - 1)
        clip_len = EDIT_BUF_SIZE - 1;
    kmemcpy(clip_buf, edit_buf + lo, clip_len);
    clip_buf[clip_len] = 0;
}

static void edit_cut(void) {
    edit_record();
    edit_copy();
    edit_delete_sel();
}

static void edit_paste(void) {
    edit_record();
    if (clip_len <= 0)
        return;
    if (edit_sel_a != edit_sel_b)
        edit_delete_sel();
    int n = clip_len;
    if (edit_len + n > EDIT_BUF_SIZE - 1)
        n = EDIT_BUF_SIZE - 1 - edit_len;
    if (n <= 0)
        return;
    for (int i = edit_len - 1; i >= edit_caret; i--)
        edit_buf[i + n] = edit_buf[i];
    kmemcpy(edit_buf + edit_caret, clip_buf, n);
    edit_len += n;
    edit_caret += n;
    edit_buf[edit_len] = 0;
    edit_sel_collapse();
    edit_saved_ok = 0;
}

static void edit_clear(void) {
    edit_len = 0;
    edit_caret = 0;
    edit_buf[0] = 0;
    edit_file = -1;
    edit_saved_ok = 0;
    edit_scroll = 0;
    edit_dragging = 0;
    edit_sel_collapse();
}

static void edit_load(int id) {
    if (!fs_valid(id) || fs_is_dir(id) || fs_is_app(id))
        return;
    edit_len = fs_read(id, edit_buf, EDIT_BUF_SIZE);
    if (edit_len < 0)
        edit_len = 0;
    edit_buf[edit_len] = 0;
    edit_caret = edit_len;
    edit_file = id;
    edit_identity=fs_identity(id);
    edit_saved_ok = 1;
    edit_scroll = 0;
    edit_dragging = 0;
    edit_sel_collapse();
}

static void edit_insert(char c) {
    edit_record();
    if (edit_sel_a != edit_sel_b)
        edit_delete_sel();
    if (edit_len >= EDIT_BUF_SIZE - 1)
        return;
    for (int i = edit_len; i > edit_caret; i--)
        edit_buf[i] = edit_buf[i - 1];
    edit_buf[edit_caret++] = c;
    edit_len++;
    edit_buf[edit_len] = 0;
    edit_sel_collapse();
    edit_saved_ok = 0;
}

static void edit_backspace(void) {
    edit_record();
    if (edit_sel_a != edit_sel_b) {
        edit_delete_sel();
        return;
    }
    if (edit_caret <= 0)
        return;
    for (int i = edit_caret - 1; i < edit_len; i++)
        edit_buf[i] = edit_buf[i + 1];
    edit_caret--;
    edit_len--;
    edit_sel_collapse();
    edit_saved_ok = 0;
}

static void namedlg_open(int target, const char *initial);

static int edit_write_named(const char *name) {
    int parent = fs_valid(fm_cwd) ? fm_cwd : fs_root();
    int id = fs_find_child(parent, name);
    if (id < 0)
        id = fs_create(parent, name);
    if (id < 0)
        return 0;
    if (fs_write(id, edit_buf, edit_len) < 0)
        return 0;
    edit_file = id;
    edit_identity=fs_identity(id);
    edit_saved_ok = 1;
    if (find_open_kind(WK_FILES) >= 0)
        fm_refresh();
    return 1;
}

static int edit_save(void) {
    if(edit_file>=0 && fs_identity(edit_file)!=edit_identity)edit_file=-1;
    if (edit_file < 0) {
        namedlg_open(0, "untitled.txt");
        return 1;
    }
    if (fs_write(edit_file, edit_buf, edit_len) < 0)
        return 0;
    edit_saved_ok = 1;
    return 1;
}

static void open_files(int cwd) {
    if (win_open(WK_FILES) < 0) return;
    fm_cwd = cwd;
    fm_selected = 0;
    fm_refresh();
    dirty = 1;
}

static void open_edit(void) {
    if (win_open(WK_EDIT) < 0) return;
    edit_clear();
    dirty = 1;
}

static int clip_is_number(void) {
    if (clip_len <= 0)
        return 0;
    int i = 0;
    int dots = 0;
    int digits = 0;
    if (clip_buf[0] == '-')
        i = 1;
    if (i >= clip_len)
        return 0;
    for (; i < clip_len; i++) {
        char c = clip_buf[i];
        if (c == '.') {
            dots++;
            if (dots > 1)
                return 0;
        } else if (c >= '0' && c <= '9') {
            digits++;
        } else {
            return 0;
        }
    }
    return digits > 0;
}

static void calc_reset(void) {
    calc_entry[0] = '0';
    calc_entry[1] = 0;
    calc_entry_len = 1;
    calc_has_dot = 0;
    calc_fresh = 1;
    calc_error = 0;
    calc_acc = 0;
    calc_op = 0;
}

static void open_calc(void) {
    open_dlg = 0;
    if (find_open_kind(WK_CALC) < 0)
        calc_reset();
    win_open(WK_CALC);
    dirty = 1;
}

static int calc_digit_count(void) {
    int n = 0;
    for (int i = 0; i < calc_entry_len; i++) {
        if (calc_entry[i] >= '0' && calc_entry[i] <= '9')
            n++;
    }
    return n;
}

static int calc_parse(const char *s) {
    int neg = 0;
    int i = 0;
    if (s[0] == '-') {
        neg = 1;
        i = 1;
    }
    int ip = 0;
    while (s[i] >= '0' && s[i] <= '9') {
        if (ip > 200000000)
            return neg ? -2000000000 : 2000000000;
        ip = ip * 10 + (s[i] - '0');
        i++;
    }
    int frac = 0;
    int fd = 0;
    if (s[i] == '.') {
        i++;
        while (s[i] >= '0' && s[i] <= '9') {
            if (fd < 3)
                frac = frac * 10 + (s[i] - '0');
            fd++;
            i++;
        }
    }
    if (fd > 3)
        fd = 3;
    while (fd < 3) {
        frac *= 10;
        fd++;
    }
    int v = ip * CALC_SCALE + frac;
    return neg ? -v : v;
}

static int calc_mul(int a, int b) {
    /* (a * b) / 1000 in 32-bit, a and b are milles. */
    int neg = 0;
    if (a < 0) { a = -a; neg = !neg; }
    if (b < 0) { b = -b; neg = !neg; }
    int ai = a / CALC_SCALE;
    int af = a % CALC_SCALE;
    int r = ai * b + (af * b) / CALC_SCALE;
    return neg ? -r : r;
}

static int calc_div(int a, int b) {
    /* (a * 1000) / b without a 64-bit divide. */
    int neg = 0;
    if (a < 0) { a = -a; neg = !neg; }
    if (b < 0) { b = -b; neg = !neg; }
    int r = (a / b) * CALC_SCALE + ((a % b) * CALC_SCALE) / b;
    return neg ? -r : r;
}

static void calc_format(int v, char *out) {
    int neg = 0;
    if (v < 0) {
        neg = 1;
        v = -v;
    }
    int ip = v / CALC_SCALE;
    int frac = v % CALC_SCALE;
    char tmp[20];
    int n = 0;
    if (ip == 0) {
        tmp[n++] = '0';
    } else {
        char d[16];
        int nd = 0;
        while (ip && nd < 16) {
            d[nd++] = (char)('0' + (ip % 10));
            ip /= 10;
        }
        while (nd)
            tmp[n++] = d[--nd];
    }
    if (n > CALC_DIGITS) {
        kstrcpy(out, "Error");
        return;
    }
    int pos = 0;
    if (neg)
        out[pos++] = '-';
    for (int i = 0; i < n; i++)
        out[pos++] = tmp[i];
    if (frac) {
        out[pos++] = '.';
        out[pos++] = (char)('0' + (frac / 100));
        int f2 = (frac / 10) % 10;
        int f3 = frac % 10;
        if (f2 || f3)
            out[pos++] = (char)('0' + f2);
        if (f3)
            out[pos++] = (char)('0' + f3);
    }
    out[pos] = 0;
}

static void calc_set_entry_from_acc(int v) {
    calc_format(v, calc_entry);
    if (kstrcmp(calc_entry, "Error") == 0) {
        calc_error = 1;
        calc_entry_len = 5;
        calc_has_dot = 0;
        calc_fresh = 1;
        calc_op = 0;
        return;
    }
    calc_entry_len = kstrlen(calc_entry);
    calc_has_dot = 0;
    for (int i = 0; i < calc_entry_len; i++) {
        if (calc_entry[i] == '.')
            calc_has_dot = 1;
    }
    calc_fresh = 1;
}

static int calc_compute(void) {
    int right = calc_parse(calc_entry);
    int r = 0;
    if (calc_op == '/') {
        if (right == 0) {
            calc_error = 1;
            kstrcpy(calc_entry, "Error");
            calc_entry_len = 5;
            calc_has_dot = 0;
            calc_fresh = 1;
            calc_op = 0;
            return 0;
        }
        r = calc_div(calc_acc, right);
    } else if (calc_op == '*') {
        r = calc_mul(calc_acc, right);
    } else if (calc_op == '+') {
        r = calc_acc + right;
    } else if (calc_op == '-') {
        r = calc_acc - right;
    } else {
        return 1;
    }
    calc_acc = r;
    calc_set_entry_from_acc(r);
    return !calc_error;
}

static void calc_digit(char d) {
    if (calc_error)
        return;
    if (calc_fresh) {
        calc_entry[0] = d;
        calc_entry[1] = 0;
        calc_entry_len = 1;
        calc_has_dot = 0;
        calc_fresh = 0;
        return;
    }
    if (calc_digit_count() >= CALC_DIGITS)
        return;
    if (calc_entry_len == 1 && calc_entry[0] == '0' && d != '.') {
        calc_entry[0] = d;
        return;
    }
    if (calc_entry_len >= 18)
        return;
    calc_entry[calc_entry_len++] = d;
    calc_entry[calc_entry_len] = 0;
}

static void calc_dot(void) {
    if (calc_error)
        return;
    if (calc_fresh) {
        calc_entry[0] = '0';
        calc_entry[1] = '.';
        calc_entry[2] = 0;
        calc_entry_len = 2;
        calc_has_dot = 1;
        calc_fresh = 0;
        return;
    }
    if (calc_has_dot)
        return;
    if (calc_entry_len >= 18)
        return;
    calc_entry[calc_entry_len++] = '.';
    calc_entry[calc_entry_len] = 0;
    calc_has_dot = 1;
}

static void calc_set_op(int op) {
    if (calc_error)
        return;
    if (calc_op && !calc_fresh) {
        if (!calc_compute())
            return;
    }
    calc_acc = calc_parse(calc_entry);
    calc_op = op;
    calc_fresh = 1;
}

static void calc_equals(void) {
    if (calc_error)
        return;
    if (calc_op) {
        calc_compute();
        calc_op = 0;
        calc_fresh = 1;
    }
}

static void calc_press(char ch) {
    if (ch == 'C') {
        calc_reset();
        return;
    }
    if (calc_error)
        return;
    if (ch >= '0' && ch <= '9')
        calc_digit(ch);
    else if (ch == '.')
        calc_dot();
    else if (ch == '+' || ch == '-' || ch == '*' || ch == '/')
        calc_set_op(ch);
    else if (ch == '=')
        calc_equals();
}

static void calc_copy(void) {
    if (calc_error || calc_entry_len <= 0)
        return;
    clip_len = calc_entry_len;
    if (clip_len > EDIT_BUF_SIZE - 1)
        clip_len = EDIT_BUF_SIZE - 1;
    kmemcpy(clip_buf, calc_entry, clip_len);
    clip_buf[clip_len] = 0;
}

static void calc_paste(void) {
    if (!clip_is_number())
        return;
    int n = clip_len;
    if (n > 18)
        n = 18;
    kmemcpy(calc_entry, clip_buf, n);
    calc_entry[n] = 0;
    calc_entry_len = n;
    calc_has_dot = 0;
    for (int i = 0; i < n; i++) {
        if (calc_entry[i] == '.')
            calc_has_dot = 1;
    }
    calc_error = 0;
    calc_fresh = 0;
}

static void calc_key_rect(int kx, int ky, int i, int *x, int *y, int *w, int *h) {
    int row = calc_keys[i].row;
    int col = calc_keys[i].col;
    *w = CALC_KEY_W;
    *h = CALC_KEY_H;
    *x = kx + col * (CALC_KEY_W + CALC_GAP);
    if (row == 0)
        *y = ky;
    else
        *y = ky + CALC_KEY_H + CALC_GAP + (row - 1) * (CALC_KEY_H + CALC_GAP);
}

static void draw_calc(int wx, int wy, int ww, int wh, int inactive) {
    (void)ww;
    (void)wh;
    gui_draw_window(wx, wy, CALC_W, CALC_H, "Calculator", 0,
                    inactive ? WIN_INACTIVE : 0);

    draw_rect(wx, wy + TITLE_H + 1, CALC_W, CALC_H - TITLE_H - 1, ui_chrome);
    int dx = wx + CALC_MARGIN;
    int dy = wy + TITLE_H + 12;
    int dw = CALC_W - 2 * CALC_MARGIN;
    int dh = CALC_DISP_H + 10;
    draw_round_rect(dx, dy, dw, dh, 6, ui_chrome_dk);
    draw_round_rect(dx + 1, dy + 1, dw - 2, dh - 2, 5, COLOR_WHITE);

    const char *s = calc_entry;
    int tw = logo_string_w(s);
    int use_logo = 1;
    for (const char *q = s; *q; q++) {
        if ((*q < '0' || *q > '9') && *q != '.' && *q != '-') {
            use_logo = 0;
            break;
        }
    }
    if (use_logo && tw <= dw - 20) {
        int tx = dx + dw - 12 - tw;
        draw_logo_string(s, tx, dy + (dh - LOGO_FONT_H) / 2, ui_text);
    } else {
        tw = uib_string_w(s);
        int tx = dx + dw - 12 - tw;
        if (tx < dx + 6)
            tx = dx + 6;
        draw_string_bold_clip(s, tx, dy + (dh - CHAR_H) / 2, ui_text, dx + dw - 6);
    }

    int kx = wx + CALC_MARGIN;
    int ky = dy + dh + 12;
    for (int i = 0; i < CALC_NKEYS; i++) {
        int x, y, w, h;
        calc_key_rect(kx, ky, i, &x, &y, &w, &h);
        char lab[2];
        lab[0] = calc_keys[i].ch;
        lab[1] = 0;
        char ch = calc_keys[i].ch;
        draw_button_styled(x, y, w, h, lab, ch == '=');
    }
}

static void handle_calc_click(int wx, int wy, int ww, int wh) {
    (void)ww;
    (void)wh;
    int dy = wy + TITLE_H + 12;
    int kx = wx + CALC_MARGIN;
    int ky = dy + CALC_DISP_H + 10 + 12;
    for (int i = 0; i < CALC_NKEYS; i++) {
        int x, y, w, h;
        calc_key_rect(kx, ky, i, &x, &y, &w, &h);
        if (hit(mouse_x, mouse_y, x, y, w, h)) {
            calc_press(calc_keys[i].ch);
            dirty = 1;
            return;
        }
    }
}

#define PAINT_W        160
#define PAINT_H        100
#define PAINT_SCALE    3
#define PAINT_TOOL_H   36
#define PAINT_WELL_H   52
#define PAINT_CELL     28
#define PAINT_TGAP     4
#define PAINT_NTOOLS   7
#define PAINT_NWELL    12
#define PAINT_NWELL2   16
_Static_assert(PAINT_W * PAINT_H <= 0x4000, "paint canvas overlaps viewer");
_Static_assert(0xC000 + 8 + PAINT_W * PAINT_H <= PAINT_CAPACITY, "paint staging overflow");
_Static_assert(0x8000 + 2 * PAINT_W * PAINT_H <= PAINT_CAPACITY, "paint fill stack overflow");

#define PT_PENCIL  0
#define PT_FILL    1
#define PT_LINE    2
#define PT_RECT    3
#define PT_ELLIPSE 4
#define PT_TEXT    5
#define PT_ERASER  6

static History paint_history;
static void paint_undo(int redo);
static uint8_t *paint_pix;
static uint8_t *view_pix;
static int paint_ready;
static int paint_tool;
static int paint_brush = 1;
static uint8_t paint_color;
static int paint_dragging;
static int paint_shape_drag;
static int paint_lx0, paint_ly0, paint_lx1, paint_ly1;
static int paint_last_lx, paint_last_ly;
static int paint_text_on;
static int paint_text_x, paint_text_y, paint_text_origin_x;
static int paint_text_last_adv;

static int view_ok;
static int view_iw, view_ih;
static int view_file;

static const uint8_t paint_well[PAINT_NWELL] = {
    0, 1, 2, 15, 4, 5, 6, 7, 8, 9, 10, 11
};

/* Second row: picks from the 6x6x6 cube (>= 16), none of which exist in 0..15. */
static const uint32_t paint_well2_rgb[PAINT_NWELL2] = {
    0xFF00AF, 0xFFAFD7, 0xFFAFAF, 0xAF5F00, 0xFFAF00, 0xFFFF00, 0xD7FF00, 0x00FF00,
    0x00FFAF, 0x00FFFF, 0x87D7FF, 0x0087FF, 0x5F00FF, 0xAF00FF, 0xD7AFFF, 0xFFD7AF
};
static uint8_t paint_well2[PAINT_NWELL2];

static int plot_dest; /* 0 = paint_pix, 1 = 3x screen overlay */
static int plot_ox, plot_oy;
static uint8_t plot_col;

static void paint_init(void) {
    if (!paint_history.capacity) paint_history=(History){0,0,8,PAINT_W*PAINT_H,
        (unsigned char *)APPS_BASE + sizeof(WindowState)*MAX_WIN};
    for (int i = 0; i < PAINT_NWELL2; i++)
        paint_well2[i] = idx24(paint_well2_rgb[i]);
    if (!paint_pix)
        paint_pix = (uint8_t *)PAINT_MEM;
    if (!view_pix)
        view_pix = (uint8_t *)(PAINT_MEM + 0x4000);
    if (!paint_ready) {
        kmemset(paint_pix, COLOR_WHITE, PAINT_W * PAINT_H);
        paint_tool = PT_PENCIL;
        paint_color = COLOR_BLACK;
        paint_ready = 1;
    }
}

static void paint_clear(void) {
    paint_init(); history_record(&paint_history,paint_pix);
    kmemset(paint_pix, COLOR_WHITE, PAINT_W * PAINT_H);
    paint_text_on = 0;
    paint_dragging = 0;
    paint_shape_drag = 0;
    dirty = 1;
}

static void plot_lg(int x, int y) {
    if (x < 0 || y < 0 || x >= PAINT_W || y >= PAINT_H)
        return;
    if (plot_dest == 0) {
        paint_pix[y * PAINT_W + x] = plot_col;
    } else {
        int sx = plot_ox + x * PAINT_SCALE;
        int sy = plot_oy + y * PAINT_SCALE;
        draw_rect(sx, sy, PAINT_SCALE, PAINT_SCALE, plot_col);
    }
}

static void paint_stamp(int x, int y, int r, uint8_t c) {
    plot_dest = 0;
    plot_col = c;
    for (int j = -r; j <= r; j++)
        for (int i = -r; i <= r; i++)
            plot_lg(x + i, y + j);
}

static void bresenham_lg(int x0, int y0, int x1, int y1) {
    int dx = x1 - x0;
    if (dx < 0)
        dx = -dx;
    int dy = y1 - y0;
    if (dy < 0)
        dy = -dy;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        plot_lg(x0, y0);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = err * 2;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static void rect_lg(int x0, int y0, int x1, int y1) {
    int xa = x0 < x1 ? x0 : x1;
    int xb = x0 < x1 ? x1 : x0;
    int ya = y0 < y1 ? y0 : y1;
    int yb = y0 < y1 ? y1 : y0;
    for (int x = xa; x <= xb; x++) {
        plot_lg(x, ya);
        plot_lg(x, yb);
    }
    for (int y = ya; y <= yb; y++) {
        plot_lg(xa, y);
        plot_lg(xb, y);
    }
}

static void ellipse_pts(int cx, int cy, int x, int y) {
    plot_lg(cx + x, cy + y);
    plot_lg(cx - x, cy + y);
    plot_lg(cx + x, cy - y);
    plot_lg(cx - x, cy - y);
}

static void ellipse_lg(int x0, int y0, int x1, int y1) {
    int cx = (x0 + x1) / 2;
    int cy = (y0 + y1) / 2;
    int rx = x1 - x0;
    if (rx < 0)
        rx = -rx;
    rx /= 2;
    int ry = y1 - y0;
    if (ry < 0)
        ry = -ry;
    ry /= 2;
    if (rx <= 0 && ry <= 0) {
        plot_lg(cx, cy);
        return;
    }
    if (rx <= 0) {
        for (int y = -ry; y <= ry; y++)
            plot_lg(cx, cy + y);
        return;
    }
    if (ry <= 0) {
        for (int x = -rx; x <= rx; x++)
            plot_lg(cx + x, cy);
        return;
    }
    int x = 0;
    int y = ry;
    long rx2 = (long)rx * rx;
    long ry2 = (long)ry * ry;
    long p = ry2 - rx2 * ry + rx2 / 4;
    while (2 * ry2 * x < 2 * rx2 * y) {
        ellipse_pts(cx, cy, x, y);
        x++;
        if (p < 0) {
            p += 2 * ry2 * x + ry2;
        } else {
            y--;
            p += 2 * ry2 * x - 2 * rx2 * y + ry2;
        }
    }
    p = ry2 * (x + 1) * (x + 1) + rx2 * (y - 1) * (y - 1) - rx2 * ry2;
    while (y >= 0) {
        ellipse_pts(cx, cy, x, y);
        y--;
        if (p > 0) {
            p += -2 * rx2 * y + rx2;
        } else {
            x++;
            p += 2 * ry2 * x - 2 * rx2 * y + rx2;
        }
    }
}

static void paint_commit_shape(void) {
    plot_dest = 0;
    plot_col = paint_color;
    if (paint_tool == PT_LINE)
        bresenham_lg(paint_lx0, paint_ly0, paint_lx1, paint_ly1);
    else if (paint_tool == PT_RECT)
        rect_lg(paint_lx0, paint_ly0, paint_lx1, paint_ly1);
    else if (paint_tool == PT_ELLIPSE)
        ellipse_lg(paint_lx0, paint_ly0, paint_lx1, paint_ly1);
}

static void paint_flood(int sx, int sy, uint8_t nc) {
    if (sx < 0 || sy < 0 || sx >= PAINT_W || sy >= PAINT_H)
        return;
    uint8_t oc = paint_pix[sy * PAINT_W + sx];
    if (oc == nc)
        return;
    uint16_t *st = (uint16_t *)(PAINT_MEM + 0x8000);
    int sp = 0;
    st[sp++] = (uint16_t)(sx + sy * PAINT_W);
    int n = 0;
    int cap = PAINT_W * PAINT_H;
    while (sp > 0 && n < cap) {
        int p = st[--sp];
        if (paint_pix[p] != oc)
            continue;
        paint_pix[p] = nc;
        n++;
        int x = p % PAINT_W;
        int y = p / PAINT_W;
        if (x > 0 && sp < cap - 1)
            st[sp++] = (uint16_t)(p - 1);
        if (x < PAINT_W - 1 && sp < cap - 1)
            st[sp++] = (uint16_t)(p + 1);
        if (y > 0 && sp < cap - 1)
            st[sp++] = (uint16_t)(p - PAINT_W);
        if (y < PAINT_H - 1 && sp < cap - 1)
            st[sp++] = (uint16_t)(p + PAINT_W);
    }
}

static int paint_putchar(int x, int y, char ch, uint8_t col) {
    int i = ui_index(ch);
    int adv = ui_advance(ch);
    if (ch == ' ')
        return adv;
    if (i < 0)
        return adv;
    int bpr, gh, gw;
    const uint8_t *gp = ui_glyph(ch, &bpr, &gh, &gw);
    for (int row = 0; row < gh; row++) {
        for (int colb = 0; colb < gw; colb++) {
            if (glyph_level(gp, bpr, row, colb) < 8)
                continue;
            int px = x + colb;
            int py = y + row;
            if (px >= 0 && py >= 0 && px < PAINT_W && py < PAINT_H)
                paint_pix[py * PAINT_W + px] = col;
        }
    }
    return adv;
}

static void paint_canvas_geom(int wx, int wy, int ww, int wh,
                              int *px, int *py, int *pw, int *ph,
                              int *cx, int *cy) {
    *px = wx + 1;
    *py = wy + TITLE_H + PAINT_TOOL_H;
    *pw = ww - 2;
    *ph = wh - TITLE_H - PAINT_TOOL_H - PAINT_WELL_H;
    int cw = PAINT_W * PAINT_SCALE;
    int ch = PAINT_H * PAINT_SCALE;
    *cx = *px + (*pw - cw) / 2;
    *cy = *py + (*ph - ch) / 2;
}

static int paint_mouse_logical(int wx, int wy, int ww, int wh,
                               int mx, int my, int *lx, int *ly) {
    int px, py, pw, ph, cx, cy;
    paint_canvas_geom(wx, wy, ww, wh, &px, &py, &pw, &ph, &cx, &cy);
    int cw = PAINT_W * PAINT_SCALE;
    int ch = PAINT_H * PAINT_SCALE;
    if (mx < cx || my < cy || mx >= cx + cw || my >= cy + ch)
        return 0;
    *lx = (mx - cx) / PAINT_SCALE;
    *ly = (my - cy) / PAINT_SCALE;
    if (*lx < 0)
        *lx = 0;
    if (*ly < 0)
        *ly = 0;
    if (*lx >= PAINT_W)
        *lx = PAINT_W - 1;
    if (*ly >= PAINT_H)
        *ly = PAINT_H - 1;
    return 1;
}

static void paint_tool_cell(int wx, int wy, int i, int *x, int *y) {
    *x = wx + 12 + i * (PAINT_CELL + PAINT_TGAP);
    *y = wy + TITLE_H + (PAINT_TOOL_H - PAINT_CELL) / 2;
}

static void paint_brush_cell(int wx, int wy, int i, int *x, int *y) {
    int base = 12 + PAINT_NTOOLS * (PAINT_CELL + PAINT_TGAP) + 16;
    *x = wx + base + i * (PAINT_CELL + PAINT_TGAP);
    *y = wy + TITLE_H + (PAINT_TOOL_H - PAINT_CELL) / 2;
}

static void paint_chip_cell(int wx, int wy, int ww, int *x, int *y) {
    *x = wx + ww - 12 - 40;
    *y = wy + TITLE_H + (PAINT_TOOL_H - 24) / 2;
}

static void paint_well_cell(int wx, int wy, int wh, int i, int *x, int *y) {
    *x = wx + 12 + i * (22 + 4);
    *y = wy + wh - 24;
}

static void paint_well2_cell(int wx, int wy, int wh, int i, int *x, int *y) {
    *x = wx + 12 + i * (22 + 4);
    *y = wy + wh - 48;
}

static void draw_tool_doodle(int kind, int x, int y) {
    int x0 = x + 6, y0 = y + 6, x1 = x + 21, y1 = y + 21;
    int cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    if (kind == PT_PENCIL) {
        for (int i = 0; i <= 14; i++) {
            put_pixel(x0 + i, y1 - i, COLOR_BLACK);
            put_pixel(x0 + i, y1 - i - 1, COLOR_BLACK);
        }
        draw_rect(x1 - 2, y0, 3, 3, COLOR_BLACK);
    } else if (kind == PT_FILL) {
        uint8_t ink = COLOR_BLACK;
        draw_round_rect(cx - 5, cy - 3, 11, 9, 2, ink);
        draw_round_rect(cx - 4, cy - 2, 9, 6, 1, gfx_gray(0xE0));
        draw_line(cx - 4, cy - 3, cx + 1, cy - 8, ink);
        draw_rect(cx + 4, cy - 8, 2, 6, ink);
        draw_round_rect(cx + 3, cy + 4, 3, 5, 1, paint_color);
        put_pixel(cx + 4, cy + 9, paint_color);
    } else if (kind == PT_LINE) {
        for (int i = 0; i <= 14; i++)
            put_pixel(x0 + i, y1 - i, COLOR_BLACK);
    } else if (kind == PT_RECT) {
        draw_rect(x0, y0 + 2, x1 - x0, y1 - y0 - 4, COLOR_WHITE);
        draw_rect(x0, y0 + 2, x1 - x0, 1, COLOR_BLACK);
        draw_rect(x0, y1 - 3, x1 - x0, 1, COLOR_BLACK);
        draw_rect(x0, y0 + 2, 1, y1 - y0 - 4, COLOR_BLACK);
        draw_rect(x1 - 1, y0 + 2, 1, y1 - y0 - 4, COLOR_BLACK);
    } else if (kind == PT_ELLIPSE) {
        plot_dest = 1;
        plot_ox = 0;
        plot_oy = 0;
        plot_col = COLOR_BLACK;
        /* draw oval into screen pixels at 1x via plot_lg scaled; use tiny ellipse */
        int ecx = cx, ecy = cy;
        int a = 7, b = 5;
        for (int yy = -b; yy <= b; yy++) {
            for (int xx = -a; xx <= a; xx++) {
                int inside = xx * xx * b * b + yy * yy * a * a;
                int ring = a * a * b * b;
                int inner = (a - 1) * (a - 1) * (b - 1) * (b - 1);
                if (inside <= ring && inside >= inner)
                    put_pixel(ecx + xx, ecy + yy, COLOR_BLACK);
            }
        }
    } else if (kind == PT_TEXT) {
        draw_char('A', cx - 4, cy - 8, COLOR_BLACK);
    } else if (kind == PT_ERASER) {
        draw_round_rect(cx - 6, cy - 2, 12, 8, 2, gfx_rgb(0xF0, 0x90, 0xA8));
        draw_round_rect(cx - 6, cy - 2, 7, 8, 2, gfx_rgb(0xB8, 0x50, 0x70));
        draw_vline(cx - 1, cy - 2, 8, gfx_gray(0x40));
    }
}

static void draw_paint(int wx, int wy, int ww, int wh, int inactive) {
    paint_init();
    gui_draw_window(wx, wy, ww, wh, "Paint", 0,
                    inactive ? WIN_INACTIVE : 0);

    /* tool strip */
    int strip_y = wy + TITLE_H;
    draw_rect(wx, strip_y + 1, ww, PAINT_TOOL_H - 1, ui_chrome);
    draw_hline(wx, strip_y + PAINT_TOOL_H, ww, ui_chrome_dk);
    for (int i = 0; i < PAINT_NTOOLS; i++) {
        int tx, ty;
        paint_tool_cell(wx, wy, i, &tx, &ty);
        if (i == paint_tool) {
            draw_round_rect(tx, ty, PAINT_CELL, PAINT_CELL, 6, ui_accent_dk);
            draw_round_rect(tx + 1, ty + 1, PAINT_CELL - 2, PAINT_CELL - 2, 5, PAL_ACCENT + 2);
        } else {
            draw_round_rect(tx, ty, PAINT_CELL, PAINT_CELL, 6, gfx_gray(0xB0));
            draw_round_rect(tx + 1, ty + 1, PAINT_CELL - 2, PAINT_CELL - 2, 5, COLOR_WHITE);
        }
        draw_tool_doodle(i, tx, ty);
    }

    {
        int dx, dy;
        paint_brush_cell(wx, wy, 0, &dx, &dy);
        draw_vline(dx - 9, dy + 2, PAINT_CELL - 4, ui_chrome_dk);
        for (int i = 0; i < 3; i++) {
            int bx, by;
            paint_brush_cell(wx, wy, i, &bx, &by);
            int on = (paint_brush == i + 1);
            if (on) {
                draw_round_rect(bx, by, PAINT_CELL, PAINT_CELL, 6, ui_accent_dk);
                draw_round_rect(bx + 1, by + 1, PAINT_CELL - 2, PAINT_CELL - 2, 5, PAL_ACCENT + 2);
            } else {
                draw_round_rect(bx, by, PAINT_CELL, PAINT_CELL, 6, gfx_gray(0xB0));
                draw_round_rect(bx + 1, by + 1, PAINT_CELL - 2, PAINT_CELL - 2, 5, COLOR_WHITE);
            }
            int rad = i + 1;
            uint8_t dotc = on ? COLOR_WHITE : ui_text;
            draw_round_rect(bx + PAINT_CELL / 2 - rad, by + PAINT_CELL / 2 - rad,
                            rad * 2, rad * 2, rad, dotc);
        }
        int cx, cy;
        paint_chip_cell(wx, wy, ww, &cx, &cy);
        draw_round_rect(cx - 2, cy - 2, 44, 28, 7, gfx_gray(0x50));
        draw_round_rect(cx - 1, cy - 1, 42, 26, 6, COLOR_WHITE);
        draw_round_rect(cx + 2, cy + 2, 36, 20, 4, paint_color);
    }

    /* canvas pane + 1px black inset */
    int px, py, pw, ph, cx, cy;
    paint_canvas_geom(wx, wy, ww, wh, &px, &py, &pw, &ph, &cx, &cy);
    draw_rect(px, py, pw, 1, COLOR_BLACK);
    draw_rect(px, py + ph - 1, pw, 1, COLOR_BLACK);
    draw_rect(px, py, 1, ph, COLOR_BLACK);
    draw_rect(px + pw - 1, py, 1, ph, COLOR_BLACK);

    for (int ly = 0; ly < PAINT_H; ly++) {
        for (int lx = 0; lx < PAINT_W; lx++) {
            uint8_t c = paint_pix[ly * PAINT_W + lx];
            draw_rect(cx + lx * PAINT_SCALE, cy + ly * PAINT_SCALE,
                      PAINT_SCALE, PAINT_SCALE, c);
        }
    }

    if (paint_shape_drag &&
        (paint_tool == PT_LINE || paint_tool == PT_RECT || paint_tool == PT_ELLIPSE)) {
        plot_dest = 1;
        plot_ox = cx;
        plot_oy = cy;
        plot_col = paint_color;
        if (paint_tool == PT_LINE)
            bresenham_lg(paint_lx0, paint_ly0, paint_lx1, paint_ly1);
        else if (paint_tool == PT_RECT)
            rect_lg(paint_lx0, paint_ly0, paint_lx1, paint_ly1);
        else
            ellipse_lg(paint_lx0, paint_ly0, paint_lx1, paint_ly1);
        plot_dest = 0;
    }

    /* color well */
    int well_top = wy + wh - PAINT_WELL_H;
    draw_rect(wx, well_top, ww, PAINT_WELL_H, ui_chrome);
    draw_hline(wx, well_top, ww, ui_chrome_dk);
    for (int i = 0; i < PAINT_NWELL + PAINT_NWELL2; i++) {
        int sx, sy;
        uint8_t c;
        if (i < PAINT_NWELL) {
            paint_well_cell(wx, wy, wh, i, &sx, &sy);
            c = paint_well[i];
        } else {
            paint_well2_cell(wx, wy, wh, i - PAINT_NWELL, &sx, &sy);
            c = paint_well2[i - PAINT_NWELL];
        }
        int cur = (paint_color == c);
        draw_round_rect(sx, sy, 22, 18, 4, cur ? ui_text : gfx_gray(0xA0));
        draw_round_rect(sx + 1, sy + 1, 20, 16, 3, c);
        if (cur) {
            uint8_t mk = gfx_luma(c) < 128 ? COLOR_WHITE : ui_text;
            draw_round_frame(sx + 3, sy + 3, 16, 12, 2, mk);
        }
    }
}

static int unique_untitled_pbm(int parent, char *out) {
    if (fs_find_child(parent, "untitled.pbm") < 0) {
        kstrcpy(out, "untitled.pbm");
        return 0;
    }
    for (int n = 2; n < 100; n++) {
        char name[FS_NAME_LEN];
        int pos = 0;
        const char *base = "untitled ";
        while (base[pos]) {
            name[pos] = base[pos];
            pos++;
        }
        int x = n;
        char digs[4];
        int nd = 0;
        do {
            digs[nd++] = (char)('0' + (x % 10));
            x /= 10;
        } while (x && nd < 4);
        while (nd--)
            name[pos++] = digs[nd];
        name[pos++] = '.';
        name[pos++] = 'p';
        name[pos++] = 'b';
        name[pos++] = 'm';
        name[pos] = 0;
        if (fs_find_child(parent, name) < 0) {
            kstrcpy(out, name);
            return 0;
        }
    }
    return -1;
}

static int paint_write_named(const char *name) {
    paint_init();
    int pics = fs_find_child(fs_root(), "Pictures");
    if (pics < 0)
        pics = fs_mkdir(fs_root(), "Pictures");
    if (pics < 0)
        return 0;
    int id = fs_find_child(pics, name);
    if (id < 0)
        id = fs_create(pics, name);
    if (id < 0)
        return 0;
    int nbytes = 8 + PAINT_W * PAINT_H;
    if (nbytes > FS_MAX_SIZE)
        return 0;
    char *buf = (char *)(PAINT_MEM + 0xC000);
    buf[0] = 'B'; buf[1] = 'O'; buf[2] = 'S'; buf[3] = '1';
    buf[4] = (char)(PAINT_W & 0xFF);
    buf[5] = (char)((PAINT_W >> 8) & 0xFF);
    buf[6] = (char)(PAINT_H & 0xFF);
    buf[7] = (char)((PAINT_H >> 8) & 0xFF);
    kmemcpy(buf + 8, paint_pix, PAINT_W * PAINT_H);
    fs_write(id, buf, nbytes);
    if (find_open_kind(WK_FILES) >= 0)
        fm_refresh();
    dirty = 1;
    return 1;
}

static void paint_save(void) {
    paint_init();
    int pics = fs_find_child(fs_root(), "Pictures");
    if (pics < 0)
        pics = fs_mkdir(fs_root(), "Pictures");
    if (pics < 0)
        return;
    char name[FS_NAME_LEN];
    if (unique_untitled_pbm(pics, name) < 0)
        kstrcpy(name, "art.pbm");
    namedlg_open(1, name);
}

static void open_paint(void) {
    paint_init();
    open_dlg = 0;
    win_open(WK_PAINT);
    dirty = 1;
}

static int is_bos1(int id) {
    if (!fs_valid(id) || fs_is_dir(id) || fs_is_app(id))
        return 0;
    if (fs_size(id) < 8)
        return 0;
    const char *d = fs_data(id);
    return d[0] == 'B' && d[1] == 'O' && d[2] == 'S' && d[3] == '1';
}

static int view_load(int id) {
    if (id < 0 || !is_bos1(id))
        return 0;
    const unsigned char *d = (const unsigned char *)fs_data(id);
    int w = d[4] | (d[5] << 8);
    int h = d[6] | (d[7] << 8);
    int need = 8 + w * h;
    if (w < 1 || h < 1 || w > 400 || h > 400)
        return 0;
    if (need > fs_size(id) || w * h > 16376)
        return 0;
    kmemcpy(view_pix, d + 8, w * h);
    view_file = id;
    view_iw = w;
    view_ih = h;
    view_ok = 1;
    return 1;
}

/* Viewer holds a picture or nothing: drop the window if the file went away. */
static void view_check_file(void) {
    int i = find_open_kind(WK_VIEW);
    if (i < 0)
        return;
    if (is_bos1(view_file))
        return;
    win_close(i);
    view_ok = 0;
    view_file = -1;
}

static void view_apply_geom(int i) {
    if (i < 0)
        return;
    layout_window(WK_VIEW, &wins[i].x, &wins[i].y, &wins[i].w, &wins[i].h);
    win_clamp(&wins[i]);
}

static void open_view(int file_id) {
    if (!view_load(file_id))
        return;
    paint_init();
    open_dlg = 0;
    int i = find_open_kind(WK_VIEW);
    if (i < 0)
        i = win_open(WK_VIEW);
    else {
        view_apply_geom(i);
        win_focus(i);
    }
    /* win_open already laid out using current view_* */
    view_apply_geom(i);
    dirty = 1;
}

static void draw_view(int wx, int wy, int ww, int wh, int inactive) {
    gui_draw_window(wx, wy, ww, wh, fs_name(view_file), 0,
                    inactive ? WIN_INACTIVE : 0);
    int body_y = wy + TITLE_H + 1;
    int body_h = wh - TITLE_H - 2;
    int body_x = wx + 1;
    int body_w = ww - 2;
    int cw = view_iw * PAINT_SCALE;
    int ch = view_ih * PAINT_SCALE;
    int cx = body_x + (body_w - cw) / 2;
    int cy = body_y + (body_h - ch) / 2;
    draw_rect(body_x, body_y, body_w, body_h, ui_chrome);
    draw_shadow(cx, cy, cw, ch);
    for (int ly = 0; ly < view_ih; ly++) {
        for (int lx = 0; lx < view_iw; lx++) {
            uint8_t c = view_pix[ly * view_iw + lx];
            draw_rect(cx + lx * PAINT_SCALE, cy + ly * PAINT_SCALE,
                      PAINT_SCALE, PAINT_SCALE, c);
        }
    }
}

static void handle_paint_click(int wx, int wy, int ww, int wh) {
    paint_init();
    if (paint_text_on)
        paint_text_on = 0;

    for (int i = 0; i < PAINT_NTOOLS; i++) {
        int tx, ty;
        paint_tool_cell(wx, wy, i, &tx, &ty);
        if (hit(mouse_x, mouse_y, tx, ty, PAINT_CELL, PAINT_CELL)) {
            paint_tool = i;
            dirty = 1;
            return;
        }
    }
    for (int i = 0; i < 3; i++) {
        int bx, by;
        paint_brush_cell(wx, wy, i, &bx, &by);
        if (hit(mouse_x, mouse_y, bx, by, PAINT_CELL, PAINT_CELL)) {
            paint_brush = i + 1;
            dirty = 1;
            return;
        }
    }
    for (int i = 0; i < PAINT_NWELL; i++) {
        int sx, sy;
        paint_well_cell(wx, wy, wh, i, &sx, &sy);
        if (hit(mouse_x, mouse_y, sx, sy, 22, 18)) {
            paint_color = paint_well[i];
            dirty = 1;
            return;
        }
    }
    for (int i = 0; i < PAINT_NWELL2; i++) {
        int sx, sy;
        paint_well2_cell(wx, wy, wh, i, &sx, &sy);
        if (hit(mouse_x, mouse_y, sx, sy, 22, 18)) {
            paint_color = paint_well2[i];
            dirty = 1;
            return;
        }
    }

    int lx, ly;
    if (!paint_mouse_logical(wx, wy, ww, wh, mouse_x, mouse_y, &lx, &ly))
        return;

    history_record(&paint_history,paint_pix);
    if (paint_tool == PT_PENCIL) {
        paint_stamp(lx, ly, paint_brush - 1, paint_color);
        paint_dragging = 1;
        paint_last_lx = lx;
        paint_last_ly = ly;
        dirty = 1;
    } else if (paint_tool == PT_ERASER) {
        paint_stamp(lx, ly, paint_brush, COLOR_WHITE);
        paint_dragging = 1;
        paint_last_lx = lx;
        paint_last_ly = ly;
        dirty = 1;
    } else if (paint_tool == PT_FILL) {
        paint_flood(lx, ly, paint_color);
        dirty = 1;
    } else if (paint_tool == PT_LINE || paint_tool == PT_RECT ||
               paint_tool == PT_ELLIPSE) {
        paint_shape_drag = 1;
        paint_lx0 = paint_lx1 = lx;
        paint_ly0 = paint_ly1 = ly;
        dirty = 1;
    } else if (paint_tool == PT_TEXT) {
        paint_text_on = 1;
        paint_text_x = lx;
        paint_text_y = ly;
        paint_text_origin_x = lx;
        paint_text_last_adv = 0;
        dirty = 1;
    }
}

static void paint_drag_tick(void) {
    if (front_kind() != WK_PAINT || open_dlg)
        return;
    int wx, wy, ww, wh;
    if (!win_geom_kind(WK_PAINT, &wx, &wy, &ww, &wh))
        return;
    int lx, ly;
    int on = paint_mouse_logical(wx, wy, ww, wh, mouse_x, mouse_y, &lx, &ly);
    if (paint_dragging && on) {
        uint8_t c = paint_tool == PT_ERASER ? COLOR_WHITE : paint_color;
        int r = paint_tool == PT_ERASER ? 2 : 1;
        plot_dest = 0;
        plot_col = c;
        bresenham_lg(paint_last_lx, paint_last_ly, lx, ly);
        paint_stamp(lx, ly, r, c);
        paint_last_lx = lx;
        paint_last_ly = ly;
        dirty = 1;
    }
    if (paint_shape_drag && on) {
        if (lx != paint_lx1 || ly != paint_ly1) {
            paint_lx1 = lx;
            paint_ly1 = ly;
            dirty = 1;
        }
    }
}

static void paint_mouse_up(void) {
    if (paint_shape_drag) {
        paint_commit_shape();
        paint_shape_drag = 0;
        dirty = 1;
    }
    paint_dragging = 0;
}

static void paint_undo(int redo) {
    paint_init();
    if (history_step(&paint_history,paint_pix,redo)) {
        paint_dragging=paint_shape_drag=paint_text_on=0; dirty=1;
    }
}
static void handle_paint_key(void) {
    if (key_sc == KEY_ESC) {
        if (paint_text_on) {
            paint_text_on = 0;
            dirty = 1;
            return;
        }
        close_front();
        return;
    }
    if (!paint_text_on)
        return;
    if (key_sc == KEY_ENTER) {
        paint_text_on = 0;
        dirty = 1;
        return;
    }
    if (key_sc == KEY_BACKSPACE) {
        history_record(&paint_history,paint_pix);
        if (paint_text_last_adv > 0 && paint_text_x >= paint_text_origin_x) {
            paint_text_x -= paint_text_last_adv;
            if (paint_text_x < paint_text_origin_x)
                paint_text_x = paint_text_origin_x;
            /* erase last glyph box */
            for (int j = 0; j < UI_FONT_H; j++)
                for (int i = 0; i < paint_text_last_adv; i++) {
                    int px = paint_text_x + i;
                    int py = paint_text_y + j;
                    if (px >= 0 && py >= 0 && px < PAINT_W && py < PAINT_H)
                        paint_pix[py * PAINT_W + px] = COLOR_WHITE;
                }
            paint_text_last_adv = 0;
            dirty = 1;
        }
        return;
    }
    if (key_char) {
        history_record(&paint_history,paint_pix);
        paint_text_last_adv = paint_putchar(paint_text_x, paint_text_y, key_char,
                                            paint_color);
        paint_text_x += paint_text_last_adv;
        dirty = 1;
    }
}

/* ---------- Snake ---------- */

#define SNK_COLS 30
#define SNK_ROWS 20
#define SNK_CELL 16
#define SNK_MAX  (SNK_COLS * SNK_ROWS)
#define SNK_TICK 8

/* Direction: 0 right, 1 down, 2 left, 3 up. */
static unsigned char snk_x[SNK_MAX];
static unsigned char snk_y[SNK_MAX];
static int snk_len;
static int snk_dir;
static int snk_next_dir;
static int snk_food_x;
static int snk_food_y;
static int snk_score;
static int snk_over;
static uint32_t snk_last_tick;
static unsigned snk_rng;

static unsigned snk_rand(void) {
    snk_rng = snk_rng * 1103515245u + 12345u;
    return (snk_rng >> 16) & 0x7FFF;
}

static int snk_on_body(int cx, int cy, int upto) {
    for (int i = 0; i < upto; i++) {
        if (snk_x[i] == cx && snk_y[i] == cy)
            return 1;
    }
    return 0;
}

static void snk_place_food(void) {
    for (int tries = 0; tries < 400; tries++) {
        int cx = (int)(snk_rand() % SNK_COLS);
        int cy = (int)(snk_rand() % SNK_ROWS);
        if (!snk_on_body(cx, cy, snk_len)) {
            snk_food_x = cx;
            snk_food_y = cy;
            return;
        }
    }
    for (int cy = 0; cy < SNK_ROWS; cy++) {
        for (int cx = 0; cx < SNK_COLS; cx++) {
            if (!snk_on_body(cx, cy, snk_len)) {
                snk_food_x = cx;
                snk_food_y = cy;
                return;
            }
        }
    }
}

static void snake_reset(void) {
    snk_len = 4;
    for (int i = 0; i < snk_len; i++) {
        snk_x[i] = (unsigned char)(SNK_COLS / 2 - i);
        snk_y[i] = (unsigned char)(SNK_ROWS / 2);
    }
    snk_dir = 0;
    snk_next_dir = 0;
    snk_score = 0;
    snk_over = 0;
    snk_rng = frame_count * 2654435761u + 1u;
    snk_place_food();
    snk_last_tick = frame_count;
    dirty = 1;
}

static void snake_tick(void) {
    if (find_open_kind(WK_SNAKE) < 0 || snk_over)
        return;
    if (frame_count - snk_last_tick < SNK_TICK)
        return;
    snk_last_tick = frame_count;

    snk_dir = snk_next_dir;
    int nx = snk_x[0];
    int ny = snk_y[0];
    if (snk_dir == 0)
        nx++;
    else if (snk_dir == 1)
        ny++;
    else if (snk_dir == 2)
        nx--;
    else
        ny--;

    if (nx < 0 || nx >= SNK_COLS || ny < 0 || ny >= SNK_ROWS ||
        snk_on_body(nx, ny, snk_len - 1)) {
        snk_over = 1;
        dirty = 1;
        return;
    }

    int ate = (nx == snk_food_x && ny == snk_food_y);
    if (ate && snk_len < SNK_MAX)
        snk_len++;
    for (int i = snk_len - 1; i > 0; i--) {
        snk_x[i] = snk_x[i - 1];
        snk_y[i] = snk_y[i - 1];
    }
    snk_x[0] = (unsigned char)nx;
    snk_y[0] = (unsigned char)ny;
    if (ate) {
        snk_score++;
        snk_place_food();
    }
    dirty = 1;
}

static void snake_turn(int dir) {
    if ((dir ^ snk_dir) == 2)
        return;
    snk_next_dir = dir;
}

static void handle_snake_key(void) {
    if (key_sc == KEY_ESC) {
        close_front();
        return;
    }
    if (key_sc == KEY_RIGHT)
        snake_turn(0);
    else if (key_sc == KEY_DOWN)
        snake_turn(1);
    else if (key_sc == KEY_LEFT)
        snake_turn(2);
    else if (key_sc == KEY_UP)
        snake_turn(3);
}

static void snake_info(char *out, int cap) {
    const char *lbl = "Score: ";
    char num[12];
    int i = 0;
    for (int p = 0; lbl[p] && i < cap - 1; p++)
        out[i++] = lbl[p];
    utoa((unsigned)snk_score, num);
    for (int p = 0; num[p] && i < cap - 1; p++)
        out[i++] = num[p];
    if (snk_over) {
        const char *go = "  GAME OVER";
        for (int p = 0; go[p] && i < cap - 1; p++)
            out[i++] = go[p];
    }
    out[i] = 0;
}

static void draw_snake(int wx, int wy, int ww, int wh, int inactive) {
    char info[32];
    snake_info(info, 32);
    gui_draw_window(wx, wy, ww, wh, "Snake", info,
                    WIN_INFO | (inactive ? WIN_INACTIVE : 0));

    int bx = wx + 1;
    int by = wy + TITLE_H + INFO_H + 1;
    int bw = ww - 2;
    int bh = wh - TITLE_H - INFO_H - 2;
    if (bw < 1 || bh < 1)
        return;
    uint8_t field = gfx_rgb(0x16, 0x1A, 0x24);
    uint8_t grid = gfx_rgb(0x1E, 0x23, 0x30);
    draw_rect(bx, by, bw, bh, field);

    int gx = bx + (bw - SNK_COLS * SNK_CELL) / 2;
    int gy = by + (bh - SNK_ROWS * SNK_CELL) / 2;
    for (int r = 0; r <= SNK_ROWS; r++)
        draw_hline(gx, gy + r * SNK_CELL, SNK_COLS * SNK_CELL, grid);
    for (int c = 0; c <= SNK_COLS; c++)
        draw_vline(gx + c * SNK_CELL, gy, SNK_ROWS * SNK_CELL, grid);
    for (int i = 0; i < snk_len; i++) {
        uint8_t body = (i == 0) ? PAL_ACCENT + 4 : ui_accent;
        draw_round_rect(gx + snk_x[i] * SNK_CELL + 1, gy + snk_y[i] * SNK_CELL + 1,
                        SNK_CELL - 2, SNK_CELL - 2, 3, body);
    }
    draw_round_rect(gx + snk_food_x * SNK_CELL + 2, gy + snk_food_y * SNK_CELL + 2,
                    SNK_CELL - 4, SNK_CELL - 4, (SNK_CELL - 4) / 2, gfx_rgb(0xF0, 0x50, 0x48));
}

static void open_snake(void) {
    if (find_open_kind(WK_SNAKE) < 0)
        snake_reset();
    open_dlg = 0;
    win_open(WK_SNAKE);
    dirty = 1;
}

static void open_wordle(void) {
    if (find_open_kind(WK_WORDLE) < 0)
        wordle_new_game(frame_count);
    open_dlg = 0;
    win_open(WK_WORDLE);
    dirty = 1;
}

/* ---------- Terminal ---------- */

static int term_text(const char *s, int x, int y, int col, int cols) {
    while (*s && col < cols) {
        draw_edit_char(*s++, x + col * EDIT_CHAR_W, y, TERM_FG);
        col++;
    }
    return col;
}

static void draw_term(int wx, int wy, int ww, int wh, int inactive) {
    gui_draw_window(wx, wy, ww, wh, "Terminal", 0, inactive ? WIN_INACTIVE : 0);
    draw_rect(wx, wy + TITLE_H + 1, ww, wh - TITLE_H - 1, gfx_gray(0x20));

    int ax = wx + 1 + TERM_PAD;
    int ay = wy + TITLE_H + 1 + TERM_PAD;
    int canvas_height=0;
    const unsigned char *canvas=term_canvas();
    if(canvas){int scale=wh>360?2:1;canvas_height=100*scale+8;
        for(int y=0;y<100;y++)for(int x=0;x<160;x++)draw_rect(ax+x*scale,ay+y*scale,scale,scale,canvas[y*160+x]);
        ay+=canvas_height;
    }
    int cols = (ww - 2 - TERM_PAD * 2) / EDIT_CHAR_W;
    int rows = (wh - TITLE_H - 2 - TERM_PAD * 2 - canvas_height) / EDIT_LINE_H;
    if (cols < 1 || rows < 1)
        return;

    /* Reflow output to the current window width; manuals remain readable
     * even in a narrow terminal, and scrolling counts visual rows. */
    char live[TERM_COLS*2+16];
    term_prompt(live,TERM_COLS+8);kstrcpy(live+kstrlen(live),term_input());
    int count=term_count(),total=0;
    for(int i=0;i<=count;i++){
        int len=kstrlen(i==count?live:term_get(i));
        total+=i==count?len/cols+1:(len? (len+cols-1)/cols:1);
    }
    term_set_view(rows,total);
    int first=total>rows?total-rows-term_scroll_offset():0,visual=0;
    for(int i=0;i<=count;i++){
        const char *text=i==count?live:term_get(i);int len=kstrlen(text);
        int chunks=i==count?len/cols+1:(len?(len+cols-1)/cols:1);
        for(int chunk=0;chunk<chunks;chunk++,visual++){
            int row=visual-first;
            if(row<0||row>=rows)continue;
            int y=ay+row*EDIT_LINE_H;
            term_text(text+chunk*cols,ax,y,0,cols);
            if(i==count&&chunk==chunks-1&&!inactive)
                draw_rect(ax+(len%cols)*EDIT_CHAR_W,y,1,EDIT_FONT_H,TERM_FG);
        }
    }
}

static void open_term(void) {
    open_dlg = 0;
    win_open(WK_TERM);
    dirty = 1;
}

/* ---------- Todo ---------- */

#define TODO_CIRCLE  12
#define TODO_MARK_X  12
#define TODO_LABEL_X 32

static void draw_todo_mark(int x, int y, int done) {
    int s = TODO_CIRCLE + 4;
    if (done) {
        draw_round_rect(x - 2, y - 2, s, s, s / 2, ui_accent);
        uint8_t w = COLOR_WHITE;
        draw_line(x + 2, y + 6, x + 5, y + 9, w);
        draw_line(x + 3, y + 6, x + 6, y + 9, w);
        draw_line(x + 5, y + 9, x + 11, y + 3, w);
        draw_line(x + 6, y + 9, x + 12, y + 3, w);
    } else {
        draw_round_rect(x - 2, y - 2, s, s, s / 2, gfx_gray(0xA8));
        draw_round_rect(x - 1, y - 1, s - 2, s - 2, s / 2 - 1, COLOR_WHITE);
    }
}

static void todo_field_box(int wx, int wy, int ww, int wh,
                           int *fx, int *fy, int *fw, int *fh) {
    *fx = wx + 12;
    *fy = wy + wh - TODO_FOOT_H + 8;
    *fw = ww - 24;
    *fh = 24;
}

static int todo_rows_fit(int wh) {
    int room = wh - TITLE_H - 8 - TODO_FOOT_H;
    int n = room / TODO_ROW_H;
    if (n > TODO_MAX)
        n = TODO_MAX;
    return n < 0 ? 0 : n;
}

static void draw_todo(int wx, int wy, int ww, int wh, int inactive) {
    gui_draw_window(wx, wy, ww, wh, "Todo", 0, inactive ? WIN_INACTIVE : 0);
    draw_rect(wx + 1, wy + TITLE_H + 1, ww - 2, wh - TITLE_H - 2, COLOR_WHITE);

    int rows = todo_rows_fit(wh);
    int n = todo_count();
    if (n > rows)
        n = rows;
    for (int i = 0; i < n; i++) {
        int ry = wy + TITLE_H + 8 + i * TODO_ROW_H;
        draw_todo_mark(wx + TODO_MARK_X, ry + (TODO_ROW_H - TODO_CIRCLE) / 2,
                       todo_done(i));
        draw_string_clip(todo_text(i), wx + TODO_LABEL_X,
                         ry + (TODO_ROW_H - CHAR_H) / 2,
                         todo_done(i) ? ui_text_dim : ui_text, wx + ww - 12);
        if (todo_done(i))
            draw_hline(wx + TODO_LABEL_X, ry + TODO_ROW_H / 2,
                       ui_string_w(todo_text(i)), ui_text_dim);
    }

    int ruley = wy + wh - TODO_FOOT_H;
    draw_rect(wx, ruley, ww, TODO_FOOT_H, ui_chrome);
    draw_hline(wx, ruley, ww, ui_chrome_dk);

    int fx, fy, fw, fh;
    todo_field_box(wx, wy, ww, wh, &fx, &fy, &fw, &fh);
    int active = todo_field_active();
    draw_round_rect(fx, fy, fw, fh, 5, active ? ui_accent : gfx_gray(0xA8));
    draw_round_rect(fx + 1, fy + 1, fw - 2, fh - 2, 4, COLOR_WHITE);

    const char *txt = todo_field();
    int ty = fy + (fh - CHAR_H) / 2;
    if (txt[0])
        draw_string_clip(txt, fx + 8, ty, ui_text, fx + fw - 6);
    else
        draw_string_clip("Add a task and press Enter", fx + 8, ty, ui_text_dim, fx + fw - 6);
    if (active) {
        int cx = fx + 8 + ui_string_w(txt);
        if (cx < fx + fw - 6)
            draw_rect(cx, fy + 4, 1, fh - 8, ui_text);
    }
}

static void handle_todo_click(int wx, int wy, int ww, int wh) {
    int fx, fy, fw, fh;
    todo_field_box(wx, wy, ww, wh, &fx, &fy, &fw, &fh);
    if (hit(mouse_x, mouse_y, fx, fy, fw, fh)) {
        todo_focus_field();
        dirty = 1;
        return;
    }
    int rows = todo_rows_fit(wh);
    int n = todo_count();
    if (n > rows)
        n = rows;
    for (int i = 0; i < n; i++) {
        int ry = wy + TITLE_H + 8 + i * TODO_ROW_H;
        if (hit(mouse_x, mouse_y, wx + 1, ry, ww - 2, TODO_ROW_H)) {
            todo_toggle(i);
            dirty = 1;
            return;
        }
    }
}

static void open_todo(void) {
    if (find_open_kind(WK_TODO) < 0)
        todo_load();
    open_dlg = 0;
    win_open(WK_TODO);
    dirty = 1;
}

static void open_fs_file(int id) {
    if (!fs_valid(id))
        return;
    const char *n = fs_name(id);
    if (kstrcmp(n, "Calculator") == 0) {
        open_calc();
        return;
    }
    if (kstrcmp(n, "Paint") == 0) {
        open_paint();
        return;
    }
    if (kstrcmp(n, "Image Viewer") == 0) {
        start_open_dialog(1);
        return;
    }
    if (kstrcmp(n, "Snake") == 0) {
        open_snake();
        return;
    }
    if (kstrcmp(n, "Wordle") == 0) {
        open_wordle();
        return;
    }
    if (kstrcmp(n, "Terminal") == 0) {
        open_term();
        return;
    }
    if (kstrcmp(n, "Todo") == 0) {
        open_todo();
        return;
    }
    if (kstrcmp(n, "Clock") == 0) {
        win_open(WK_CLOCK);
        return;
    }
    if (kstrcmp(n, "Calendar") == 0) {
        cal_reset();
        win_open(WK_CAL);
        return;
    }
    if (kstrcmp(n, "Minesweeper") == 0) {
        win_open(WK_MINES);
        return;
    }
    if (kstrcmp(n, "2048") == 0) {
        win_open(WK_2048);
        return;
    }
    if (kstrcmp(n, "Breakout") == 0) {
        win_open(WK_BREAKOUT);
        return;
    }
    if (kstrcmp(n, "System Monitor") == 0) {
        win_open(WK_SYSMON);
        return;
    }
    if (is_bos1(id)) {
        open_view(id);
        return;
    }
    if (kstrcmp(n, "Hello") == 0) {
        win_open(WK_HELLO);
        dirty = 1;
        return;
    }
    if (fs_is_app(id))
        return;
    if (win_open(WK_EDIT) < 0) return;
    fm_cwd = fs_parent(id);
    edit_load(id);
    dirty = 1;
}




static void layout_window(int kind, int *x, int *y, int *w, int *h) {
    switch (kind) {
    case WK_PROPERTIES:
        *x=300;*y=140;*w=500;*h=290;break;
    case WK_HELLO:
        *w = 520;
        *h = 170;
        break;
    case WK_HELP:
        *w = 640;
        *h = 250;
        break;
    case WK_ABOUT:
        *w = 520;
        *h = 360;
        break;
    case WK_SETTINGS:
        *w = 560;
        *h = 360;
        break;
    case WK_FILES:
        *w = 720;
        *h = 480;
        break;
    case WK_EDIT:
        *w = 960;
        *h = 520;
        break;
    case WK_CALC:
        *w = CALC_W;
        *h = CALC_H;
        break;
    case WK_PAINT:
        *w = 760;
        *h = 500;
        break;
    case WK_VIEW:
        if (view_ok && view_iw > 0 && view_ih > 0) {
            *w = view_iw * 3 + 8;
            *h = view_ih * 3 + 8 + TITLE_H;
        } else {
            *w = 320;
            *h = 160;
        }
        if (*w < 200) *w = 200;
        if (*w > 800) *w = 800;
        if (*h < 120) *h = 120;
        if (*h > 520) *h = 520;
        break;
    case WK_SNAKE:
        *w = 520;
        *h = 400;
        break;
    case WK_WORDLE:
        *w = WORDLE_W;
        *h = WORDLE_WIN_H;
        break;
    case WK_TERM:
        *w = TERM_WIN_W;
        *h = TERM_WIN_H;
        break;
    case WK_TODO:
        *w = TODO_W;
        *h = TODO_WIN_H;
        break;
    case WK_CLOCK:
        *w = CLOCK_W;
        *h = CLOCK_H + TITLE_H;
        break;
    case WK_CAL:
        *w = CAL_W;
        *h = CAL_H + TITLE_H;
        break;
    case WK_MINES:
        *w = MINES_W;
        *h = MINES_H + TITLE_H;
        break;
    case WK_2048:
        *w = G2048_W;
        *h = G2048_H + TITLE_H;
        break;
    case WK_BREAKOUT:
        *w = BO_W;
        *h = BO_H + TITLE_H;
        break;
    case WK_SYSMON:
        *w = SYSMON_W;
        *h = SYSMON_H + TITLE_H;
        break;
    default:
        *w = 320;
        *h = 180;
        break;
    }
    int maxh = fb_h - MENUBAR_H - TASKBAR_H - 8;
    if (*w > fb_w - 24)
        *w = fb_w - 24;
    if (*h > maxh)
        *h = maxh;
    *x = (fb_w - *w) / 2;
    *y = MENUBAR_H + (fb_h - MENUBAR_H - TASKBAR_H - *h) / 2;
    if (kind == WK_FILES) {
        *x = 80;
        *y = 60;
    }
    if (kind == WK_EDIT) {
        *x = 20;
        *y = MENUBAR_H + 12;
    }
    if (kind == WK_PAINT) {
        *x = 80;
        *y = 48;
    }
    if (kind == WK_SNAKE) {
        *x = 200;
        *y = 80;
    }
    if (kind == WK_WORDLE) {
        *x = 220;
        *y = 56;
    }
    if (kind == WK_TERM) {
        *x = 120;
        *y = 80;
    }
    if (kind == WK_TODO) {
        *x = 180;
        *y = 90;
    }
    if (*y < MENUBAR_H)
        *y = MENUBAR_H;
}

static void edit_area(int wx, int wy, int ww, int wh,
                      int *ax, int *ay, int *aw, int *ah) {
    int top = TITLE_H + INFO_H;
    *ax = wx + 18;
    *ay = wy + top + 14;
    *aw = ww - 36 - SB;
    *ah = wh - top - 24 - SB;
}

static void edit_caret_cell(int cols, int *row, int *col) {
    int r = 0, c = 0;
    for (int i = 0; i < edit_caret; i++) {
        if (edit_buf[i] == '\n') {
            r++;
            c = 0;
        } else {
            c++;
            if (c >= cols) {
                r++;
                c = 0;
            }
        }
    }
    *row = r;
    *col = c;
}

static int edit_cell_to_index(int cols, int row, int col) {
    int r = 0, c = 0;
    for (int i = 0; i < edit_len; i++) {
        if (r == row && c == col)
            return i;
        if (edit_buf[i] == '\n') {
            if (r == row)
                return i;
            r++;
            c = 0;
        } else {
            c++;
            if (c >= cols) {
                r++;
                c = 0;
            }
        }
        if (r > row)
            return i;
    }
    return edit_len;
}

static void edit_ensure_caret_visible(int rows, int cols) {
    int r, c;
    edit_caret_cell(cols, &r, &c);
    if (r < edit_scroll)
        edit_scroll = r;
    if (r >= edit_scroll + rows)
        edit_scroll = r - rows + 1;
    if (edit_scroll < 0)
        edit_scroll = 0;
}

static int double_click(int item, int *last_item, uint32_t *last_frame) {
    unsigned dt = frame_count - *last_frame;
    int is_dbl = (item == *last_item && dt < TIMER_HZ * 2 / 5);
    *last_item = item;
    *last_frame = frame_count;
    return is_dbl;
}

static void fm_open_selected(void) {
    int vis = fm_vis_count();
    if (vis <= 0 || fm_selected < 0 || fm_selected >= vis)
        return;
    int id = fm_row_id(fm_selected);
    if (id < 0) {
        fm_go_up();
        return;
    }
    if (fs_is_dir(id)) {
        fm_cwd = id;
        fm_selected = 0;
        fm_refresh();
        dirty = 1;
    } else {
        open_fs_file(id);
    }
}

static void fm_go_up(void) {
    int p = fs_parent(fm_cwd);
    if (p >= 0) {
        fm_cwd = p;
        fm_selected = 0;
        fm_refresh();
        dirty = 1;
    }
}

static void fm_new_file(void) {
    char name[FS_NAME_LEN];
    if (fs_unique_file(fm_cwd, name) < 0)
        return;
    int id = fs_create(fm_cwd, name);
    if (id < 0)
        return;
    fm_refresh();
    for (int i = 0; i < fm_count; i++) {
        if (fm_ids[i] == id)
            fm_selected = i + (fm_has_parent() ? 1 : 0);
    }
    dirty = 1;
}

static void od_refresh(void);

static void fm_select_id(int id) {
    for (int i = 0; i < fm_count; i++) {
        if (fm_ids[i] == id)
            fm_selected = i + (fm_has_parent() ? 1 : 0);
    }
}

static void fm_rename_cancel(void) {
    if (!fm_renaming)
        return;
    fm_renaming = 0;
    fm_rename_id = -1;
    fm_rename_len = 0;
    fm_rename_buf[0] = 0;
    dirty = 1;
}

static void fm_rename_begin(int id) {
    if (id < 0 || !fs_valid(id))
        return;
    if (fm_renaming && fm_rename_id == id)
        return;
    const char *nm = fs_name(id);
    int n = 0;
    while (nm[n] && n < FS_NAME_LEN - 1) {
        fm_rename_buf[n] = nm[n];
        n++;
    }
    fm_rename_buf[n] = 0;
    fm_rename_len = n;
    fm_rename_id = id;
    fm_renaming = 1;
    dirty = 1;
}

static void fm_rename_commit(void) {
    if (!fm_renaming)
        return;
    if (fm_rename_len <= 0) {
        dirty = 1;
        return;
    }
    if (fs_rename(fm_rename_id, fm_rename_buf) < 0) {
        dirty = 1;
        return;
    }
    {
        int id = fm_rename_id;
        fm_renaming = 0;
        fm_rename_id = -1;
        fm_refresh();
        fm_select_id(id);
        dirty = 1;
    }
}

static void do_duplicate(void) {
    if (front_kind() != WK_FILES || open_dlg)
        return;
    int id = fm_row_id(fm_selected);
    if (id < 0)
        return;
    if (fs_is_app(id) || kstrcmp(fs_name(id), "Calculator") == 0)
        return;
    int parent = fs_parent(id);
    if (parent < 0)
        parent = fm_cwd;
    fm_rename_cancel();
    int copy = fs_copy(id, parent);
    if (copy < 0)
        return;
    fm_refresh();
    fm_select_id(copy);
    dirty = 1;
}

static void do_new_folder(void) {
    /* Disk window only. untitled folder, then untitled folder 2. */
    if (front_kind() != WK_FILES || open_dlg)
        return;
    fm_rename_cancel();
    char name[FS_NAME_LEN];
    if (fs_unique_dir(fm_cwd, name) < 0)
        return;
    int id = fs_mkdir(fm_cwd, name);
    if (id < 0)
        return;
    fm_refresh();
    fm_select_id(id);
    dirty = 1;
}

static void do_empty_trash(void) {
    if (trash_id < 0)
        return;
    fs_empty_dir(trash_id);
    kprint_debug("Empty trash\n");
    if (edit_file >= 0 && !fs_valid(edit_file))
        edit_clear();
    view_check_file();
    if (find_open_kind(WK_FILES) >= 0) {
        if (!fs_valid(fm_cwd))
            fm_cwd = trash_id;
        fm_refresh();
    }
    if (open_dlg)
        od_refresh();
    dirty = 1;
}

static int od_row_enabled(int id) {
    if (!pick_pics_only)
        return 1;
    return fs_is_dir(id) || is_bos1(id);
}

/* Selection never rests on a row the dialog would refuse to open. */
static int od_next_enabled(int from, int dir) {
    for (int n = 0; n < pick_count; n++) {
        int i = (from + dir * (n + 1)) % pick_count;
        if (i < 0)
            i += pick_count;
        if (od_row_enabled(pick_ids[i]))
            return i;
    }
    return -1;
}

static void od_refresh(void) {
    int raw[FS_MAX_NODES];
    int n = fs_list(pick_cwd, raw, FS_MAX_NODES);
    pick_count = 0;
    for (int i = 0; i < n; i++) {
        if (pick_cwd == fs_root() && trash_id >= 0 && raw[i] == trash_id)
            continue;
        if (pick_cwd == fs_root() && prefs_id >= 0 && raw[i] == prefs_id)
            continue;
        pick_ids[pick_count++] = raw[i];
    }
    if (pick_selected >= pick_count)
        pick_selected = pick_count ? pick_count - 1 : 0;
    if (pick_selected < 0)
        pick_selected = 0;
    if (pick_count && !od_row_enabled(pick_ids[pick_selected]))
        pick_selected = od_next_enabled(pick_selected, 1);
}

static void start_open_dialog(int pics_only) {
    open_dlg = 1;
    pick_focus=0;pick_first=0;
    pick_pics_only = pics_only;
    pick_cwd = fs_root();
    pick_selected = 0;
    od_refresh();
    dirty = 1;
}

static void close_open_dialog(void) {
    open_dlg = 0;
    dirty = 1;
}

static void od_open_selected(void) {
    if (pick_count <= 0 || pick_selected < 0 || pick_selected >= pick_count)
        return;
    int id = pick_ids[pick_selected];
    if (!od_row_enabled(id))
        return;
    if (fs_is_dir(id)) {
        pick_cwd = id;
        pick_selected = 0;
        od_refresh();
        dirty = 1;
        return;
    }
    open_dlg = 0;
    open_fs_file(id);
}

/* ---------- Drawing ---------- */

static void files_title(char *title, int max) {
    if (fm_cwd == fs_root()) {
        kstrcpy(title, "Files");
        return;
    }
    const char *nm = fs_name(fm_cwd);
    int i = 0;
    while (nm[i] && i < max - 1) {
        title[i] = nm[i];
        i++;
    }
    title[i] = 0;
}

static void files_info(char *info, int max) {
    fs_path(fm_cwd, info, max);
}

static void draw_mini_folder(int x, int y, uint8_t fg, uint8_t bg) {
    draw_rect(x, y + 2, 12, 8, fg);
    draw_rect(x + 1, y, 5, 3, fg);
    draw_rect(x + 1, y + 3, 10, 6, bg);
}

static void draw_mini_doc(int x, int y, uint8_t fg, uint8_t bg) {
    draw_rect(x, y, 10, 12, fg);
    draw_rect(x + 1, y + 1, 8, 10, bg);
    put_pixel(x + 8, y, bg);
    put_pixel(x + 9, y, bg);
    put_pixel(x + 9, y + 1, fg);
}

static void draw_mini_calc(int x, int y, uint8_t fg, uint8_t bg) {
    /* 12x12 body, display strip, 2x2 key suggestion. Distinct from folder/doc. */
    draw_rect(x, y, 12, 12, fg);
    draw_rect(x + 1, y + 1, 10, 10, bg);
    draw_rect(x + 2, y + 2, 8, 2, fg);
    draw_rect(x + 2, y + 6, 3, 2, fg);
    draw_rect(x + 7, y + 6, 3, 2, fg);
    draw_rect(x + 2, y + 9, 3, 2, fg);
    draw_rect(x + 7, y + 9, 3, 2, fg);
}

static void draw_mini_paint(int x, int y, uint8_t fg, uint8_t bg) {
    draw_rect(x, y, 12, 12, bg);
    for (int i = 0; i < 10; i++) {
        put_pixel(x + 1 + i, y + 10 - i, fg);
        put_pixel(x + 2 + i, y + 10 - i, fg);
    }
    draw_rect(x + 8, y + 1, 3, 3, fg);
}

static void draw_mini_pic(int x, int y, uint8_t fg, uint8_t bg) {
    draw_rect(x, y, 12, 12, fg);
    draw_rect(x + 1, y + 1, 10, 10, bg);
    draw_rect(x + 2, y + 2, 3, 3, fg);
    for (int i = 0; i < 5; i++)
        draw_rect(x + 2 + i, y + 9 - i, 8 - 2 * i, 1, fg);
}

static void draw_mini_view(int x, int y, uint8_t fg, uint8_t bg) {
    /* Open frame, sun left, mountain — distinct from pic-file and paint. */
    draw_rect(x, y + 1, 12, 10, fg);
    draw_rect(x + 1, y + 2, 10, 8, bg);
    put_pixel(x + 3, y + 4, fg);
    put_pixel(x + 4, y + 4, fg);
    put_pixel(x + 3, y + 5, fg);
    put_pixel(x + 9, y + 3, fg);
    for (int i = 0; i < 4; i++)
        put_pixel(x + 2 + i, y + 9 - i, fg);
    for (int i = 0; i < 4; i++)
        put_pixel(x + 6 + i, y + 6 + (i < 2 ? i : 4 - i), fg);
}

static void draw_mini_snake(int x, int y, uint8_t fg, uint8_t bg) {
    /* Coiled run of cells with a pip of food at the tail end. */
    draw_rect(x, y, 12, 12, bg);
    draw_rect(x, y + 1, 9, 2, fg);
    draw_rect(x + 7, y + 1, 2, 5, fg);
    draw_rect(x + 2, y + 4, 7, 2, fg);
    draw_rect(x + 2, y + 4, 2, 5, fg);
    draw_rect(x + 2, y + 7, 6, 2, fg);
    draw_rect(x + 10, y + 9, 2, 2, fg);
}

static void draw_mini_term(int x, int y, uint8_t fg, uint8_t bg) {
    /* Screen with a prompt chevron and a rule for the caret. */
    draw_rect(x, y, 12, 12, fg);
    draw_rect(x + 1, y + 1, 10, 10, bg);
    put_pixel(x + 3, y + 3, fg);
    put_pixel(x + 4, y + 4, fg);
    put_pixel(x + 5, y + 5, fg);
    put_pixel(x + 4, y + 6, fg);
    put_pixel(x + 3, y + 7, fg);
    draw_rect(x + 6, y + 7, 3, 1, fg);
}

static void draw_mini_wordle(int x, int y, uint8_t fg, uint8_t bg) {
    /* Three rows of guess tiles. */
    draw_rect(x, y, 12, 12, bg);
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) {
            int tx = x + c * 4;
            int ty = y + 1 + r * 4;
            if (r == c)
                draw_rect(tx, ty, 3, 3, fg);
            else {
                draw_rect(tx, ty, 3, 1, fg);
                draw_rect(tx, ty + 2, 3, 1, fg);
                draw_rect(tx, ty, 1, 3, fg);
                draw_rect(tx + 2, ty, 1, 3, fg);
            }
        }
}

static void draw_file_row_named(int x, int y, int w, const char *name,
                               int is_dir, int is_app, int sel, int dim) {
    uint8_t fg = dim ? ui_text_dim : (sel ? COLOR_WHITE : ui_text);
    uint8_t bg = sel ? ui_accent : COLOR_WHITE;
    if (dim) {
        sel = 0;
        bg = COLOR_WHITE;
    }
    if (sel)
        draw_round_rect(x + 2, y, w - 4, ROW_H - 2, 5, ui_accent);
    int icon_y = y + (ROW_H - 12) / 2;
    if (is_dir)
        draw_mini_folder(x + 6, icon_y, fg, bg);
    else if (is_app == 2)
        draw_mini_paint(x + 6, icon_y, fg, bg);
    else if (is_app == 3)
        draw_mini_view(x + 6, icon_y, fg, bg);
    else if (is_app == 4)
        draw_mini_pic(x + 6, icon_y, fg, bg);
    else if (is_app == 5)
        draw_mini_snake(x + 6, icon_y, fg, bg);
    else if (is_app == 6)
        draw_mini_term(x + 6, icon_y, fg, bg);
    else if (is_app == 7)
        draw_mini_wordle(x + 6, icon_y, fg, bg);
    else if (is_app)
        draw_mini_calc(x + 6, icon_y, fg, bg);
    else
        draw_mini_doc(x + 6, icon_y, fg, bg);
    int ty = y + (ROW_H - CHAR_H) / 2;
    int end=x+w-12;
    if(w>420){
        const char *kind=is_dir?"Folder":(is_app?"Application":"Document");
        draw_string(kind,x+w-110,ty,sel?COLOR_WHITE:ui_text_dim);
        end=x+w-130;
    }
    draw_string_clip(name, x + 28, ty, fg, end);
}

static void draw_file_row(int x, int y, int w, int id, int sel, int dim) {
    const char *nm = fs_name(id);
    int is_app = 0;
    if (fs_is_app(id) || kstrcmp(nm, "Calculator") == 0) {
        if (kstrcmp(nm, "Paint") == 0)
            is_app = 2;
        else if (kstrcmp(nm, "Image Viewer") == 0)
            is_app = 3;
        else if (kstrcmp(nm, "Snake") == 0)
            is_app = 5;
        else if (kstrcmp(nm, "Terminal") == 0)
            is_app = 6;
        else if (kstrcmp(nm, "Wordle") == 0)
            is_app = 7;
        else
            is_app = 1;
    } else if (is_bos1(id)) {
        is_app = 4;
    }
    int renaming = fm_renaming && id == fm_rename_id;
    draw_file_row_named(x, y, w, renaming ? "" : fs_name(id),
                        fs_is_dir(id), is_app, sel, dim);
    if (!renaming)
        return;
    int fx = x + 26;
    int fy = y - 1;
    int fh = ROW_H - 2;
    int tw = ui_string_w(fm_rename_buf) + 16;
    if (tw < 80)
        tw = 80;
    int fw = tw;
    if (fx + fw > x + w - 4)
        fw = x + w - 4 - fx;
    if (fw < 24)
        fw = 24;
    /* White field, black type, 1px inset. */
    draw_rect(fx, fy, fw, fh, COLOR_WHITE);
    draw_rect(fx, fy, fw, 1, COLOR_BLACK);
    draw_rect(fx, fy + fh - 1, fw, 1, COLOR_BLACK);
    draw_rect(fx, fy, 1, fh, COLOR_BLACK);
    draw_rect(fx + fw - 1, fy, 1, fh, COLOR_BLACK);
    int ty = fy + (fh - CHAR_H) / 2;
    draw_string_clip(fm_rename_buf, fx + 4, ty, COLOR_BLACK, fx + fw - 4);
    {
        int cw = ui_string_w(fm_rename_buf);
        int cx = fx + 4 + cw;
        if (cx < fx + fw - 3 && (frame_count / 35) % 2 == 0)
            draw_rect(cx, ty, 1, CHAR_H, COLOR_BLACK);
    }
}

static int files_list_y(int wy) {
    return wy + TITLE_H + INFO_H + 34;
}

static void files_scroll(int rows){
    if(rows<1)rows=1;
    if(fm_selected<fm_first)fm_first=fm_selected;
    if(fm_selected>=fm_first+rows)fm_first=fm_selected-rows+1;
    if(fm_first<0)fm_first=0;
}
static void draw_files(int wx, int wy, int ww, int wh, int inactive) {
    char title[24];
    char info[FS_PATH_LEN];
    files_title(title, 24);
    files_info(info, FS_PATH_LEN);
    gui_draw_window(wx, wy, ww, wh, title, info,
                    WIN_INFO | WIN_SCROLL | (inactive ? WIN_INACTIVE : 0));

    int head_y=wy+TITLE_H+INFO_H+6;
    draw_string("Name",wx+36,head_y,ui_text_dim);
    if(ww-16-SB>420)draw_string("Type",wx+ww-SB-118,head_y,ui_text_dim);
    draw_hline(wx+12,head_y+CHAR_H+5,ww-SB-24,ui_chrome_dk);
    int list_y = files_list_y(wy);
    int list_b = wy + wh - 4 - SB;
    int rows = (list_b - list_y) / ROW_H;
    int lw = ww - 16 - SB;
    int vis = fm_vis_count();
    int up = fm_has_parent() ? 1 : 0;
    if (vis == 0) {
        draw_string("This folder is empty", wx + 20, list_y + 12, ui_text_dim);
        return;
    }
    files_scroll(rows);
    for (int i = fm_first; i < vis && i < fm_first+rows; i++) {
        int iy = list_y + (i-fm_first) * ROW_H;
        if (up && i == 0)
            draw_file_row_named(wx + 8, iy, lw, "..", 1, 0, i == fm_selected, 0);
        else
            draw_file_row(wx + 8, iy, lw, fm_ids[i - up], i == fm_selected, 0);
    }
}

static void draw_rename_overlay(void) {
    int wx, wy, ww, wh, vis, iy, fx, fy, fh, tw, fw, ty;
    if (!fm_renaming)
        return;
    if (!win_geom_kind(WK_FILES, &wx, &wy, &ww, &wh))
        return;
    vis = fm_vis_count();
    if (fm_selected < 0 || fm_selected >= vis)
        return;
    iy = files_list_y(wy) + (fm_selected-fm_first) * ROW_H;
    fx = wx + 8 + 26;
    fy = iy - 1;
    fh = ROW_H - 2;
    tw = ui_string_w(fm_rename_buf) + 16;
    if (tw < 96)
        tw = 96;
    fw = tw;
    if (fx + fw > wx + ww - 8 - SB)
        fw = wx + ww - 8 - SB - fx;
    if (fw < 24)
        fw = 24;
    draw_rect(fx, fy, fw, fh, COLOR_WHITE);
    draw_rect(fx, fy, fw, 1, COLOR_BLACK);
    draw_rect(fx, fy + fh - 1, fw, 1, COLOR_BLACK);
    draw_rect(fx, fy, 1, fh, COLOR_BLACK);
    draw_rect(fx + fw - 1, fy, 1, fh, COLOR_BLACK);
    ty = fy + (fh - CHAR_H) / 2;
    draw_string_clip(fm_rename_buf, fx + 4, ty, COLOR_BLACK, fx + fw - 4);
    {
        int cx = fx + 4 + ui_string_w(fm_rename_buf);
        if (cx < fx + fw - 3)
            draw_rect(cx, ty, 1, CHAR_H, COLOR_BLACK);
    }
}

static void pick_geom(int *px, int *py, int *pw, int *ph) {
    *pw = 480;
    *ph = 340;
    *px = (fb_w - *pw) / 2;
    *py = (fb_h - *ph) / 2;
}

static void od_buttons(int px, int py, int pw, int ph, int *ox, int *oy, int *ow,
                       int *cx, int *cy, int *cw) {
    *ow = btn_w("Open");
    *cw = btn_w("Cancel");
    *oy = *cy = py + ph - 16 - BTN_H;
    *cx = px + pw - 20 - *cw;
    *ox = *cx - 12 - *ow;
}

static int od_list_y(int py) { return py + TITLE_H + 8; }

static void draw_open_dialog(void) {
    int px, py, pw, ph;
    pick_geom(&px, &py, &pw, &ph);
    gui_draw_window(px, py, pw, ph, "Open", 0, WIN_NOCLOSE);

    int ox, oy, ow, cx, cy, cw;
    od_buttons(px, py, pw, ph, &ox, &oy, &ow, &cx, &cy, &cw);
    draw_default_button(ox, oy, ow, BTN_H, "Open");
    draw_button(cx, cy, cw, BTN_H, "Cancel");
    if(pick_focus)draw_frame((pick_focus==1?cx:ox)-3,oy-3,(pick_focus==1?cw:ow)+6,BTN_H+6,ui_accent);

    int ly = od_list_y(py);
    int lb = oy - 12;
    int rows = (lb - ly) / ROW_H;
    int lw = pw - 16;
    if (pick_count == 0) {
        draw_string("(empty)", px + 16, ly, COLOR_BLACK);
        return;
    }
    if(pick_selected<pick_first)pick_first=pick_selected;
    if(pick_selected>=pick_first+rows)pick_first=pick_selected-rows+1;
    for (int i = pick_first; i < pick_count && i < pick_first+rows; i++)
        draw_file_row(px + 8, ly + (i-pick_first) * ROW_H, lw, pick_ids[i],
                      i == pick_selected, !od_row_enabled(pick_ids[i]));
}

static void draw_editor(int wx, int wy, int ww, int wh, int inactive) {
    char title[40];
    char info[32];
    if (edit_file >= 0) {
        const char *nm = fs_name(edit_file);
        int t = 0;
        while (nm[t] && t < 28) {
            title[t] = nm[t];
            t++;
        }
        title[t] = 0;
    } else {
        kstrcpy(title, "untitled");
    }
    if (!edit_saved_ok) {
        int t = kstrlen(title);
        if (t < 36) {
            title[t++] = ' ';
            title[t++] = '*';
            title[t] = 0;
        }
    }

    char nbuf[8];
    utoa((unsigned)edit_len, nbuf);
    int i = 0, p = 0;
    while (nbuf[p] && i < 20)
        info[i++] = nbuf[p++];
    const char *s = " bytes";
    p = 0;
    while (s[p] && i < 30)
        info[i++] = s[p++];
    if (!edit_saved_ok && i < 28) {
        info[i++] = ' ';
        info[i++] = '*';
    }
    info[i] = 0;

    gui_draw_window(wx, wy, ww, wh, title, info,
                    WIN_INFO | WIN_SCROLL | (inactive ? WIN_INACTIVE : 0));

    int ax, ay, aw, ah;
    edit_area(wx, wy, ww, wh, &ax, &ay, &aw, &ah);
    /* The text sits directly on the page, without an inset black rule. */

    int cols = aw / EDIT_CHAR_W;
    int rows = ah / EDIT_LINE_H;
    if (cols < 1)
        cols = 1;
    if (rows < 1)
        rows = 1;
    edit_ensure_caret_visible(rows, cols);

    int r = 0, c = 0;
    int sel_lo = edit_sel_a < edit_sel_b ? edit_sel_a : edit_sel_b;
    int sel_hi = edit_sel_a > edit_sel_b ? edit_sel_a : edit_sel_b;
    int has_sel = sel_lo != sel_hi;

    for (int ei = 0; ei <= edit_len; ei++) {
        int vis_row = r - edit_scroll;
        int selected = has_sel && ei >= sel_lo && ei < sel_hi;
        if (selected && vis_row >= 0 && vis_row < rows) {
            int cw = (ei < edit_len && edit_buf[ei] == '\n') ? 4 : EDIT_CHAR_W;
            draw_rect(ax + c * EDIT_CHAR_W, ay + vis_row * EDIT_LINE_H, cw,
                      EDIT_LINE_H, ui_accent);
        }
        if (!has_sel && ei == edit_caret && vis_row >= 0 && vis_row < rows) {
            int cx = ax + c * EDIT_CHAR_W;
            int cy = ay + vis_row * EDIT_LINE_H;
            if ((frame_count / 35) % 2 == 0)
                draw_rect(cx, cy, 1, EDIT_FONT_H, COLOR_BLACK);
        }
        if (ei == edit_len)
            break;
        if (edit_buf[ei] == '\n') {
            r++;
            c = 0;
            continue;
        }
        vis_row = r - edit_scroll;
        if (vis_row >= 0 && vis_row < rows)
            draw_edit_char(edit_buf[ei], ax + c * EDIT_CHAR_W,
                           ay + vis_row * EDIT_LINE_H,
                           selected ? COLOR_WHITE : COLOR_BLACK);
        c++;
        if (c >= cols) {
            r++;
            c = 0;
        }
    }
}

static void draw_string_in_win(const char *str, int wx, int ww, int y, uint8_t color) {
    int tw = ui_string_w(str);
    int x = wx + (ww - tw) / 2;
    draw_string(str, x, y, color);
}

static const char *win_app_name(int kind) {
    switch (kind) {
    case WK_FILES: return "Files";
    case WK_EDIT: return "Editor";
    case WK_CALC: return "Calculator";
    case WK_ABOUT: return "About";
    case WK_HELP: return "Help";
    case WK_HELLO: return "Hello";
    case WK_SETTINGS: return "Settings";
    case WK_PAINT: return "Paint";
    case WK_VIEW: return "Viewer";
    case WK_SNAKE: return "Snake";
    case WK_WORDLE: return "Wordle";
    case WK_TERM: return "Terminal";
    case WK_TODO: return "Todo";
    case WK_CLOCK: return "Clock";
    case WK_CAL: return "Calendar";
    case WK_MINES: return "Mines";
    case WK_2048: return "2048";
    case WK_BREAKOUT: return "Breakout";
    case WK_SYSMON: return "Monitor";
    case WK_PROPERTIES: return "Properties";
    default: return "App";
    }
}

static void draw_icon16(int x, int y, const char *art, uint8_t ink) {
    draw_icon_art(x, y, art, 1, ink, -1);
}

static const char icon_calc16[] =
    "################"
    "#..............#"
    "#.############.#"
    "#.#..........#.#"
    "#.############.#"
    "#..............#"
    "#.##.##.##.##..#"
    "#.##.##.##.##..#"
    "#..............#"
    "#.##.##.##.##..#"
    "#.##.##.##.##..#"
    "#..............#"
    "#.##.##.######.#"
    "#.##.##.######.#"
    "#..............#"
    "################";

static const char icon_paint16[] =
    "################"
    "#..............#"
    "#............#.#"
    "#...........##.#"
    "#..........##..#"
    "#.........##...#"
    "#........##....#"
    "#.......##.....#"
    "#......##......#"
    "#.....##.......#"
    "#....##........#"
    "#...#####......#"
    "#...#####......#"
    "#..............#"
    "#..............#"
    "################";

static const char icon_view16[] =
    "################"
    "#..............#"
    "#.############.#"
    "#.#..........#.#"
    "#.#..##......#.#"
    "#.#.##.##....#.#"
    "#.#..........#.#"
    "#.#....#.....#.#"
    "#.#...###....#.#"
    "#.#..##.##...#.#"
    "#.#.##...##..#.#"
    "#.#..........#.#"
    "#.############.#"
    "#..............#"
    "#..............#"
    "################";

static const char icon_snake16[] =
    "################"
    "#..............#"
    "#..#######.....#"
    "#..#.....#.....#"
    "#..#.###.#.....#"
    "#....#.#.#.....#"
    "#....#.#.#.....#"
    "#..###.#.#.....#"
    "#..#...#.#.....#"
    "#..#.###.#.....#"
    "#..#.....#.....#"
    "#..#######..##.#"
    "#...........##.#"
    "#..............#"
    "#..............#"
    "################";

static const char icon_wordle16[] =
    "################"
    "#..............#"
    "#.##.##.##.##..#"
    "#.#..#..#..#...#"
    "#.##.##.##.##..#"
    "#..............#"
    "#.##.##.##.##..#"
    "#.#..#..#..#...#"
    "#.##.##.##.##..#"
    "#..............#"
    "#.##.##.##.##..#"
    "#.#..#..#..#...#"
    "#.##.##.##.##..#"
    "#..............#"
    "#..............#"
    "################";

static const char icon_term16[] =
    "################"
    "#..............#"
    "#.############.#"
    "#.############.#"
    "#.##........##.#"
    "#.##.#......##.#"
    "#.##.##.....##.#"
    "#.##..##....##.#"
    "#.##...##...##.#"
    "#.##..##....##.#"
    "#.##.##.....##.#"
    "#.##.#..###.##.#"
    "#.##........##.#"
    "#.############.#"
    "#..............#"
    "################";

static const char icon_todo16[] =
    "################"
    "#..............#"
    "#.###..######..#"
    "#.#.#..........#"
    "#.###..........#"
    "#..............#"
    "#.###..######..#"
    "#.#.#..........#"
    "#.###..........#"
    "#..............#"
    "#.###..######..#"
    "#.#.#..........#"
    "#.###..........#"
    "#..............#"
    "#..............#"
    "################";

static void draw_tb_icon(int kind, int x, int y, uint8_t invert) {
    if (kind == WK_FILES)
        draw_icon16(x, y, icon_folder16, invert);
    else if (kind == WK_CLOCK)
        draw_icon16(x, y, icon_clock16, invert);
    else if (kind == WK_CAL)
        draw_icon16(x, y, icon_cal16, invert);
    else if (kind == WK_MINES)
        draw_icon16(x, y, icon_mine16, invert);
    else if (kind == WK_2048)
        draw_icon16(x, y, icon_2048_16, invert);
    else if (kind == WK_BREAKOUT)
        draw_icon16(x, y, icon_brick16, invert);
    else if (kind == WK_SYSMON)
        draw_icon16(x, y, icon_mon16, invert);
    else if (kind == WK_CALC)
        draw_icon16(x, y, icon_calc16, invert);
    else if (kind == WK_PAINT)
        draw_icon16(x, y, icon_paint16, invert);
    else if (kind == WK_VIEW)
        draw_icon16(x, y, icon_view16, invert);
    else if (kind == WK_SNAKE)
        draw_icon16(x, y, icon_snake16, invert);
    else if (kind == WK_WORDLE)
        draw_icon16(x, y, icon_wordle16, invert);
    else if (kind == WK_TERM)
        draw_icon16(x, y, icon_term16, invert);
    else if (kind == WK_TODO)
        draw_icon16(x, y, icon_todo16, invert);
    else if (kind == WK_EDIT)
        draw_icon16(x, y, icon_edit16, invert);
    else if (kind == WK_SETTINGS)
        draw_icon16(x, y, icon_gear16, invert);
    else {
        uint8_t fg = invert;
        draw_frame(x + 1, y + 1, 14, 14, fg);
        draw_rect(x + 4, y + 5, 8, 1, fg);
        draw_rect(x + 4, y + 8, 8, 1, fg);
        draw_rect(x + 4, y + 11, 5, 1, fg);
    }
}

static void draw_window_contents(Win *w, int inactive) {
    context_set((int)(w - wins));
    int wx = w->x, wy = w->y, ww = w->w, wh = w->h;
    int fl = inactive ? WIN_INACTIVE : 0;
    if(w->kind==WK_PROPERTIES){
        gui_draw_window(wx,wy,ww,wh,"Properties",0,fl);
        int y=wy+TITLE_H+14;char row[96],number[16];
        #define PROP(text) do { draw_string_clip(text,wx+14,y,ui_text,wx+ww-14);y+=24; }while(0)
        if(fs_valid(properties_id)){
            PROP(fs_name(properties_id));
            PROP(fs_is_dir(properties_id)?"Type: folder":fs_is_app(properties_id)?"Type: application":"Type: file");
            fmt_uint(number,fs_size(properties_id));kstrcpy(row,"Bytes: ");kstrcpy(row+7,number);PROP(row);
            unsigned stamp=fs_modified(properties_id);
            if(!stamp)PROP("Modified: unknown (older file)");
            else {unsigned days=stamp/86400,year=2000,month=1;int md[]={31,28,31,30,31,30,31,31,30,31,30,31};
                for(;;){unsigned yd=(year%4==0&&(year%100!=0||year%400==0))?366:365;if(days<yd)break;days-=yd;year++;}
                md[1]=(year%4==0&&(year%100!=0||year%400==0))?29:28;
                while(month<12&&days>=(unsigned)md[month-1])days-=md[month++-1];
                kstrcpy(row,"Modified: ");fmt_uint(row+10,year);int n=kstrlen(row);row[n++]='-';fmt_pad2(row+n,month);n+=2;row[n++]='-';fmt_pad2(row+n,days+1);n+=2;row[n++]=' ';fmt_pad2(row+n,stamp/3600%24);n+=2;row[n++]=':';fmt_pad2(row+n,stamp/60%60);n+=2;kstrcpy(row+n," UTC");PROP(row);
            }
        }else PROP("File is no longer available.");
        fmt_uint(number,fs_used_bytes());kstrcpy(row,"Volume bytes used: ");kstrcpy(row+19,number);PROP(row);
        fmt_uint(number,FS_MAX_NODES-fs_node_count());kstrcpy(row,"Free file/folder slots: ");kstrcpy(row+kstrlen(row),number);PROP(row);
        PROP("Maximum file size: 16383 bytes");
        PROP(fs_storage_status()?fs_storage_status():"Disk is synchronized");
        #undef PROP
    } else if (w->kind == WK_HELLO) {
        gui_draw_window(wx, wy, ww, wh, "Hello", 0, fl);
        int lw = logo_string_w("BaseOS");
        draw_logo_string("BaseOS", wx + (ww - lw) / 2, wy + TITLE_H + 26, ui_accent);
        draw_string_in_win("Hello, world. Everything you see is drawn from scratch.", wx, ww,
                           wy + TITLE_H + 26 + LOGO_FONT_H + 10, ui_text_dim);
    } else if (w->kind == WK_HELP) {
        gui_draw_window(wx, wy, ww, wh, "Help", 0, fl);
        int y = wy + TITLE_H + 18;
        int x = wx + 24;
        static const char *tips[] = {
            "Double-click a desktop icon to launch an app",
            "File menu: New, New Folder, Open, Save, Duplicate",
            "Drag files onto Trash to remove them, System > Empty Trash to purge",
            "Ctrl+Space opens the launcher, Ctrl+Tab cycles windows",
            "Ctrl+N new, Ctrl+S save, Ctrl+W close, Ctrl+M minimize",
            "Escape or the close button leaves a window",
        };
        draw_string_bold("Getting around", x, y, ui_text);
        y += LINE_H + 8;
        for (int t = 0; t < 6; t++) {
            draw_round_rect(x, y + CHAR_H / 2 - 2, 6, 6, 3, ui_accent);
            draw_string(tips[t], x + 16, y, ui_text);
            y += LINE_H + 4;
        }
    } else if (w->kind == WK_ABOUT) {
        draw_about(wx, wy, ww, wh, fl);
    } else if (w->kind == WK_SETTINGS) {
        draw_settings(wx, wy, ww, wh, fl);
    } else if (w->kind == WK_FILES) {
        fm_refresh();
        draw_files(wx, wy, ww, wh, inactive);
    } else if (w->kind == WK_EDIT) {
        draw_editor(wx, wy, ww, wh, inactive);
    } else if (w->kind == WK_CALC) {
        draw_calc(wx, wy, ww, wh, inactive);
    } else if (w->kind == WK_PAINT) {
        draw_paint(wx, wy, ww, wh, inactive);
    } else if (w->kind == WK_VIEW) {
        draw_view(wx, wy, ww, wh, inactive);
    } else if (w->kind == WK_SNAKE) {
        draw_snake(wx, wy, ww, wh, inactive);
    } else if (w->kind == WK_WORDLE) {
        gui_draw_window(wx, wy, ww, wh, "Wordle", 0, fl);
        wordle_draw(wx, wy + TITLE_H);
    } else if (w->kind == WK_TERM) {
        draw_term(wx, wy, ww, wh, inactive);
    } else if (w->kind == WK_TODO) {
        draw_todo(wx, wy, ww, wh, inactive);
    } else if (w->kind == WK_CLOCK) {
        gui_draw_window(wx, wy, ww, wh, "Clock", 0, fl);
        clock_draw(wx, wy + TITLE_H + 1, ww, wh - TITLE_H - 1);
    } else if (w->kind == WK_CAL) {
        gui_draw_window(wx, wy, ww, wh, "Calendar", 0, fl);
        cal_draw(wx, wy + TITLE_H + 1, ww, wh - TITLE_H - 1);
    } else if (w->kind == WK_MINES) {
        gui_draw_window(wx, wy, ww, wh, "Minesweeper", 0, fl);
        mines_draw(wx, wy + TITLE_H + 1);
    } else if (w->kind == WK_2048) {
        gui_draw_window(wx, wy, ww, wh, "2048", 0, fl);
        g2048_draw(wx, wy + TITLE_H + 1);
    } else if (w->kind == WK_BREAKOUT) {
        gui_draw_window(wx, wy, ww, wh, "Breakout", 0, fl);
        bo_draw(wx, wy + TITLE_H + 1);
    } else if (w->kind == WK_SYSMON) {
        gui_draw_window(wx, wy, ww, wh, "System Monitor", 0, fl);
        SysInfo si;
        sysinfo_fill(&si);
        sysmon_draw(wx, wy + TITLE_H + 1, ww, wh - TITLE_H - 1, &si);
    }
}

static void draw_one_window(Win *w,int inactive) {
    uint8_t corners[256];
    gfx_window_corners(w->x-1,w->y-1,w->w+2,w->h+2,corners,0,ui_border);
    draw_window_contents(w,inactive);
    gfx_window_corners(w->x-1,w->y-1,w->w+2,w->h+2,corners,1,ui_border);
}
static int render_skip=-1,drag_cached=-1;
void draw_ui(void) {
    int order[MAX_WIN];
    int n = 0;
    int front = win_front();
    wins_by_z(order, &n, 0); /* back to front */
    for (int k = 0; k < n; k++)
        if(order[k]!=render_skip)draw_one_window(&wins[order[k]], order[k] != front);
    context_set(front);
}

static void taskbar_layout(void) {
    int order[MAX_WIN];
    int n = 0;
    for (int i = 0; i < MAX_WIN; i++) {
        if (wins[i].open)
            order[n++] = i;
    }
    for (int a = 0; a < n; a++) {
        for (int b = a + 1; b < n; b++) {
            if (wins[order[b]].seq < wins[order[a]].seq) {
                int t = order[a];
                order[a] = order[b];
                order[b] = t;
            }
        }
    }
    tb_n = 0;
    int total=0;
    int limit=n?(fb_w-128-(n-1)*6)/n:0;
    for (int k = 0; k < n; k++) {
        int i = order[k];
        int tile_w = 42 + ui_string_w(win_app_name(wins[i].kind));
        if(tile_w>limit)tile_w=limit;
        tb_id[tb_n] = i;
        tb_w[tb_n++] = tile_w;
        total+=tile_w+6;
    }
    if(total)total-=6;
    int x=112+(fb_w-124-total)/2;
    for(int i=0;i<tb_n;i++){tb_x[i]=x;x+=tb_w[i]+6;}
}

static int taskbar_hover = -2;
static int taskbar_hover_at(void) {
    if(open_dlg||name_dlg||launcher_on||mouse_y<TASKBAR_Y)return -2;
    if(hit(mouse_x,mouse_y,8,TASKBAR_Y+4,90,TASKBAR_H-8))return -1;
    taskbar_layout();
    for(int t=0;t<tb_n;t++)if(hit(mouse_x,mouse_y,tb_x[t],TASKBAR_Y+4,tb_w[t],TASKBAR_H-8))return tb_id[t];
    return -2;
}
static void draw_taskbar(void) {
    taskbar_layout();
    int y = TASKBAR_Y;
    draw_rect(0,y,fb_w,TASKBAR_H,ui_chrome);
    draw_hline(0,y,fb_w,ui_chrome_dk);
    int front = win_front();
    int ty = y + (TASKBAR_H - CHAR_H) / 2;
    int iy = y + (TASKBAR_H - 16) / 2;
    draw_round_rect(8,y+6,90,TASKBAR_H-12,7,launcher_on?ui_accent:(taskbar_hover==-1?ui_chrome_dk:COLOR_WHITE));
    uint8_t appink=launcher_on?COLOR_WHITE:ui_text;
    for(int j=0;j<2;j++)for(int i=0;i<2;i++)draw_round_rect(19+i*7,y+16+j*7,5,5,1,appink);
    draw_string("Apps",41,ty,appink);
    for (int t = 0; t < tb_n; t++) {
        int i = tb_id[t];
        const char *title = win_app_name(wins[i].kind);
        int active = (i == front);
        uint8_t ink = active ? ui_accent_dk : (wins[i].min ? ui_text_dim : ui_text);
        if(!active&&taskbar_hover==i)draw_round_rect(tb_x[t],y+6,tb_w[t],TASKBAR_H-12,7,ui_chrome_dk);
        if(active)draw_round_rect(tb_x[t],y+6,tb_w[t],TASKBAR_H-12,7,COLOR_WHITE);
        draw_tb_icon(wins[i].kind,tb_x[t]+8,iy,ink);
        draw_string_clip(title,tb_x[t]+30,ty,ink,tb_x[t]+tb_w[t]-6);
        if(!wins[i].min)draw_round_rect(tb_x[t]+tb_w[t]/2-7,y+TASKBAR_H-5,14,2,1,active?ui_accent:ui_chrome_dk);
    }
    if(tb_n==0)draw_string("Ctrl + Space to search",112,ty,ui_text_dim);
}

static int taskbar_hit(void) {
    if (mouse_y < TASKBAR_Y)
        return 0;
    if (hit(mouse_x,mouse_y,8,TASKBAR_Y+4,90,TASKBAR_H-8)) { launcher_open(); return 1; }
    taskbar_layout();
    for (int t = 0; t < tb_n; t++) {
        if (hit(mouse_x, mouse_y, tb_x[t], TASKBAR_Y, tb_w[t], TASKBAR_H)) {
            int id = tb_id[t];
            if (!wins[id].min && win_front() == id)
                win_minimize(id);
            else
                win_focus(id);
            return 1;
        }
    }
    return 1; /* swallow clicks on empty strip */
}

/* ---------- Input handling ---------- */

static int close_hit(int wx, int wy) {
    /* Open is a modal: no close box, and that corner must not dismiss. */
    if (open_dlg)
        return 0;
    int cx, cy;
    close_box_pos(wx, wy, &cx, &cy);
    return hit(mouse_x, mouse_y, cx-3, cy-3, CLOSE_S+6, CLOSE_S+6);
}

static int min_hit(int wx, int wy) {
    if (open_dlg)
        return 0;
    int cx, cy;
    min_box_pos(wx, wy, &cx, &cy);
    return hit(mouse_x, mouse_y, cx-3, cy-3, CLOSE_S+6, CLOSE_S+6);
}

static int fm_name_hit(int wx, int iy, int id) {
    /* Label side of the row: past the 12px icon, not the icon itself. */
    int nx = wx + 8 + 20;
    int ny = iy;
    int nh = ROW_H;
    int nw;
    if (fm_renaming && fm_rename_id == id) {
        nw = ui_string_w(fm_rename_buf) + 24;
        if (nw < 100)
            nw = 100;
    } else {
        nw = ui_string_w(fs_name(id)) + 24;
        if (nw < 100)
            nw = 100;
    }
    return hit(mouse_x, mouse_y, nx, ny, nw, nh);
}

static int click_on_rename_field(void) {
    int wx, wy, ww, wh;
    int up, row, iy, i;
    if (!fm_renaming)
        return 0;
    if (!win_geom_kind(WK_FILES, &wx, &wy, &ww, &wh))
        return 0;
    up = fm_has_parent() ? 1 : 0;
    row = -1;
    for (i = 0; i < fm_count; i++) {
        if (fm_ids[i] == fm_rename_id) {
            row = i + up;
            break;
        }
    }
    if (row < 0)
        return 0;
    iy = files_list_y(wy) + (row-fm_first) * ROW_H;
    return fm_name_hit(wx, iy, fm_rename_id);
}

static void handle_files_click(int wx, int wy, int ww, int wh) {
    int list_y = files_list_y(wy);
    int list_b = wy + wh - 4 - SB;
    int rows = (list_b - list_y) / ROW_H;
    int lw = ww - 16 - SB;
    int vis = fm_vis_count();
    int up = fm_has_parent() ? 1 : 0;
    files_scroll(rows);
    for (int i = fm_first; i < vis && i < fm_first+rows; i++) {
        int iy = list_y + (i-fm_first) * ROW_H;
        if (hit(mouse_x, mouse_y, wx + 8, iy, lw, ROW_H)) {
            int id = fm_row_id(i);
            int already = (i == fm_selected);
            int dbl = double_click(i, &fm_last_click_item, &fm_last_click_frame);
            int fast = dbl;
            dirty = 1;
            if (up && i == 0) {
                /* Click or double-click on ".." goes up. Synthetic row. */
                fm_rename_cancel();
                fm_selected = i;
                fm_dragging = 0;
                fm_drag_active = 0;
                fm_drag_id = -1;
                fm_go_up();
                return;
            }
            if (fast) {
                fm_rename_cancel();
                fm_selected = i;
                fm_dragging = 0;
                fm_drag_active = 0;
                fm_drag_id = -1;
                fm_open_selected();
                return;
            }
            /* Name of an already-selected row: rename-in-place.
             * Hit anywhere on the label side (past the icon). */
            if (already && id >= 0 && mouse_x >= wx + 8 + 20) {
                fm_selected = i;
                fm_dragging = 0;
                fm_drag_active = 0;
                fm_drag_id = -1;
                fm_rename_begin(id);
                return;
            }
            if (fm_renaming && id == fm_rename_id) {
                fm_dragging = 0;
                fm_drag_active = 0;
                fm_drag_id = -1;
                return;
            }
            fm_rename_cancel();
            fm_selected = i;
            fm_dragging = 1;
            fm_drag_active = 0;
            fm_drag_id = fm_ids[i - up];
            fm_drag_sx = mouse_x;
            fm_drag_sy = mouse_y;
            return;
        }
    }
    fm_rename_cancel();
}

static void files_drop(void) {
    int id = fm_drag_id;
    int active = fm_drag_active;
    fm_dragging = 0;
    fm_drag_active = 0;
    fm_drag_id = -1;
    if (!active || !fs_valid(id))
        return;
    if (id == trash_id)
        return;
    if (!icon_hit(ICON_TRASH, mouse_x, mouse_y))
        return;
    if (trash_id < 0)
        return;
    /* Already in the trash can: drop is a no-op. */
    if (fs_parent(id) == trash_id)
        return;
    if (fs_move(id, trash_id) < 0)
        return;
    kprint_debug("Trash drop\n");
    if (find_open_kind(WK_FILES) >= 0) {
        if (fm_cwd == id) {
            int p = fs_parent(id);
            fm_cwd = (p >= 0) ? p : fs_root();
        }
        fm_refresh();
    }
    if (open_dlg)
        od_refresh();
    dirty = 1;
}

static void draw_drag_ghost(void) {
    if (!fm_drag_active || !fs_valid(fm_drag_id))
        return;
    int w = 18, h = 18;
    int x = mouse_x + 12;
    int y = mouse_y + 8;
    if (x + w > fb_w - 2) x = mouse_x - w - 6;
    if (y + h > fb_h - 2) y = mouse_y - h - 6;
    if (x < 2) x = 2;
    if (y < MENUBAR_H) y = MENUBAR_H;
    draw_rect(x, y, w, 1, COLOR_BLACK);
    draw_rect(x, y + h - 1, w, 1, COLOR_BLACK);
    draw_rect(x, y, 1, h, COLOR_BLACK);
    draw_rect(x + w - 1, y, 1, h, COLOR_BLACK);
}

static int edit_index_at(int wx, int wy, int ww, int wh, int mx, int my) {
    int ax, ay, aw, ah;
    edit_area(wx, wy, ww, wh, &ax, &ay, &aw, &ah);
    int cols = aw / EDIT_CHAR_W;
    if (cols < 1)
        cols = 1;
    int col = (mx - ax) / EDIT_CHAR_W;
    int row = (my - ay) / EDIT_LINE_H + edit_scroll;
    if (col < 0)
        col = 0;
    if (col >= cols)
        col = cols - 1;
    if (row < 0)
        row = 0;
    return edit_cell_to_index(cols, row, col);
}

static void handle_edit_click(int wx, int wy, int ww, int wh) {
    int ax, ay, aw, ah;
    edit_area(wx, wy, ww, wh, &ax, &ay, &aw, &ah);
    if (hit(mouse_x, mouse_y, ax, ay, aw, ah)) {
        int idx = edit_index_at(wx, wy, ww, wh, mouse_x, mouse_y);
        edit_caret = idx;
        edit_sel_a = idx;
        edit_sel_b = idx;
        edit_dragging = 1;
        dirty = 1;
    }
}

static void handle_open_dlg_click(void) {
    int px, py, pw, ph;
    pick_geom(&px, &py, &pw, &ph);
    int ox, oy, ow, cx, cy, cw;
    od_buttons(px, py, pw, ph, &ox, &oy, &ow, &cx, &cy, &cw);

    /* Modal: no close box. Only Cancel dismisses (ESC is handled in keys). */
    if (hit(mouse_x, mouse_y, cx, cy, cw, BTN_H)) {
        close_open_dialog();
        return;
    }
    if (hit(mouse_x, mouse_y, ox - 3, oy - 3, ow + 6, BTN_H + 6)) {
        od_open_selected();
        return;
    }
    int ly = od_list_y(py);
    int lb = oy - 12;
    int rows = (lb - ly) / ROW_H;
    int lw = pw - 16;
    for (int i = pick_first; i < pick_count && i < pick_first+rows; i++) {
        int iy = ly + (i-pick_first) * ROW_H;
        if (hit(mouse_x, mouse_y, px + 8, iy, lw, ROW_H)) {
            if (!od_row_enabled(pick_ids[i]))
                return;
            int dbl =
                double_click(i, &pick_last_click_item, &pick_last_click_frame);
            pick_selected = i;
            dirty = 1;
            if (dbl)
                od_open_selected();
            return;
        }
    }
    /* Modal: swallow clicks on the desktop/window underneath. */
}

static void icon_open(int id) {
    switch (id) {
    case ICON_FILES: open_files(fs_root()); break;
    case ICON_EDIT: open_edit(); break;
    case ICON_CALC: open_calc(); break;
    case ICON_PAINT: open_paint(); break;
    case ICON_VIEW: start_open_dialog(1); break;
    case ICON_SNAKE: open_snake(); break;
    case ICON_WORDLE: open_wordle(); break;
    case ICON_TERM: open_term(); break;
    case ICON_TODO: open_todo(); break;
    case ICON_SETTINGS: win_open(WK_SETTINGS); break;
    case ICON_CLOCK: win_open(WK_CLOCK); break;
    case ICON_CAL: cal_reset(); win_open(WK_CAL); break;
    case ICON_MINES: win_open(WK_MINES); break;
    case ICON_2048: win_open(WK_2048); break;
    case ICON_BREAKOUT: win_open(WK_BREAKOUT); break;
    case ICON_SYSMON: win_open(WK_SYSMON); break;
    case ICON_TRASH: open_files(trash_id >= 0 ? trash_id : fs_root()); break;
    default: break;
    }
    dirty = 1;
}

static void menu_activate(int m, int item) {
    open_menu = MENU_NONE;
    menu_sel = -1;
    fm_last_click_item = -1;
    dirty = 1;

    if (!menu_item_enabled(m, item))
        return;

    if (m == MENU_BASEOS) {
        if (item == 0)
            win_open(WK_ABOUT);
        else if (item == 1)
            launcher_open();
        else if (item == 2)
            win_open(WK_SETTINGS);
        else if (item == 3)
            win_open(WK_HELP);
        return;
    }
    if (m == MENU_FILE) {
        if (item == 0) {
            open_dlg = 0;
            if (front_kind() == WK_PAINT) {
                paint_clear();
            } else if (front_kind() == WK_SNAKE) {
                snake_reset();
            } else if (front_kind() == WK_WORDLE) {
                wordle_new_game(frame_count);
            } else if (front_kind() == WK_TODO) {
                todo_focus_field();
            } else {
                /* File -> New opens the editor. */
                if (front_kind() == WK_FILES) open_files(fm_cwd);
                else if (front_kind() == WK_TERM) open_term();
                else open_edit();
            }
        } else if (item == 1) {
            do_new_folder();
        } else if (item == 2) {
            start_open_dialog(front_kind() == WK_VIEW);
        } else if (item == 3) {
            close_front();
        } else if (item == 4) {
            if (front_kind() == WK_PAINT && !open_dlg) {
                paint_save();
            } else if (front_kind() == WK_EDIT && !open_dlg) {
                edit_save();
                dirty = 1;
            }
        } else if (item == 6) {
            properties_id=fm_row_id(fm_selected);win_open(WK_PROPERTIES);
        } else if (item == 5) {
            do_duplicate();
        }
        return;
    }
    if (m == MENU_EDITM) {
        if (front_kind() == WK_CALC) {
            if (item == 1)
                calc_copy();
            else if (item == 2)
                calc_paste();
        } else if (item == 0)
            edit_cut();
        else if (item == 1)
            edit_copy();
        else if (item == 2)
            edit_paste();
        dirty = 1;
        return;
    }
    if (m == MENU_SPECIAL) {
        if (item == 0) {
            do_empty_trash();
        } else if (item == 1) {
            show_desktop();
        } else if (item == 2) {
            saver_start();
        } else if (item == 4) {
            do_shutdown();
        }
        return;
    }
}

typedef struct {
    const char *name;
    int icon;
    int file;
} LaunchItem;

static LaunchItem launch_items[24];
static int launch_n = 0;

static int str_has(const char *hay, const char *needle) {
    if (!needle[0])
        return 1;
    for (int i = 0; hay[i]; i++) {
        int k = 0;
        while (needle[k] && hay[i + k]) {
            char a = hay[i + k], b = needle[k];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b)
                break;
            k++;
        }
        if (!needle[k])
            return 1;
    }
    return 0;
}

static void launcher_refresh(void) {
    launch_n = 0;
    launch_buf[launch_len] = 0;
    for (int i = 0; i < ICON_TRASH && launch_n < 24; i++) {
        if (str_has(icons[i].label, launch_buf)) {
            launch_items[launch_n].name = icons[i].label;
            launch_items[launch_n].icon = i;
            launch_items[launch_n].file = -1;
            launch_n++;
        }
    }
    int ids[FS_MAX_NODES];
    int n = fs_list_files(ids, FS_MAX_NODES);
    for (int i = 0; i < n && launch_n < 24; i++) {
        if (fs_is_app(ids[i]) || fs_is_dir(ids[i]))
            continue;
        if (launch_len == 0 && launch_n >= 8)
            break;
        if (str_has(fs_name(ids[i]), launch_buf)) {
            launch_items[launch_n].name = fs_name(ids[i]);
            launch_items[launch_n].icon = -1;
            launch_items[launch_n].file = ids[i];
            launch_n++;
        }
    }
    if (launch_sel >= launch_n)
        launch_sel = launch_n ? launch_n - 1 : 0;
    if (launch_sel < 0)
        launch_sel = 0;
}

static void launcher_open(void) {
    launcher_on = 1;
    launch_len = 0;
    launch_sel = 0;
    open_menu = MENU_NONE;
    launcher_refresh();
    dirty = 1;
}

static void launcher_close(void) {
    launcher_on = 0;
    dirty = 1;
}

static void launcher_geom(int *x, int *y, int *w, int *h) {
    int rows = launch_n < 8 ? launch_n : 8;
    *w = 520;
    *h = 64 + rows * 30 + (rows ? 12 : 0);
    *x = (fb_w - *w) / 2;
    *y = MENUBAR_H + 90;
}

static void launcher_run(int i) {
    if (i < 0 || i >= launch_n)
        return;
    LaunchItem it = launch_items[i];
    launcher_close();
    if (it.file >= 0)
        open_fs_file(it.file);
    else
        icon_open(it.icon);
}

static void launcher_key(void) {
    if (key_sc == KEY_ESC) {
        launcher_close();
    } else if (key_sc == KEY_ENTER) {
        launcher_run(launch_sel);
    } else if (key_sc == KEY_UP) {
        if (launch_sel > 0)
            launch_sel--;
        dirty = 1;
    } else if (key_sc == KEY_DOWN) {
        if (launch_sel + 1 < launch_n)
            launch_sel++;
        dirty = 1;
    } else if (key_sc == KEY_BACKSPACE) {
        if (launch_len > 0)
            launch_len--;
        launcher_refresh();
        dirty = 1;
    } else if (key_char && launch_len < 22) {
        launch_buf[launch_len++] = key_char;
        launcher_refresh();
        dirty = 1;
    }
}

static void draw_launcher(void) {
    if (!launcher_on)
        return;
    int x, y, w, h;
    launcher_geom(&x, &y, &w, &h);
    shade_rect(0, MENUBAR_H, fb_w, TASKBAR_Y - MENUBAR_H, 1);
    draw_shadow(x, y, w, h);
    draw_round_rect(x - 1, y - 1, w + 2, h + 2, 11, ui_border);
    draw_round_rect(x, y, w, h, 10, COLOR_WHITE);
    int fx = x + 16, fy = y + 14, fw = w - 32, fh = 36;
    draw_round_rect(fx, fy, fw, fh, 8, ui_accent);
    draw_round_rect(fx + 1, fy + 1, fw - 2, fh - 2, 7, gfx_gray(0xF6));
    int r = 7;
    draw_round_frame(fx + 12, fy + 9, 2 * r, 2 * r, r, ui_text_dim);
    draw_round_frame(fx + 13, fy + 10, 2 * r - 2, 2 * r - 2, r - 1, ui_text_dim);
    draw_line(fx + 12 + 2 * r - 3, fy + 9 + 2 * r - 3, fx + 12 + 2 * r + 2, fy + 9 + 2 * r + 2, ui_text_dim);
    draw_line(fx + 12 + 2 * r - 2, fy + 9 + 2 * r - 3, fx + 12 + 2 * r + 3, fy + 9 + 2 * r + 2, ui_text_dim);
    launch_buf[launch_len] = 0;
    int tx = fx + 40;
    int ty = fy + (fh - CHAR_H) / 2;
    if (launch_len)
        draw_string(launch_buf, tx, ty, ui_text);
    else
        draw_string("Search apps and files", tx, ty, ui_text_dim);
    int cx = tx + ui_string_w(launch_buf);
    if ((frame_count / 35) & 1)
        draw_rect(cx + 1, fy + 9, 2, fh - 18, ui_accent);
    int ly = fy + fh + 10;
    int rows = launch_n < 8 ? launch_n : 8;
    for (int i = 0; i < rows; i++) {
        int ry = ly + i * 30;
        int sel = (i == launch_sel);
        if (sel)
            draw_round_rect(fx, ry, fw, 28, 6, ui_chrome);
        if(sel)draw_round_rect(fx,ry+7,3,14,1,ui_accent);
        uint8_t ink = ui_text;
        if (launch_items[i].icon >= 0) {
            const DeskIcon *ic = &icons[launch_items[i].icon];
            draw_round_rect(fx + 8, ry + 4, 20, 20, 5, idx24(ic->color));
            draw_icon_art(fx + 10, ry + 6, ic->art, 1, COLOR_WHITE, -1);
            draw_string_clip(launch_items[i].name, fx + 38, ry + 5, ink, fx + fw - 120);
            draw_string("Application", fx + fw - 12 - ui_string_w("Application"), ry + 5, ui_text_dim);
        } else {
            draw_mini_doc(fx + 12, ry + 8, ink, sel ? ui_chrome : COLOR_WHITE);
            draw_string_clip(launch_items[i].name, fx + 38, ry + 5, ink, fx + fw - 120);
            draw_string("File", fx + fw - 12 - ui_string_w("File"), ry + 5, ui_text_dim);
        }
    }
    if (launch_n == 0)
        draw_string("No matches", fx + 4, ly - 4, ui_text_dim);
}

static int launcher_click(void) {
    int x, y, w, h;
    launcher_geom(&x, &y, &w, &h);
    if (!hit(mouse_x, mouse_y, x, y, w, h)) {
        launcher_close();
        return 1;
    }
    int ly = y + 14 + 36 + 10;
    int rows = launch_n < 8 ? launch_n : 8;
    for (int i = 0; i < rows; i++) {
        if (hit(mouse_x, mouse_y, x + 16, ly + i * 30, w - 32, 28)) {
            launcher_run(i);
            return 1;
        }
    }
    return 1;
}

static void saver_start(void) {
    saver_on = 1;
    open_menu = MENU_NONE;
}

static void saver_stop(void) {
    saver_on = 0;
    last_input_frame = frame_count;
    dirty = 1;
}

static void saver_frame(void) {
    static int lx = -1, ly, vx = 2, vy = 2;
    static int inited = 0;
    static int star_x[64], star_y[64], star_v[64];
    int size = 96;
    if (!inited) {
        inited = 1;
        lx = fb_w / 2 - size / 2;
        ly = fb_h / 2 - size / 2;
        unsigned r = 12345;
        for (int i = 0; i < 64; i++) {
            r = r * 1103515245u + 12345u;
            star_x[i] = (int)((r >> 8) % (unsigned)fb_w);
            r = r * 1103515245u + 12345u;
            star_y[i] = (int)((r >> 8) % (unsigned)fb_h);
            star_v[i] = 1 + (int)(i % 3);
        }
    }
    draw_rect(0, 0, fb_w, fb_h, gfx_rgb(0x06, 0x08, 0x10));
    for (int i = 0; i < 64; i++) {
        star_y[i] += star_v[i];
        if (star_y[i] >= fb_h) {
            star_y[i] = 0;
            star_x[i] = (star_x[i] * 7 + 131) % fb_w;
        }
        uint8_t c = star_v[i] == 3 ? COLOR_WHITE : (star_v[i] == 2 ? gfx_gray(0xA0) : gfx_gray(0x60));
        put_pixel(star_x[i], star_y[i], c);
        if (star_v[i] == 3)
            put_pixel(star_x[i], star_y[i] + 1, c);
    }
    lx += vx;
    ly += vy;
    if (lx <= 0 || lx + size >= fb_w) vx = -vx;
    if (ly <= 0 || ly + size + 40 >= fb_h) vy = -vy;
    draw_logo(lx, ly, size);
    int tw = uib_string_w("BaseOS");
    draw_string_bold("BaseOS", lx + (size - tw) / 2, ly + size + 14, gfx_gray(0xB0));
    cursor_on = 0;
    flip_vga();
}

static void sysinfo_fill(SysInfo *si) {
    si->uptime_sec = (unsigned)(frame_count / 70);
    si->frames = (unsigned)frame_count;
    si->win_n = 0;
    int order[MAX_WIN];
    int n = 0;
    for (int i = 0; i < MAX_WIN; i++)
        if (wins[i].open)
            order[n++] = i;
    for (int a = 0; a < n; a++)
        for (int b = a + 1; b < n; b++)
            if (wins[order[b]].seq < wins[order[a]].seq) {
                int t = order[a];
                order[a] = order[b];
                order[b] = t;
            }
    for (int k = 0; k < n && k < SYSMON_MAX_WIN; k++) {
        int i = order[k];
        si->win_name[k] = win_app_name(wins[i].kind);
        si->win_id[k] = i;
        si->win_min[k] = wins[i].min;
        si->win_n++;
    }
    int ids[FS_MAX_NODES];
    int total = fs_list_files(ids, FS_MAX_NODES);
    int bytes = 0;
    for (int i = 0; i < total; i++)
        bytes += fs_size(ids[i]);
    si->fs_nodes = fs_node_count();
    si->fs_max = FS_MAX_NODES;
    si->fs_bytes = bytes;
    si->fs_cap = fs_capacity();
    si->fb_w = fb_w;
    si->fb_h = fb_h;
    si->fb_bpp = fb_bpp;
    si->mem_mb = platform_memory_mb();
    si->redraws = redraw_count;
}

static int name_target = 0;
static char name_buf[FS_NAME_LEN];
static int name_len = 0;
static int name_focus;

static void namedlg_open(int target, const char *initial) {
    name_dlg = 1;
    name_focus = 0;
    name_target = target;
    name_len = 0;
    for (int i = 0; initial && initial[i] && i < FS_NAME_LEN - 1; i++)
        name_buf[name_len++] = initial[i];
    name_buf[name_len] = 0;
    dirty = 1;
}

static void namedlg_close(void) {
    name_dlg = 0;
    dirty = 1;
}

static void namedlg_geom(int *x, int *y, int *w, int *h) {
    *w = 420;
    *h = 168;
    *x = (fb_w - *w) / 2;
    *y = MENUBAR_H + 140;
}

static void namedlg_buttons(int x, int y, int w, int h, int *sx, int *cx, int *by, int *bw) {
    *bw = 96;
    *by = y + h - 20 - BTN_H;
    *sx = x + w - 20 - *bw;
    *cx = *sx - 12 - *bw;
}

static void namedlg_commit(void) {
    name_buf[name_len] = 0;
    if (name_len == 0)
        return;
    int ok = name_target == 0 ? edit_write_named(name_buf)
                              : paint_write_named(name_buf);
    if (ok)
        namedlg_close();
}

static void draw_namedlg(void) {
    if (!name_dlg)
        return;
    int x, y, w, h;
    namedlg_geom(&x, &y, &w, &h);
    shade_rect(0, MENUBAR_H, fb_w, TASKBAR_Y - MENUBAR_H, 1);
    draw_shadow(x, y, w, h);
    draw_round_rect(x - 1, y - 1, w + 2, h + 2, 11, ui_border);
    draw_round_rect(x, y, w, h, 10, COLOR_WHITE);
    draw_string_bold(name_target == 0 ? "Save document as" : "Save picture as",
                     x + 22, y + 20, ui_text);
    draw_string(name_target == 0 ? "Name your file in the current folder"
                                 : "Saved to the Pictures folder",
                x + 22, y + 20 + CHAR_H + 6, ui_text_dim);
    int fx = x + 22, fy = y + 64, fw = w - 44, fh = 34;
    draw_round_rect(fx, fy, fw, fh, 7, ui_accent);
    draw_round_rect(fx + 1, fy + 1, fw - 2, fh - 2, 6, gfx_gray(0xF7));
    name_buf[name_len] = 0;
    draw_string(name_buf, fx + 12, fy + (fh - CHAR_H) / 2, ui_text);
    int cx2 = fx + 12 + ui_string_w(name_buf);
    if ((frame_count / 35) & 1)
        draw_rect(cx2 + 1, fy + 8, 2, fh - 16, ui_accent);
    int sx, cx, by, bw;
    namedlg_buttons(x, y, w, h, &sx, &cx, &by, &bw);
    draw_button(cx, by, bw, BTN_H, "Cancel");
    draw_default_button(sx, by, bw, BTN_H, "Save");
    if (name_focus) draw_frame((name_focus==1?cx:sx)-3,by-3,bw+6,BTN_H+6,ui_accent);
}

static int namedlg_click(void) {
    int x, y, w, h;
    namedlg_geom(&x, &y, &w, &h);
    if (!hit(mouse_x, mouse_y, x, y, w, h)) {
        namedlg_close();
        return 1;
    }
    int sx, cx, by, bw;
    namedlg_buttons(x, y, w, h, &sx, &cx, &by, &bw);
    if (hit(mouse_x, mouse_y, sx, by, bw, BTN_H))
        namedlg_commit();
    else if (hit(mouse_x, mouse_y, cx, by, bw, BTN_H))
        namedlg_close();
    return 1;
}

static void namedlg_key(void) {
    if (key_sc == KEY_TAB) { name_focus=(name_focus+(shift_down?2:1))%3;dirty=1;return; }
    if (key_sc == KEY_ENTER && name_focus==1) { namedlg_close();return; }
    if (name_focus && key_sc!=KEY_ENTER && key_sc!=KEY_ESC) return;
    if (key_sc == KEY_ESC)
        namedlg_close();
    else if (key_sc == KEY_ENTER)
        namedlg_commit();
    else if (key_sc == KEY_BACKSPACE) {
        if (name_len > 0)
            name_buf[--name_len] = 0;
        dirty = 1;
    } else if (key_char && key_char != '/' && name_len < FS_NAME_LEN - 1) {
        name_buf[name_len++] = key_char;
        name_buf[name_len] = 0;
        dirty = 1;
    }
}

static void handle_rclick(void) {
    if (launcher_on || open_dlg || open_menu >= 0)
        return;
    int i = win_front();
    if (i < 0)
        return;
    Win *w = &wins[i];
    if (w->kind == WK_MINES && hit(mouse_x, mouse_y, w->x, w->y, w->w, w->h)) {
        if (mines_click(w->x, w->y + TITLE_H + 1, mouse_x, mouse_y, 1))
            dirty = 1;
    }
}

static void handle_click(void) {
    if (name_dlg) {
        namedlg_click();
        return;
    }
    if (launcher_on) {
        launcher_click();
        return;
    }
    /* Keep rename if the click is in the Files body; handle_files_click
     * decides. Clicking chrome/menus/desktop cancels (Return commits). */
    if (fm_renaming && !click_on_rename_field()) {
        int wx, wy, ww, wh;
        int in_body = 0;
        if (win_geom_kind(WK_FILES, &wx, &wy, &ww, &wh) &&
            hit(mouse_x, mouse_y, wx, wy + TITLE_H, ww, wh - TITLE_H))
            in_body = 1;
        if (!in_body)
            fm_rename_cancel();
    }

    if (open_menu >= 0) {
        int item = menu_item_at(open_menu, mouse_x, mouse_y);
        if (item >= 0) {
            menu_activate(open_menu, item);
            return;
        }
        for (int i = 0; i < MENU_N; i++) {
            if (hit(mouse_x, mouse_y, bar_x[i], 0, bar_w[i], MENUBAR_H)) {
                if (i == open_menu) {
                    open_menu = MENU_NONE;
                    menu_sel = -1;
                } else {
                    open_menu = i;
                    menu_sel = menu_item_at(i, mouse_x, mouse_y);
                }
                dirty = 1;
                return;
            }
        }
        open_menu = MENU_NONE;
        menu_sel = -1;
        dirty = 1;
        return;
    }

    for (int i = 0; i < MENU_N; i++) {
        if (hit(mouse_x, mouse_y, bar_x[i], 0, bar_w[i], MENUBAR_H)) {
            open_menu = i;
            menu_sel = -1;
            fm_last_click_item = -1;
            dirty = 1;
            return;
        }
    }

    if (open_dlg) {
        handle_open_dlg_click();
        return;
    }

    /* Taskbar, then windows front-to-back, then desktop icons. */
    if (mouse_y >= TASKBAR_Y) {
        taskbar_hit();
        return;
    }

    int order[MAX_WIN];
    int n = 0;
    wins_by_z(order, &n, 1);
    for (int k = 0; k < n; k++) {
        int i = order[k];
        if (i < 0 || i >= MAX_WIN)
            continue;
        Win *w = &wins[i];
        if (!hit(mouse_x, mouse_y, w->x, w->y, w->w, w->h))
            continue;
        win_focus(i);
        resize_edges = (mouse_x < w->x+4 ? 1 : 0) | (mouse_x >= w->x+w->w-5 ? 2 : 0) |
                       (mouse_y < w->y+3 ? 4 : 0) | (mouse_y >= w->y+w->h-5 ? 8 : 0);
        if (resize_edges) {
            resizing_win=i; drag_from_x=mouse_x; drag_from_y=mouse_y;
            drag_orig_x=w->x; drag_orig_y=w->y; resize_start_w=w->w; resize_start_h=w->h;
            w->maximized=0; return;
        }
        int max_x, max_y; min_box_pos(w->x,w->y,&max_x,&max_y);
        if (hit(mouse_x,mouse_y,max_x+CLOSE_S+3,max_y-3,CLOSE_S+6,CLOSE_S+6)) { win_arrange(i,0); return; }
        if (min_hit(w->x, w->y)) {
            win_minimize(i);
            return;
        }
        if (close_hit(w->x, w->y)) {
            win_close(i);
            return;
        }
        if (hit(mouse_x, mouse_y, w->x, w->y, w->w, TITLE_H)) {
            if (title_click_window == i && frame_count-title_click_time < 28) {
                title_click_window=-1; win_arrange(i,0); return;
            }
            title_click_window=i; title_click_time=frame_count;
            dragging_win = i;
            drag_active = 0;
            drag_from_x = mouse_x;
            drag_from_y = mouse_y;
            drag_orig_x = w->x;
            drag_orig_y = w->y;
            fm_dragging = 0;
            fm_drag_active = 0;
            return;
        }
        if (w->kind == WK_FILES)
            handle_files_click(w->x, w->y, w->w, w->h);
        else if (w->kind == WK_EDIT)
            handle_edit_click(w->x, w->y, w->w, w->h);
        else if (w->kind == WK_CALC)
            handle_calc_click(w->x, w->y, w->w, w->h);
        else if (w->kind == WK_SETTINGS)
            handle_settings_click(w->x, w->y, w->w, w->h);
        else if (w->kind == WK_PAINT)
            handle_paint_click(w->x, w->y, w->w, w->h);
        else if (w->kind == WK_WORDLE) {
            if (wordle_click(w->x, w->y + TITLE_H, mouse_x, mouse_y))
                dirty = 1;
        }
        else if (w->kind == WK_TODO)
            handle_todo_click(w->x, w->y, w->w, w->h);
        else if (w->kind == WK_CAL) {
            if (cal_click(w->x, w->y + TITLE_H + 1, w->w, mouse_x, mouse_y))
                dirty = 1;
        } else if (w->kind == WK_MINES) {
            if (mines_click(w->x, w->y + TITLE_H + 1, mouse_x, mouse_y, shift_down))
                dirty = 1;
        } else if (w->kind == WK_BREAKOUT) {
            bo_click();
            dirty = 1;
        } else if (w->kind == WK_SYSMON) {
            SysInfo si;
            sysinfo_fill(&si);
            int id = sysmon_click(w->x, w->y + TITLE_H + 1, w->w, mouse_x, mouse_y, &si);
            if (id >= 0)
                win_close(id);
        }
        return;
    }

    int hit_icon = -1;
    for (int i = 0; i < ICON_COUNT; i++) {
        if (icon_hit(i, mouse_x, mouse_y))
            hit_icon = i;
    }
    if (hit_icon >= 0) {
        int dbl = double_click(hit_icon, &icon_last, &icon_last_frame);
        icon_sel = hit_icon;
        dirty = 1;
        if (dbl)
            icon_open(hit_icon);
    } else if (icon_sel >= 0) {
        icon_sel = -1;
        dirty = 1;
    }
}

static void launcher_key(void);

static void handle_key(void) {
    context_set(win_front());
    if (!name_dlg && !open_dlg && alt_down && key_sc == KEY_TAB) { win_cycle(); return; }
    if (!name_dlg && !open_dlg && alt_down && key_sc == KEY_ENTER) { win_arrange(win_front(),0); return; }
    if (!name_dlg && !open_dlg && alt_down && (key_sc == KEY_LEFT || key_sc == KEY_RIGHT)) {
        win_arrange(win_front(),key_sc==KEY_LEFT?1:2); return;
    }
    if (name_dlg) {
        namedlg_key();
        return;
    }
    if (launcher_on) {
        launcher_key();
        return;
    }
    if (ctrl_down && !open_dlg) {
        if(key_sc==0x17 && front_kind()==WK_FILES){menu_activate(MENU_FILE,6);return;}
        if (key_sc == 0x2c || key_sc == 0x15) {
            int redo = key_sc == 0x15 || shift_down;
            if (front_kind() == WK_EDIT) edit_undo(redo);
            else if (front_kind() == WK_PAINT) paint_undo(redo);
            return;
        }
        if (key_sc == 0x18) { start_open_dialog(0); return; }
        if (front_kind()==WK_EDIT && key_sc==0x1e) { edit_sel_a=0;edit_sel_b=edit_len;dirty=1;return; }
        if (front_kind()==WK_EDIT && key_sc==0x2e) { edit_copy();return; }
        if (front_kind()==WK_EDIT && key_sc==0x2d) { edit_cut();dirty=1;return; }
        if (front_kind()==WK_EDIT && key_sc==0x2f) { edit_paste();dirty=1;return; }

        if (key_sc == KEY_SPACE) {
            launcher_open();
            return;
        }
        if (key_sc == KEY_TAB) {
            open_menu = MENU_NONE;
            win_cycle();
            return;
        }
        if (key_sc == KEY_M) {
            win_minimize(win_front());
            return;
        }
        if (key_sc == KEY_W) {
            close_front();
            dirty = 1;
            return;
        }
        if (key_sc == KEY_N) {
            menu_activate(MENU_FILE, 0);
            return;
        }
        if (key_sc == KEY_S && (front_kind() == WK_PAINT || front_kind() == WK_EDIT)) {
            menu_activate(MENU_FILE, 4);
            return;
        }
    }
    if(key_sc==0x44){open_menu=MENU_FILE;menu_sel=0;dirty=1;return;}
    if (open_menu >= 0) {
        if(key_sc==KEY_LEFT||key_sc==KEY_RIGHT){open_menu=(open_menu+(key_sc==KEY_LEFT?MENU_N-1:1))%MENU_N;menu_sel=0;}
        else if(key_sc==KEY_UP||key_sc==KEY_DOWN){
            int d=key_sc==KEY_UP?-1:1;
            for(int n=0;n<menu_count[open_menu];n++){menu_sel=(menu_sel+d+menu_count[open_menu])%menu_count[open_menu];if(menu_item_enabled(open_menu,menu_sel)&&kstrcmp(menu_items[open_menu][menu_sel],"-"))break;}
        }else if(key_sc==KEY_ENTER && menu_sel>=0 && menu_item_enabled(open_menu,menu_sel)) {int m=open_menu,i=menu_sel;open_menu=MENU_NONE;menu_activate(m,i);}
        dirty=1;
        if (key_sc == KEY_ESC) {
            open_menu = MENU_NONE;
            menu_sel = -1;
            dirty = 1;
        }
        return;
    }

    if (open_dlg) {
        if(key_sc==KEY_TAB){pick_focus=(pick_focus+(shift_down?2:1))%3;dirty=1;return;}
        if(key_sc==KEY_ENTER&&pick_focus==1){close_open_dialog();return;}
        if (key_sc == KEY_UP) {
            if (pick_count)
                pick_selected = od_next_enabled(pick_selected, -1);
            dirty = 1;
        } else if (key_sc == KEY_DOWN) {
            if (pick_count)
                pick_selected = od_next_enabled(pick_selected, 1);
            dirty = 1;
        } else if (key_sc == KEY_ENTER) {
            od_open_selected();
        } else if (key_sc == KEY_ESC) {
            close_open_dialog();
        }
        return;
    }

    int fk = front_kind();
    if (fk == WK_NONE) {
        if (key_sc==KEY_TAB || key_sc==KEY_RIGHT || key_sc==KEY_DOWN)
            icon_sel=(icon_sel+1)%ICON_COUNT;
        else if(key_sc==KEY_LEFT || key_sc==KEY_UP)icon_sel=(icon_sel+ICON_COUNT-1)%ICON_COUNT;
        else if(key_sc==KEY_ENTER && icon_sel>=0)icon_open(icon_sel);
        dirty=1;return;
    }

    if (fk == WK_FILES) {
        if (fm_renaming) {
            if (key_sc == KEY_ESC) {
                fm_rename_cancel();
            } else if (key_sc == KEY_ENTER) {
                fm_rename_commit();
            } else if (key_sc == KEY_BACKSPACE) {
                if (fm_rename_len > 0) {
                    fm_rename_buf[--fm_rename_len] = 0;
                    dirty = 1;
                }
            } else if (key_char && fm_rename_len < FS_NAME_LEN - 1) {
                fm_rename_buf[fm_rename_len++] = key_char;
                fm_rename_buf[fm_rename_len] = 0;
                dirty = 1;
            }
            return;
        }
        int vis = fm_vis_count();
        if (key_sc == KEY_UP) {
            if (vis)
                fm_selected = (fm_selected - 1 + vis) % vis;
            dirty = 1;
        } else if (key_sc == KEY_DOWN) {
            if (vis)
                fm_selected = (fm_selected + 1) % vis;
            dirty = 1;
        } else if (key_sc == KEY_ENTER) {
            fm_open_selected();
        } else if (key_sc == KEY_BACKSPACE) {
            fm_go_up();
        } else if (key_sc == KEY_ESC) {
            close_front();
        } else if (key_char == 'n' || key_char == 'N') {
            fm_new_file();
        }
        return;
    }

    if (fk == WK_EDIT) {
        if (key_sc == KEY_ESC) {
            close_front();
            return;
        }
        if (key_sc == KEY_BACKSPACE) {
            edit_backspace();
            dirty = 1;
            return;
        }
        if (key_sc == KEY_ENTER) {
            edit_insert('\n');
            dirty = 1;
            return;
        }
        if (key_sc == KEY_TAB) {
            edit_insert(' ');
            edit_insert(' ');
            dirty = 1;
            return;
        }
        if (key_sc == KEY_LEFT) {
            if (edit_sel_a != edit_sel_b) {
                edit_caret = edit_sel_a < edit_sel_b ? edit_sel_a : edit_sel_b;
            } else if (edit_caret > 0) {
                edit_caret--;
            }
            edit_sel_collapse();
            dirty = 1;
            return;
        }
        if (key_sc == KEY_RIGHT) {
            if (edit_sel_a != edit_sel_b) {
                edit_caret = edit_sel_a > edit_sel_b ? edit_sel_a : edit_sel_b;
            } else if (edit_caret < edit_len) {
                edit_caret++;
            }
            edit_sel_collapse();
            dirty = 1;
            return;
        }
        if (key_sc == KEY_UP || key_sc == KEY_DOWN) {
            int wx, wy, ww, wh, ax, ay, aw, ah;
            if (!win_geom_kind(WK_EDIT, &wx, &wy, &ww, &wh))
                return;
            edit_area(wx, wy, ww, wh, &ax, &ay, &aw, &ah);
            int cols = aw / EDIT_CHAR_W;
            if (cols < 1)
                cols = 1;
            int r, c;
            edit_caret_cell(cols, &r, &c);
            if (key_sc == KEY_UP)
                r--;
            else
                r++;
            if (r < 0)
                r = 0;
            edit_caret = edit_cell_to_index(cols, r, c);
            edit_sel_collapse();
            dirty = 1;
            return;
        }
        if (ctrl_down && key_sc == 0x1F) {
            edit_save();
            dirty = 1;
            return;
        }
        if (key_char) {
            edit_insert(key_char);
            dirty = 1;
        }
        return;
    }

    if(fk==WK_SETTINGS){
        if(key_sc==KEY_TAB||key_sc==KEY_RIGHT||key_sc==KEY_DOWN)theme_set((theme_id+(shift_down?THEME_N-1:1))%THEME_N);
        else if(key_sc==KEY_LEFT||key_sc==KEY_UP)theme_set((theme_id+THEME_N-1)%THEME_N);
        else if(key_sc==KEY_SPACE){saver_enabled=!saver_enabled;saver_save();dirty=1;}
        else if(key_sc==KEY_ESC)close_front();
        return;
    }
    if (fk == WK_CALC) {
        /* Mouse clicks only; ESC still dismisses the window. */
        if (key_sc == KEY_ESC)
            close_front();
        return;
    }

    if (fk == WK_PAINT) {
        if(key_sc==KEY_TAB){paint_tool=(paint_tool+(shift_down?PAINT_NTOOLS-1:1))%PAINT_NTOOLS;dirty=1;return;}
        handle_paint_key();
        return;
    }

    if (fk == WK_SNAKE) {
        handle_snake_key();
        return;
    }

    if (fk == WK_WORDLE) {
        if (key_sc == KEY_ESC) {
            close_front();
            return;
        }
        int act = WORDLE_ACT_NONE;
        if (key_sc == KEY_ENTER)
            act = WORDLE_ACT_ENTER;
        else if (key_sc == KEY_BACKSPACE)
            act = WORDLE_ACT_DELETE;
        if (wordle_key(act, key_char))
            dirty = 1;
        return;
    }

    if (fk == WK_TODO) {
        if (key_sc == KEY_ESC) {
            close_front();
        } else if (key_sc == KEY_ENTER) {
            if (todo_field_enter())
                dirty = 1;
        } else if (key_sc == KEY_BACKSPACE) {
            if (todo_field_backspace())
                dirty = 1;
        } else if (key_char) {
            if (todo_field_char(key_char))
                dirty = 1;
        }
        return;
    }

    if (fk == WK_CAL || fk == WK_2048 || fk == WK_BREAKOUT || fk == WK_MINES ||
        fk == WK_CLOCK || fk == WK_SYSMON) {
        if (key_sc == KEY_ESC) {
            close_front();
            return;
        }
        int changed = 0;
        if (fk == WK_CAL)
            changed = cal_key(key_sc);
        else if (fk == WK_2048)
            changed = g2048_key(key_sc);
        else if (fk == WK_BREAKOUT)
            changed = bo_key(key_sc);
        else if (fk == WK_MINES && (key_sc == KEY_SPACE || key_sc == KEY_ENTER)) {
            mines_new(frame_count);
            changed = 1;
        }
        if (changed)
            dirty = 1;
        return;
    }

    if (fk == WK_TERM) {
        if(key_sc==0x49||key_sc==0x51){term_scroll(key_sc==0x49?8:-8);dirty=1;return;}
        if(key_sc==KEY_UP||key_sc==KEY_DOWN){term_history(key_sc==KEY_UP?-1:1);dirty=1;return;}
        if(key_sc==KEY_TAB){term_complete();dirty=1;return;}
        if (key_sc == KEY_ESC) {
            close_front();
        } else if (key_sc == KEY_ENTER) {
            term_enter();
            dirty = 1;
        } else if (key_sc == KEY_BACKSPACE) {
            term_backspace();
            dirty = 1;
        } else if (key_char) {
            term_char(key_char);
            dirty = 1;
        }
        return;
    }

    if (key_sc == KEY_ESC)
        close_front();
}


/* ---------- Themes, Kilroy, Settings, splash ---------- */

static void theme_save(void) {
    int prefs = prefs_id;
    if (prefs < 0)
        prefs = fs_find_child(fs_root(), "prefs");
    if (prefs < 0)
        prefs = fs_mkdir(fs_root(), "prefs");
    if (prefs < 0)
        return;
    prefs_id = prefs;
    int f = fs_find_child(prefs, "theme");
    if (f < 0)
        f = fs_create(prefs, "theme");
    if (f < 0)
        return;
    char b[2];
    b[0] = (char)('0' + theme_id);
    b[1] = 0;
    fs_write(f, b, 1);
}

static void saver_save(void) {
    int prefs = prefs_id;
    if (prefs < 0)
        prefs = fs_find_child(fs_root(), "prefs");
    if (prefs < 0)
        prefs = fs_mkdir(fs_root(), "prefs");
    if (prefs < 0)
        return;
    prefs_id = prefs;
    int f = fs_find_child(prefs, "saver");
    if (f < 0)
        f = fs_create(prefs, "saver");
    if (f < 0)
        return;
    char b[2];
    b[0] = saver_enabled ? '1' : '0';
    b[1] = 0;
    fs_write(f, b, 1);
}

static void saver_load(void) {
    int prefs = fs_find_child(fs_root(), "prefs");
    if (prefs < 0)
        return;
    int f = fs_find_child(prefs, "saver");
    if (f < 0)
        return;
    char b[2];
    if (fs_read(f, b, 2) >= 1)
        saver_enabled = (b[0] == '1');
}

static void theme_load(void) {
    theme_id = 0;
    int prefs = prefs_id;
    if (prefs < 0)
        prefs = fs_find_child(fs_root(), "prefs");
    if (prefs < 0)
        return;
    prefs_id = prefs;
    int f = fs_find_child(prefs, "theme");
    if (f < 0)
        return;
    char b[2];
    int n = fs_read(f, b, 2);
    if (n >= 1 && b[0] >= '0' && b[0] <= '7')
        theme_id = b[0] - '0';
}

static void theme_set(int id) {
    if (id < 0 || id >= THEME_N)
        return;
    theme_id = id;
    theme_apply();
    theme_save();
    dirty = 1;
}

static void draw_about(int wx, int wy, int ww, int wh, int fl) {
    gui_draw_window(wx, wy, ww, wh, "About BaseOS", 0, fl);
    int ox = wx + (ww - KILROY_ABOUT_W) / 2;
    int oy = wy + TITLE_H + 14;
    blit_bits_color(ox, oy, KILROY_ABOUT_W, KILROY_ABOUT_H, kilroy_about_bits, ui_text);
    int y = oy + KILROY_ABOUT_H + 12;
    draw_string_bold("BaseOS", wx + (ww - uib_string_w("BaseOS")) / 2, y, ui_text);
    y += CHAR_H + 10;
    draw_string_in_win("Version 0.8  |  1280x720  |  256 colors", wx, ww, y, ui_text);
    y += CHAR_H + 10;
    draw_string_in_win("(C) 2026 CCG", wx, ww, y, ui_text_dim);
}

#define SW_W   96
#define SW_H   60
#define SW_GAP 16

static void settings_swatch_pos(int wx, int wy, int i, int *sx, int *sy) {
    int ox = wx + 24;
    int oy = wy + TITLE_H + 48;
    int col = i % 4;
    int row = i / 4;
    *sx = ox + col * (SW_W + SW_GAP);
    *sy = oy + row * (SW_H + 44);
}

static void settings_saver_box(int wx, int wy, int wh, int *x, int *y) {
    *x = wx + 24;
    *y = wy + wh - 44;
}

static void draw_settings(int wx, int wy, int ww, int wh, int fl) {
    gui_draw_window(wx, wy, ww, wh, "Settings", 0, fl);
    {
        int bx, by;
        settings_saver_box(wx, wy, wh, &bx, &by);
        draw_hline(wx + 24, by - 14, ww - 48, ui_chrome_dk);
        draw_round_rect(bx, by, 18, 18, 4, saver_enabled ? ui_accent : gfx_gray(0xA0));
        if (saver_enabled) {
            draw_line(bx + 4, by + 9, bx + 8, by + 13, COLOR_WHITE);
            draw_line(bx + 5, by + 9, bx + 9, by + 13, COLOR_WHITE);
            draw_line(bx + 8, by + 13, bx + 14, by + 5, COLOR_WHITE);
            draw_line(bx + 9, by + 13, bx + 15, by + 5, COLOR_WHITE);
        } else {
            draw_round_rect(bx + 1, by + 1, 16, 16, 3, COLOR_WHITE);
        }
        draw_string("Start the screen saver after 90 seconds idle", bx + 28, by, ui_text);
    }
    draw_string_bold("Appearance", wx + 24, wy + TITLE_H + 16, ui_text);
    draw_hline(wx + 24, wy + TITLE_H + 38, ww - 48, ui_chrome_dk);
    for (int i = 0; i < THEME_N; i++) {
        int sx, sy;
        settings_swatch_pos(wx, wy, i, &sx, &sy);
        const Theme *t = &themes[i];
        int cur = (i == theme_id);
        if (cur) {
            draw_round_rect(sx - 4, sy - 4, SW_W + 8, SW_H + 8, 9, ui_accent);
            draw_round_rect(sx - 2, sy - 2, SW_W + 4, SW_H + 4, 7, COLOR_WHITE);
        }
        draw_round_rect(sx, sy, SW_W, SW_H, 6, ui_border);
        if (i == theme_id) {
            draw_ramp_round(sx + 1, sy + 1, SW_W - 2, SW_H - 2, 5, 1, PAL_DESK, PAL_DESK_N, 0, RAMP_END(PAL_DESK_N));
        } else {
            int bands = 6;
            int bh = (SW_H - 2 + bands - 1) / bands;
            for (int b = 0; b < bands; b++) {
                uint32_t c = rgb_lerp(t->desk_top, t->desk_bot, b, bands - 1);
                int by = sy + 1 + b * bh;
                int h2 = bh;
                if (by + h2 > sy + SW_H - 1)
                    h2 = sy + SW_H - 1 - by;
                if (b == 0)
                    draw_round_top(sx + 1, by, SW_W - 2, h2 + 6, 5, idx24(c));
                else if (b == bands - 1)
                    draw_round_rect(sx + 1, by - 6, SW_W - 2, h2 + 6, 5, idx24(c));
                else
                    draw_rect(sx + 1, by, SW_W - 2, h2, idx24(c));
            }
        }
        draw_round_rect(sx + 10, sy + 12, SW_W - 20, 30, 3, COLOR_WHITE);
        draw_round_rect(sx + 15, sy + 16, 4, 4, 2, idx24(t->accent));
        draw_round_rect(sx + 10, sy + 26, SW_W - 20, 18, 3, COLOR_WHITE);
        draw_hline(sx + 14, sy + 32, SW_W - 28, ui_chrome_dk);
        draw_hline(sx + 14, sy + 37, SW_W - 36, ui_chrome_dk);
        int nw = ui_string_w(t->name);
        int nx = sx + (SW_W - nw) / 2;
        draw_string(t->name, nx, sy + SW_H + 12, cur ? ui_accent_dk : ui_text);
    }
}
static void handle_settings_click(int wx, int wy, int ww, int wh) {
    (void)ww;
    {
        int bx, by;
        settings_saver_box(wx, wy, wh, &bx, &by);
        if (hit(mouse_x, mouse_y, bx, by, 300, 20)) {
            saver_enabled = !saver_enabled;
            saver_save();
            dirty = 1;
            return;
        }
    }
    for (int i = 0; i < THEME_N; i++) {
        int sx, sy;
        settings_swatch_pos(wx, wy, i, &sx, &sy);
        if (hit(mouse_x, mouse_y, sx, sy, SW_W, SW_H)) {
            theme_set(i);
            return;
        }
    }
}


static void draw_logo(int x, int y, int size) {
    int r = size / 4;
    draw_round_rect(x - 1, y - 1, size + 2, size + 2, r + 1, ui_accent_dk);
    draw_ramp_round(x, y, size, size, r, 1, PAL_ACCENT, PAL_ACCENT_N, 1 * 16, 11 * 16);
    draw_hline(x + r, y + 1, size - 2 * r, PAL_ACCENT);

    int m = size * 22 / 100;
    int sw = size * 13 / 100;
    int gh = size - 2 * m;
    int bx = x + m;
    int by = y + m;
    int bowl_w = size * 52 / 100;
    int bowl_h = gh * 55 / 100;
    int cut = sw;
    uint8_t ink = COLOR_WHITE;
    uint8_t hole = PAL_ACCENT + 7;
    draw_round_rect(bx, by, bowl_w, bowl_h, bowl_h / 2, ink);
    draw_round_rect(bx + sw, by + cut, bowl_w - sw - cut, bowl_h - 2 * cut, (bowl_h - 2 * cut) / 2, hole);
    int by2 = by + gh - bowl_h - sw / 2;
    int bowl_w2 = bowl_w + sw;
    draw_round_rect(bx, by2, bowl_w2, bowl_h + sw / 2, (bowl_h + sw / 2) / 2, ink);
    draw_round_rect(bx + sw, by2 + cut, bowl_w2 - sw - cut, bowl_h + sw / 2 - 2 * cut,
                    (bowl_h + sw / 2 - 2 * cut) / 2, hole);
    draw_rect(bx, by, sw, gh, ink);
}

static void boot_splash(void) {
    draw_ramp(0, 0, fb_w, fb_h, PAL_DESK, PAL_DESK_N, 0, RAMP_END(PAL_DESK_N));
    int size = 128;
    int lx = (fb_w - size) / 2;
    int ly = fb_h / 2 - size + 10;
    draw_logo(lx, ly, size);
    int ty = ly + size + 30;
    int lw = logo_string_w("BaseOS");
    draw_logo_string("BaseOS", (fb_w - lw) / 2 + 1, ty + 1, PAL_DESK + PAL_DESK_N - 1);
    draw_logo_string("BaseOS", (fb_w - lw) / 2, ty, COLOR_WHITE);
    int sy = ty + LOGO_FONT_H + 8;
    const char *sub = "a tiny operating system";
    draw_string(sub, (fb_w - ui_string_w(sub)) / 2, sy, PAL_DESK + 8);
    int bw = 200;
    int bx = (fb_w - bw) / 2;
    int by = sy + CHAR_H + 36;
    uint8_t track = PAL_DESK + PAL_DESK_N - 8;
    draw_round_rect(bx, by, bw, 6, 3, track);
    flip_vga();
    kprint_debug("SPLASH\n");

    uint32_t start = frame_count;
    int shown = 0;
    while (1) {
        poll_time();
        context_set(win_front());
        drain_8042();
        int prog = (int)((frame_count - start) * bw / 150);
        if (prog > bw)
            prog = bw;
        if (prog - shown >= 8) {
            shown = prog;
            draw_round_rect(bx, by, bw, 6, 3, track);
            draw_round_rect(bx, by, prog, 6, 3, COLOR_WHITE);
            flip_rect(bx, by, bw, 6);
        }
        if ((frame_count - start) >= 150)
            break;
    }
    kprint_debug("DESKTOP\n");
    dirty = 1;
}


typedef struct { int open,kind,x,y,w,h,min,z,caret; char path[FS_PATH_LEN]; } SavedWindow;
typedef struct { unsigned magic,version; SavedWindow win[MAX_WIN]; } SavedSession;
static int session_ready;

static int session_put(int dir,const char *name,const void *data,int size){
    int id=fs_find_child(dir,name);
    if(id>=0&&fs_size(id)==size){const unsigned char *a=(const unsigned char *)fs_data(id),*b=data;int i=0;while(i<size&&a[i]==b[i])i++;if(i==size)return 0;}
    if(id<0)id=fs_create(dir,name);
    return id<0?-1:fs_write(id,data,size);
}
static void session_save(void){
    if(!session_ready)return;
    int dir=fs_find_child(fs_root(),"prefs");if(dir<0)dir=fs_mkdir(fs_root(),"prefs");
    if(dir<0){session_status="Session not saved: no free folder slot.";return;}
    int needed=0;
    if(!fs_is_dir(dir))goto failure;
    for(int i=-2;i<MAX_WIN;i++){
        char draft[]="draft0.txt";const char *name;
        if(i==-2)name="session";
        else if(i==-1){if(!paint_ready)continue;name="paint-draft";}
        else {if(!wins[i].open||wins[i].kind!=WK_EDIT)continue;draft[5]+=(char)i;name=draft;}
        int id=fs_find_child(dir,name);if(id<0)needed++;else if(fs_is_dir(id)||fs_is_app(id))goto failure;
    }
    if(needed>FS_MAX_NODES-fs_node_count())goto failure;
    SavedSession snap;kmemset(&snap,0,sizeof snap);snap.magic=0x53534542;snap.version=1;
    for(int i=0;i<MAX_WIN;i++){
        Win *w=&wins[i];SavedWindow *v=&snap.win[i];
        if(!w->open||w->kind==WK_PROPERTIES)continue;
        context_set(i);v->open=1;v->kind=w->kind;v->x=w->x;v->y=w->y;v->w=w->w;v->h=w->h;v->min=w->min;v->z=w->z;
        int id=w->kind==WK_EDIT?edit_file:w->kind==WK_FILES?fm_cwd:w->kind==WK_TERM?term_cwd():-1;
        if(w->kind==WK_EDIT && fs_identity(id)!=edit_identity)id=-1;
        if(fs_valid(id))fs_path(id,v->path,sizeof v->path);
        if(w->kind==WK_EDIT){char name[]="draft0.txt";name[5]+=(char)i;v->caret=edit_caret;
            if(session_put(dir,name,edit_buf,edit_len)<0)goto failure;}
    }
    if(paint_ready && session_put(dir,"paint-draft",paint_pix,PAINT_W*PAINT_H)<0)goto failure;
    if(session_put(dir,"session",&snap,sizeof snap)<0)goto failure;
    session_status="";context_set(win_front());return;
failure:
    session_status="Session not saved: storage is full.";context_set(win_front());
}
static void session_restore(void){
    int dir=fs_find_child(fs_root(),"prefs"),id=fs_find_child(dir,"session");SavedSession snap;
    if(id>=0 && fs_size(id)==sizeof snap){kmemcpy(&snap,fs_data(id),sizeof snap);
        if(snap.magic==0x53534542&&snap.version==1)for(int i=0;i<MAX_WIN;i++){
            SavedWindow *v=&snap.win[i];v->path[FS_PATH_LEN-1]=0;
            if(!v->open||v->kind<0||v->kind>WK_SYSMON||v->w<1||v->h<1||v->w>4096||v->h>4096||v->x<0||v->x>4096||v->y<0||v->y>4096)continue;
            int slot=win_open(v->kind);if(slot<0)break;Win *w=&wins[slot];w->x=v->x;w->y=v->y;w->w=v->w;w->h=v->h;w->min=!!v->min;w->z=v->z>=0&&v->z<100000?v->z:slot;win_clamp(w);
            if(w->z>wm_z)wm_z=w->z;
            int target=v->path[0]?fs_resolve(fs_root(),v->path):-1;
            if(v->kind==WK_FILES){fm_cwd=fs_is_dir(target)?target:fs_root();fm_refresh();}
            if(v->kind==WK_TERM)term_set_cwd(target);
            if(v->kind==WK_EDIT){edit_clear();if(fs_valid(target)&&!fs_is_dir(target))edit_load(target);
                char name[]="draft0.txt";name[5]+=(char)i;int draft=fs_find_child(dir,name);
                if(draft>=0&&!fs_is_dir(draft)&&!fs_is_app(draft)){edit_len=fs_read(draft,edit_buf,EDIT_BUF_SIZE-1);edit_buf[edit_len]=0;edit_saved_ok=0;}
                edit_caret=v->caret>=0&&v->caret<=edit_len?v->caret:edit_len;edit_sel_a=edit_sel_b=edit_caret;
            }
        }
    }
    id=fs_find_child(dir,"paint-draft");if(id>=0&&fs_size(id)==PAINT_W*PAINT_H){paint_init();kmemcpy(paint_pix,fs_data(id),PAINT_W*PAINT_H);}
    session_ready=1;context_set(win_front());dirty=1;
}
int program_key(void){
    drain_8042();int result=0;
    for(int i=0;i<kqn;i++){key_char=0;key_sc=0;keyboard_handle_byte(kq[i]);if(key_char)result=key_char;else if(key_sc==KEY_ESC)result=27;}
    kqn=0;return result;
}
void program_present(void){cursor_restore();draw_ui();flip_vga();cursor_on=0;dirty=1;}
static void install_examples(void){
    int dir=fs_find_child(fs_root(),"Programs");if(dir<0)dir=fs_mkdir(fs_root(),"Programs");if(dir<0)return;
    if(fs_find_child(dir,"hello.bex")<0){int id=fs_create(dir,"hello.bex");if(id>=0)fs_write(id,(const char *)native_example,sizeof native_example);}
    const char *demo="10 PRINT \"BASIC: press a key while the picture draws\"\n20 LET A=0\n30 RECT A,A/2,8,8,A+32\n40 INKEY B\n50 IF B > 0 THEN 100\n60 WAIT 20\n70 LET A=A+2\n80 IF A < 150 THEN 30\n90 END\n100 PRINT B\n110 END\n";
    if(fs_find_child(dir,"demo.bas")<0){int id=fs_create(dir,"demo.bas");if(id>=0)fs_write(id,demo,kstrlen(demo));}
    const char *script="pwd\nls /\necho Scripts run one command per line.\ndf\n";
    if(fs_find_child(dir,"demo.sh")<0){int id=fs_create(dir,"demo.sh");if(id>=0)fs_write(id,script,kstrlen(script));}
}

static void render_scene(void) {
                int isel = icon_sel;
                if (fm_drag_active && icon_hit(ICON_TRASH, mouse_x, mouse_y))
                    isel = ICON_TRASH;
                draw_desktop();
                draw_desktop_icons(isel);
                draw_ui();
                if (open_dlg)
                    draw_open_dialog();
                draw_menubar(open_menu, menu_sel);
                draw_taskbar();
                draw_pulldown(open_menu, menu_sel);
                draw_drag_ghost();
                draw_rename_overlay();
                draw_launcher();
                draw_namedlg();
}
static void render_desktop_frame(void) {
    int moving=dragging_win>=0 && dragging_win==win_front() && drag_active && !open_dlg && !name_dlg && !launcher_on && open_menu<0;
    if(moving){
        if(drag_cached!=dragging_win){
            render_skip=dragging_win;render_scene();render_skip=-1;
            kmemcpy((void *)DRAG_CACHE,fb,fb_w*fb_h);drag_cached=dragging_win;
        }else kmemcpy(fb,(void *)DRAG_CACHE,fb_w*fb_h);
        draw_one_window(&wins[dragging_win],0);context_set(win_front());
    }else {drag_cached=-1;render_scene();}
}

/* ---------- kmain ---------- */

void kmain(void) {
    kprint_debug("Kernel started\n");
    platform_validate_memory();
    kmemset(window_state, 0, sizeof(WindowState) * MAX_WIN);
    disk_configure(((const BootInfo *)BOOTINFO_ADDR)->sectors_per_track);
    poll_time();

    video_init();

    fs_init();
    int mounted = fs_load_disk();
    if (mounted == 0)
        kprint_debug("FS loaded from disk\n");
    else if (mounted == FS_LOAD_BLANK)
        kprint_debug("FS seeded fresh\n");
    else
        kprint_debug("FS load failed; disk protected, session is RAM-only\n");
    trash_id = fs_find_child(fs_root(), "trash");
    prefs_id = fs_find_child(fs_root(), "prefs");
    install_examples();
    theme_load();
    saver_load();
    theme_apply();
    term_reset();
    kprint_debug("FS ready\n");

    menu_bar_init();
    icons_init();
    mouse_x = fb_w / 2;
    mouse_y = fb_h / 2;

    mouse_init();
    if (mouse_ok)
        kprint_debug("Mouse ready\n");
    else
        kprint_debug("Mouse init failed\n");

    drain_8042();
    kqn = 0;

    input_ready=1;
    boot_splash();
    session_restore();
#ifdef FEATURE_TEST
    extern void feature_test(void);feature_test();
#endif
    last_input_frame = frame_count;

    uint32_t last_blink = 0;
    uint32_t last_clock = 0;
    uint32_t last_session = frame_count;

    while (1) {
        poll_time();
        context_set(win_front());
        drain_8042();

        if (saver_on) {
            int woke = kqn > 0 || mouse_clicked || mouse_moved || mouse_rclicked;
            kqn = 0;
            mouse_clicked = 0;
            mouse_rclicked = 0;
            mouse_moved = 0;
            if (woke) {
                saver_stop();
            } else {
                saver_frame();
                continue;
            }
        }
        if (kqn > 0 || mouse_clicked || mouse_moved || mouse_rclicked)
            last_input_frame = frame_count;
        else if (saver_enabled && frame_count - last_input_frame > SAVER_DELAY && !open_dlg)
            saver_start();

        while (kqn > 0) {
            uint8_t sc=kq[0];
            for(int j=1;j<kqn;j++)kq[j-1]=kq[j];
            kqn--;
            key_pressed = 0;
            key_char = 0;
            key_sc = 0;
            keyboard_handle_byte(sc);
            if (key_pressed)
                handle_key();
        }
        kqn = 0;
        if (mouse_rclicked) {
            handle_rclick();
            mouse_rclicked = 0;
        }

        if (open_menu >= 0 && mouse_moved) {
            int nsel = menu_item_at(open_menu, mouse_x, mouse_y);
            if (nsel != menu_sel) {
                menu_sel = nsel;
                dirty = 1;
            }
        }

        if (mouse_clicked) {
            handle_click();
            mouse_clicked = 0;
        }

        win_resize_tick();
        if (dragging_win >= 0 && mouse_left && !open_dlg) {
            Win *dw = &wins[dragging_win];
            int dx = mouse_x - drag_from_x;
            int dy = mouse_y - drag_from_y;
            int adx = dx < 0 ? -dx : dx;
            int ady = dy < 0 ? -dy : dy;
            if (!drag_active && (adx > 3 || ady > 3))
                drag_active = 1;
            if (drag_active) {
                int nx = drag_orig_x + dx;
                int ny = drag_orig_y + dy;
                if (nx != dw->x || ny != dw->y) {
                    dw->x = nx;
                    dw->y = ny;
                    win_clamp(dw);
                    dirty = 1;
                }
            }
        }
        if (fm_dragging && mouse_left) {
            int dx = mouse_x - fm_drag_sx;
            int dy = mouse_y - fm_drag_sy;
            if (dx < 0)
                dx = -dx;
            if (dy < 0)
                dy = -dy;
            if (!fm_drag_active && (dx > 4 || dy > 4)) {
                fm_drag_active = 1;
                fm_last_click_item = -1;
                dirty = 1;
            }
            if (fm_drag_active && mouse_moved)
                dirty = 1;
        }
        if (!mouse_left) {
            resizing_win=-1;
            if (dragging_win >= 0 && drag_active) {
                if (mouse_x < 12) win_arrange(dragging_win,1);
                else if (mouse_x > fb_w-12) win_arrange(dragging_win,2);
                else if (mouse_y < MENUBAR_H+12) win_arrange(dragging_win,0);
            }
            if (dragging_win >= 0 && !drag_active && !open_dlg &&
                wins[dragging_win].kind == WK_FILES && fm_cwd != fs_root())
                fm_go_up();
            dragging_win = -1;
            drag_active = 0;
            if (fm_dragging)
                files_drop();
            edit_dragging = 0;
            paint_mouse_up();
        }

        /* Flush changed files once the mouse is up, so a save or a drag
         * lands on the floppy without waiting for Shutdown. */
        if (!mouse_left && !fm_renaming && fs_needs_sync()) {
            const char *before = fs_storage_status();
            fs_autosync();
            if (before != fs_storage_status()) dirty = 1;
        }
        if (paint_dragging || paint_shape_drag)
            paint_drag_tick();
        snake_tick();
        if (find_open_kind(WK_BREAKOUT) >= 0 && !wins[find_open_kind(WK_BREAKOUT)].min) {
            static uint32_t bo_last = 0;
            if (frame_count != bo_last) {
                bo_last = frame_count;
                bo_tick();
                dirty = 1;
            }
        }
        if (name_dlg) {
            static uint32_t nb = 0;
            uint32_t b = frame_count / 35;
            if (b != nb) { nb = b; dirty = 1; }
        }
        if (launcher_on || find_open_kind(WK_CLOCK) >= 0) {
            static uint32_t blink_last = 0;
            uint32_t b = frame_count / 35;
            if (b != blink_last) {
                blink_last = b;
                dirty = 1;
            }
        }
        if (edit_dragging && front_kind() == WK_EDIT && !open_dlg) {
            int wx, wy, ww, wh;
            if (win_geom_kind(WK_EDIT, &wx, &wy, &ww, &wh)) {
                int idx = edit_index_at(wx, wy, ww, wh, mouse_x, mouse_y);
                if (idx != edit_sel_b) {
                    edit_sel_b = idx;
                    edit_caret = idx;
                    dirty = 1;
                }
            }
        }

        if ((front_kind() == WK_EDIT && !open_dlg && edit_sel_a == edit_sel_b) ||
            fm_renaming) {
            uint32_t b = frame_count / 35;
            if (b != last_blink) {
                last_blink = b;
                dirty = 1;
            }
        }

        uint32_t ck = frame_count / 70;
        if (ck != last_clock) {
            last_clock = ck;
            dirty = 1;
        }

        if(frame_count-last_session>=5*TIMER_HZ&&frame_count-last_input_frame>=TIMER_HZ&&!mouse_left&&!name_dlg&&!open_dlg){session_save();last_session=frame_count;}
        if(drag_cached>=0 && dragging_win<0)dirty=1;
        if(mouse_moved&&dragging_win<0){int h=taskbar_hover_at();if(h!=taskbar_hover){taskbar_hover=h;dirty=1;}}
        int moved = mouse_moved;
        mouse_moved = 0;
        if (dirty || moved || !cursor_on) {
            int ox = cursor_sx, oy = cursor_sy, oon = cursor_on;
            cursor_restore();
            if (dirty) {
                render_desktop_frame();
                redraw_count++;
                dirty = 0;
                cursor_save_draw();
                gfx_present();
            } else {
                cursor_save_draw();
                if (oon)
                    flip_rect(ox, oy, CURSOR_W, CURSOR_H);
                flip_rect(cursor_sx, cursor_sy, CURSOR_W, CURSOR_H);
            }
        }
        /* PIT wakes us at 70Hz; do not burn a CPU polling an idle desktop. */
        if(!dirty&&!mouse_left&&!mouse_moved&&kqn==0)__asm__ volatile("hlt");
    }
}
