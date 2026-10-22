/*
 * BaseOS desktop shell: windows, taskbar, launcher, the built-in desktop
 * apps and session restore. The kernel calls it only through baseos_shell
 * (kernel/shell.h); everything below the shell is in src/kernel.
 */

#include "kernel/kernel.h"
#include "kernel/shell.h"
#include "lib/canvas_view.h"
#include "apps/app_view.h"
#include "kernel/gfx.h"
#include "build_info.h"
#include "lib/decimal.h"
#include "lib/text_search.h"
#include "kernel/display.h"
#include "kernel/audio.h"
#include "kernel/net.h"
#include "kernel/download.h"
#include "apps/browser.h"
#include "apps/player.h"
#include "apps/writer.h"
#include "apps/sheet.h"
#include "lib/provision.h"
#include "apps/file_clipboard.h"
#include "apps/file_view.h"
#include "lib/video.h"
#include "apps/image_viewer.h"
#include "lib/image_decode.h"
#include "ui/app_icons.h"
#include "ui/theme.h"
#include "ui/kit.h"
#include "ui/ui_icons.h"
#include "kernel/platform.h"
#include "kernel/physmem.h"
#include "kernel/input_ingress.h"
#include "lib/history.h"
#include "kernel/program.h"
#include "ui/app.h"
#include "ui/toolbar.h"
#include "ui/icon_draw.h"
#include "kernel/font.h"
#include "apps/clock.h"
#include "apps/calendar.h"
#include "apps/calendar_agenda.h"
#include "apps/mines.h"
#include "apps/game2048.h"
#include "apps/breakout.h"
#include "apps/sysmon.h"
#include "kernel/uproc.h"
#include "kernel/fs.h"
#include "kernel/native_sync.h"
#include "kernel/native_ui.h"
#include "apps/wordle.h"
#include "apps/term.h"
#include "apps/todo.h"
#include "assets/kilroy_about.h"
#include "assets/cursor_art.h"

#define CHAR_H         UI_FONT_H
#define LINE_H         (CHAR_H + 4)
#define EDIT_LINE_H    (EDIT_FONT_H + 1)

/* Window chrome comes from the active style (ui/theme.h). Classic draws a
 * 1px border, a 28px title bar and a 24px menu row inside every window;
 * Modern draws a 38px header and shows the front window's menus in a global
 * top bar. TITLE_BAR is everything above the menu row, TITLE_H everything
 * above the client area. */
#define MODERN         (ui_theme.style == UI_STYLE_MODERN)
#define MENUBAR_H      (ui_theme.topbar_h)
#define TITLE_BAR      (ui_theme.border + ui_theme.title_h + (ui_theme.menu_h ? 0 : 1))
#define WMENU_H        (ui_theme.menu_h)
#define TITLE_H        (TITLE_BAR + WMENU_H)
#define INFO_H         24
#define SB             (ui_theme.scrollbar)
#define ROW_H          (ui_theme.row_h)
#define ICON_S         32
#define TILE_S         32
#define CURSOR_W       CURSOR_ART_W
#define CURSOR_H       CURSOR_ART_H
#define MENU_ROW       (ui_theme.menu_item_h)
#define BTN_H          (ui_theme.control_h)
/* Classic: a 34px taskbar. Modern: a 48px dock floating 8px above the
 * bottom edge, with the work area ending 4px above the dock. */
#define TASKBAR_H      (MODERN ? 60 : ui_theme.taskbar_h)
#define TASKBAR_Y      (fb_h - TASKBAR_H)
#define MAX_WIN        8

#define KEY_LCTRL      0x1D
#define KEY_LSHIFT     0x2A
#define KEY_RSHIFT     0x36
#define KEY_M          0x32
#define KEY_N          0x31
#define KEY_S          0x1F
#define KEY_W          0x11
#define KEY_Q          0x10

#define EDIT_BUF_SIZE  65536

/* Appearance preferences. */
static int ui_style = UI_STYLE_CLASSIC;
static int ui_accent_id, ui_desktop_id, ui_pattern;
/* Window behavior: outline drag unless contents are shown, double-click on a
 * title bar maximizes, windows snap to screen edges within snap distance. */
#define WINOPT_DRAG_CONTENTS 1
#define WINOPT_DOUBLE_CLICK  2
#define WINOPT_SNAP          4
#define WINOPT_LINE_NUMBERS  8   /* Editor line numbers */
static int win_options = WINOPT_DOUBLE_CLICK | WINOPT_SNAP | WINOPT_LINE_NUMBERS;
static int win_snap_distance = 8;
#define SNAP_DISTANCE_MAX 32
/* Screen saver delay choices, in seconds. */
#define SAVER_DELAY_N 5
static const unsigned saver_delay_secs[SAVER_DELAY_N] = {90, 180, 300, 600, 1800};
static int saver_delay;
/* Taskbar clock: 12-hour time, no date. */
#define CLOCK_12H 1
#define CLOCK_NO_DATE 2
static int clock_flags;
/* Double-click speed: slow, normal, fast. */
static int dbl_click_speed = 1;
static unsigned dbl_click_ticks(void) {
    static const unsigned ms[3] = {600, 400, 250};
    return TIMER_HZ * ms[dbl_click_speed < 0 || dbl_click_speed > 2 ? 1 : dbl_click_speed] / 1000;
}
static const char *session_status="";
static const char *native_launch_status="";

/* Shell drawing uses the theme tokens directly. These names keep the older
 * role vocabulary readable at call sites. */
#define ui_accent      ((uint8_t)TK_ACCENT)
#define ui_accent_dk   ((uint8_t)TK_ACCENT_700)
#define ui_text        ((uint8_t)TK_TEXT)
#define ui_text_dim    ((uint8_t)TK_TEXT2)
#define ui_chrome      ((uint8_t)TK_BASE)
#define ui_chrome_dk   ((uint8_t)TK_LINE_SUBTLE)
#define ui_border      ((uint8_t)TK_LINE)
#define ui_field       ((uint8_t)TK_RAISED)
#define ui_paper       ((uint8_t)TK_RAISED)
#define ui_body        ((uint8_t)ui_theme.content)
#define ui_on_accent   ((uint8_t)TK_ON_ACCENT)
#define ui_track       ((uint8_t)TK_SUNKEN)
static int desk_cache_theme = -1;
static int display_pending = 0, display_previous = -1;
static uint32_t display_deadline;
static const char *display_message = "";
static void display_revert(void);
static void display_keep(void);
static int display_request(int mode);
static int saver_enabled = 1;
static int saver_on = 0;
static uint32_t last_input_frame = 0;
static int launcher_on = 0;
static char launch_buf[24];
static int launch_len = 0;
static int launch_sel = 0;
static int launch_scroll;
static unsigned redraw_count = 0;
static int mouse_x;
static int mouse_y;
static int mouse_left = 0;
static int mouse_right = 0;
static int mouse_moved = 0;

static uint8_t idx24(uint32_t c) {
    return gfx_rgb((c >> 16) & 255, (c >> 8) & 255, c & 255);
}

/* Older drawing code names neutrals by darkness, 0 (lightest surface) to 31
 * (darkest text). Each level maps to the nearest token of the style. */
static uint8_t ink_token(int i) {
    static const uint8_t map[32] = {
        TK_RAISED, TK_RAISED, TK_BASE, TK_SUNKEN, TK_HOVER, TK_HOVER, TK_LINE_SUBTLE, TK_LINE_SUBTLE,
        TK_LINE_SUBTLE, TK_LINE, TK_LINE, TK_LINE, TK_LINE_STRONG, TK_LINE_STRONG, TK_LINE_STRONG, TK_LINE_STRONG,
        TK_LINE_STRONG, TK_TEXT2, TK_TEXT2, TK_TEXT2, TK_TEXT2, TK_TEXT2, TK_ICON, TK_ICON,
        TK_ICON, TK_ICON, TK_ICON, TK_ICON, TK_TEXT, TK_TEXT, TK_TEXT, TK_TEXT
    };
    return map[i < 0 ? 0 : i > 31 ? 31 : i];
}
#define INK(i) ink_token(i)

static void menu_bar_init(void);
static void theme_apply(void) {
    ui_theme_install(ui_style, ui_accent_id, ui_desktop_id);
    desk_cache_theme = -1;
    menu_bar_init();
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

static int btn_w(const char *label) { return kit_button_w(label, -1); }
/* App icon (AI_*) shown in the title bar of the window being drawn, or -1,
 * and the kind whose menu model the window row shows. */
static int chrome_icon = -1;
static int chrome_kind = -1;
static int chrome_slot = -1;
static int chrome_flags;  /* WIN_MAXED and WIN_DIALOG for the window being drawn */

/* Hover and press state for a control under the pointer. */
static int ctl_state(int x, int y, int w, int h) {
    if (!hit(mouse_x, mouse_y, x, y, w, h)) return 0;
    return mouse_left ? KIT_HOVER | KIT_PRESSED : KIT_HOVER;
}

/* ---------- Shared desktop controls ---------- */

#define WIN_INFO 1
#define WIN_SCROLL 2
#define WIN_NOCLOSE 4
#define WIN_INACTIVE 8
#define WIN_NOMENU 16
#define WIN_MAXED 32
#define WIN_DIALOG 64   /* close button only */

/* Caption buttons: minimize, maximize and close at the right of the title
 * bar, each caption_w x caption_h. Drawing and hit testing share these. */
#define CAP_H (ui_theme.caption_h)
#define CAP_W (ui_theme.caption_w)
#define CAP_CLOSE_W CAP_W
static int cap_y(int wy) { return wy + (MODERN ? 6 : 5); }
static void close_box_pos(int wx, int wy, int *cx, int *cy) {
    (void)wx; *cx = -1; *cy = cap_y(wy);
}
static int close_box_x(int wx, int ww) { return wx + ww - (MODERN ? 34 : 29); }
static void min_box_pos(int wx, int wy, int *cx, int *cy) {
    (void)wx; *cx = -1; *cy = cap_y(wy);
}
static int max_box_x(int wx, int ww) { return close_box_x(wx, ww) - CAP_W - ui_theme.caption_gap; }
static int min_box_x(int wx, int ww) { return max_box_x(wx, ww) - CAP_W - ui_theme.caption_gap; }

static void draw_win_menu_strip(int x, int y, int w, int inactive, int nomenu);
static int caption_pointer_ok(void);

static void draw_caption(int bx, int by, int icon, int active, int close, int hot) {
    int hover = hot && caption_pointer_ok() && hit(mouse_x, mouse_y, bx, by, CAP_W, CAP_H);
    int pressed = hover && mouse_left;
    uint8_t ink = MODERN ? (active ? TK_ICON : TK_LINE_STRONG) : (active ? TK_ON_ACCENT : TK_TEXT2);
    uint8_t bg = MODERN ? (active ? ui_theme.chrome : TK_46) : (active ? TK_ACCENT_600 : TK_45);
    if (hover) {
        if (close) bg = pressed ? TK_RED_700 : TK_RED;
        else bg = MODERN ? TK_HOVER : active ? (pressed ? TK_ACCENT_700 : TK_ACCENT) : TK_HOVER;
        if (close) ink = TK_ON_ACCENT;
        kit_fill(bx, by, CAP_W, CAP_H, MODERN ? 6 : KIT_R_SQUARE, bg, 1);
    }
    ui_icon_draw(icon, bx + (CAP_W - 16) / 2, by + (CAP_H - 16) / 2, 16, ink, bg);
}

static void draw_win_control(int wx, int wy, int ww, int active, int flags) {
    int y = cap_y(wy);
    draw_caption(close_box_x(wx, ww), y, UI_ICON_X, active, 1, 1);
    if (flags & WIN_DIALOG) return;
    draw_caption(max_box_x(wx, ww), y, (flags & WIN_MAXED) ? UI_ICON_RESTORE : UI_ICON_MAX, active, 0, 1);
    draw_caption(min_box_x(wx, ww), y, UI_ICON_MIN, active, 0, 1);
}

/* Scrollbars along the client edges of a window, for views that page their
 * content in whole rows (pos, visible and total are in rows). */
static void draw_vscroll(int x, int y, int h, int pos, int visible, int total) {
    kit_vscroll(x, y, h, pos, visible, total, ctl_state(x, y, SB, h));
}

static void draw_scrollbars(int x, int y, int w, int h, int top) {
    int vx = x + w - 1 - SB, vy = y + top;
    draw_vscroll(vx, vy, h - top - 1, 0, 1, 1);
}

void gui_draw_window(int x, int y, int w, int h, const char *title,
                     const char *info, int flags) {
    flags |= chrome_flags;
    int noclose = flags & WIN_NOCLOSE;
    int active = !(flags & WIN_INACTIVE);
    int maxed = flags & WIN_MAXED;
    int menu = !(flags & WIN_NOMENU) && WMENU_H > 0;
    int top = menu ? TITLE_H : TITLE_BAR;
    uint8_t title_ink;
    int tx, ty, icon_x, icon_y;
    if (MODERN) {
        int r = maxed ? KIT_R_SQUARE : ui_theme.r_window;
        if (!maxed) kit_shadow(x, y, w, h, r);
        kit_fill(x, y, w, h, r, ui_theme.content, 0);
        kit_fill_top(x, y, w, TITLE_BAR - 1, r, active ? ui_theme.chrome : TK_46);
        draw_hline(x + 1, y + TITLE_BAR - 1, w - 2, TK_LINE);
        kit_frame(x, y, w, h, r, active ? TK_LINE_STRONG : TK_47);
        title_ink = active ? TK_TEXT : TK_TEXT2;
        icon_x = x + 12; icon_y = y + 12; tx = x + 36; ty = y + 11;
    } else {
        if (!maxed) kit_shadow(x, y, w, h, 0);
        draw_frame(x, y, w, h, active ? TK_ACCENT_700 : TK_LINE_STRONG);
        draw_rect(x + 1, y + 1, w - 2, ui_theme.title_h, active ? TK_ACCENT_600 : TK_45);
        if (menu) {
            draw_rect(x + 1, y + TITLE_BAR, w - 2, WMENU_H - 1, TK_BASE);
            draw_hline(x + 1, y + TITLE_H - 1, w - 2, TK_LINE_SUBTLE);
        }
        draw_rect(x + 1, y + top, w - 2, h - top - 1, ui_theme.content);
        title_ink = active ? TK_ON_ACCENT : TK_TEXT2;
        icon_x = x + 8; icon_y = y + 7; tx = x + 30; ty = y + 6;
    }
    if (menu) draw_win_menu_strip(x, y, w, !active, 0);
    if (!noclose) draw_win_control(x, y, w, active, flags);
    if (chrome_icon >= 0) app_icon_draw(chrome_icon, icon_x, icon_y, AI_SMALL);
    else tx = icon_x;
    if (title) {
        int xmax = noclose ? x + w - 12 : ((flags & WIN_DIALOG) ? close_box_x(x, w) : min_box_x(x, w)) - 8;
        kit_text(title, tx, ty, title_ink, xmax - tx, 1);
    }

    if ((flags & WIN_INFO) && info) {
        kit_toolbar(x + 1, y + top, w - 2, INFO_H);
        draw_string_clip(info, x + 10, kit_ty(y + top, INFO_H - 1), ui_text_dim, x + w - 8);
        top += INFO_H;
    }

    if (flags & WIN_SCROLL)
        draw_scrollbars(x, y, w, h, top);
}

static void draw_button_styled(int x, int y, int w, int h, const char *label, int primary) {
    kit_button(x, y, w, h, label, -1, primary ? KB_PRIMARY : KB_DEFAULT, ctl_state(x, y, w, h));
}

void draw_button(int x, int y, int w, int h, const char *label) {
    draw_button_styled(x, y, w, h, label, 0);
}

static void draw_default_button(int x, int y, int w, int h, const char *label) {
    draw_button_styled(x, y, w, h, label, 1);
}

/* The app registry: every built-in app, in launcher order. Desktop icons are
 * a separate, smaller set (System, Home, Trash). */
typedef struct {
    const char *label;
    int icon; /* ICON_APP_* glyph */
    int x, y;
} DeskIcon;

enum {
    ICON_FILES = 0,
    ICON_DESK_EDIT,
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
    ICON_DESK_SETTINGS,
    ICON_BROWSER,
    ICON_PLAYER,
    ICON_WRITER,
    ICON_SPREADSHEET,
    ICON_DESK_TRASH,
    ICON_DESK_COUNT
};
#define ICON_DISK ICON_FILES

static DeskIcon icons[ICON_DESK_COUNT];

enum { DESK_SYSTEM, DESK_HOME, DESK_TRASH, DESK_COUNT };
typedef struct { const char *label; int x, y; } DesktopIcon;
static DesktopIcon desk[DESK_COUNT];

static void icon_set(int id, const char *label, int icon) {
    icons[id].label = label;
    icons[id].icon = icon;
}

static int desk_icon_w(void) { return MODERN ? 84 : 76; }
static int desk_icon_pitch(void) { return MODERN ? 88 : 80; }

static void icons_init(void) {
    icon_set(ICON_FILES, "Files", ICON_APP_FILES);
    icon_set(ICON_DESK_EDIT, "Editor", ICON_APP_EDITOR);
    icon_set(ICON_CALC, "Calculator", ICON_APP_CALC);
    icon_set(ICON_PAINT, "Paint", ICON_APP_PAINT);
    icon_set(ICON_VIEW, "Viewer", ICON_APP_VIEWER);
    icon_set(ICON_SNAKE, "Snake", ICON_APP_SNAKE);
    icon_set(ICON_WORDLE, "Wordle", ICON_APP_WORDLE);
    icon_set(ICON_TERM, "Terminal", ICON_APP_TERMINAL);
    icon_set(ICON_TODO, "Todo", ICON_APP_TODO);
    icon_set(ICON_CLOCK, "Clock", ICON_APP_CLOCK);
    icon_set(ICON_CAL, "Calendar", ICON_APP_CALENDAR);
    icon_set(ICON_MINES, "Mines", ICON_APP_MINES);
    icon_set(ICON_2048, "2048", ICON_APP_2048);
    icon_set(ICON_BREAKOUT, "Breakout", ICON_APP_BREAKOUT);
    icon_set(ICON_SYSMON, "Monitor", ICON_APP_MONITOR);
    icon_set(ICON_DESK_SETTINGS, "Settings", ICON_APP_SETTINGS);
    icon_set(ICON_BROWSER, "Browser", ICON_APP_BROWSER);
    icon_set(ICON_PLAYER, "Media Player", ICON_APP_PLAYER);
    icon_set(ICON_WRITER, "Writer", ICON_APP_WRITER);
    icon_set(ICON_SPREADSHEET, "Spreadsheet", ICON_APP_SHEET);
    icon_set(ICON_DESK_TRASH, "Trash", ICON_APP_TRASH);

    static const char *const names[DESK_COUNT] = {"System", "Home", "Trash"};
    int x0 = MODERN ? 12 : 10, y0 = MENUBAR_H + (MODERN ? 12 : 8);
    for (int i = 0; i < DESK_COUNT; i++) {
        desk[i].label = names[i];
        desk[i].x = x0;
        desk[i].y = y0 + i * desk_icon_pitch();
    }
}

/* Label box under a desktop icon (one line; the names are short). */
static void desk_label_pos(int id, int *lx, int *ly, int *lw) {
    int tw = ui_string_w(desk[id].label), pad = MODERN ? 6 : 4;
    *lw = tw + 2 * pad;
    *lx = desk[id].x + (desk_icon_w() - *lw) / 2;
    *ly = desk[id].y + 4 + 32 + (MODERN ? 6 : 4);
}

static int icon_hit(int id, int px, int py) {
    if (id < 0 || id >= DESK_COUNT) return 0;
    return hit(px, py, desk[id].x, desk[id].y, desk_icon_w(), 4 + 32 + 6 + CHAR_H + 4);
}

/* The color icon for a Lucide application glyph (icons[].icon, kind_icon()). */
static int app_icon_for(int glyph) {
    switch (glyph) {
    case ICON_APP_FILES: return AI_FILES;       case ICON_APP_EDITOR: return AI_EDITOR;
    case ICON_APP_CALC: return AI_CALC;         case ICON_APP_PAINT: return AI_PAINT;
    case ICON_APP_VIEWER: return AI_VIEWER;     case ICON_APP_SNAKE: return AI_SNAKE;
    case ICON_APP_WORDLE: return AI_WORDLE;     case ICON_APP_TERMINAL: return AI_TERMINAL;
    case ICON_APP_TODO: return AI_TODO;         case ICON_APP_CLOCK: return AI_CLOCK;
    case ICON_APP_CALENDAR: return AI_CALENDAR; case ICON_APP_MINES: return AI_MINES;
    case ICON_APP_2048: return AI_G2048;        case ICON_APP_BREAKOUT: return AI_BREAKOUT;
    case ICON_APP_MONITOR: return AI_MONITOR;   case ICON_APP_SETTINGS: return AI_SETTINGS;
    case ICON_APP_BROWSER: return AI_BROWSER;   case ICON_APP_PLAYER: return AI_PLAYER;
    case ICON_APP_WRITER: return AI_WRITER;     case ICON_APP_SHEET: return AI_SHEET;
    case ICON_APP_TRASH: return AI_TRASH;       case ICON_APP_NATIVE: return AI_NATIVE;
    case ICON_APP_HELLO: return AI_HELLO;       case ICON_APP_ABOUT: return AI_ABOUT;
    case ICON_APP_HELP: return AI_HELP;         case ICON_FOLDER: return AI_FILES;
    case ICON_FILE_TEXT: return AI_DOCUMENT;    default: return AI_WINDOW;
    }
}

static const int desk_ui_icon[DESK_COUNT] = {UI_ICON_A_DISK, UI_ICON_A_FILES, UI_ICON_A_TRASH};

static void draw_desktop_icons(int selected) {
    for (int i = 0; i < DESK_COUNT; i++) {
        int sel = (i == selected);
        int lx, ly, lw;
        desk_label_pos(i, &lx, &ly, &lw);
        int iw = desk_icon_w();
        if (sel && MODERN) kit_fill(desk[i].x, desk[i].y, iw, ly + CHAR_H + 6 - desk[i].y, 8, TK_LINE, 1);
        ui_icon_draw(desk_ui_icon[i], desk[i].x + (iw - 32) / 2, desk[i].y + (MODERN ? 6 : 4), 32, TK_ICON, TK_DESKTOP);
        if (sel) kit_fill(lx, ly, lw, CHAR_H + (MODERN ? 2 : 0), MODERN ? 4 : KIT_R_SQUARE, TK_ACCENT, 1);
        draw_string(desk[i].label, lx + (MODERN ? 6 : 4), ly + (MODERN ? 1 : 0), sel ? TK_ON_ACCENT : TK_TEXT);
    }
}

/* The desktop is one token fill with an optional pattern drawn a step
 * darker. Changing the desktop preset rewrites the token, so the cached
 * frame only needs rebuilding when the style or pattern changes. */
static void desktop_render(void) {
    draw_rect(0, 0, fb_w, fb_h, TK_DESKTOP);
    if (ui_pattern == UI_PATTERN_NONE) return;
    uint8_t ink = gfx_mix(TK_DESKTOP, TK_TEXT, 36);
    if (ink == TK_DESKTOP) ink = TK_SHADOW;
    if (ui_pattern == UI_PATTERN_DOTS) {
        for (int y = 4; y < fb_h; y += 8)
            for (int x = 4; x < fb_w; x += 8) put_pixel(x, y, ink);
    } else {
        for (int y = 15; y < fb_h; y += 16) draw_hline(0, y, fb_w, ink);
        for (int x = 15; x < fb_w; x += 16) draw_vline(x, 0, fb_h, ink);
    }
}

void draw_desktop(void) {
    uint8_t *cache = (uint8_t *)DESK_CACHE;
    int n = fb_w * fb_h;
    int key = (ui_style << 8) | (ui_pattern << 4) | ui_desktop_id;
    if (desk_cache_theme != key) {
        desktop_render();
        kmemcpy(cache, fb, n);
        desk_cache_theme = key;
        return;
    }
    kmemcpy(fb, cache, n);
}

/* ---------- Menus ----------
 * Each app kind declares its menu row as data: menus of items with a label
 * (an '&' marks the mnemonic), a shortcut text, an action and flags. The
 * shell draws the model in the window's menu row (Classic) or the top bar
 * (Modern) and dispatches actions. The legacy File / Edit / Window action
 * groups below remain the implementation of the shared actions. */
enum {
    MENU_NONE = -1,
    MENU_FILE = 0,
    MENU_EDITM,
    MENU_WINDOW,
    MENU_N
};
#define LEG(m, i) (0x100 | ((m) << 4) | (i))
enum {
    ACT_SEP = -1, ACT_NONE = 0,
    ACT_NEW = LEG(MENU_FILE, 0), ACT_NEW_FOLDER = LEG(MENU_FILE, 1), ACT_OPEN = LEG(MENU_FILE, 2),
    ACT_CLOSE = LEG(MENU_FILE, 3), ACT_SAVE = LEG(MENU_FILE, 4), ACT_DUPLICATE = LEG(MENU_FILE, 5),
    ACT_PROPERTIES = LEG(MENU_FILE, 6), ACT_EMPTY_TRASH = LEG(MENU_FILE, 8),
    ACT_CUT = LEG(MENU_EDITM, 0), ACT_COPY = LEG(MENU_EDITM, 1), ACT_PASTE = LEG(MENU_EDITM, 2),
    ACT_FIND = LEG(MENU_EDITM, 3), ACT_REPLACE = LEG(MENU_EDITM, 4),
    ACT_MINIMIZE = LEG(MENU_WINDOW, 0), ACT_MAXIMIZE = LEG(MENU_WINDOW, 1),
    ACT_SHOW_DESKTOP = LEG(MENU_WINDOW, 2), ACT_ABOUT = LEG(MENU_WINDOW, 4), ACT_HELP = LEG(MENU_WINDOW, 5),
    ACT_NEW_WINDOW = 0x200, ACT_SAVE_AS, ACT_UNDO, ACT_REDO, ACT_SELECT_ALL, ACT_FIND_NEXT,
    ACT_OPEN_ITEM, ACT_NEW_FILE, ACT_RENAME, ACT_TRASH, ACT_REFRESH, ACT_FILTER,
    ACT_GO_BACK, ACT_GO_FORWARD, ACT_GO_UP, ACT_GO_HOME, ACT_GO_DOCUMENTS, ACT_GO_PICTURES, ACT_GO_TRASH,
    ACT_SORT_NAME, ACT_SORT_KIND, ACT_SORT_SIZE, ACT_SORT_MODIFIED, ACT_SORT_DESCENDING,
    ACT_LINE_NUMBERS, ACT_RESTORE, ACT_MOVE, ACT_SIZE, ACT_SETTINGS, ACT_EXPORT_PDF,
    ACT_RECENT_MENU, ACT_RECENT_CLEAR, ACT_COPY_PATH, ACT_NEW_GAME, ACT_REVERT, ACT_QUIT,
    ACT_RESTART, ACT_SHUTDOWN, ACT_DESK_OPEN, ACT_DESK_PROPERTIES,
    ACT_RECENT = 0x300 /* + index */
};

typedef struct { const char *label; const char *key; int act; int flags; } MItem;
typedef struct { const char *name; const MItem *items; int n; } MMenu;
#define MSEP {"-", "", ACT_SEP, 0}
#define MN(name, items) {name, items, (int)(sizeof(items) / sizeof(items[0]))}

static const MItem mi_help[] = {
    {"&Help Topics", "F1", ACT_HELP, 0}, MSEP, {"&About BaseOS", "", ACT_ABOUT, 0},
};
static const MItem mi_files_file[] = {
    {"&New Window", "Ctrl+N", ACT_NEW, 0}, {"New &Folder", "Ctrl+Shift+N", ACT_NEW_FOLDER, 0},
    {"New Fi&le", "", ACT_NEW_FILE, 0}, MSEP,
    {"&Open", "Enter", ACT_OPEN_ITEM, 0}, {"&Rename", "F2", ACT_RENAME, 0},
    {"&Duplicate", "Ctrl+D", ACT_DUPLICATE, 0}, {"Move to &Trash", "Del", ACT_TRASH, 0},
    {"Propert&ies", "Alt+Enter", ACT_PROPERTIES, 0}, MSEP,
    {"&Empty Trash", "", ACT_EMPTY_TRASH, 0}, MSEP, {"&Close Window", "Ctrl+W", ACT_CLOSE, 0},
};
static const MItem mi_files_edit[] = {
    {"Cu&t", "Ctrl+X", ACT_CUT, 0}, {"&Copy", "Ctrl+C", ACT_COPY, 0}, {"&Paste", "Ctrl+V", ACT_PASTE, 0},
    MSEP, {"Copy Pat&h", "", ACT_COPY_PATH, 0}, MSEP, {"&Find", "Ctrl+F", ACT_FILTER, 0},
};
static const MItem mi_files_view[] = {
    {"By &Name", "Ctrl+1", ACT_SORT_NAME, KIT_MENU_RADIO}, {"By &Kind", "Ctrl+2", ACT_SORT_KIND, KIT_MENU_RADIO},
    {"By &Size", "Ctrl+3", ACT_SORT_SIZE, KIT_MENU_RADIO}, {"By &Modified", "Ctrl+4", ACT_SORT_MODIFIED, KIT_MENU_RADIO},
    MSEP, {"&Descending", "", ACT_SORT_DESCENDING, KIT_MENU_CHECK}, MSEP, {"&Refresh", "F5", ACT_REFRESH, 0},
};
static const MItem mi_files_go[] = {
    {"&Back", "Alt+Left", ACT_GO_BACK, 0}, {"&Forward", "Alt+Right", ACT_GO_FORWARD, 0},
    {"&Up", "Alt+Up", ACT_GO_UP, 0}, MSEP, {"&Home", "", ACT_GO_HOME, 0},
    {"&Documents", "", ACT_GO_DOCUMENTS, 0}, {"&Pictures", "", ACT_GO_PICTURES, 0}, {"&Trash", "", ACT_GO_TRASH, 0},
};
static const MMenu mb_files[] = {
    MN("&File", mi_files_file), MN("&Edit", mi_files_edit), MN("&View", mi_files_view),
    MN("&Go", mi_files_go), MN("&Help", mi_help),
};
static const MItem mi_editor_file[] = {
    {"&New", "Ctrl+N", ACT_NEW, 0}, {"&Open...", "Ctrl+O", ACT_OPEN, 0},
    {"Open &Recent", "", ACT_RECENT_MENU, KIT_MENU_SUB}, MSEP,
    {"&Save", "Ctrl+S", ACT_SAVE, 0}, {"Save &As...", "Ctrl+Shift+S", ACT_SAVE_AS, 0}, MSEP,
    {"&Close Window", "Ctrl+W", ACT_CLOSE, 0},
};
static const MItem mi_editor_edit[] = {
    {"&Undo", "Ctrl+Z", ACT_UNDO, 0}, {"&Redo", "Ctrl+Y", ACT_REDO, 0}, MSEP,
    {"Cu&t", "Ctrl+X", ACT_CUT, 0}, {"&Copy", "Ctrl+C", ACT_COPY, 0}, {"&Paste", "Ctrl+V", ACT_PASTE, 0},
    {"Select &All", "Ctrl+A", ACT_SELECT_ALL, 0}, MSEP,
    {"&Find...", "Ctrl+F", ACT_FIND, 0}, {"Find &Next", "F3", ACT_FIND_NEXT, 0}, {"R&eplace...", "Ctrl+H", ACT_REPLACE, 0},
};
static const MItem mi_editor_view[] = {
    {"Line &Numbers", "", ACT_LINE_NUMBERS, KIT_MENU_CHECK},
};
static const MMenu mb_editor[] = {
    MN("&File", mi_editor_file), MN("&Edit", mi_editor_edit), MN("&View", mi_editor_view), MN("&Help", mi_help),
};
static const MItem mi_doc_file[] = {
    {"&New", "Ctrl+N", ACT_NEW, 0}, {"&Open...", "Ctrl+O", ACT_OPEN, 0},
    {"Open &Recent", "", ACT_RECENT_MENU, KIT_MENU_SUB}, MSEP,
    {"&Save", "Ctrl+S", ACT_SAVE, 0}, MSEP, {"&Close Window", "Ctrl+W", ACT_CLOSE, 0},
};
static const MItem mi_doc_edit[] = {
    {"&Undo", "Ctrl+Z", ACT_UNDO, 0}, {"&Redo", "Ctrl+Y", ACT_REDO, 0}, MSEP,
    {"Cu&t", "Ctrl+X", ACT_CUT, 0}, {"&Copy", "Ctrl+C", ACT_COPY, 0}, {"&Paste", "Ctrl+V", ACT_PASTE, 0},
    {"Select &All", "Ctrl+A", ACT_SELECT_ALL, 0},
};
static const MItem mi_writer_edit[] = {
    {"&Undo", "Ctrl+Z", ACT_UNDO, 0}, {"&Redo", "Ctrl+Y", ACT_REDO, 0}, MSEP,
    {"Cu&t", "Ctrl+X", ACT_CUT, 0}, {"&Copy", "Ctrl+C", ACT_COPY, 0}, {"&Paste", "Ctrl+V", ACT_PASTE, 0},
    {"Select &All", "Ctrl+A", ACT_SELECT_ALL, 0}, MSEP,
    {"&Find...", "Ctrl+F", ACT_FIND, 0}, {"R&eplace...", "Ctrl+H", ACT_REPLACE, 0},
};
static const MMenu mb_writer[] = { MN("&File", mi_doc_file), MN("&Edit", mi_writer_edit), MN("&Help", mi_help) };
static const MMenu mb_sheet[] = { MN("&File", mi_doc_file), MN("&Edit", mi_doc_edit), MN("&Help", mi_help) };
static const MItem mi_paint_file[] = {
    {"&New", "Ctrl+N", ACT_NEW, 0}, {"&Open...", "Ctrl+O", ACT_OPEN, 0}, MSEP,
    {"&Save", "Ctrl+S", ACT_SAVE, 0}, MSEP, {"&Close Window", "Ctrl+W", ACT_CLOSE, 0},
};
static const MItem mi_paint_edit[] = { {"&Undo", "Ctrl+Z", ACT_UNDO, 0}, {"&Redo", "Ctrl+Y", ACT_REDO, 0} };
static const MMenu mb_paint[] = { MN("&File", mi_paint_file), MN("&Edit", mi_paint_edit), MN("&Help", mi_help) };
static const MItem mi_term_shell[] = {
    {"&New Window", "Ctrl+N", ACT_NEW, 0}, MSEP, {"&Close Window", "Ctrl+W", ACT_CLOSE, 0},
};
static const MItem mi_term_edit[] = { {"&Copy", "Ctrl+C", ACT_COPY, 0}, {"&Paste", "Ctrl+V", ACT_PASTE, 0} };
static const MMenu mb_term[] = { MN("&Shell", mi_term_shell), MN("&Edit", mi_term_edit), MN("&Help", mi_help) };
static const MItem mi_game[] = {
    {"&New Game", "Ctrl+N", ACT_NEW, 0}, MSEP, {"&Close Window", "Ctrl+W", ACT_CLOSE, 0},
};
static const MMenu mb_game[] = { MN("&Game", mi_game), MN("&Help", mi_help) };
static const MItem mi_calc_file[] = { {"&Close Window", "Ctrl+W", ACT_CLOSE, 0} };
static const MItem mi_calc_edit[] = { {"&Copy", "Ctrl+C", ACT_COPY, 0}, {"&Paste", "Ctrl+V", ACT_PASTE, 0} };
static const MMenu mb_calc[] = { MN("&File", mi_calc_file), MN("&Edit", mi_calc_edit), MN("&Help", mi_help) };
static const MItem mi_viewer_file[] = {
    {"&Open...", "Ctrl+O", ACT_OPEN, 0}, MSEP, {"&Close Window", "Ctrl+W", ACT_CLOSE, 0},
};
static const MMenu mb_viewer[] = { MN("&File", mi_viewer_file), MN("&Help", mi_help) };
static const MItem mi_save_file[] = {
    {"&New", "Ctrl+N", ACT_NEW, 0}, {"&Save", "Ctrl+S", ACT_SAVE, 0}, MSEP,
    {"&Close Window", "Ctrl+W", ACT_CLOSE, 0},
};
static const MMenu mb_save[] = { MN("&File", mi_save_file), MN("&Help", mi_help) };
static const MItem mi_plain_file[] = { {"&Close Window", "Ctrl+W", ACT_CLOSE, 0} };
static const MMenu mb_plain[] = { MN("&File", mi_plain_file), MN("&Help", mi_help) };
static const MItem mi_window[] = {
    {"&Restore", "", ACT_RESTORE, 0}, {"&Move", "", ACT_MOVE, 0}, {"&Size", "", ACT_SIZE, 0},
    {"Mi&nimize", "", ACT_MINIMIZE, 0}, {"Ma&ximize", "", ACT_MAXIMIZE, 0}, MSEP,
    {"&Close", "Alt+F4", ACT_CLOSE, 0},
};
static const MItem mi_files_context[] = {
    {"&Open", "Enter", ACT_OPEN_ITEM, 0}, MSEP, {"&Rename", "F2", ACT_RENAME, 0},
    {"&Duplicate", "Ctrl+D", ACT_DUPLICATE, 0}, {"Move to &Trash", "Del", ACT_TRASH, 0}, MSEP,
    {"Cu&t", "Ctrl+X", ACT_CUT, 0}, {"&Copy", "Ctrl+C", ACT_COPY, 0}, {"Copy Pat&h", "", ACT_COPY_PATH, 0}, MSEP,
    {"Propert&ies", "Alt+Enter", ACT_PROPERTIES, 0},
};
static const MItem mi_folder_context[] = {
    {"New &Folder", "Ctrl+Shift+N", ACT_NEW_FOLDER, 0}, {"New Fi&le", "", ACT_NEW_FILE, 0}, MSEP,
    {"&Paste", "Ctrl+V", ACT_PASTE, 0}, MSEP, {"&Refresh", "F5", ACT_REFRESH, 0},
};
static const MItem mi_desktop_context[] = {
    {"&Open Files", "", ACT_GO_HOME, 0}, MSEP, {"&Show Desktop", "Super+D", ACT_SHOW_DESKTOP, 0},
    {"&Settings...", "", ACT_SETTINGS, 0},
};

static int win_kbd_mode, win_kbd_slot = -1;  /* Alt+Space Move (1) or Size (2) */
static int menu_bar_focus = -1;              /* Alt or F10: row focused, no pulldown */
#define MENU_BAR 101
static int switcher_on;                      /* Alt+Tab panel is up */
static int key_ext;                          /* the key event carried the E0 prefix */
static int super_down, super_used;           /* Super held; a shortcut consumed it */
static int alt_tap;                          /* Alt pressed with nothing else yet */
static int drag_outline_x, drag_outline_y;   /* outline position while dragging */

static const MItem mi_desktop_file[] = {
    {"&Open Home", "", ACT_GO_HOME, 0}, {"&Settings...", "", ACT_SETTINGS, 0}, MSEP,
    {"Show &Desktop", "Super+D", ACT_SHOW_DESKTOP, 0},
};
static const MMenu mb_desktop[] = { MN("&File", mi_desktop_file), MN("&Help", mi_help) };
static const MItem mi_system[] = {
    {"&About BaseOS", "", ACT_ABOUT, 0}, MSEP, {"&Settings...", "", ACT_SETTINGS, 0},
    {"Show &Desktop", "Super+D", ACT_SHOW_DESKTOP, 0}, MSEP,
    {"&Restart", "", ACT_RESTART, 0}, {"Shut &Down", "", ACT_SHUTDOWN, 0},
};
static void menu_open_bar(int m);
static void menu_open_context(const MItem *items, int n, int x, int y);
static void menu_open_window(int slot);

/* Recent files: the shell remembers the last documents opened in any app. */
#define RECENT_N 8
typedef struct { int id; unsigned identity; } RecentFile;
static RecentFile recent[RECENT_N];
static int recent_n;
static MItem mi_recent[RECENT_N + 2];
static int mi_recent_n;
static void recent_build_menu(void);

static int front_kind(void);
static int menu_kind(void);

/* The menu row for a window kind (WK_* values; see enum WinKind). */
static const MMenu *menubar_for(int kind, int *n);

static const MMenu *bar_menus;
static int bar_n;
static int bar_x[8]; /* offsets from the row's left edge */
static int bar_w[8];

/* Open popup chain: [0] is a pulldown, context or window menu, [1] a
 * submenu of [0]. */
typedef struct { const MItem *items; int n; int x, y, w, h; int sel; } Popup;
static Popup popup[2];
static int popup_n;
#define MENU_CONTEXT 100  /* open_menu while a context or window menu is up */

static int mi_is_sep(const MItem *it) { return it->act == ACT_SEP; }

/* Label without its '&' marker, and the index of the mnemonic letter. */
static void mi_text(const char *label, char *out, int cap, int *mnemonic) {
    int n = 0;
    if (mnemonic) *mnemonic = -1;
    for (int i = 0; label[i] && n < cap - 1; i++) {
        if (label[i] == '&' && label[i + 1]) {
            if (mnemonic) *mnemonic = n;
            continue;
        }
        out[n++] = label[i];
    }
    out[n] = 0;
}

static char mi_mnemonic(const char *label) {
    for (int i = 0; label[i]; i++)
        if (label[i] == '&' && label[i + 1]) {
            char c = label[i + 1];
            return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
        }
    return 0;
}

static void menu_bar_init(void) {
    bar_menus = menubar_for(menu_kind(), &bar_n);
    if (bar_n > 8) bar_n = 8;
    int x = MODERN ? 0 : 3;
    for (int i = 0; i < bar_n; i++) {
        char t[32];
        mi_text(bar_menus[i].name, t, sizeof t, 0);
        bar_x[i] = x;
        bar_w[i] = ui_string_w(t) + (MODERN ? 18 : 16);
        x += bar_w[i] + (MODERN ? 2 : 0);
    }
}

static int popup_item_h(const MItem *it) { return mi_is_sep(it) ? kit_menu_sep_h() : MENU_ROW; }

static void popup_size(Popup *p) {
    int w = ui_theme.menu_min_w;
    for (int i = 0; i < p->n; i++) {
        char t[96];
        mi_text(p->items[i].label, t, sizeof t, 0);
        int iw = kit_menu_width(t, p->items[i].key) + ((p->items[i].flags & KIT_MENU_SUB) ? 18 : 0);
        if (iw > w) w = iw;
    }
    if (w > ui_theme.menu_max_w) w = ui_theme.menu_max_w;
    p->w = w;
    p->h = 2 * kit_menu_pad();
    for (int i = 0; i < p->n; i++) p->h += popup_item_h(&p->items[i]);
}

/* Place a popup at x,y, flipping left or up so it stays on screen. */
static void popup_place(Popup *p, int x, int y, int flip_x) {
    popup_size(p);
    if (x + p->w > fb_w - 2) x = flip_x >= 0 ? flip_x - p->w : fb_w - 2 - p->w;
    if (x < 2) x = 2;
    if (y + p->h > fb_h - 2) y = fb_h - 2 - p->h;
    if (y < 2) y = 2;
    p->x = x;
    p->y = y;
}

static int popup_item_y(const Popup *p, int i) {
    int y = p->y + kit_menu_pad();
    for (int k = 0; k < i; k++) y += popup_item_h(&p->items[k]);
    return y;
}

static int popup_item_at(const Popup *p, int px, int py) {
    if (!hit(px, py, p->x, p->y, p->w, p->h)) return -1;
    for (int i = 0; i < p->n; i++) {
        int y = popup_item_y(p, i);
        if (py >= y && py < y + popup_item_h(&p->items[i])) return mi_is_sep(&p->items[i]) ? -1 : i;
    }
    return -1;
}

static void front_window_origin(int *x, int *y);

static int menu_item_enabled(int m, int item);
static int act_enabled(int act);
static int act_checked(int act);
static void act_run(int act);
static void desk_open(int i);
static void settings_key(void);
static void win_kbd_begin(void);
static void display_click(void);
static void launcher_open(void);
static void saver_start(void);
static void show_desktop(void);
static void sysinfo_fill(SysInfo *si);
static const char *win_display_title(int slot, char *buffer);
static void draw_logo(int x, int y, int size);
static void icon_open(int id);
static void open_fs_file(int id);
static void recent_add(int id);
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
static void fm_history_reset(void);
static void fm_history_step(int dir);
static int fm_history_target(int dir);
static void fm_navigate(int id);
static void theme_load(void);
static void draw_about(int wx, int wy, int ww, int wh, int fl);
static void handle_about_click(int wx, int wy, int ww, int wh);
static void draw_settings(int wx, int wy, int ww, int wh, int fl);
static int settings_window_h(void);
static void handle_settings_click(int wx, int wy, int ww, int wh);
static void saver_save(void);
static void preferences_retry(void);
static void boot_splash(void);
static void start_open_dialog(int pics_only);

/* Modern top bar: the system button, the front app's name in bold, then its
 * menus, all 22px items 3px below the top. */
static const char *win_app_name(int kind);
#define TOPBAR_SYS_X 6
#define TOPBAR_SYS_W 28
static const char *topbar_app_name(void) {
    int k = menu_kind();
    return k < 0 ? "Desktop" : win_app_name(k);
}
static int topbar_app_x(void) { return TOPBAR_SYS_X + TOPBAR_SYS_W + 2; }
static int topbar_app_w(void) { return kit_text_w(topbar_app_name(), 1) + 18; }

/* Menu row origin: inside the front window (Classic) or the top bar. */
static void menu_row_origin(int *x, int *y, int *h) {
    if (MODERN) { *x = topbar_app_x() + topbar_app_w() + 2; *y = 3; *h = 22; return; }
    int wx, wy;
    front_window_origin(&wx, &wy);
    *x = wx + 1;
    *y = wy + TITLE_BAR;
    *h = WMENU_H - 2;
}

/* Open pulldown m of the current menu row. */
static void menu_open(int m) {
    menu_bar_init();
    if (m < 0 || m >= bar_n) return;
    int rx, ry, rh;
    menu_row_origin(&rx, &ry, &rh);
    Popup *p = &popup[0];
    p->items = bar_menus[m].items;
    p->n = bar_menus[m].n;
    p->sel = -1;
    popup_place(p, rx + bar_x[m], MODERN ? 30 : ry + rh + 1, -1);
    popup_n = 1;
}

static void menu_open_popup(const MItem *items, int n, int x, int y) {
    Popup *p = &popup[0];
    p->items = items;
    p->n = n;
    p->sel = -1;
    popup_place(p, x, y, x);
    popup_n = 1;
}

static void menu_open_sub(int parent) {
    Popup *p = &popup[0];
    if (parent < 0 || parent >= p->n || !(p->items[parent].flags & KIT_MENU_SUB)) return;
    if (p->items[parent].act == ACT_RECENT_MENU) recent_build_menu();
    Popup *s = &popup[1];
    s->items = mi_recent;
    s->n = mi_recent_n;
    s->sel = -1;
    popup_place(s, p->x + p->w - 4, popup_item_y(p, parent) - kit_menu_pad() - 1, p->x + 4);
    popup_n = 2;
}

static uint32_t frame_count = 0;

/* "Thu Oct 15  20:34" for the taskbar clock. */

static int alt_down;
/* Draw a label with its mnemonic underlined while Alt is held. */
static int menu_by_key;  /* menus opened from the keyboard show mnemonics */
static void draw_mnemonic(const char *label, int x, int y, uint8_t ink, int maxw, int show) {
    char t[96];
    int mn;
    mi_text(label, t, sizeof t, &mn);
    kit_text(t, x, y, ink, maxw, 0);
    if (show && mn >= 0) {
        char pre[96];
        for (int i = 0; i < mn; i++) pre[i] = t[i];
        pre[mn] = 0;
        char one[2] = {t[mn], 0};
        int ux = x + ui_string_w(pre);
        draw_hline(ux, y + 15, ui_string_w(one), ink);
    }
}

static void draw_popup(const Popup *p) {
    kit_menu_frame(p->x, p->y, p->w, p->h);
    for (int i = 0; i < p->n; i++) {
        const MItem *it = &p->items[i];
        int y = popup_item_y(p, i);
        if (mi_is_sep(it)) { kit_menu_sep(p->x, y, p->w); continue; }
        int en = act_enabled(it->act);
        int st = (i == p->sel ? KIT_HOVER : 0) | (en ? 0 : KIT_DISABLED) |
                 ((it->flags & (KIT_MENU_CHECK | KIT_MENU_RADIO)) && act_checked(it->act) ? KIT_ON : 0);
        char t[96];
        int mn;
        mi_text(it->label, t, sizeof t, &mn);
        kit_menu_item(p->x, y, p->w, t, it->key, it->flags, st);
        if ((alt_down || menu_by_key) && mn >= 0) {
            char pre[96];
            for (int k = 0; k < mn; k++) pre[k] = t[k];
            pre[mn] = 0;
            char one[2] = {t[mn], 0};
            int lx = p->x + (MODERN ? 36 : 28) + ui_string_w(pre);
            draw_hline(lx, y + kit_ty(0, MENU_ROW) + 15, ui_string_w(one),
                       (i == p->sel && en) ? TK_ON_ACCENT : en ? TK_TEXT : TK_LINE_STRONG);
        }
    }
}

static void draw_pulldown(int open_menu, int menu_sel) {
    (void)menu_sel;
    if (open_menu < 0)
        return;
    for (int i = 0; i < popup_n; i++) draw_popup(&popup[i]);
}


/* ---------- PS/2 mouse (polled) ---------- */

static uint32_t input_sample_ticks;
static int input_routing, input_cursor_moved, input_tail_pending;
static unsigned input_suppressed, input_saver_buttons, input_unknown_buttons, input_sync_loss;
static uint64_t input_wake_serial;
static void desktop_input_cancel(unsigned buttons);
static void desktop_input_fence(void);
static int desktop_input_blocked(void);
static unsigned desktop_input_turn(void);
static int desktop_program_key(void);
static uint32_t input_gesture_ticks(void) { return input_routing ? input_sample_ticks : frame_count; }

static uint8_t cursor_saved[CURSOR_W * CURSOR_H];
static int cursor_sx = -1, cursor_sy = -1;
static int cursor_on = 0;


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
static int ctrl_down = 0;
static uint8_t key_sc = 0;
static char key_char = 0;

static void poll_time(void) {
    frame_count = timer_ticks();
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
    WK_PROPERTIES,
    WK_BROWSER,
    WK_PLAYER,
    WK_WRITER,
    WK_SPREADSHEET,
    WK_NATIVE /* Ephemeral owned native primary window; never persisted. */
};
_Static_assert(WK_TERM==11&&WK_SPREADSHEET==23,"persisted built-in kinds must not move");
static int session_window_kind(int kind){
    return kind>=WK_HELLO&&kind<=WK_SPREADSHEET&&kind!=WK_PROPERTIES;
}

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
static Win display_saved_windows[MAX_WIN];
static int wm_z;
static int wm_seq;
static int native_output_scroll[MAX_WIN];
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
static const char *properties_reason="";

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
/* Source versions belong to the live binding, never to an undo snapshot. */
typedef struct { unsigned size,hash_a,hash_b; } EditorBinding;
typedef struct { unsigned valid; EditorBinding source; } EditorSource;
typedef struct {
    int open,replace_mode,focus,field,exact,position[2],selected[2];
    char text[2][64],message[64];
} EditorSearch;
typedef struct {
    int cwd;
    int cwd_tracked;
    unsigned cwd_identity;
    int manual_files_scroll,manual_editor_scroll;
    int selected;
    int first;
    int *ids;                /* kmalloc'd; ids_capacity entries */
    unsigned *ids_identity;
    int ids_capacity;
    int count;
    int files_total;
    FileViewOptions files_view;
    int filter_open, filter_focus, filter_select_all;
    int hist[12], hist_n, hist_pos;
    unsigned hist_identity[12];
    uint32_t last_click_frame;
    int last_click_item;
    Document doc, undo[8];
    EditorSource editor_source;
    int editor_save_busy;
    EditorSearch search;
    History history;
} WindowState;
static WindowState *window_state = (WindowState *)EDITOR_BASE;
static int context_slot;
static void context_set(int slot) { if (slot >= 0 && slot < MAX_WIN) { context_slot = slot; term_select(slot); } }
static void window_state_clear(int slot) {
    kfree(window_state[slot].ids);
    kfree(window_state[slot].ids_identity);
    kmemset(&window_state[slot], 0, sizeof(WindowState));
}
/* A folder's rows: room for every child, kept when memory runs short. */
static int window_state_reserve(WindowState *state, int count) {
    if (count <= state->ids_capacity) return state->ids_capacity;
    int *ids = kmalloc((uint32_t)count * sizeof *ids);
    unsigned *identities = kmalloc((uint32_t)count * sizeof *identities);
    if (!ids || !identities) {
        kfree(ids);
        kfree(identities);
        return state->ids_capacity;
    }
    kfree(state->ids);
    kfree(state->ids_identity);
    state->ids = ids;
    state->ids_identity = identities;
    state->ids_capacity = count;
    return count;
}
_Static_assert(sizeof(WindowState) * MAX_WIN < EDITOR_CAPACITY - 2 * EDIT_BUF_SIZE, "window arena overflow");
#define fm_cwd (window_state[context_slot].cwd)
#define fm_manual_scroll (window_state[context_slot].manual_files_scroll)
#define edit_manual_scroll (window_state[context_slot].manual_editor_scroll)
#define fm_selected (window_state[context_slot].selected)
#define fm_first (window_state[context_slot].first)
#define fm_ids (window_state[context_slot].ids)
#define fm_ids_identity (window_state[context_slot].ids_identity)
#define fm_count (window_state[context_slot].count)
#define fm_total (window_state[context_slot].files_total)
#define fm_view (window_state[context_slot].files_view)
#define fm_filter_open (window_state[context_slot].filter_open)
#define fm_filter_focus (window_state[context_slot].filter_focus)
#define fm_filter_select_all (window_state[context_slot].filter_select_all)
#define fm_last_click_frame (window_state[context_slot].last_click_frame)
#define fm_last_click_item (window_state[context_slot].last_click_item)
#define edit_search (window_state[context_slot].search)
#define edit_buf (window_state[context_slot].doc.buf)
#define edit_len (window_state[context_slot].doc.len)
#define edit_caret (window_state[context_slot].doc.caret)
#define edit_file (window_state[context_slot].doc.file)
#define edit_identity (window_state[context_slot].doc.identity)
#define edit_saved_ok (window_state[context_slot].doc.saved_ok)
#define edit_save_busy (window_state[context_slot].editor_save_busy)
#define edit_scroll (window_state[context_slot].doc.scroll)
#define edit_sel_a (window_state[context_slot].doc.sel_a)
#define edit_sel_b (window_state[context_slot].doc.sel_b)
#define edit_dragging (window_state[context_slot].doc.dragging)
#define edit_source (window_state[context_slot].editor_source)
static void edit_fingerprint(const void *bytes,unsigned size,EditorBinding *out) {
    const unsigned char *data=bytes;
    unsigned fnv=2166136261u,crc=~0u;
    for(unsigned i=0;i<size;i++){
        fnv=(fnv^data[i])*16777619u;crc^=data[i];
        for(unsigned bit=0;bit<8;bit++)crc=(crc>>1)^(0xEDB88320u&(0u-(crc&1u)));
        if(!(i&2047u))platform_poll();
    }
    out->size=size;out->hash_a=fnv;out->hash_b=~crc;
}
static int edit_fingerprint_matches(const void *bytes,unsigned size,const EditorBinding *binding) {
    if(size!=binding->size)return 0;
    EditorBinding actual;edit_fingerprint(bytes,size,&actual);
    return actual.hash_a==binding->hash_a&&actual.hash_b==binding->hash_b;
}
static int edit_binding_matches(int id,const EditorBinding *binding) {
    return fs_valid(id)&&!fs_is_dir(id)&&!fs_is_app(id)&&fs_size(id)<EDIT_BUF_SIZE&&
        edit_fingerprint_matches(fs_data(id),(unsigned)fs_size(id),binding);
}
static int edit_binding_valid(void) {
    return fs_valid(edit_file)&&!fs_is_dir(edit_file)&&!fs_is_app(edit_file)&&
        fs_identity(edit_file)==edit_identity;
}
static void edit_capture_source(void) {
    edit_fingerprint(edit_buf,(unsigned)edit_len,&edit_source.source);
    edit_source.valid=1;
}
/* Folder contexts belong to an object incarnation, not a reusable node slot. */
static void fm_set_cwd(int id) {
    WindowState *state=&window_state[context_slot];
    if(!fs_is_dir(id))id=fs_root();
    unsigned identity=fs_identity(id);
    if(state->cwd!=id||state->cwd_identity!=identity){
        state->manual_files_scroll=state->first=state->count=state->files_total=0;
        state->selected=0;
        state->files_view.filter[0]=0;
        state->filter_open=state->filter_focus=state->filter_select_all=0;
        state->last_click_item=-1;
    }
    state->cwd=state->cwd_tracked=id;
    state->cwd_identity=identity;
}
static int fm_cwd_valid(void) {
    WindowState *state=&window_state[context_slot];
    return fs_is_dir(state->cwd)&&state->cwd_tracked==state->cwd&&
           state->cwd_identity==fs_identity(state->cwd);
}
static int fm_checked_cwd(void) {
    WindowState *state=&window_state[context_slot];
    if(!fs_is_dir(state->cwd)||state->cwd_tracked!=state->cwd||
       state->cwd_identity!=fs_identity(state->cwd))fm_set_cwd(fs_root());
    return state->cwd;
}
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

static char *const clip_buf=(char *)(EDITOR_BASE+EDITOR_CAPACITY-EDIT_BUF_SIZE);
static char *const edit_scratch=(char *)(EDITOR_BASE+EDITOR_CAPACITY-2*EDIT_BUF_SIZE);
static int clip_len = 0;
static unsigned clip_generation;
static uint32_t files_message_until;
static int files_action_busy;
static int files_storage_busy(void) {
    files_action_busy = fs_sync_busy();
    if (files_action_busy) {
        files_message_until = frame_count + 8 * TIMER_HZ;
        dirty = 1;
    }
    return files_action_busy;
}
static void clipboard_changed(void) {
    file_clipboard_clear();
    files_message_until = 0;
    dirty = 1;
    if (!++clip_generation) ++clip_generation;
}
unsigned writer_clipboard_set(const char *text, unsigned length) {
    if (!text || length >= EDIT_BUF_SIZE) return 0;
    kmemcpy(clip_buf, text, length);
    clip_buf[length] = 0;
    clip_len = (int)length;
    clipboard_changed();
    return clip_generation;
}
int writer_clipboard_get(char *text, unsigned capacity, unsigned *generation) {
    if (!text || file_clipboard_mode()!=FILE_CLIPBOARD_NONE || (unsigned)clip_len >= capacity) return -1;
    kmemcpy(text, clip_buf, clip_len + 1);
    if (generation) *generation = clip_generation;
    return clip_len;
}

unsigned spreadsheet_clipboard_set(const char *text, unsigned length) {
    return writer_clipboard_set(text, length);
}
int spreadsheet_clipboard_get(char *text, unsigned capacity, unsigned *generation) {
    return writer_clipboard_get(text, capacity, generation);
}

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
static int name_failed;
static const char *name_failure_message;
/* A close request owns a window incarnation, never whichever app draws last. */
static int edit_close_owner = -1, edit_close_seq, edit_close_kind;
static int edit_close_dlg, edit_close_focus, edit_close_failed;
enum { DOCUMENT_CLOSE, DOCUMENT_NEW, DOCUMENT_OPEN };
static int document_action, document_target = -1;
static unsigned document_target_identity, document_target_incarnation, document_target_version;
static unsigned document_pending_owner, document_pending_handle;
static void edit_close_cancel(void) {
    edit_close_owner = -1;
    document_pending_owner = document_pending_handle = 0;
    document_action = DOCUMENT_CLOSE;
    document_target = -1;
    edit_close_dlg = edit_close_failed = 0;
    dirty = 1;
}
static int edit_close_valid(void) {
    return edit_close_owner >= 0 && edit_close_owner < MAX_WIN &&
        wins[edit_close_owner].open &&
        (wins[edit_close_owner].kind == WK_EDIT || wins[edit_close_owner].kind == WK_WRITER ||
         wins[edit_close_owner].kind == WK_SPREADSHEET) &&
        wins[edit_close_owner].seq == edit_close_seq && wins[edit_close_owner].kind == edit_close_kind;
}
static int pick_cwd = 0;
static int pick_focus, pick_first;
static int *pick_ids;           /* kmalloc'd; pick_capacity entries */
static unsigned *pick_identity;
static int pick_capacity;
static int pick_count = 0;
static int pick_selected = 0;
static uint32_t pick_last_click_frame = 0;
static int pick_last_click_item = -1;
static int pick_pics_only = 0;
static int pick_writer, pick_sheet, pick_owner = -1, pick_owner_seq;

static uint32_t icon_last_frame = 0;
static int icon_last = -1;

static int fm_dragging = 0;
static int fm_drag_active = 0;
static int fm_drag_id = -1;
static unsigned fm_drag_identity;
static int fm_drag_parent = -1;
static int fm_drag_sx = 0;
static int fm_drag_sy = 0;

static int fm_renaming = 0;
static int fm_rename_id = -1;
static unsigned fm_rename_identity;
static char fm_rename_buf[FS_NAME_LEN];
static int fm_rename_len = 0;

static int preferences_prepare_shutdown(void);
static int win_open(int kind);
/* Restart runs the same save path as Shut Down and resets at the end. */
static int power_restart;
static void do_shutdown(void) {
    int restart = power_restart;
    power_restart = 0;
    if (preferences_prepare_shutdown() < 0) {
        if (win_open(WK_SETTINGS) < 0)
            session_status="Shutdown paused. Open Settings after closing a window.";
        dirty = 1;
        return;
    }
    /* Collect owned save outcomes after the first drain, before session_save
     * captures latest private edits. Never execute deferred navigation here. */
    (void)writer_persistence_poll(); (void)spreadsheet_persistence_poll();
    if (todo_prepare_shutdown() < 0) {
        if (win_open(WK_TODO) < 0)
            session_status="Shutdown paused. Open Todo after closing a window.";
        dirty=1;
        return;
    }
    if (cal_agenda_prepare_shutdown() < 0) {
        if (win_open(WK_CAL) < 0)
            session_status="Shutdown paused. Open Calendar after closing a window.";
        dirty=1;
        return;
    }
    session_save();
    if(session_status[0]){dirty=1;return;}
    if (fs_sync() < 0) {
        dirty = 1;
        return; /* Keep unsaved work available after a failed flush. */
    }
    if (restart) kernel_restart();
    kernel_power_off();
}
static void do_restart(void) {
    power_restart = 1;
    do_shutdown();
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
static void front_window_origin(int *x, int *y) {
    int f = win_front();
    *x = f >= 0 ? wins[f].x : 8;
    *y = f >= 0 ? wins[f].y : 0;
}
static int win_has_menu(int kind) {
    return kind != WK_ABOUT && kind != WK_HELP && kind != WK_HELLO && kind != WK_PROPERTIES;
}
/* Fixed-size information windows: a close button only, no snapping. */
static int win_is_dialog(int kind) {
    return kind == WK_ABOUT || kind == WK_PROPERTIES;
}
/* The menu row item under the pointer, or -1. Classic rows live inside the
 * front window; Modern shows the front window's menus in the top bar. */
static int win_menu_at(int px, int py) {
    if (!MODERN) {
        int f = win_front();
        if (f < 0 || !win_has_menu(wins[f].kind)) return -1;
    }
    menu_bar_init();
    int rx, ry, rh;
    menu_row_origin(&rx, &ry, &rh);
    for (int i = 0; i < bar_n; i++)
        if (hit(px, py, rx + bar_x[i], ry, bar_w[i], rh)) return i;
    return -1;
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
    else if(w->kind==WK_NATIVE){*mw=240;*mh=180;}
    else if(w->kind==WK_VIEW){*mw=IMAGE_VIEWER_MIN_W+2;*mh=IMAGE_VIEWER_MIN_H+TITLE_H+2;}
    else if(w->kind==WK_BROWSER){*mw=BROWSER_MIN_W+2;*mh=BROWSER_MIN_H+TITLE_H+2;}
    else if(w->kind==WK_PLAYER){*mw=PLAYER_MIN_W+2;*mh=PLAYER_MIN_H+TITLE_H+2;}
    else if(w->kind==WK_WRITER){*mw=WRITER_MIN_W+2;*mh=WRITER_MIN_H+TITLE_H+2;}
    else if(w->kind==WK_SPREADSHEET){*mw=SPREADSHEET_MIN_W+2;*mh=SPREADSHEET_MIN_H+TITLE_H+2;}
    else { int x, y; layout_window(w->kind, &x, &y, mw, mh); }
    /* Every window keeps at least a 240x120 client area. */
    if (*mw < 242) *mw = 242;
    if (*mh < 120 + TITLE_H + 1) *mh = 120 + TITLE_H + 1;
}

/* Window states other than normal: maximized, or snapped to a screen half.
 * A maximized window's border falls just outside the work area. */
enum { SNAP_NONE, SNAP_MAX, SNAP_LEFT, SNAP_RIGHT };
static void win_snap_rect(int state, int *x, int *y, int *w, int *h) {
    int top = MENUBAR_H, work = TASKBAR_Y - MENUBAR_H;
    if (state == SNAP_MAX) {
        *x = -1; *w = fb_w + 2;
        *y = top - 1; *h = MODERN ? work + 1 : work + 2;
        return;
    }
    int half = fb_w / 2;
    *x = state == SNAP_RIGHT ? half : 0;
    *w = state == SNAP_RIGHT ? fb_w - half : half;
    *y = top; *h = work;
}
static void win_clamp(Win *w) {
    if (w->maximized) { win_snap_rect(w->maximized, &w->x, &w->y, &w->w, &w->h); return; }
    int mw, mh; win_minimum(w, &mw, &mh);
    if (w->w < mw) w->w = mw;
    if (w->h < mh) w->h = mh;
    if (w->w > fb_w) w->w = fb_w;
    if (w->h > TASKBAR_Y - MENUBAR_H) w->h = TASKBAR_Y - MENUBAR_H;
    if (w->x + w->w > fb_w) w->x = fb_w - w->w;
    if (w->y + w->h > TASKBAR_Y) w->y = TASKBAR_Y - w->h;
    if (w->x < 0) w->x = 0;
    if (w->y < MENUBAR_H) w->y = MENUBAR_H;
}
/* mode 0 toggles maximized, 1 and 2 toggle the left and right halves,
 * 3 restores. */
static void win_arrange(int i, int mode) {
    if (i < 0 || i >= MAX_WIN || !wins[i].open) return;
    Win *w = &wins[i];
    int target = mode == 0 ? SNAP_MAX : mode == 1 ? SNAP_LEFT : mode == 2 ? SNAP_RIGHT : SNAP_NONE;
    if (target == w->maximized) target = SNAP_NONE;
    if (!w->maximized) { w->old_x = w->x; w->old_y = w->y; w->old_w = w->w; w->old_h = w->h; }
    if (target == SNAP_NONE) {
        if (w->maximized) { w->x = w->old_x; w->y = w->old_y; w->w = w->old_w; w->h = w->old_h; }
        w->maximized = SNAP_NONE;
    } else w->maximized = target;
    win_clamp(w);
    dirty = 1;
}

/* Edge magnetism: a moving window's edges stick to the work area edges. */
static void win_snap_edges(int *x, int *y, int w, int h) {
    if (!(win_options & WINOPT_SNAP) || win_snap_distance <= 0) return;
    int d = win_snap_distance;
    if (*x < d && *x > -d) *x = 0;
    if (fb_w - (*x + w) < d && fb_w - (*x + w) > -d) *x = fb_w - w;
    if (*y - MENUBAR_H < d && *y - MENUBAR_H > -d) *y = MENUBAR_H;
    if (TASKBAR_Y - (*y + h) < d && TASKBAR_Y - (*y + h) > -d) *y = TASKBAR_Y - h;
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

/* New windows cascade from the top left of the work area. */
static int cascade_n;
static void win_place(Win *w) {
    int x = 48 + 24 * cascade_n, y = MENUBAR_H + 24 + 24 * cascade_n;
    if (x + w->w > fb_w || y + w->h > TASKBAR_Y) { cascade_n = 0; x = 48; y = MENUBAR_H + 24; }
    cascade_n++;
    w->x = x;
    w->y = y;
    win_clamp(w);
}

/* Resize zones: the frame plus 3px (Classic) or 4px (Modern), with larger
 * corner squares. Returns edge bits: 1 left, 2 right, 4 top, 8 bottom. */
static int win_resize_edges(const Win *w, int px, int py) {
    if (w->maximized == SNAP_MAX) return 0;
    int z = MODERN ? 5 : 4, c = MODERN ? 16 : 12, e = 0;
    if (!hit(px, py, w->x, w->y, w->w, w->h)) return 0;
    int l = px < w->x + z, r = px >= w->x + w->w - z, t = py < w->y + z, b = py >= w->y + w->h - z;
    if (l || r) { e |= l ? 1 : 2; if (py < w->y + c) e |= 4; else if (py >= w->y + w->h - c) e |= 8; }
    if (t || b) { e |= t ? 4 : 8; if (px < w->x + c) e |= 1; else if (px >= w->x + w->w - c) e |= 2; }
    return e;
}

static int win_open(int kind) {
    /* Owned native windows require the pre-start transaction, never this
     * built-in factory's immediate focus/reset side effects. */
    if(kind==WK_NATIVE)return -1;
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
    window_state_clear(slot);
    context_set(slot);
    fm_set_cwd(fs_root());
    edit_file = -1;
    fm_last_click_item = -1;
    window_state[slot].history = (History){0, 0, 8, sizeof(Document), (unsigned char *)window_state[slot].undo};
    if (kind == WK_TERM) term_reset();
    if (kind == WK_BROWSER) browser_init();
    if (kind == WK_PLAYER) player_init();
    if (kind == WK_WRITER) writer_init();
    if (kind == WK_SPREADSHEET) spreadsheet_init();
    if (kind == WK_SYSMON) sysmon_reset();
    wins[slot].kind = kind;
    wins[slot].open = 1;
    wins[slot].min = 0;
    wins[slot].seq = ++wm_seq;
    layout_window(kind, &wins[slot].x, &wins[slot].y, &wins[slot].w, &wins[slot].h);
    win_place(&wins[slot]);
    win_focus(slot);
    return slot;
}

static void win_close(int i) {
    if (i < 0 || i >= MAX_WIN || !wins[i].open)
        return;
    if (i == edit_close_owner) {
        name_dlg = 0;
        edit_close_cancel();
    }
    if(wins[i].kind==WK_VIEW)image_viewer_close();
    if(wins[i].kind==WK_TERM&&!term_task_close(i))return;
    if(wins[i].kind==WK_NATIVE&&!app_view_close(i))return;
    if(wins[i].kind==WK_BROWSER)browser_close();
    if(wins[i].kind==WK_PLAYER)player_close();
    if(wins[i].kind==WK_WRITER)writer_close();
    if(wins[i].kind==WK_SPREADSHEET)spreadsheet_close();
    wins[i].open = 0;
    native_launch_status=""; /* Closing any window frees native launch capacity. */
    context_set(win_front());
    if (dragging_win == i) {
        dragging_win = -1;
        drag_active = 0;
    }
    dirty = 1;
}

static void native_window_complete(AppViewUpdate update){
    int slot=update.slot;
    if((update.flags&APP_VIEW_AUTO_CLOSE)&&slot>=0&&slot<MAX_WIN&&
       wins[slot].open&&wins[slot].kind==WK_NATIVE)win_close(slot);
}

/* An action owns its window and source-file incarnations throughout Save As. */
static void document_finish(int owner, int action, int target, unsigned identity) {
    if (action == DOCUMENT_CLOSE) { win_close(owner); return; }
    if (owner < 0 || owner >= MAX_WIN || !wins[owner].open ||
        (wins[owner].kind != WK_WRITER && wins[owner].kind != WK_SPREADSHEET)) return;
    context_set(owner);
    int sheet = wins[owner].kind == WK_SPREADSHEET;
    if (action == DOCUMENT_NEW) { if(sheet)spreadsheet_new();else writer_new(); }
    else if (fs_valid(target) && fs_identity(target) == identity &&
        (sheet ? spreadsheet_open_file(target) : writer_open_file(target)))
        fm_set_cwd(fs_parent(target));
    dirty = 1;
}
static void document_request(int i, int action, int target) {
    if (i < 0 || i >= MAX_WIN || !wins[i].open || edit_close_owner >= 0) return;
    Document *doc = &window_state[i].doc;
    int needs_save = wins[i].kind == WK_SPREADSHEET ?
        (spreadsheet_dirty() || (spreadsheet_file() >= 0 && fs_identity(spreadsheet_file()) != spreadsheet_file_identity())) :
        wins[i].kind == WK_WRITER ?
        (writer_dirty() || (writer_file() >= 0 && fs_identity(writer_file()) != writer_file_identity())) :
        wins[i].kind == WK_EDIT && !(doc->saved_ok &&
        (doc->file < 0 || fs_identity(doc->file) == doc->identity));
    unsigned identity = fs_identity(target);
    if (!needs_save) { document_finish(i, action, target, identity); return; }
    win_focus(i);
    edit_close_owner = i;
    edit_close_seq = wins[i].seq; edit_close_kind = wins[i].kind;
    document_pending_owner = document_pending_handle = 0;
    document_action = action;
    document_target = target;
    document_target_identity = identity;
    document_target_incarnation = fs_incarnation();
    document_target_version = fs_content_revision(target);
    edit_close_dlg = 1;
    edit_close_focus = 2; /* Enter starts on Cancel, never Discard. */
    edit_close_failed = 0;
    open_menu = MENU_NONE;
    dragging_win = resizing_win = -1;
    drag_active = fm_dragging = fm_drag_active = edit_dragging = 0;
    writer_release();
    spreadsheet_release();
    dirty = 1;
}
static void win_request_close(int i) { document_request(i, DOCUMENT_CLOSE, -1); }

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
    if (m == MENU_WINDOW) {
        if (item <= 1) return !open_dlg && win_front() >= 0;
        return 1;
    }
    if (m == MENU_EDITM) {
        if(item>=3)return !open_dlg&&(front_kind()==WK_EDIT||front_kind()==WK_WRITER);
        /* File and text clipboard formats remain distinct. */
        if (open_dlg)
            return 0;
        if (front_kind() == WK_FILES) {
            if (item == 2) return fm_cwd_valid()&&file_clipboard_can_paste(fm_cwd);
            int id=fm_row_id(fm_selected);
            return (item==0||item==1)&&id>=0&&id!=fs_root()&&!fs_is_app(id);
        }
        if (front_kind() == WK_CALC) {
            if (item == 0) /* Cut */
                return 0;
            if (item == 1) /* Copy */
                return !calc_error && calc_entry_len > 0;
            if (item == 2) /* Paste */
                return clip_is_number();
            return 0;
        }
        if (front_kind() == WK_SPREADSHEET) {
            if (item == 0 || item == 1) return 1;
            return item == 2 && clip_generation != 0 && file_clipboard_mode()==FILE_CLIPBOARD_NONE;
        }
        if (front_kind() == WK_WRITER) {
            if (item == 0 || item == 1) return writer_caret() != writer_anchor();
            return item == 2 && clip_len > 0;
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
        if(item==8)return !open_dlg && trash_id >= 0 && fs_child_count(trash_id) > 0;
        if (!open_dlg && (snake_is_front() || wordle_is_front()))
            return item == 0 || item == 3; /* New Game, Close */
        if (item == 1) {
            if (open_dlg)
                return 0;
            return front_kind() == WK_FILES;
        }
        if (item == 4) { /* Save */
            int fk = front_kind();
            return !open_dlg && (fk == WK_EDIT || fk == WK_PAINT || fk == WK_WRITER || fk == WK_SPREADSHEET || fk == WK_TODO || fk == WK_CAL);
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
    (void)item;
    return 1;
}

static void close_front(void) {
    int i = win_front();
    if (i >= 0)
        win_request_close(i);
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
    if(!fm_cwd_valid())return -2;
    int up = fm_has_parent() ? 1 : 0;
    if (up && row == 0)
        return -1; /* synthetic ".." */
    int i = row - up;
    if (i < 0 || i >= fm_count ||
        !file_view_valid(fm_cwd,fm_ids[i],fm_ids_identity[i])) return -2;
    return fm_ids[i];
}

static void fm_refresh(void) {
    /* A row position is not a selection identity. Preserve only the same live
     * object; filtering, deletion or node-slot reuse clears the selection. */
    int selected_id=fm_row_id(fm_selected);
    unsigned selected_identity=selected_id>=0?fs_identity(selected_id):0;
    int parent_selected=selected_id==-1;
    fm_checked_cwd();
    fm_view.hidden[0]=fm_cwd==fs_root()?trash_id:-1;
    fm_view.hidden[1]=fm_cwd==fs_root()?prefs_id:-1;
    int capacity=window_state_reserve(&window_state[context_slot],fs_child_count(fm_cwd));
    fm_count=file_view_build(fm_cwd,&fm_view,fm_ids,fm_ids_identity,capacity,&fm_total);
    int index=file_view_find(fm_ids,fm_ids_identity,fm_count,selected_id,selected_identity);
    fm_selected=index>=0?index+(fm_has_parent()?1:0):parent_selected&&fm_has_parent()?0:-1;
}

/* The filter belongs to this folder; leaving the folder clears it. */
static void fm_filter_show(void) {
    fm_rename_cancel();
    fm_filter_open=fm_filter_focus=1;
    fm_filter_select_all=fm_view.filter[0]!=0;
    fm_last_click_item=-1;
    dirty=1;
}

static void fm_filter_clear(int close) {
    fm_rename_cancel();
    fm_view.filter[0]=0;
    fm_filter_open=fm_filter_focus=!close;
    fm_filter_select_all=0;
    fm_manual_scroll=fm_first=0;
    fm_last_click_item=-1;
    fm_refresh();
    dirty=1;
}

static void fm_sort_by(int sort) {
    fm_rename_cancel();
    fm_filter_focus=fm_filter_select_all=0;
    if(fm_view.sort==sort)fm_view.descending=!fm_view.descending;
    else {fm_view.sort=sort;fm_view.descending=sort==FILE_VIEW_MODIFIED;}
    fm_manual_scroll=0;
    if(fm_selected<0)fm_first=0;
    fm_last_click_item=-1;
    fm_refresh();
    dirty=1;
}

static int fm_filter_key(void) {
    if(!fm_filter_open)return 0;
    if(key_sc==KEY_ESC){fm_filter_clear(1);return 1;}
    if(!fm_filter_focus||key_sc==0x44)return 0;
    if(key_sc==KEY_ENTER||key_sc==KEY_TAB){fm_filter_focus=fm_filter_select_all=0;dirty=1;return 1;}
    if(ctrl_down){
        if(key_sc==0x1e){fm_filter_select_all=1;dirty=1;return 1;}
        /* File clipboard shortcuts must never act on a hidden input selection. */
        if(key_sc==0x2d||key_sc==0x2e||key_sc==0x2f)return 1;
        return 0;
    }
    if(alt_down)return 0;
    int length=kstrlen(fm_view.filter),changed=0;
    if(key_sc==KEY_BACKSPACE||key_sc==0x53){
        if(fm_filter_select_all){fm_view.filter[0]=0;changed=1;}
        else if(key_sc==KEY_BACKSPACE&&length){fm_view.filter[length-1]=0;changed=1;}
        fm_filter_select_all=0;
    }else if(key_char>=32&&key_char<=126){
        if(fm_filter_select_all)length=0;
        fm_filter_select_all=0;
        if(length<FILE_VIEW_FILTER_LEN-1){fm_view.filter[length++]=key_char;fm_view.filter[length]=0;changed=1;}
    }else if(key_sc==KEY_UP||key_sc==KEY_DOWN){
        fm_filter_focus=fm_filter_select_all=0;dirty=1;
        if(fm_selected<0&&fm_count){fm_selected=(key_sc==KEY_UP?fm_count-1:0)+(fm_has_parent()?1:0);return 1;}
        return 0;
    }else return 1;
    if(changed){fm_manual_scroll=fm_first=0;fm_last_click_item=-1;fm_refresh();}
    dirty=1;return 1;
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
    clipboard_changed();
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
    edit_save_busy = 0;
    edit_len = 0;
    edit_caret = 0;
    edit_buf[0] = 0;
    edit_file = -1;
    edit_identity = 0;
    edit_source.valid = 0;
    edit_saved_ok = 1; /* An untouched empty document has nothing to lose. */
    edit_scroll = 0;
    edit_dragging = 0;
    edit_sel_collapse();
}

static void edit_load(int id) {
    if (!fs_valid(id) || fs_is_dir(id) || fs_is_app(id) || fs_size(id) >= EDIT_BUF_SIZE)
        return;
    edit_save_busy = 0;
    edit_len = fs_read(id, edit_buf, EDIT_BUF_SIZE);
    if (edit_len < 0)
        edit_len = 0;
    edit_buf[edit_len] = 0;
    edit_caret = edit_len;
    edit_file = id;
    edit_identity=fs_identity(id);
    edit_capture_source();
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

static int edit_search_height(void){return !edit_search.open?0:edit_search.replace_mode?100:68;}
static void edit_search_open(int replacing){
    EditorSearch *q=&edit_search;q->open=1;q->replace_mode=replacing;q->focus=1;q->field=0;
    int lo=edit_sel_a<edit_sel_b?edit_sel_a:edit_sel_b,hi=edit_sel_a>edit_sel_b?edit_sel_a:edit_sel_b;
    if(hi>lo&&hi-lo<64){int okay=1;for(int i=lo;i<hi;i++)if(edit_buf[i]=='\n')okay=0;
        if(okay){kmemcpy(q->text[0],edit_buf+lo,hi-lo);q->text[0][hi-lo]=0;}}
    q->position[0]=kstrlen(q->text[0]);q->selected[0]=1;q->message[0]=0;dirty=1;
}
static int edit_find_next(int direction){
    EditorSearch *q=&edit_search;int size=kstrlen(q->text[0]);
    if(!size){kstrcpy(q->message,"Enter text to find.");dirty=1;return 0;}
    int start=direction<0?(edit_sel_a<edit_sel_b?edit_sel_a:edit_sel_b)-1:edit_caret;
    int found=text_find(edit_buf,edit_len,q->text[0],start,direction,q->exact);
    if(found<0){kstrcpy(q->message,"Text not found.");dirty=1;return 0;}
    edit_sel_a=found;edit_sel_b=found+size;edit_caret=edit_sel_b;
    kstrcpy(q->message,"Match selected.");dirty=1;return 1;
}
static void edit_replace_one(void){
    EditorSearch *q=&edit_search;int lo=edit_sel_a<edit_sel_b?edit_sel_a:edit_sel_b;
    int hi=edit_sel_a>edit_sel_b?edit_sel_a:edit_sel_b;
    int n=kstrlen(q->text[0]),r=kstrlen(q->text[1]);
    if(!n||hi-lo!=n||!text_match(edit_buf,edit_len,lo,q->text[0],q->exact)){
        if(!edit_find_next(1))return;
        lo=edit_sel_a;hi=edit_sel_b;
    }
    if(edit_len-(hi-lo)+r>=EDIT_BUF_SIZE){kstrcpy(q->message,"Replacement exceeds the document limit.");dirty=1;return;}
    edit_record();edit_delete_sel();
    for(int i=edit_len;i>=lo;i--)edit_buf[i+r]=edit_buf[i];
    kmemcpy(edit_buf+lo,q->text[1],r);edit_len+=r;edit_caret=lo+r;edit_sel_collapse();edit_saved_ok=0;
    edit_find_next(1);dirty=1;
}
static void edit_replace_everywhere(void){
    EditorSearch *q=&edit_search;int count=0;
    int length=text_replace_all(edit_scratch,EDIT_BUF_SIZE,edit_buf,edit_len,q->text[0],q->text[1],q->exact,&count);
    if(length<0){kstrcpy(q->message,"Empty search or replacement exceeds limit.");dirty=1;return;}
    if(count){edit_record();kmemcpy(edit_buf,edit_scratch,length+1);edit_len=length;edit_caret=0;edit_sel_collapse();edit_saved_ok=0;}
    fmt_uint(q->message,count);kstrcpy(q->message+kstrlen(q->message)," replacements.");dirty=1;
}
static void edit_search_key(void){
    EditorSearch *q=&edit_search;int f=q->field;char *text=q->text[f];int length=kstrlen(text),pos=q->position[f];
    if(key_sc==KEY_ESC){q->open=q->focus=0;dirty=1;return;}
    if(key_sc==KEY_TAB){q->field=q->replace_mode?1-f:0;q->position[q->field]=kstrlen(q->text[q->field]);q->selected[q->field]=1;dirty=1;return;}
    if(key_sc==KEY_ENTER){if(ctrl_down&&q->replace_mode){if(shift_down)edit_replace_everywhere();else edit_replace_one();}else edit_find_next(shift_down?-1:1);return;}
    if(ctrl_down&&key_sc==0x1e){q->selected[f]=1;dirty=1;return;}
    if(ctrl_down&&(key_sc==0x2e||key_sc==0x2d)&&q->selected[f]){
        clip_len=length;kmemcpy(clip_buf,text,length+1);clipboard_changed();
        if(key_sc==0x2d){text[0]=0;q->position[f]=0;q->selected[f]=0;}dirty=1;return;
    }
    if(ctrl_down&&key_sc==0x2f){
        if(q->selected[f]){text[0]=0;length=pos=0;q->selected[f]=0;}
        for(int j=0;j<clip_len&&length<63;j++)if(clip_buf[j]>=32&&clip_buf[j]<=126){
            for(int i=length;i>=pos;i--)text[i+1]=text[i];
            text[pos++]=clip_buf[j];length++;}
        q->position[f]=pos;dirty=1;return;
    }
    if(key_sc==KEY_LEFT){if(pos>0)pos--;q->selected[f]=0;}
    else if(key_sc==KEY_RIGHT){if(pos<length)pos++;q->selected[f]=0;}
    else if(key_sc==0x47){pos=0;q->selected[f]=0;}
    else if(key_sc==0x4f){pos=length;q->selected[f]=0;}
    else if(key_sc==KEY_BACKSPACE||key_sc==0x53){
        if(q->selected[f]){text[0]=0;pos=0;q->selected[f]=0;}
        else {int at=key_sc==KEY_BACKSPACE?pos-1:pos;if(at>=0&&at<length){for(int i=at;i<length;i++)text[i]=text[i+1];if(key_sc==KEY_BACKSPACE)pos--;}}
    }else if(key_char>=32&&key_char<=126&&!ctrl_down&&!alt_down){
        if(q->selected[f]){text[0]=0;length=pos=0;q->selected[f]=0;}
        if(length<63){for(int i=length;i>=pos;i--)text[i+1]=text[i];text[pos++]=key_char;}
    }
    q->position[f]=pos;q->message[0]=0;dirty=1;
}
/* Find bar buttons: next, previous, [replace, replace all], match case, close. */
static int edit_search_buttons(int wx,int ww,int replace,int *xs,int *ws){
    int n=0,x=wx+12;
    xs[n]=x;ws[n++]=32;x+=36;
    xs[n]=x;ws[n++]=32;x+=44;
    if(replace){xs[n]=x;ws[n]=tool_button_w("Replace");x+=ws[n++]+4;xs[n]=x;ws[n]=tool_button_w("All");x+=ws[n++]+8;}
    xs[n]=x;ws[n++]=40;
    xs[n]=wx+ww-44;ws[n++]=32;
    return n;
}
static void draw_edit_search(int wx,int wy,int ww,int wh){
    if(!edit_search.open)return;
    EditorSearch *q=&edit_search;
    int y=wy+wh-SB-edit_search_height();draw_rect(wx+1,y,ww-2,edit_search_height(),ui_chrome);
    draw_hline(wx+1,y,ww-2,ui_chrome_dk);
    for(int f=0;f<(q->replace_mode?2:1);f++){
        int fy=y+6+f*32;draw_string(f?"With":"Find",wx+12,fy+5,ui_text);
        draw_round_rect(wx+66,fy,ww-82,26,4,q->focus&&q->field==f?ui_accent:ui_border);
        draw_round_rect(wx+67,fy+1,ww-84,24,3,ui_field);
        int selected=q->focus&&q->field==f&&q->selected[f];
        if(selected)draw_rect(wx+72,fy+4,ww-96,18,ui_accent);
        draw_string_clip(q->text[f],wx+72,fy+5,selected?ui_on_accent:ui_text,wx+ww-22);
        if(q->focus&&q->field==f&&!selected){char prefix[64];kmemcpy(prefix,q->text[f],q->position[f]);prefix[q->position[f]]=0;
            int x=wx+72+ui_string_w(prefix);if(x<wx+ww-22)draw_rect(x,fy+4,1,18,ui_text);}
    }
    int by=y+(q->replace_mode?70:38);
    int xs[6],ws[6],n=edit_search_buttons(wx,ww,q->replace_mode,xs,ws);
    static const int icon[6]={ICON_CHEVRON_DOWN,ICON_CHEVRON_UP,ICON_REPLACE,ICON_REPLACE,-1,ICON_STOP};
    static const char *label[6]={0,0,"Replace","All","Aa",0};
    for(int i=0;i<n;i++){
        int b=q->replace_mode||i<2?i:i+2;
        tool_button(xs[i],by,ws[i],26,icon[b],label[b],b==4?(q->exact?TB_ACTIVE:TB_NORMAL):b==5?TB_FLAT:TB_NORMAL);
    }
}
static int edit_search_click(int wx,int wy,int ww,int wh){
    if(!edit_search.open)return 0;
    EditorSearch *q=&edit_search;int y=wy+wh-SB-edit_search_height();
    if(!hit(mouse_x,mouse_y,wx,y,ww,edit_search_height()))return 0;
    q->focus=1;
    for(int f=0;f<(q->replace_mode?2:1);f++)if(hit(mouse_x,mouse_y,wx+66,y+6+f*32,ww-82,26)){
        q->field=f;q->position[f]=kstrlen(q->text[f]);q->selected[f]=1;dirty=1;return 1;}
    int by=y+(q->replace_mode?70:38);
    int xs[6],ws[6],n=edit_search_buttons(wx,ww,q->replace_mode,xs,ws),pressed=-1;
    for(int i=0;i<n;i++)if(hit(mouse_x,mouse_y,xs[i],by,ws[i],26)){pressed=q->replace_mode||i<2?i:i+2;break;}
    if(pressed==0)edit_find_next(1);
    else if(pressed==1)edit_find_next(-1);
    else if(pressed==2)edit_replace_one();
    else if(pressed==3)edit_replace_everywhere();
    else if(pressed==4)q->exact=!q->exact;
    else if(pressed==5)q->open=q->focus=0;
    dirty=1;return 1;
}

static void namedlg_open(int target, const char *initial);
static const char *edit_conflict_message="Source changed. Save with a new name.";

static int edit_storage_busy(void) {
    edit_save_busy = fs_sync_busy();
    if (edit_save_busy) dirty = 1;
    return edit_save_busy;
}

static int edit_source_unchanged(void) {
    if(edit_source.valid&&edit_binding_matches(edit_file,&edit_source.source))return 1;
    edit_saved_ok=0;
    return 0;
}

static int edit_write_to(int id) {
    if (edit_storage_busy()) return 0;
    edit_saved_ok=0;
    if(fs_write(id,edit_buf,edit_len)!=edit_len)return 0;
    edit_file=id;edit_identity=fs_identity(id);
    /* A successful RAM write is ours even if its disk commit fails. Undo must
     * retain this new baseline, so a later retry never conflicts with itself. */
    edit_capture_source();
    if(fs_sync()<0)return 0;
    edit_saved_ok=1;
    return 1;
}

static int edit_write_named(const char *name) {
    name_failure_message=0;
    if (edit_storage_busy()) {
        name_failure_message="Disk saving. Retry Save shortly.";
        return 0;
    }
    int parent = fm_checked_cwd();
    int id = fs_find_child(parent, name);
    int created = id < 0;
    if(!created&&(id!=edit_file||!edit_binding_valid())){
        name_failure_message="That name exists. Choose another name.";
        return 0;
    }
    if(!created&&!edit_source_unchanged()){
        name_failure_message=edit_conflict_message;
        return 0;
    }
    if(edit_len<0||(unsigned)edit_len>fs_file_limit())return 0;
    if (created)
        id = fs_create(parent, name);
    if (id < 0)
        return 0;
    if(!edit_write_to(id)){
        /* Do not remove an owned empty-file write whose disk sync failed. */
        if(created&&(edit_file!=id||edit_identity!=fs_identity(id)))fs_delete(id);
        return 0;
    }
    if (find_open_kind(WK_FILES) >= 0)
        fm_refresh();
    return 1;
}

static int edit_save(void) {
    if (edit_storage_busy()) return 0;
    if (edit_file >= 0 && !edit_binding_valid()) {
        edit_file = -1;
        edit_identity = 0;
        edit_source.valid = 0;
        edit_saved_ok = 0;
    }
    if (edit_file < 0) {
        namedlg_open(0, "untitled.txt");
        return 0; /* A pending Save As is not a completed save. */
    }
    if(!edit_source_unchanged()){
        namedlg_open(0,"untitled.txt");
        name_failed=1;name_failure_message=edit_conflict_message;
        return 0;
    }
    return edit_write_to(edit_file);
}

static int writer_save_document(void) {
    int had_binding = writer_file() >= 0;
    int result = writer_save();
    if (result == WRITER_SAVE_NEEDS_NAME) {
        namedlg_open(2, "untitled.bwr");
        if (had_binding) name_failed = 1; /* Explain a changed-file conflict. */
    }
    dirty = 1;
    return result;
}
static void writer_result(int result) {
    if (result & WRITER_CHANGED) dirty = 1;
    if (result & WRITER_REQUEST_SAVE) writer_save_document();
    if (result & WRITER_REQUEST_SAVE_AS) namedlg_open(2, "untitled.bwr");
    if (result & WRITER_REQUEST_EXPORT) namedlg_open(3, "document.rtf");
    if (result & WRITER_REQUEST_PDF) namedlg_open(6, "document.pdf");
}
static int spreadsheet_save_document(void) {
    int had_binding = spreadsheet_file() >= 0;
    int result = spreadsheet_save();
    if (result == SPREADSHEET_SAVE_NEEDS_NAME) {
        namedlg_open(4, "untitled.bsh");
        if (had_binding) name_failed = 1;
    }
    dirty = 1;
    return result;
}
static void spreadsheet_result(int result) {
    if (result & SPREADSHEET_CHANGED) dirty = 1;
    if (result & SPREADSHEET_REQUEST_SAVE) spreadsheet_save_document();
    if (result & SPREADSHEET_REQUEST_SAVE_AS) namedlg_open(4, "untitled.bsh");
    if (result & SPREADSHEET_REQUEST_EXPORT) namedlg_open(5, "spreadsheet.csv");
}
/* These metadata hooks run only at desktop boundaries, not from storage
 * polling. One exact request can finish one guarded navigation action. */
static void document_save_info(int kind, DocumentSave *out) {
    if (kind == WK_WRITER) writer_save_info(out);
    else if (kind == WK_SPREADSHEET) spreadsheet_save_info(out);
    else *out = (DocumentSave){0};
}
static int document_destination_valid(void) {
    return document_action != DOCUMENT_OPEN ||
        (document_target_identity && document_target_version && document_target_incarnation &&
         fs_incarnation() == document_target_incarnation && fs_valid(document_target) &&
         fs_identity(document_target) == document_target_identity &&
         fs_content_revision(document_target) == document_target_version);
}
static int document_wait_for_save(void) {
    if (!edit_close_valid()) return 0;
    DocumentSave save; document_save_info(edit_close_kind, &save);
    if (!save.owner || !save.handle || !save.pending || save.kind != DOCUMENT_SAVE_NATIVE) return 0;
    document_pending_owner = save.owner; document_pending_handle = save.handle;
    edit_close_dlg = 1; edit_close_failed = 0; edit_close_focus = 2; dirty = 1;
    return 1;
}
static void document_persistence_poll(void) {
    int changed = writer_persistence_poll();
    changed |= spreadsheet_persistence_poll();
    if (changed) dirty = 1;
    if (!document_pending_owner) return;
    if (!edit_close_valid()) { edit_close_cancel(); return; }
    DocumentSave save; document_save_info(edit_close_kind, &save);
    if (save.owner != document_pending_owner || save.handle != document_pending_handle || save.kind != DOCUMENT_SAVE_NATIVE) {
        document_pending_owner = document_pending_handle = 0;
        edit_close_failed = 1; edit_close_dlg = 1; edit_close_focus = 2; dirty = 1; return;
    }
    if (save.pending) return;
    document_pending_owner = document_pending_handle = 0;
    if (save.result != 0 || !document_destination_valid()) {
        edit_close_failed = save.result != 0 ? 1 : 3;
        edit_close_dlg = 1; edit_close_focus = 2; dirty = 1; return;
    }
    if (edit_close_kind == WK_WRITER ? writer_dirty() : spreadsheet_dirty()) {
        edit_close_failed = 2; edit_close_dlg = 1; edit_close_focus = 2; dirty = 1; return;
    }
    int owner = edit_close_owner, action = document_action, target = document_target;
    unsigned identity = document_target_identity;
    edit_close_cancel(); document_finish(owner, action, target, identity);
}
static void edit_close_choose(int choice) {
    if (!edit_close_valid()) { edit_close_cancel(); return; }
    int owner = edit_close_owner, action = document_action, target = document_target;
    unsigned identity = document_target_identity;
    if (choice == 2) { edit_close_cancel(); return; }
    if (document_pending_owner) return; /* Cancel navigation, never the commit. */
    if (!document_destination_valid()) { edit_close_failed = 3; edit_close_focus = 2; dirty = 1; return; }
    if (choice == 1) { edit_close_cancel(); document_finish(owner, action, target, identity); return; }
    context_set(owner);
    int result = wins[owner].kind == WK_SPREADSHEET ? spreadsheet_save_document() :
                 wins[owner].kind == WK_WRITER ? writer_save_document() : edit_save();
    if (result == 1) {
        edit_close_cancel(); document_finish(owner, action, target, identity);
    } else if (result == DOCUMENT_SAVE_PENDING && document_wait_for_save()) {
        /* Exact owned boundary retained until the top-level completion hook. */
    } else if (name_dlg) {
        edit_close_dlg = 0; /* Retain the owner while Save As is pending. */
    } else { edit_close_failed = 1; edit_close_focus = 2; }
    dirty = 1;
}

static void open_files(int cwd) {
    if (win_open(WK_FILES) < 0) return;
    fm_set_cwd(cwd);
    fm_history_reset();
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

static void calc_fail(void) {
    calc_error = 1;
    kstrcpy(calc_entry, "Error");
    calc_entry_len = 5;
    calc_has_dot = 0;
    calc_fresh = 1;
    calc_op = 0;
}

static void calc_format(int v, char *out) {
    int neg = 0;
    uint32_t magnitude = v < 0 ? (uint32_t)(-(int64_t)v) : (uint32_t)v;
    neg = v < 0;
    int ip = (int)(magnitude / CALC_SCALE);
    int frac = (int)(magnitude % CALC_SCALE);
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
    int32_t right, result;
    if (!decimal_parse(calc_entry, &right) ||
        !decimal_calculate(calc_acc, right, (char)calc_op, &result)) {
        calc_fail();
        return 0;
    }
    calc_acc = result;
    calc_set_entry_from_acc(result);
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
    if (!decimal_parse(calc_entry, &calc_acc)) { calc_fail(); return; }
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
    clipboard_changed();
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
    gui_draw_window(wx, wy, CALC_W, wh, "Calculator", 0,
                    inactive ? WIN_INACTIVE : 0);
    int dx = wx + CALC_MARGIN;
    int dy = wy + TITLE_H + 12;
    int dw = CALC_W - 2 * CALC_MARGIN;
    int dh = CALC_DISP_H + 10;
    draw_round_rect(dx, dy, dw, dh, 6, ui_chrome_dk);
    draw_round_rect(dx + 1, dy + 1, dw - 2, dh - 2, 5, INK(1));

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
#define PAINT_FILL_OFFSET 0x4000
_Static_assert(PAINT_W * PAINT_H <= PAINT_FILL_OFFSET, "paint canvas overlaps fill queue");
_Static_assert(PAINT_W * PAINT_H <= 0x10000, "paint fill index overflow");
_Static_assert(PAINT_FILL_OFFSET + sizeof(uint16_t) * PAINT_W * PAINT_H <= 0xC000,
               "paint fill queue overlaps save staging");
_Static_assert(0xC000 + 8 + PAINT_W * PAINT_H <= PAINT_CAPACITY, "paint staging overflow");

#define PT_PENCIL  0
#define PT_FILL    1
#define PT_LINE    2
#define PT_RECT    3
#define PT_ELLIPSE 4
#define PT_TEXT    5
#define PT_ERASER  6

static History paint_history;
_Static_assert(8*PAINT_W*PAINT_H<=PAINT_HISTORY_CAPACITY,"paint history arena overflow");
static void paint_undo(int redo);
static uint8_t *paint_pix;
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
        (unsigned char *)PAINT_HISTORY_BASE};
    for (int i = 0; i < PAINT_NWELL2; i++)
        paint_well2[i] = idx24(paint_well2_rgb[i]);
    if (!paint_pix)
        paint_pix = (uint8_t *)PAINT_MEM;
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
    int start = sy * PAINT_W + sx;
    uint8_t oc = paint_pix[start];
    if (oc == nc)
        return;
    uint16_t *queue = (uint16_t *)(PAINT_MEM + PAINT_FILL_OFFSET);
    int head = 0, tail = 0;
    /* Mark on enqueue: every pixel enters at most once, so the fixed queue
     * needs at most PAINT_W * PAINT_H entries without dropping any neighbors.
     * Canvas, queue, save staging and undo history all have separate storage. */
    paint_pix[start] = nc;
    queue[tail++] = (uint16_t)start;
    while (head < tail) {
        int p = queue[head++];
        int x = p % PAINT_W;
        int y = p / PAINT_W;
        if (x > 0 && paint_pix[p - 1] == oc) {
            paint_pix[p - 1] = nc;
            queue[tail++] = (uint16_t)(p - 1);
        }
        if (x < PAINT_W - 1 && paint_pix[p + 1] == oc) {
            paint_pix[p + 1] = nc;
            queue[tail++] = (uint16_t)(p + 1);
        }
        if (y > 0 && paint_pix[p - PAINT_W] == oc) {
            paint_pix[p - PAINT_W] = nc;
            queue[tail++] = (uint16_t)(p - PAINT_W);
        }
        if (y < PAINT_H - 1 && paint_pix[p + PAINT_W] == oc) {
            paint_pix[p + PAINT_W] = nc;
            queue[tail++] = (uint16_t)(p + PAINT_W);
        }
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

static uint8_t doodle_ink;
static void draw_tool_doodle(int kind, int x, int y) {
    int x0 = x + 6, y0 = y + 6, x1 = x + 21, y1 = y + 21;
    int cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    if (kind == PT_PENCIL) {
        for (int i = 0; i <= 14; i++) {
            put_pixel(x0 + i, y1 - i, doodle_ink);
            put_pixel(x0 + i, y1 - i - 1, doodle_ink);
        }
        draw_rect(x1 - 2, y0, 3, 3, doodle_ink);
    } else if (kind == PT_FILL) {
        uint8_t ink = doodle_ink;
        draw_round_rect(cx - 5, cy - 3, 11, 9, 2, ink);
        draw_round_rect(cx - 4, cy - 2, 9, 6, 1, INK(5));
        draw_line(cx - 4, cy - 3, cx + 1, cy - 8, ink);
        draw_rect(cx + 4, cy - 8, 2, 6, ink);
        draw_round_rect(cx + 3, cy + 4, 3, 5, 1, paint_color);
        put_pixel(cx + 4, cy + 9, paint_color);
    } else if (kind == PT_LINE) {
        for (int i = 0; i <= 14; i++)
            put_pixel(x0 + i, y1 - i, doodle_ink);
    } else if (kind == PT_RECT) {
        draw_rect(x0, y0 + 2, x1 - x0, y1 - y0 - 4, INK(4));
        draw_rect(x0, y0 + 2, x1 - x0, 1, doodle_ink);
        draw_rect(x0, y1 - 3, x1 - x0, 1, doodle_ink);
        draw_rect(x0, y0 + 2, 1, y1 - y0 - 4, doodle_ink);
        draw_rect(x1 - 1, y0 + 2, 1, y1 - y0 - 4, doodle_ink);
    } else if (kind == PT_ELLIPSE) {
        plot_dest = 1;
        plot_ox = 0;
        plot_oy = 0;
        plot_col = doodle_ink;
        /* draw oval into screen pixels at 1x via plot_lg scaled; use tiny ellipse */
        int ecx = cx, ecy = cy;
        int a = 7, b = 5;
        for (int yy = -b; yy <= b; yy++) {
            for (int xx = -a; xx <= a; xx++) {
                int inside = xx * xx * b * b + yy * yy * a * a;
                int ring = a * a * b * b;
                int inner = (a - 1) * (a - 1) * (b - 1) * (b - 1);
                if (inside <= ring && inside >= inner)
                    put_pixel(ecx + xx, ecy + yy, doodle_ink);
            }
        }
    } else if (kind == PT_TEXT) {
        draw_char('A', cx - 4, cy - 8, doodle_ink);
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
            draw_round_rect(tx + 1, ty + 1, PAINT_CELL - 2, PAINT_CELL - 2, 5, TK_ACCENT_100);
        } else {
            draw_round_rect(tx, ty, PAINT_CELL, PAINT_CELL, 6, INK(10));
            draw_round_rect(tx + 1, ty + 1, PAINT_CELL - 2, PAINT_CELL - 2, 5, INK(6));
        }
        doodle_ink = i == paint_tool ? ui_on_accent : ui_text;
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
                draw_round_rect(bx + 1, by + 1, PAINT_CELL - 2, PAINT_CELL - 2, 5, TK_ACCENT_100);
            } else {
                draw_round_rect(bx, by, PAINT_CELL, PAINT_CELL, 6, INK(10));
                draw_round_rect(bx + 1, by + 1, PAINT_CELL - 2, PAINT_CELL - 2, 5, INK(6));
            }
            int rad = i + 1;
            uint8_t dotc = on ? ui_on_accent : ui_text;
            draw_round_rect(bx + PAINT_CELL / 2 - rad, by + PAINT_CELL / 2 - rad,
                            rad * 2, rad * 2, rad, dotc);
        }
        int cx, cy;
        paint_chip_cell(wx, wy, ww, &cx, &cy);
        draw_round_rect(cx - 2, cy - 2, 44, 28, 7, INK(12));
        draw_round_rect(cx - 1, cy - 1, 42, 26, 6, INK(2));
        draw_round_rect(cx + 2, cy + 2, 36, 20, 4, paint_color);
    }

    /* canvas pane + 1px black inset */
    int px, py, pw, ph, cx, cy;
    paint_canvas_geom(wx, wy, ww, wh, &px, &py, &pw, &ph, &cx, &cy);
    draw_rect(px, py, pw, 1, INK(9));
    draw_rect(px, py + ph - 1, pw, 1, INK(9));
    draw_rect(px, py, 1, ph, INK(9));
    draw_rect(px + pw - 1, py, 1, ph, INK(9));

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
        draw_round_rect(sx, sy, 22, 18, 4, cur ? ui_text : INK(10));
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
    name_failure_message=0;
    if (fs_sync_busy()) {
        name_failure_message="Disk saving. Retry Save shortly.";
        return 0;
    }
    paint_init();
    int pics = fs_find_child(fs_root(), "Pictures");
    if (pics >= 0 && !fs_is_dir(pics)) {
        name_failure_message="Pictures is not a folder.";
        return 0;
    }
    /* Paint has no owned-file binding: every save needs a new name. */
    if (pics >= 0 && fs_find_child(pics, name) >= 0) {
        name_failure_message="Name exists. Choose another name.";
        return 0;
    }
    int created_pics = pics < 0;
    int count = fs_node_count() + 1 + created_pics;
    int nbytes = 8 + PAINT_W * PAINT_H;
    if (count > fs_node_limit()) {
        name_failure_message="Not enough free file slots.";
        return 0;
    }
    unsigned capacity = fs_capacity_for_nodes((unsigned)count);
    unsigned used = fs_used_bytes();
    if (nbytes > FS_MAX_SIZE || (unsigned)nbytes > fs_file_limit() ||
        used > capacity || (unsigned)nbytes > capacity - used) {
        name_failure_message="Not enough space for this picture.";
        return 0;
    }
    if (created_pics) pics = fs_mkdir(fs_root(), "Pictures");
    if (pics < 0) {
        name_failure_message="Cannot create Pictures folder.";
        return 0;
    }
    int id = fs_create(pics, name);
    if (id < 0) {
        if (created_pics) fs_delete(pics);
        name_failure_message="Save failed. Check the file name.";
        return 0;
    }
    char *buf = (char *)(PAINT_MEM + 0xC000);
    buf[0] = 'B'; buf[1] = 'O'; buf[2] = 'S'; buf[3] = '1';
    buf[4] = (char)(PAINT_W & 0xFF);
    buf[5] = (char)((PAINT_W >> 8) & 0xFF);
    buf[6] = (char)(PAINT_H & 0xFF);
    buf[7] = (char)((PAINT_H >> 8) & 0xFF);
    kmemcpy(buf + 8, paint_pix, PAINT_W * PAINT_H);
    if (fs_write(id, buf, nbytes) != nbytes) {
        /* No application dispatch or snapshot start occurs within this compound
         * operation. Only our new nodes are removed; old paths stay untouched. */
        fs_delete(id);
        if (created_pics) fs_delete(pics);
        name_failure_message="Picture not saved. Try again.";
        return 0;
    }
    if (find_open_kind(WK_FILES) >= 0)
        fm_refresh();
    dirty = 1;
    return 1;
}

static void paint_save(void) {
    paint_init();
    if (fs_sync_busy()) {
        namedlg_open(1, "untitled.pbm");
        name_failed=1;name_failure_message="Disk saving. Retry Save shortly.";
        return;
    }
    /* Opening or canceling the dialog must not create a filesystem path. */
    int pics = fs_find_child(fs_root(), "Pictures");
    char name[FS_NAME_LEN];
    kstrcpy(name, "untitled.pbm");
    if (fs_is_dir(pics) && unique_untitled_pbm(pics, name) < 0)
        kstrcpy(name, "art.pbm");
    namedlg_open(1, name);
    if (pics >= 0 && !fs_is_dir(pics)) {
        name_failed=1;name_failure_message="Pictures is not a folder.";
    }
}

static void open_paint(void) {
    paint_init();
    open_dlg = 0;
    win_open(WK_PAINT);
    dirty = 1;
}

static int is_image_file(int id) {
    return fs_valid(id)&&!fs_is_dir(id)&&!fs_is_app(id)&&
           image_probe(fs_data(id),(unsigned)fs_size(id))!=IMAGE_FORMAT_NONE;
}

static void open_view(int file_id) {
    if(!fs_valid(file_id)||fs_is_dir(file_id)||fs_is_app(file_id))return;
    if(win_open(WK_VIEW)<0)return;
    image_viewer_open(fs_data(file_id),(unsigned)fs_size(file_id),fs_name(file_id));
    open_dlg=0;dirty=1;
}

static void draw_view(int wx,int wy,int ww,int wh,int inactive) {
    gui_draw_window(wx,wy,ww,wh,image_viewer_title(),0,inactive?WIN_INACTIVE:0);
    image_viewer_draw(wx+1,wy+TITLE_H+1,ww-2,wh-TITLE_H-2);
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

    /* A same-color fill is a no-op, including the undo/redo chain. */
    if (paint_tool == PT_FILL && paint_pix[ly * PAINT_W + lx] == paint_color)
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
        uint8_t body = (i == 0) ? TK_ACCENT_700 : ui_accent;
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

/* Both full composition and stable native-task updates use this exact layout.
 * A zero-height result retains the original eight-pixel canvas gap. */
static int term_canvas_geometry(int ww,int wh,int source_w,int source_h,
                                int *target_w,int *target_h) {
    CanvasView view;
    int occupied=canvas_view_layout(&view,0,0,ww,wh,source_w,source_h,TITLE_H,TERM_PAD,EDIT_LINE_H);
    *target_w=view.viewport_w;*target_h=view.viewport_h;
    return occupied;
}
/* Blit a stable published snapshot. Polling here only acquires device input;
 * it never schedules an app or publishes a partially drawn frame. */
static void draw_app_canvas(AppCanvasFrame frame,const CanvasView *view){
    if(!frame.pixels||view->viewport_w<=0||view->viewport_h<=0)return;
    for(int y=0;y<view->viewport_h;y++){
        int source_row=(y*frame.height/view->viewport_h)*frame.width;
        for(int x=0;x<view->viewport_w;x++)
            put_pixel(view->x+x,view->y+y,frame.pixels[source_row+x*frame.width/view->viewport_w]);
        if(!(y&31))platform_poll();
    }
}
static int draw_term_canvas(int wx,int wy,int ww,int wh) {
    const unsigned char *canvas=term_canvas();
    if(!canvas)return 0;
    int source_w=term_canvas_width(),source_h=term_canvas_height();
    CanvasView view;
    int occupied=canvas_view_layout(&view,wx,wy,ww,wh,source_w,source_h,TITLE_H,TERM_PAD,EDIT_LINE_H);
    draw_app_canvas((AppCanvasFrame){canvas,source_w,source_h},&view);
    return occupied;
}

#define NATIVE_TOOLBAR_H 30
#define NATIVE_CLIENT_PAD 8
/* Sole per-backend geometry resolver, shared by drawing, snapshots and hit
 * testing. Owned Output preserves the published transform but blocks input. */
static CanvasView native_canvas_geometry(int slot){
    const Win *w=&wins[slot];AppCanvasFrame frame=app_view_frame(slot);CanvasView view;
    if(!frame.pixels)frame.width=frame.height=0;
    if(w->kind==WK_NATIVE)
        canvas_view_native_layout(&view,w->x+1+NATIVE_CLIENT_PAD,
            w->y+TITLE_H+1+NATIVE_TOOLBAR_H+NATIVE_CLIENT_PAD,
            w->w-2-2*NATIVE_CLIENT_PAD,w->h-TITLE_H-2-NATIVE_TOOLBAR_H-2*NATIVE_CLIENT_PAD,
            frame.width,frame.height);
    else canvas_view_layout(&view,w->x,w->y,w->w,w->h,frame.width,frame.height,TITLE_H,TERM_PAD,EDIT_LINE_H);
    return view;
}
static int native_output_rows(const Win *w){
    int rows=(w->h-TITLE_H-2-NATIVE_TOOLBAR_H-2*NATIVE_CLIENT_PAD)/EDIT_LINE_H;
    return rows>0?rows:1;
}
static int native_output_columns(const Win *w){
    int cols=(w->w-2-2*NATIVE_CLIENT_PAD)/EDIT_CHAR_W;
    return cols>0?cols:1;
}
static int native_output_total(int slot){
    int cols=native_output_columns(&wins[slot]),total=0;
    for(int i=0;i<app_view_output_count(slot);i++){
        int len=kstrlen(app_view_output_line(slot,i));
        total+=len?(len+cols-1)/cols:1;
    }
    return total;
}
static void native_output_scroll_by(int slot,int delta){
    if(!app_view_output_visible(slot))return;
    int max=native_output_total(slot)-native_output_rows(&wins[slot]);
    if(max<0)max=0;
    int offset=native_output_scroll[slot]+delta;
    native_output_scroll[slot]=offset<0?0:offset>max?max:offset;dirty=1;
}
static void native_output_toggle(int slot){
    app_view_output_show(slot,!app_view_output_visible(slot));
    native_output_scroll[slot]=0;
    /* Capture cancellation and held-button suppression use the same engine
     * as modal/geometry changes; the toggling click cannot reach the canvas. */
    native_ui_refresh(timer_ticks(),device_input.buttons);dirty=1;
}
static void draw_native_window(int slot,int inactive){
    Win *w=&wins[slot];char title[APP_VIEW_TITLE_LEN];
    gui_draw_window(w->x,w->y,w->w,w->h,win_display_title(slot,title),0,inactive?WIN_INACTIVE:0);
    int top=w->y+TITLE_H+1;
    draw_rect(w->x+1,top,w->w-2,w->h-TITLE_H-2,INK(30));
    draw_rect(w->x+1,top,w->w-2,NATIVE_TOOLBAR_H,ui_chrome_dk);
    int output=app_view_output_visible(slot);
    tool_button(w->x+8,top+3,100,NATIVE_TOOLBAR_H-6,ICON_LIST,app_view_output_dropped(slot)?"Output*":"Output",
                output?TB_ACTIVE:TB_FLAT);
    AppViewResult result;
    int ended=app_view_result(slot,&result);
    const char *status=ended?
        (result.reason==PROCESS_EXIT_STOP?"Stopped":"Ended; see Output"):
        "Ctrl+C stops; Close ends task";
    if(output&&app_view_output_dropped(slot))status="Older output discarded";
    draw_string_clip(status,w->x+90,top+7,ui_text_dim,w->x+w->w-12);
    if(output){
        int rows=native_output_rows(w),count=app_view_output_count(slot);
        int total=native_output_total(slot),max=total>rows?total-rows:0;
        if(native_output_scroll[slot]>max)native_output_scroll[slot]=max;
        int first=total>rows?total-rows-native_output_scroll[slot]:0;
        int cols=native_output_columns(w),visual=0;
        int x=w->x+1+NATIVE_CLIENT_PAD,y=top+NATIVE_TOOLBAR_H+NATIVE_CLIENT_PAD;
        for(int i=0;i<count;i++){
            const char *text=app_view_output_line(slot,i);int len=kstrlen(text);
            int chunks=len?(len+cols-1)/cols:1;
            for(int chunk=0;chunk<chunks;chunk++,visual++){
                int row=visual-first;
                if(row>=0&&row<rows)term_text(text+chunk*cols,x,y+row*EDIT_LINE_H,0,cols);
            }
        }
        if(!count)term_text("No output yet.",x,y,0,cols);
    }else{
        AppCanvasFrame frame=app_view_frame(slot);CanvasView view=native_canvas_geometry(slot);
        if(frame.pixels)draw_app_canvas(frame,&view);
        else draw_string_clip(ended?"No published frame; see Output":"Starting...",
                              w->x+16,top+NATIVE_TOOLBAR_H+16,TERM_FG,w->x+w->w-16);
    }
}

static void draw_term(int wx, int wy, int ww, int wh, int inactive) {
    char title[TERM_TASK_TITLE_LEN];
    gui_draw_window(wx, wy, ww, wh, win_display_title(context_slot, title), 0, inactive ? WIN_INACTIVE : 0);
    draw_rect(wx, wy + TITLE_H + 1, ww, wh - TITLE_H - 1, INK(30));

    int ax = wx + 1 + TERM_PAD;
    int canvas_height=draw_term_canvas(wx,wy,ww,wh);
    int ay = wy + TITLE_H + 1 + TERM_PAD + canvas_height;
    int cols = (ww - 2 - TERM_PAD * 2) / EDIT_CHAR_W;
    int rows = (wh - TITLE_H - 2 - TERM_PAD * 2 - canvas_height) / EDIT_LINE_H;
    if (cols < 1 || rows < 1)
        return;

    /* Reflow output to the current window width; manuals remain readable
     * even in a narrow terminal, and scrolling counts visual rows. */
    char live[TERM_COLS*2+16];
    if(term_task_running(context_slot))kstrcpy(live,"Native task active. Ctrl+C stops; keys go to this task.");
    else {term_prompt(live,TERM_COLS+8);kstrcpy(live+kstrlen(live),term_input());}
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
        uint8_t w = ui_on_accent;
        draw_line(x + 2, y + 6, x + 5, y + 9, w);
        draw_line(x + 3, y + 6, x + 6, y + 9, w);
        draw_line(x + 5, y + 9, x + 11, y + 3, w);
        draw_line(x + 6, y + 9, x + 12, y + 3, w);
    } else {
        draw_round_rect(x - 2, y - 2, s, s, s / 2, INK(12));
        draw_round_rect(x - 1, y - 1, s - 2, s - 2, s / 2 - 1, ui_field);
    }
}

static void todo_field_box(int wx, int wy, int ww, int wh,
                           int *fx, int *fy, int *fw, int *fh) {
    *fx = wx + 12;
    *fy = wy + wh - TODO_FOOT_H + 8;
    *fw = ww - 100;
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
    draw_rect(wx + 1, wy + TITLE_H + 1, ww - 2, wh - TITLE_H - 2, ui_body);

    int rows = todo_rows_fit(wh);
    int n = todo_count();
    if (n > rows)
        n = rows;
    if (!n) {
        const char *empty = "No tasks yet", *hint = "Type below and press Enter to add one.";
        int my = wy + TITLE_H + (wh - TITLE_H - TODO_FOOT_H) / 2 - CHAR_H;
        draw_string_bold(empty, wx + (ww - uib_string_w(empty)) / 2, my, ui_text);
        draw_string(hint, wx + (ww - ui_string_w(hint)) / 2, my + CHAR_H + 6, ui_text_dim);
    }
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
    tool_button(wx+ww-76, fy, 64, fh, ICON_SAVE, 0, TB_NORMAL);
    draw_string_clip(todo_status(), wx+12, ruley+37, ui_text_dim, wx+ww-12);
    int active = todo_field_active();
    draw_round_rect(fx, fy, fw, fh, 5, active ? ui_accent : ui_border);
    draw_round_rect(fx + 1, fy + 1, fw - 2, fh - 2, 4, ui_field);

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
    if(hit(mouse_x,mouse_y,wx+ww-76,fy,64,fh)){
        todo_retry_save();dirty=1;return;
    }
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

static int file_extension(const char *name,const char *suffix) {
    int n=kstrlen(name),m=kstrlen(suffix);if(n<m)return 0;
    for(int i=0;i<m;i++){char c=name[n-m+i];if(c>='A'&&c<='Z')c+=32;if(c!=suffix[i])return 0;}
    return 1;
}
static void open_browser(int file) {
    if(win_open(WK_BROWSER)<0)return;
    if(file>=0)browser_open_file(file);
    dirty=1;
}
static void open_player(int file) {
    if(win_open(WK_PLAYER)<0)return;
    player_refresh();if(file>=0)player_open_file(file);
    dirty=1;
}

static void open_writer(int file) {
    int slot = win_open(WK_WRITER);
    if (slot < 0) return;
    if (file >= 0) document_request(slot, DOCUMENT_OPEN, file);
    else if (!fs_is_dir(fm_cwd) || fm_cwd == fs_root()) {
        int docs = fs_find_child(fs_root(), "Documents");
        fm_set_cwd(fs_is_dir(docs) ? docs : fs_root());
    }
    dirty = 1;
}
static void open_spreadsheet(int file) {
    int slot = win_open(WK_SPREADSHEET);
    if (slot < 0) return;
    if (file >= 0) document_request(slot, DOCUMENT_OPEN, file);
    else if (!fs_is_dir(fm_cwd) || fm_cwd == fs_root()) {
        int docs = fs_find_child(fs_root(), "Documents");
        fm_set_cwd(fs_is_dir(docs) ? docs : fs_root());
    }
    dirty = 1;
}
enum { NATIVE_LAUNCH_WINDOW=-40,NATIVE_LAUNCH_SERVICE=-41,NATIVE_LAUNCH_ORDER=-42 };
static const char *native_launch_error(int result){
    switch(result){
    case NATIVE_LAUNCH_WINDOW:return "Cannot start: close a window to run this app.";
    case NATIVE_LAUNCH_SERVICE:return "Cannot start: native window input is unavailable.";
    case NATIVE_LAUNCH_ORDER:return "Cannot start: window identities are exhausted.";
    case PROCESS_CREATE_MEMORY:return "Cannot start: native backing memory is unavailable.";
    case -1:return "Cannot start: all native process records are in use.";
    case PROCESS_CREATE_UNSUPPORTED:return "Cannot start: this executable format or ABI is not enabled.";
    case PROCESS_CREATE_LAYOUT:return "Cannot start: executable exceeds file or memory policy.";
    case APP_VIEW_START_INVALID:return "Cannot start: the selected program changed or is unavailable.";
    case APP_VIEW_START_BUSY:return "Cannot start: the native view is still in use.";
    case APP_VIEW_START_ARGUMENT:return "Cannot start: use an absolute document path of at most 128 bytes.";
    case APP_VIEW_START_GENERATION:return "Cannot start: this view's binding identities are exhausted.";
    case APP_VIEW_START_ATTACHMENT:return "Cannot start: native output attachment is unavailable.";
    case APP_VIEW_START_UNAVAILABLE:return "Cannot start: the native process is unavailable.";
    default:return "Cannot start: invalid native executable header or layout.";
    }
}
/* Invisible reservation -> CREATED/bound process -> committed WM, with no
 * polling or callbacks in between. No Terminal is selected or initialized. */
static int native_window_start(int id,unsigned identity,const char *argument,unsigned length){
    if(!native_ui_available())return NATIVE_LAUNCH_SERVICE;
    if(wm_seq==0x7fffffff||wm_z==0x7fffffff)return NATIVE_LAUNCH_ORDER;
    int slot=-1;
    for(int i=0;i<MAX_WIN;i++)if(!wins[i].open){slot=i;break;}
    if(slot<0)return NATIVE_LAUNCH_WINDOW;
    Win prepared={0};prepared.kind=WK_NATIVE;
    layout_window(WK_NATIVE,&prepared.x,&prepared.y,&prepared.w,&prepared.h);
    win_place(&prepared);
    int result=app_view_start_owned_file(slot,id,identity,argument,length);
    if(result)return result;
    /* START invokes no output, and no scheduler can run before publication. */
    window_state_clear(slot);
    native_output_scroll[slot]=0;
    prepared.open=1;prepared.seq=++wm_seq;wins[slot]=prepared;
    win_focus(slot);return 0;
}
static int native_file_mode(int id,unsigned identity,unsigned *mode){
    if(!identity||!fs_valid(id)||fs_is_dir(id)||fs_is_app(id)||fs_identity(id)!=identity)
        return APP_VIEW_START_INVALID;
    return process_probe_launch(fs_data(id),fs_size(id),mode);
}
static int terminal_native_launch(int caller,int id,unsigned identity,const char *argument,unsigned length){
    unsigned mode=0;int result=native_file_mode(id,identity,&mode);
    if(!result&&mode==PROCESS_LAUNCH_HOSTED)
        return term_task_start_file_with_arg(caller,id,identity,argument,length);
    if(!result)result=native_window_start(id,identity,argument,length);
    if(result){term_write_at(caller,native_launch_error(result));return TERM_COMMAND_REPORTED;}
    native_launch_status="";return 0;
}
/* Files and Launcher share the same checked launch, including early refusal. */
static void open_native_file(int id) {
    unsigned identity=fs_identity(id),mode=0;
    int result=native_file_mode(id,identity,&mode);
    if(!result&&mode==PROCESS_LAUNCH_OWNED_WINDOW)result=native_window_start(id,identity,0,0);
    else if(!result){
        int parent=fs_parent(id),slot=win_open(WK_TERM);
        if(slot<0)result=NATIVE_LAUNCH_WINDOW;
        else {term_set_cwd(parent);term_task_start_file(slot,id,identity);}
    }
    native_launch_status=result?native_launch_error(result):"";
    if(!result)open_dlg=0;
    dirty=1;
}

static void open_fs_file(int id) {
    if (!fs_valid(id))
        return;
    const char *n = fs_name(id);
    if(!fs_is_dir(id)&&!fs_is_app(id))recent_add(id);
    if(fs_is_app(id)&&!kstrcmp(n,"Browser")){open_browser(-1);return;}
    if(fs_is_app(id)&&!kstrcmp(n,"Media Player")){open_player(-1);return;}
    if(fs_is_app(id)&&!kstrcmp(n,"Writer")){open_writer(-1);return;}
    if(fs_is_app(id)&&!kstrcmp(n,"Spreadsheet")){open_spreadsheet(-1);return;}
    if(!fs_is_dir(id)&&!fs_is_app(id)){
        if(file_extension(n,".bex")){open_native_file(id);return;}
        if(file_extension(n,".bwr")){open_writer(id);return;}
        if(file_extension(n,".bsh")||file_extension(n,".csv")){open_spreadsheet(id);return;}
        if(file_extension(n,".html")||file_extension(n,".htm")){open_browser(id);return;}
        if(file_extension(n,".wav")||file_extension(n,".wave")||file_extension(n,".mp3")||file_extension(n,".mpg")||file_extension(n,".mpeg")){open_player(id);return;}
    }
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
    if (is_image_file(id)||file_extension(n,".jpg")||file_extension(n,".jpeg")||file_extension(n,".png")||file_extension(n,".bmp")||file_extension(n,".gif")) {
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
    if (fs_size(id) >= EDIT_BUF_SIZE) {
        properties_id=id;
        properties_reason="Too large for Editor (maximum 65535 bytes).";
        win_open(WK_PROPERTIES);dirty=1;return;
    }
    if (win_open(WK_EDIT) < 0) return;
    fm_set_cwd(fs_parent(id));
    edit_load(id);
    dirty = 1;
}




static void layout_window(int kind, int *x, int *y, int *w, int *h) {
    switch (kind) {
    case WK_NATIVE:
        *w=640;*h=480;break;
    case WK_SPREADSHEET:
        *w=SPREADSHEET_W+2;*h=SPREADSHEET_H+TITLE_H+2;break;
    case WK_WRITER:
        *w=WRITER_W+2;*h=WRITER_H+TITLE_H+2;break;
    case WK_BROWSER:
        *w=BROWSER_W+2;*h=BROWSER_H+TITLE_H+2;break;
    case WK_PLAYER:
        *w=PLAYER_W+2;*h=PLAYER_H+TITLE_H+2;break;
    case WK_PROPERTIES:
        *x=300;*y=140;*w=500;*h=380;break;
    case WK_HELLO:
        *w = 520;
        *h = 170;
        break;
    case WK_HELP:
        *w = 640;
        *h = 262;
        break;
    case WK_ABOUT:
        *w = 520;
        *h = 344;
        break;
    case WK_SETTINGS:
        *w = 560;
        *h = settings_window_h();
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
        *w=IMAGE_VIEWER_W+2;*h=IMAGE_VIEWER_H+TITLE_H+2;break;
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
    /* These sizes predate the window menu strip; give back its height so the
     * content keeps its original bottom margin. */
    if (kind == WK_CALC || kind == WK_SNAKE ||
        kind == WK_WORDLE || kind == WK_TERM || kind == WK_TODO || kind == WK_PAINT)
        *h += WMENU_H;
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
    *ah = wh - top - 24 - SB - edit_search_height();
    if(*ah<EDIT_LINE_H)*ah=EDIT_LINE_H;
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
    unsigned dt = input_gesture_ticks() - *last_frame;
    int is_dbl = (item == *last_item && dt < dbl_click_ticks());
    *last_item = item;
    *last_frame = input_gesture_ticks();
    return is_dbl;
}

/* Back/forward history for each Files window. Entries are folder
 * incarnations; deleted or replaced folders are skipped when stepping. */
static void fm_history_reset(void) {
    WindowState *ws = &window_state[context_slot];
    ws->hist_n = 1; ws->hist_pos = 0;
    ws->hist[0] = fm_cwd; ws->hist_identity[0] = fs_identity(fm_cwd);
}
static void fm_history_sync(void) {
    WindowState *ws = &window_state[context_slot];
    if (ws->hist_n <= 0 || ws->hist_n > 12 || ws->hist_pos < 0 || ws->hist_pos >= ws->hist_n) { fm_history_reset(); return; }
    if (ws->hist[ws->hist_pos] == fm_cwd && ws->hist_identity[ws->hist_pos] == fs_identity(fm_cwd)) return;
    ws->hist_n = ws->hist_pos + 1;
    if (ws->hist_n == 12) {
        for (int i = 1; i < 12; i++) { ws->hist[i - 1] = ws->hist[i]; ws->hist_identity[i - 1] = ws->hist_identity[i]; }
        ws->hist_n = 11;
    }
    ws->hist[ws->hist_n] = fm_cwd; ws->hist_identity[ws->hist_n] = fs_identity(fm_cwd);
    ws->hist_pos = ws->hist_n++;
}
static int fm_history_target(int dir) {
    WindowState *ws = &window_state[context_slot];
    for (int p = ws->hist_pos + dir; p >= 0 && p < ws->hist_n; p += dir)
        if (fs_is_dir(ws->hist[p]) && fs_identity(ws->hist[p]) == ws->hist_identity[p]) return p;
    return -1;
}
static void fm_history_step(int dir) {
    fm_history_sync();
    int p = fm_history_target(dir);
    if (p < 0) return;
    fm_rename_cancel();
    window_state[context_slot].hist_pos = p;
    fm_set_cwd(window_state[context_slot].hist[p]);
    fm_selected = 0;
    fm_refresh();
    dirty = 1;
}
static void fm_navigate(int id) {
    if (!fs_is_dir(id)) return;
    fm_history_sync();
    fm_rename_cancel();
    fm_set_cwd(id);
    fm_history_sync();
    fm_selected = 0;
    fm_refresh();
    dirty = 1;
}

static void fm_open_selected(void) {
    int vis = fm_vis_count();
    if (vis <= 0 || fm_selected < 0 || fm_selected >= vis)
        return;
    int id = fm_row_id(fm_selected);
    if (id == -1) {
        fm_go_up();
        return;
    }
    if (id < 0) {fm_refresh();dirty=1;return;}
    int index=fm_selected-(fm_has_parent()?1:0);
    if(fs_identity(id)!=fm_ids_identity[index]){fm_refresh();dirty=1;return;}
    if (fs_is_dir(id)) {
        fm_navigate(id);
    } else {
        open_fs_file(id);
    }
}

static void fm_go_up(void) {
    if(!fm_cwd_valid()){fm_refresh();dirty=1;return;}
    int p = fs_parent(fm_cwd);
    if (p >= 0)
        fm_navigate(p);
}

static void fm_select_id(int id);

static void fm_new_file(void) {
    if (files_storage_busy()) return;
    if(!fm_cwd_valid()){fm_refresh();dirty=1;return;}
    char name[FS_NAME_LEN];
    if (fs_unique_file(fm_cwd, name) < 0)
        return;
    int id = fs_create(fm_cwd, name);
    if (id < 0)
        return;
    fm_refresh();
    fm_select_id(id);
    dirty = 1;
}

static void od_refresh(void);

static void fm_select_id(int id) {
    unsigned identity=fs_identity(id);
    if(!fm_cwd_valid()||!file_view_valid(fm_cwd,id,identity))return;
    /* Completed new/paste/rename actions reveal their exact result. */
    if(!file_view_matches(fs_name(id),fm_view.filter)){
        fm_view.filter[0]=0;fm_filter_select_all=0;fm_refresh();
    }
    int index=file_view_find(fm_ids,fm_ids_identity,fm_count,id,identity);
    fm_selected=index>=0?index+(fm_has_parent()?1:0):-1;
    fm_manual_scroll=0;
}

static void fm_rename_cancel(void) {
    if (!fm_renaming)
        return;
    fm_renaming = 0;
    fm_rename_id = -1;
    fm_rename_identity=0;
    fm_rename_len = 0;
    fm_rename_buf[0] = 0;
    dirty = 1;
}

static void fm_rename_begin(int id) {
    if (id < 0 || fm_row_id(fm_selected)!=id)
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
    fm_rename_identity=fs_identity(id);
    fm_filter_focus=fm_filter_select_all=0;
    fm_renaming = 1;
    dirty = 1;
}

static void fm_rename_commit(void) {
    if (!fm_renaming)
        return;
    if (files_storage_busy()) return;
    if(!fm_cwd_valid()||!file_view_valid(fm_cwd,fm_rename_id,fm_rename_identity)){
        fm_rename_cancel();fm_refresh();dirty=1;return;
    }
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
    if (files_storage_busy()) return;
    int id = fm_row_id(fm_selected);
    if (id < 0)
        return;
    if (fs_is_app(id) || kstrcmp(fs_name(id), "Calculator") == 0)
        return;
    int parent = fs_parent(id);
    if (parent < 0)
        parent = fm_cwd;
    fm_rename_cancel();
    int copy = file_copy_named(id, parent);
    if (copy < 0)
        return;
    fm_refresh();
    fm_select_id(copy);
    dirty = 1;
}

static void files_clipboard_action(int item) {
    if(front_kind()!=WK_FILES||open_dlg||name_dlg)return;
    files_action_busy=0;
    if(!fm_cwd_valid()){fm_refresh();dirty=1;return;}
    fm_filter_focus=fm_filter_select_all=0;
    fm_rename_cancel();
    int owner=context_slot;
    if(item==0||item==1){
        if(!file_clipboard_set(fm_row_id(fm_selected),item==0?FILE_CLIPBOARD_CUT:FILE_CLIPBOARD_COPY)){
            clip_len=0;clip_buf[0]=0;
            if(!++clip_generation)++clip_generation;
        }
    }else if(item==2){
        int result_node=-1;
        int result=file_clipboard_paste(fm_cwd,&result_node);
        if(result>=0){
            for(int i=0;i<MAX_WIN;i++)if(wins[i].open&&wins[i].kind==WK_FILES){context_set(i);fm_refresh();}
            context_set(owner);fm_manual_scroll=0;fm_refresh();
            if(result_node>=0)fm_select_id(result_node);
        }
    }
    files_message_until=frame_count+8*TIMER_HZ;
    dirty=1;
}

static void do_new_folder(void) {
    /* Disk window only. untitled folder, then untitled folder 2. */
    if (front_kind() != WK_FILES || open_dlg)
        return;
    if (files_storage_busy()) return;
    if(!fm_cwd_valid()){fm_refresh();dirty=1;return;}
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
    if (files_storage_busy()) return;
    if (fs_empty_dir(trash_id) < 0) { dirty = 1; return; }
    kprint_debug("Empty trash\n");
    if (edit_file >= 0 && !fs_valid(edit_file))
        edit_clear();
    if (find_open_kind(WK_FILES) >= 0) {
        if (!fs_valid(fm_cwd))
            fm_set_cwd(trash_id);
        fm_refresh();
    }
    if (open_dlg)
        od_refresh();
    dirty = 1;
}

static int od_row_enabled(int id) {
    if (pick_sheet) return fs_is_dir(id) || (!fs_is_app(id) &&
        (file_extension(fs_name(id),".bsh") || file_extension(fs_name(id),".csv")));
    if (pick_writer) return !fs_is_app(id);
    if (!pick_pics_only)
        return 1;
    return fs_is_dir(id) || is_image_file(id);
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
    int listed = fs_child_count(pick_cwd);
    if (listed > pick_capacity) {
        int *ids = kmalloc((uint32_t)listed * sizeof *ids);
        unsigned *identities = kmalloc((uint32_t)listed * sizeof *identities);
        if (ids && identities) {
            kfree(pick_ids); kfree(pick_identity);
            pick_ids = ids; pick_identity = identities; pick_capacity = listed;
        } else {
            kfree(ids); kfree(identities);
        }
    }
    int *raw = kmalloc((uint32_t)(listed ? listed : 1) * sizeof *raw);
    int n = raw ? fs_list(pick_cwd, raw, listed < pick_capacity ? listed : pick_capacity) : 0;
    pick_count = 0;
    for (int i = 0; i < n; i++) {
        if (pick_cwd == fs_root() && trash_id >= 0 && raw[i] == trash_id)
            continue;
        if (pick_cwd == fs_root() && prefs_id >= 0 && raw[i] == prefs_id)
            continue;
        pick_identity[pick_count] = fs_identity(raw[i]);
        pick_ids[pick_count++] = raw[i];
    }
    kfree(raw);
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
    pick_owner = win_front();
    pick_writer = pick_owner >= 0 && wins[pick_owner].kind == WK_WRITER && !pics_only;
    pick_sheet = pick_owner >= 0 && wins[pick_owner].kind == WK_SPREADSHEET && !pics_only;
    pick_owner_seq = pick_owner >= 0 ? wins[pick_owner].seq : 0;
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
    if(fs_identity(id)!=pick_identity[pick_selected]){od_refresh();dirty=1;return;}
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
    if (pick_writer || pick_sheet) {
        if (pick_owner >= 0 && wins[pick_owner].open &&
            wins[pick_owner].kind == (pick_sheet ? WK_SPREADSHEET : WK_WRITER) &&
            wins[pick_owner].seq == pick_owner_seq) document_request(pick_owner, DOCUMENT_OPEN, id);
    } else open_fs_file(id);
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
    if((files_action_busy&&(int32_t)(files_message_until-frame_count)>0)||
       (file_clipboard_status()[0]&&((int32_t)(files_message_until-frame_count)>0||file_clipboard_pending_sync()))){
        const char *message=files_action_busy&&(int32_t)(files_message_until-frame_count)>0?
            "Disk saving. Retry the file action.":file_clipboard_status();int i=0;
        while(i+1<max&&message[i]){info[i]=message[i];i++;}
        if(max>0)info[i]=0;
    }else {
        char number[12];int n=0;
        fmt_uint(number,(unsigned)fm_count);
        while(number[n]&&n+1<max){info[n]=number[n];n++;}
        info[n]=0;
        const char *label=fm_view.filter[0]?" of ":fm_count==1?" item | ":" items | ";
        for(int i=0;label[i]&&n+1<max;i++)info[n++]=label[i];
        if(fm_view.filter[0]){
            fmt_uint(number,(unsigned)fm_total);
            for(int i=0;number[i]&&n+1<max;i++)info[n++]=number[i];
            label=" shown | ";
            for(int i=0;label[i]&&n+1<max;i++)info[n++]=label[i];
        }
        info[n]=0;
        if(n+1<max)fs_path(fm_cwd,info+n,max-n);
    }
}

static int file_extension(const char *name,const char *suffix);
/* Filled, color-coded file icons for unselected rows. */
/* Application nodes are named after their desktop icon; a few have longer names. */
static int app_icon_by_name(const char *name) {
    for (int i = 0; i < ICON_DESK_COUNT; i++)
        if (kstrcmp(icons[i].label, name) == 0) return icons[i].icon;
    if (kstrcmp(name, "Image Viewer") == 0) return ICON_APP_VIEWER;
    if (kstrcmp(name, "Minesweeper") == 0) return ICON_APP_MINES;
    if (kstrcmp(name, "Hello") == 0) return ICON_APP_HELLO;
    return ICON_WINDOW;
}
static void draw_file_row_named(int x, int y, int w, const char *name,
                               int is_dir, int is_app, int sel, int dim, int cut,
                               const char *detail, int detail_width) {
    uint8_t fg = dim ? ui_text_dim : (sel ? ui_on_accent : ui_text);
    uint8_t bg = sel ? ui_accent : ui_body;
    if (dim) {
        sel = 0;
        bg = ui_body;
    }
    if (sel)
        draw_round_rect(x + 2, y, w - 4, ROW_H - 2, 5, ui_accent);
    (void)bg;
    int icon_y = y + (ROW_H - AI_SMALL) / 2;
    app_icon_draw(is_dir ? AI_FILES : is_app ? app_icon_for(app_icon_by_name(name)) : AI_DOCUMENT, x + 4, icon_y, AI_SMALL);
    int ty = y + (ROW_H - CHAR_H) / 2;
    int end=x+w-12;
    if(detail&&detail_width){
        if(cut)detail="Cut";
        draw_string_clip(detail,x+w-detail_width,ty,sel?ui_on_accent:ui_text_dim,x+w-10);
        end=x+w-detail_width-12;
    }else if(w>420){
        const char *kind=cut?"Cut":is_dir?"Folder":(is_app?"Application":"Document");
        draw_string(kind,x+w-110,ty,sel?ui_on_accent:ui_text_dim);
        end=x+w-130;
    }else if(cut){
        draw_string("Cut",x+w-38,ty,sel?ui_on_accent:ui_text_dim);
        end=x+w-46;
    }
    draw_string_clip(name, x + 28, ty, fg, end);
}

static void draw_file_row_detail(int x, int y, int w, int id, int sel, int dim,
                                 const char *detail, int detail_width) {
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
    } else if (!fs_is_dir(id) && file_extension(nm,".bex")) {
        is_app = 6;
    } else if (is_image_file(id)) {
        is_app = 4;
    }
    int renaming = fm_renaming && id == fm_rename_id && fs_identity(id)==fm_rename_identity;
    draw_file_row_named(x, y, w, renaming ? "" : fs_name(id),
                        fs_is_dir(id), is_app, sel, dim,
                        file_clipboard_mode()==FILE_CLIPBOARD_CUT&&file_clipboard_source()==id&&
                        file_clipboard_identity()==fs_identity(id),detail,detail_width);
    if (!renaming)
        return;
    int fx = x + 26;
    int fy = y - 1;
    int fh = ROW_H - 2;
    int tw = ui_string_w(fm_rename_buf) + 16;
    if (tw < 80)
        tw = 80;
    int fw = tw;
    int end=x+w-(detail_width?detail_width+12:4);
    if (fx + fw > end)
        fw = end - fx;
    if (fw < 24)
        fw = 24;
    /* Field with accent outline. */
    draw_rect(fx, fy, fw, fh, ui_field);
    draw_rect(fx, fy, fw, 1, ui_accent);
    draw_rect(fx, fy + fh - 1, fw, 1, ui_accent);
    draw_rect(fx, fy, 1, fh, ui_accent);
    draw_rect(fx + fw - 1, fy, 1, fh, ui_accent);
    int ty = fy + (fh - CHAR_H) / 2;
    draw_string_clip(fm_rename_buf, fx + 4, ty, ui_text, fx + fw - 4);
    {
        int cw = ui_string_w(fm_rename_buf);
        int cx = fx + 4 + cw;
        if (cx < fx + fw - 3 && (frame_count / 35) % 2 == 0)
            draw_rect(cx, ty, 1, CHAR_H, ui_text);
    }
}

static void draw_file_row(int x, int y, int w, int id, int sel, int dim) {
    draw_file_row_detail(x,y,w,id,sel,dim,0,0);
}

static const int files_sort_width[FILE_VIEW_SORT_COUNT]={60,58,52,82};
static const char *files_sort_name[FILE_VIEW_SORT_COUNT]={"Name","Type","Size","Modified"};

/* Files layout: Places sidebar on the left, toolbar (back/forward/up, path,
 * Filter) across the top of the content, column headers, list, and a footer. */
#define FILES_TOOL_H 42
#define FILES_FOOT_H 24
#define FILES_PLACES 6
static int files_side_w(int ww) {return ww>=520?164:0;}
static int files_list_x(int wx,int ww) {return wx+files_side_w(ww)+8;}
static int files_list_w(int ww) {return ww-files_side_w(ww)-28;}
static int files_header_y(int wy) {return wy+TITLE_H+FILES_TOOL_H+3;}
static int files_filter_y(int wy) {return files_header_y(wy)+30;}
static int files_list_y(int wy) {return files_filter_y(wy)+(fm_filter_open?34:0);}
static int files_list_bottom(int wy,int wh) {return wy+wh-FILES_FOOT_H-2;}
static int files_rows(int wy,int wh) {return (files_list_bottom(wy,wh)-files_list_y(wy))/ROW_H;}
static const char *files_place_name[FILES_PLACES]={"Home","Documents","Pictures","Media","Programs","Trash"};
static int files_place_id(int i) {
    if(i==0)return fs_root();
    if(i==5)return fs_is_dir(trash_id)?trash_id:-1;
    int id=fs_find_child(fs_root(),files_place_name[i]);
    return fs_is_dir(id)?id:-1;
}
static int files_place_y(int wy,int i) {return wy+TITLE_H+40+i*32;}
static int files_nav_x(int wx,int ww,int i) {return wx+files_side_w(ww)+10+i*36;}
/* Breadcrumb segments from the root (shown as "Home") to the current folder;
 * leading segments are dropped when the path does not fit. */
static int files_crumbs(int wx,int ww,int *xs,int *ws,int *ids) {
    int chain[16],n=0,id=fm_cwd;
    while(id>=0&&n<16){chain[n++]=id;if(id==fs_root())break;id=fs_parent(id);}
    int left=files_nav_x(wx,ww,3)+6,right=wx+ww-56,count=0,x=left;
    int first=n-1,total=0;
    for(int k=n-1;k>=0;k--)total+=ui_string_w(chain[k]==fs_root()?"Home":fs_name(chain[k]))+22;
    while(first>0&&total>right-left){total-=ui_string_w(chain[first]==fs_root()?"Home":fs_name(chain[first]))+22;first--;}
    for(int k=first;k>=0&&count<16;k--){
        const char *name=chain[k]==fs_root()?"Home":fs_name(chain[k]);
        int w=ui_string_w(name)+10;
        if(x+w>right)w=right-x;
        if(w<=10)break;
        xs[count]=x;ws[count]=w;ids[count]=chain[k];count++;
        x+=w+12;
    }
    return count;
}

static void files_scroll(int rows){
    if(rows<1)rows=1;
    if(!fm_manual_scroll&&fm_selected>=0){
        if(fm_selected<fm_first)fm_first=fm_selected;
        if(fm_selected>=fm_first+rows)fm_first=fm_selected-rows+1;
    }
    int max=fm_vis_count()-rows;if(max<0)max=0;
    if(fm_first>max)fm_first=max;
    if(fm_first<0)fm_first=0;
}

static void files_modified_label(int id,char *out,int with_time) {
    unsigned stamp=fs_modified(id);
    if(!stamp){kstrcpy(out,"Unknown");return;}
    unsigned days=stamp/86400,year=2000,month=1;
    int md[]={31,28,31,30,31,30,31,31,30,31,30,31};
    for(;;){unsigned yd=(year%4==0&&(year%100!=0||year%400==0))?366:365;
        if(days<yd)break;
        days-=yd;year++;}
    md[1]=(year%4==0&&(year%100!=0||year%400==0))?29:28;
    while(month<12&&days>=(unsigned)md[month-1])days-=md[month++-1];
    fmt_uint(out,year);int n=kstrlen(out);out[n++]='-';fmt_pad2(out+n,month);n+=2;
    out[n++]='-';fmt_pad2(out+n,days+1);n+=2;out[n]=0;
    if(with_time){out[n++]=' ';fmt_pad2(out+n,stamp/3600%24);n+=2;
        out[n++]=':';fmt_pad2(out+n,stamp/60%60);n+=2;out[n]=0;}
}

static int files_detail_width(int width) {
    if(fm_view.sort==FILE_VIEW_MODIFIED)return width>=560?154:106;
    if(fm_view.sort==FILE_VIEW_SIZE)return 110;
    return width>420?110:0;
}

static void draw_files_place_icon(int i,int x,int y,uint8_t ink,uint8_t bg) {
    (void)bg;
    icon_draw(i==0?ICON_HOME:i==5?ICON_TRASH:ICON_FOLDER,x-3,y-3,ink);
}
static void draw_files_controls(int wx,int wy,int ww,int inactive) {
    int x=files_list_x(wx,ww),y=files_header_y(wy);
    /* Sort is a segmented control; the right-hand caption names the detail
     * column, which follows the active sort. */
    int seg_w=0;
    for(int sort=0;sort<FILE_VIEW_SORT_COUNT;sort++)seg_w+=files_sort_width[sort]+3;
    draw_round_rect(x-2,y-1,seg_w+1,28,6,ui_chrome);
    for(int sort=0;sort<FILE_VIEW_SORT_COUNT;sort++){
        int width=files_sort_width[sort],active=fm_view.sort==sort;
        if(active){draw_round_rect(x,y+1,width,24,5,ui_field);draw_round_frame(x,y+1,width,24,5,ui_chrome_dk);}
        draw_string(files_sort_name[sort],x+6,y+4,active?ui_text:ui_text_dim);
        if(active){int ax=x+width-12,ay=y+10;
            for(int n=0;n<4;n++)draw_hline(ax-n,ay+(fm_view.descending?3-n:n),2*n+1,ui_text);}
        x+=width+3;
    }
    {
        int lw=files_list_w(ww),detail=files_detail_width(lw);
        const char *caption=fm_view.sort==FILE_VIEW_MODIFIED?"Modified":fm_view.sort==FILE_VIEW_SIZE?"Size":"Kind";
        int cx=files_list_x(wx,ww)+lw-detail;
        if(detail&&cx>x+12)draw_string(caption,cx,y+4,ui_text_dim);
    }
    draw_hline(files_list_x(wx,ww),y+29,files_list_w(ww),ui_chrome_dk);
    int end=wx+ww-12;
    if(!fm_filter_open)return;
    y=files_filter_y(wy);
    int field_x=files_list_x(wx,ww),field_w=end-field_x-76;
    draw_round_rect(field_x,y,field_w,28,4,ui_field);
    draw_round_frame(field_x,y,field_w,28,4,fm_filter_focus&&!inactive?ui_accent:ui_chrome_dk);
    int text_x=field_x+7,limit=field_x+field_w-7;
    const char *text=fm_view.filter;
    while(*text&&ui_string_w(text)>field_w-16)text++;
    if(fm_filter_select_all&&fm_filter_focus&&!inactive&&text[0])
        draw_rect(text_x,y+4,ui_string_w(text),CHAR_H,ui_accent);
    draw_string_clip(text[0]?text:"Name contains...",text_x,y+5,
        text[0]?(fm_filter_select_all&&fm_filter_focus&&!inactive?ui_on_accent:ui_text):ui_text_dim,limit);
    if(fm_filter_focus&&!inactive&&!fm_filter_select_all&&(frame_count/35)%2==0){
        int caret=text_x+ui_string_w(text);
        if(caret<limit)draw_rect(caret,y+5,1,CHAR_H,ui_text);
    }
    tool_button(end-68,y,32,28,ICON_CLEAR,0,TB_FLAT);
    tool_button(end-32,y,32,28,ICON_CLOSE,0,TB_FLAT);
}

static void draw_files(int wx, int wy, int ww, int wh, int inactive) {
    char title[24];
    char info[FS_PATH_LEN];
    fm_history_sync();
    files_title(title, 24);
    files_info(info, FS_PATH_LEN);
    for(int i=0;info[i];i++)if(info[i]=='|'){info[i>0?i-1:0]=0;break;}
    gui_draw_window(wx, wy, ww, wh, title, 0, inactive ? WIN_INACTIVE : 0);
    int side=files_side_w(ww),top=wy+TITLE_H+1;
    /* Sidebar: places and storage */
    if(side){
        draw_rect(wx+1,top,side-1,wh-TITLE_H-2,INK(2));
        draw_vline(wx+side,top,wh-TITLE_H-2,INK(5));
        draw_string("PLACES",wx+16,wy+TITLE_H+14,ui_text_dim);
        for(int i=0;i<FILES_PLACES;i++){
            int id=files_place_id(i),py=files_place_y(wy,i);
            if(id<0)continue;
            int here=id==fm_cwd;
            if(here)draw_round_rect(wx+8,py,side-16,28,7,INK(6));
            if(here)draw_round_rect(wx+8,py+7,3,14,1,ui_accent);
            draw_files_place_icon(i,wx+20,py+7,here?ui_accent:ui_text_dim,here?INK(6):INK(2));
            draw_string_clip(files_place_name[i],wx+42,py+(28-CHAR_H)/2,here?ui_text:INK(24),wx+side-10);
        }
        unsigned cap=fs_capacity(),used=fs_used_bytes();
        int sy=wy+wh-62;
        if(cap&&sy>files_place_y(wy,FILES_PLACES)+8){
            char line[40];fmt_uint(line,used/1024);kstrcpy(line+kstrlen(line)," KB of ");
            fmt_uint(line+kstrlen(line),cap/1024);kstrcpy(line+kstrlen(line)," KB");
            draw_string("STORAGE",wx+16,sy,ui_text_dim);
            draw_round_rect(wx+16,sy+22,side-32,6,3,INK(6));
            unsigned cap_kb=cap/1024?cap/1024:1;
            int fill=(int)((used/1024)*(unsigned)(side-32)/cap_kb);
            if(fill<6&&used)fill=6;
            if(fill>side-32)fill=side-32;
            if(fill)draw_round_rect(wx+16,sy+22,fill,6,3,ui_accent);
            draw_string_clip(line,wx+16,sy+34,ui_text_dim,wx+side-8);
        }
    }
    /* Toolbar: back, forward, up, breadcrumbs, filter */
    int tx=wx+side+1,tw=ww-side-2;
    draw_rect(tx,top,tw,FILES_TOOL_H-1,ui_chrome);
    draw_hline(tx,top+FILES_TOOL_H-1,tw,INK(7));
    static const int nav_icon[3]={ICON_BACK,ICON_FORWARD,ICON_UP};
    for(int i=0;i<3;i++){
        int enabled=i==0?fm_history_target(-1)>=0:i==1?fm_history_target(1)>=0:fm_has_parent();
        tool_button(files_nav_x(wx,ww,i),top+7,30,28,nav_icon[i],0,enabled?TB_FLAT:TB_DISABLED);
    }
    int xs[16],widths[16],ids[16],n=files_crumbs(wx,ww,xs,widths,ids);
    for(int k=0;k<n;k++){
        int last=k==n-1;
        if(last)draw_round_rect(xs[k]-2,top+8,widths[k]+4,26,6,INK(7));
        draw_string_clip(ids[k]==fs_root()?"Home":fs_name(ids[k]),xs[k]+3,top+(FILES_TOOL_H-CHAR_H)/2-1,
                         last?ui_text:ui_text_dim,xs[k]+widths[k]);
        if(!last){int cx=xs[k]+widths[k]+4,cy=top+FILES_TOOL_H/2-1;
            draw_line(cx,cy-3,cx+3,cy,INK(13));draw_line(cx+3,cy,cx,cy+3,INK(13));}
    }
    tool_button(wx+ww-44,top+7,32,28,ICON_FILTER,0,fm_filter_open?TB_ACTIVE:TB_FLAT);
    draw_files_controls(wx,wy,ww,inactive);
    /* List */
    int lx=files_list_x(wx,ww),lw=files_list_w(ww);
    int list_y=files_list_y(wy),rows=files_rows(wy,wh);
    int vis=fm_vis_count(),up=fm_has_parent()?1:0;
    files_scroll(rows);
    for(int i=fm_first;i<vis&&i<fm_first+rows;i++){
        int iy=list_y+(i-fm_first)*ROW_H;
        if(up&&i==0)
            draw_file_row_named(lx,iy,lw,"..",1,0,i==fm_selected,0,0,0,0);
        else {
            int id=fm_ids[i-up],detail_width=files_detail_width(lw);
            char detail[32];
            if(fm_view.sort==FILE_VIEW_MODIFIED)files_modified_label(id,detail,lw>=560);
            else if(fm_view.sort==FILE_VIEW_SIZE){
                if(fs_is_dir(id)||fs_is_app(id))kstrcpy(detail,file_view_type(id));
                else {fmt_uint(detail,(unsigned)fs_size(id));kstrcpy(detail+kstrlen(detail)," B");}
            }else kstrcpy(detail,file_view_type(id));
            draw_file_row_detail(lx,iy,lw,id,i==fm_selected,0,detail,detail_width);
        }
    }
    if(!fm_count){
        int y=list_y+up*ROW_H+4;
        if(y+CHAR_H<=files_list_bottom(wy,wh))
            draw_string_clip(fm_view.filter[0]?"No matching names":"This folder is empty",lx+12,y,ui_text_dim,lx+lw);
    }
    if(vis>rows&&rows>0){
        int track=rows*ROW_H,sx=wx+ww-11;
        int th=track*rows/vis;if(th<24)th=24;
        int ty=list_y+(track-th)*fm_first/(vis-rows>0?vis-rows:1);
        draw_round_rect(sx,list_y,5,track,2,INK(3));
        draw_round_rect(sx,ty,5,th,2,INK(11));
    }
    /* Footer */
    int fy=wy+wh-FILES_FOOT_H;
    draw_rect(tx,fy,tw,FILES_FOOT_H-1,ui_chrome);
    draw_hline(tx,fy,tw,INK(5));
    draw_string_clip(info,tx+12,fy+(FILES_FOOT_H-CHAR_H)/2,ui_text_dim,wx+ww-12);
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
    fx = files_list_x(wx, ww) + 26;
    fy = iy - 1;
    fh = ROW_H - 2;
    tw = ui_string_w(fm_rename_buf) + 16;
    if (tw < 96)
        tw = 96;
    fw = tw;
    int detail=files_detail_width(files_list_w(ww));
    int end=files_list_x(wx,ww)+files_list_w(ww)-(detail?detail+12:4);
    if (fx + fw > end)
        fw = end - fx;
    if (fw < 24)
        fw = 24;
    draw_rect(fx, fy, fw, fh, ui_field);
    draw_rect(fx, fy, fw, 1, ui_accent);
    draw_rect(fx, fy + fh - 1, fw, 1, ui_accent);
    draw_rect(fx, fy, 1, fh, ui_accent);
    draw_rect(fx + fw - 1, fy, 1, fh, ui_accent);
    ty = fy + (fh - CHAR_H) / 2;
    draw_string_clip(fm_rename_buf, fx + 4, ty, ui_text, fx + fw - 4);
    {
        int cx = fx + 4 + ui_string_w(fm_rename_buf);
        if (cx < fx + fw - 3)
            draw_rect(cx, ty, 1, CHAR_H, ui_text);
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

static int od_list_y(int py) { return py + TITLE_H + 12; }

static void draw_open_dialog(void) {
    int px, py, pw, ph;
    pick_geom(&px, &py, &pw, &ph);
    gui_draw_window(px, py, pw, ph, "Open", 0, WIN_NOCLOSE | WIN_NOMENU);

    int ox, oy, ow, cx, cy, cw;
    od_buttons(px, py, pw, ph, &ox, &oy, &ow, &cx, &cy, &cw);
    draw_default_button(ox, oy, ow, BTN_H, "Open");
    draw_button(cx, cy, cw, BTN_H, "Cancel");
    if(pick_focus)draw_frame((pick_focus==1?cx:ox)-3,oy-3,(pick_focus==1?cw:ow)+6,BTN_H+6,ui_accent);

    int ly = od_list_y(py);
    int lb = oy - 12;
    int rows = (lb - ly) / ROW_H;
    int lw = pw - 16;
    {
        /* The band above the list names the folder being browsed. */
        const char *where = pick_cwd == fs_root() ? "Home" : fs_name(pick_cwd);
        int ty = py + TITLE_BAR + (ly - py - TITLE_BAR - CHAR_H) / 2;
        draw_string("Look in", px + 16, ty, ui_text_dim);
        draw_string_bold_clip(where, px + 26 + ui_string_w("Look in"), ty, ui_text, px + pw - 16);
    }
    if (pick_count == 0) {
        draw_string("(empty)", px + 16, ly, ui_text_dim);
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
    char info[112];
    if (edit_file >= 0 && fs_identity(edit_file) == edit_identity) {
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
    const char *s = edit_len == 1 ? " byte" : " bytes";
    p = 0;
    while (s[p] && i < 30)
        info[i++] = s[p++];
    if (!edit_saved_ok && i < 28) {
        info[i++] = ' ';
        info[i++] = '*';
    }
    info[i] = 0;
    if(edit_save_busy)kstrcpy(info+i," | Save not started; retry");
    else if(edit_search.open&&edit_search.message[0]){kstrcpy(info+i," | ");kstrcpy(info+i+3,edit_search.message);}

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
    if(!edit_manual_scroll)edit_ensure_caret_visible(rows, cols);

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
                draw_rect(cx, cy, 2, EDIT_FONT_H, ui_accent);
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
                           selected ? ui_on_accent : ui_text);
        c++;
        if (c >= cols) {
            r++;
            c = 0;
        }
    }
    draw_edit_search(wx,wy,ww,wh);
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
    case WK_NATIVE: return "Native app";
    case WK_TODO: return "Todo";
    case WK_CLOCK: return "Clock";
    case WK_CAL: return "Calendar";
    case WK_MINES: return "Mines";
    case WK_2048: return "2048";
    case WK_BREAKOUT: return "Breakout";
    case WK_SYSMON: return "Monitor";
    case WK_PROPERTIES: return "Properties";
    case WK_BROWSER: return "Browser";
    case WK_PLAYER: return "Media Player";
    case WK_WRITER: return "Writer";
    case WK_SPREADSHEET: return "Spreadsheet";
    default: return "App";
    }
}

static const char *win_display_title(int slot, char *buffer) {
    if (slot < 0 || slot >= MAX_WIN) return "App";
    if ((wins[slot].kind == WK_TERM || wins[slot].kind == WK_NATIVE) &&
        app_view_title(slot, buffer, APP_VIEW_TITLE_LEN))
        return buffer;
    return win_app_name(wins[slot].kind);
}

/* Lucide glyph for a window kind (taskbar, Start menu, Files rows). */
static int kind_icon(int kind) {
    switch (kind) {
    case WK_FILES: return ICON_APP_FILES;      case WK_EDIT: return ICON_APP_EDITOR;
    case WK_CALC: return ICON_APP_CALC;        case WK_PAINT: return ICON_APP_PAINT;
    case WK_VIEW: return ICON_APP_VIEWER;      case WK_SNAKE: return ICON_APP_SNAKE;
    case WK_WORDLE: return ICON_APP_WORDLE;    case WK_TERM: return ICON_APP_TERMINAL;
    case WK_TODO: return ICON_APP_TODO;        case WK_CLOCK: return ICON_APP_CLOCK;
    case WK_CAL: return ICON_APP_CALENDAR;     case WK_MINES: return ICON_APP_MINES;
    case WK_2048: return ICON_APP_2048;        case WK_BREAKOUT: return ICON_APP_BREAKOUT;
    case WK_SYSMON: return ICON_APP_MONITOR;   case WK_SETTINGS: return ICON_APP_SETTINGS;
    case WK_BROWSER: return ICON_APP_BROWSER;  case WK_PLAYER: return ICON_APP_PLAYER;
    case WK_WRITER: return ICON_APP_WRITER;    case WK_SPREADSHEET: return ICON_APP_SHEET;
    case WK_NATIVE: return ICON_APP_NATIVE;    case WK_HELLO: return ICON_APP_HELLO;
    case WK_ABOUT: return ICON_APP_ABOUT;      case WK_HELP: return ICON_APP_HELP;
    default: return ICON_WINDOW;
    }
}

static void draw_window_contents(Win *w, int inactive) {
    context_set((int)(w - wins));
    chrome_icon = app_icon_for(kind_icon(w->kind));
    int wx = w->x, wy = w->y, ww = w->w, wh = w->h;
    int fl = inactive ? WIN_INACTIVE : 0;
    if(w->kind==WK_PROPERTIES){
        gui_draw_window(wx,wy,ww,wh,"Properties",0,fl|WIN_NOMENU);
        int y=wy+TITLE_BAR+20,lx=wx+24,vx=wx+176;char row[96],number[16];
        /* Header: the item's icon and name, then label / value rows in two
         * aligned columns, grouped into the item and its volume. */
        #define PROP(label,text) do { draw_string_clip(label,lx,y,ui_text_dim,vx-12);draw_string_clip(text,vx,y,ui_text,wx+ww-24);y+=26; }while(0)
        #define PROP_RULE() do { y+=6;draw_hline(lx,y,ww-48,ui_chrome_dk);y+=16; }while(0)
        if(fs_valid(properties_id)){
            int id=properties_id;
            app_icon_draw(fs_is_dir(id)?AI_FILES:fs_is_app(id)?app_icon_for(app_icon_by_name(fs_name(id))):AI_DOCUMENT,lx,y-4,AI_BIG);
            draw_string_bold_clip(fs_name(id),lx+AI_BIG+14,y+(AI_BIG-CHAR_H)/2-4,ui_text,wx+ww-24);
            y+=AI_BIG+4;PROP_RULE();
            PROP("Type",fs_is_dir(id)?"Folder":fs_is_app(id)?"Application":"File");
            fmt_uint(number,fs_size(id));kstrcpy(row,number);kstrcpy(row+kstrlen(row),fs_size(id)==1?" byte":" bytes");PROP("Size",row);
            unsigned stamp=fs_modified(id);
            if(!stamp)PROP("Modified","Unknown (older file)");
            else {unsigned days=stamp/86400,year=2000,month=1;int md[]={31,28,31,30,31,30,31,31,30,31,30,31};
                for(;;){unsigned yd=(year%4==0&&(year%100!=0||year%400==0))?366:365;if(days<yd)break;days-=yd;year++;}
                md[1]=(year%4==0&&(year%100!=0||year%400==0))?29:28;
                while(month<12&&days>=(unsigned)md[month-1])days-=md[month++-1];
                fmt_uint(row,year);int n=kstrlen(row);row[n++]='-';fmt_pad2(row+n,month);n+=2;row[n++]='-';fmt_pad2(row+n,days+1);n+=2;row[n++]=' ';fmt_pad2(row+n,stamp/3600%24);n+=2;row[n++]=':';fmt_pad2(row+n,stamp/60%60);n+=2;kstrcpy(row+n," UTC");PROP("Modified",row);
            }
        }else {draw_string_clip("This item is no longer available.",lx,y,ui_text,wx+ww-24);y+=26;}
        if(properties_reason[0]){draw_string_clip(properties_reason,lx,y,ui_text_dim,wx+ww-24);y+=26;}
        PROP_RULE();
        PROP("Volume",fs_storage_name());
        fmt_uint(number,fs_used_bytes());kstrcpy(row,number);kstrcpy(row+kstrlen(row)," bytes");PROP("Used",row);
        if(fs_node_limit()<=FS_MAX_NODES){fmt_uint(number,fs_node_limit()-fs_node_count());PROP("Free slots",number);}
        fmt_uint(number,fs_file_limit());kstrcpy(row,number);kstrcpy(row+kstrlen(row)," bytes");PROP("Largest file",row);
        PROP("Status",fs_storage_status()?fs_storage_status():fs_needs_sync()?"Changes waiting to be saved":"Saved to disk");
        #undef PROP
        #undef PROP_RULE
    } else if (w->kind == WK_HELLO) {
        gui_draw_window(wx, wy, ww, wh, "Hello", 0, fl | WIN_NOMENU);
        int lw = logo_string_w("BaseOS");
        draw_logo_string("BaseOS", wx + (ww - lw) / 2, wy + TITLE_BAR + 30, ui_accent);
        draw_string_in_win("Hello, world. Everything you see is drawn from scratch.", wx, ww,
                           wy + TITLE_BAR + 30 + LOGO_FONT_H + 10, ui_text_dim);
    } else if (w->kind == WK_HELP) {
        gui_draw_window(wx, wy, ww, wh, "Help", 0, fl | WIN_NOMENU);
        int y = wy + TITLE_BAR + 22;
        int x = wx + 24;
        static const char *tips[] = {
            "Ctrl+Space searches apps/files; Alt+Tab changes windows",
            "Files: Ctrl+F filters; Ctrl+1 through Ctrl+4 select sorting",
            "Ctrl+C/X/V copy, cut and paste files or selected text",
            "Writer: Ctrl+S saves BWR; Ctrl+Shift+P exports PDF",
            "Open .bex files to run native apps; Ctrl+C stops a task",
            "Documents includes Writer and Spreadsheet quick starts",
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
    } else if (w->kind == WK_NATIVE) {
        draw_native_window((int)(w-wins),inactive);
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
    } else if (w->kind == WK_SPREADSHEET) {
        gui_draw_window(wx,wy,ww,wh,spreadsheet_title(),0,fl);
        spreadsheet_draw(wx+1,wy+TITLE_H+1,ww-2,wh-TITLE_H-2);
    } else if (w->kind == WK_WRITER) {
        gui_draw_window(wx,wy,ww,wh,writer_title(),0,fl);
        writer_draw(wx+1,wy+TITLE_H+1,ww-2,wh-TITLE_H-2);
    } else if (w->kind == WK_BROWSER) {
        gui_draw_window(wx,wy,ww,wh,"Browser",0,fl);
        browser_draw(wx+1,wy+TITLE_H+1,ww-2,wh-TITLE_H-2);
    } else if (w->kind == WK_PLAYER) {
        gui_draw_window(wx,wy,ww,wh,"Media Player",0,fl);
        player_draw(wx+1,wy+TITLE_H+1,ww-2,wh-TITLE_H-2);
    } else if (w->kind == WK_SYSMON) {
        gui_draw_window(wx, wy, ww, wh, "System Monitor", 0, fl);
        SysInfo si;
        sysinfo_fill(&si);
        sysmon_draw(wx, wy + TITLE_H + 1, ww, wh - TITLE_H - 1, &si);
    }
}

static void draw_one_window(Win *w,int inactive) {
    int slot=(int)(w-wins);
    chrome_kind=w->kind; chrome_slot=slot;
    chrome_flags=(w->maximized?WIN_MAXED:0)|(win_is_dialog(w->kind)?WIN_DIALOG:0);
    draw_window_contents(w,inactive);
    chrome_icon=chrome_kind=chrome_slot=-1; chrome_flags=0;
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

static void launcher_close(void);
/* ---------- Taskbar, dock and top bar ----------
 * Classic: a 34px taskbar along the bottom (Apps, one button per window,
 * tray). Modern: a floating dock (Apps, windows, Show desktop) and a 28px top
 * bar with the system button, the front app's menus, volume and clock. */
typedef struct { int x, y, w, h; } Rect;
static Rect tb_apps, tb_vol, tb_clock, tb_desk, tb_dock, tb_sys, tb_appname;
static int tb_task_y, tb_task_h;
static int volume_on;  /* volume popup */
enum { HOT_NONE = -2, HOT_APPS = -1, HOT_VOL = -3, HOT_CLOCK = -4, HOT_DESK = -5,
       HOT_SYS = -6, HOT_APPNAME = -7, HOT_MENU = -100 /* - index */ };
static int taskbar_hover = HOT_NONE;

static void date_time_text(char *date, char *time) {
    RtcTime now;
    rtc_read(&now);
    static const char *dshort[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    char t[4];
    kstrcpy(date, dshort[now.wday]);
    kstrcpy(date + kstrlen(date), " ");
    fmt_uint(t, (unsigned)now.day);
    kstrcpy(date + kstrlen(date), t);
    kstrcpy(date + kstrlen(date), " ");
    int n = kstrlen(date);
    const char *mn = rtc_month_name[now.month - 1];
    for (int i = 0; i < 3 && mn[i]; i++) date[n++] = mn[i];
    date[n] = 0;
    fmt_pad2(time, now.hour);
    time[2] = ':';
    fmt_pad2(time + 3, now.min);
}
static int clock_w(void) {
    char d[24], t[8];
    date_time_text(d, t);
    return ui_string_w(d) + 8 + uib_string_w(t);
}
static void draw_clock(int x, int y, uint8_t ink) {
    char d[24], t[8];
    date_time_text(d, t);
    draw_string(d, x, y, ink);
    draw_string_bold(t, x + ui_string_w(d) + 8, y, ink);
}

static void taskbar_order(int *order, int *n) {
    *n = 0;
    for (int i = 0; i < MAX_WIN; i++) if (wins[i].open) order[(*n)++] = i;
    for (int a = 0; a < *n; a++)
        for (int b = a + 1; b < *n; b++)
            if (wins[order[b]].seq < wins[order[a]].seq) { int t = order[a]; order[a] = order[b]; order[b] = t; }
}

static const char *tray_status(void) {
    const char *status = fs_storage_status();
    if (!status && session_status[0]) status = session_status;
    if (!status && native_launch_status[0]) status = native_launch_status;
    return status;
}

static void taskbar_layout(void) {
    int order[MAX_WIN], n;
    taskbar_order(order, &n);
    tb_n = 0;
    char title[TERM_TASK_TITLE_LEN];
    if (MODERN) {
        tb_task_h = 36;
        int apps_w = 36 + uib_string_w("Apps") + 14;
        int fixed = 6 + apps_w + 9 + 9 + 30 + 6;   /* pads, separators, show desktop */
        int room = fb_w - 32 - fixed - (n ? (n - 1) * 4 : 0);
        int each = n ? room / n : 0;
        if (each > 168) each = 168;
        int total = fixed;
        int widths[MAX_WIN];
        for (int k = 0; k < n; k++) {
            int w = 36 + 12 + uib_string_w(win_display_title(order[k], title));
            if (w > each) w = each;
            if (w < 72) w = 44;
            widths[k] = w;
            total += w + (k ? 4 : 0);
        }
        tb_dock = (Rect){(fb_w - total) / 2, fb_h - 56, total, 48};
        tb_task_y = tb_dock.y + 6;
        int x = tb_dock.x + 6;
        tb_apps = (Rect){x, tb_task_y, apps_w, 36};
        x += apps_w + 9;
        for (int k = 0; k < n; k++) {
            tb_id[tb_n] = order[k];
            tb_x[tb_n] = x;
            tb_w[tb_n++] = widths[k];
            x += widths[k] + 4;
        }
        if (n) x += 5;
        tb_desk = (Rect){tb_dock.x + tb_dock.w - 6 - 30, tb_dock.y + 9, 30, 30};
        int cw = clock_w() + 18;
        tb_clock = (Rect){fb_w - 6 - cw, 3, cw, 22};
        tb_vol = (Rect){tb_clock.x - 2 - 34, 3, 34, 22};
        tb_sys = (Rect){TOPBAR_SYS_X, 3, TOPBAR_SYS_W, 22};
        tb_appname = (Rect){topbar_app_x(), 3, topbar_app_w(), 22};
        return;
    }
    int y = TASKBAR_Y + 1;
    tb_task_y = y;
    tb_task_h = TASKBAR_H - 1;
    tb_apps = (Rect){0, y, 84, tb_task_h};
    tb_desk = (Rect){fb_w - 32, y, 32, tb_task_h};
    int cw = clock_w() + 20;
    tb_clock = (Rect){tb_desk.x - cw, y, cw, tb_task_h};
    tb_vol = (Rect){tb_clock.x - 32, y, 32, tb_task_h};
    int left = 84, right = tb_vol.x - 1;
    const char *status = tray_status();
    if (status) right -= ui_string_w(status) + 20 < (right - left) / 2 ? ui_string_w(status) + 20 : (right - left) / 2;
    int free = right - left, w = n ? free / n : 0;
    if (w > 168) w = 168;
    if (w < 40) w = 40;
    for (int k = 0; k < n && left + w <= right + w / 2; k++) {
        tb_id[tb_n] = order[k];
        tb_x[tb_n] = left;
        tb_w[tb_n++] = w;
        left += w;
    }
}

static int rect_hit(Rect r) { return hit(mouse_x, mouse_y, r.x, r.y, r.w, r.h); }

static int taskbar_hover_at(void) {
    if (open_dlg || name_dlg || edit_close_dlg || display_pending) return HOT_NONE;
    taskbar_layout();
    if (MODERN) {
        if (mouse_y < MENUBAR_H) {
            if (rect_hit(tb_sys)) return HOT_SYS;
            if (rect_hit(tb_appname)) return HOT_APPNAME;
            if (rect_hit(tb_vol)) return HOT_VOL;
            if (rect_hit(tb_clock)) return HOT_CLOCK;
            int m = win_menu_at(mouse_x, mouse_y);
            return m >= 0 ? HOT_MENU - m : HOT_NONE;
        }
        if (!rect_hit(tb_dock)) return HOT_NONE;
    } else {
        if (mouse_y < TASKBAR_Y) {
            int m = open_menu < 0 ? win_menu_at(mouse_x, mouse_y) : -1;
            return m >= 0 ? HOT_MENU - m : HOT_NONE;
        }
        if (rect_hit(tb_vol)) return HOT_VOL;
        if (rect_hit(tb_clock)) return HOT_CLOCK;
    }
    if (rect_hit(tb_apps)) return HOT_APPS;
    if (rect_hit(tb_desk)) return HOT_DESK;
    for (int t = 0; t < tb_n; t++)
        if (hit(mouse_x, mouse_y, tb_x[t], tb_task_y, tb_w[t], tb_task_h)) return tb_id[t];
    return HOT_NONE;
}

static void draw_task_button(int t, int front) {
    int i = tb_id[t], x = tb_x[t], w = tb_w[t], y = tb_task_y, h = tb_task_h;
    char native_title[TERM_TASK_TITLE_LEN];
    const char *title = win_display_title(i, native_title);
    int active = i == front && !wins[i].min, hover = taskbar_hover == i;
    int pressed = hover && mouse_left;
    uint8_t ink = wins[i].min ? TK_TEXT2 : TK_TEXT, bg;
    int icon = app_icon_for(kind_icon(wins[i].kind));
    if (MODERN) {
        bg = active ? TK_ACCENT_100 : pressed ? TK_HOVER : hover ? TK_LINE_SUBTLE : ui_theme.chrome;
        if (bg != ui_theme.chrome) kit_fill(x, y, w, h, 8, bg, 1);
        if (active) ink = TK_ACCENT_700;
        if (w < 72) { app_icon_draw(icon, x + (w - 16) / 2, y + 10, AI_SMALL); }
        else {
            app_icon_draw(icon, x + 12, y + 10, AI_SMALL);
            kit_text(title, x + 36, kit_ty(y, h), ink, w - 36 - 12, active);
        }
        if (active) kit_fill(x + w / 2 - 8, y + 31, 16, 3, 2, TK_ACCENT, 1);
        return;
    }
    bg = active ? TK_46 : pressed ? TK_HOVER : hover ? TK_BASE : TK_SUNKEN;
    draw_rect(x, y, w, h, bg);
    draw_vline(x + w - 1, y, h, active ? TK_LINE : TK_LINE_SUBTLE);
    if (active) {
        draw_vline(x - 1, y, h, TK_LINE);
        draw_rect(x, y, w - 1, 3, TK_ACCENT);
    }
    int ty = kit_ty(y + 3, h - 3);
    if (w < 72) app_icon_draw(icon, x + (w - 16) / 2, y + 9, AI_SMALL);
    else {
        app_icon_draw(icon, x + 10, y + 9, AI_SMALL);
        kit_text(title, x + 32, ty, ink, w - 32 - 10, active);
    }
}

static void draw_volume_icon(Rect r, int hover) {
    uint8_t bg = MODERN ? (hover ? TK_HOVER : TK_45) : (hover ? TK_BASE : TK_SUNKEN);
    if (hover || volume_on) {
        if (MODERN) kit_fill(r.x, r.y, r.w, r.h, 6, volume_on ? TK_ACCENT : TK_HOVER, 1);
        else draw_rect(r.x, r.y, r.w, r.h, volume_on ? TK_HOVER : TK_BASE);
        if (volume_on) bg = MODERN ? TK_ACCENT : TK_HOVER;
    }
    ui_icon_draw(UI_ICON_VOL, r.x + (r.w - 16) / 2, r.y + (r.h - 16) / 2, 16,
                 MODERN && volume_on ? TK_ON_ACCENT : TK_ICON, bg);
}

static void draw_taskbar(void) {
    taskbar_layout();
    int front = win_front();
    if (MODERN) {
        kit_shadow(tb_dock.x, tb_dock.y, tb_dock.w, tb_dock.h, 12);
        kit_fill(tb_dock.x, tb_dock.y, tb_dock.w, tb_dock.h, 12, TK_45, 1);
        kit_frame(tb_dock.x, tb_dock.y, tb_dock.w, tb_dock.h, 12, TK_LINE_STRONG);
        Rect a = tb_apps;
        int hover = taskbar_hover == HOT_APPS;
        kit_fill(a.x, a.y, a.w, a.h, 8, launcher_on ? TK_ACCENT_100 : hover ? TK_ACCENT_300 : TK_ACCENT_100, 1);
        if (launcher_on) kit_frame(a.x, a.y, a.w, a.h, 8, TK_ACCENT);
        if (launcher_on) kit_frame(a.x + 1, a.y + 1, a.w - 2, a.h - 2, 7, TK_ACCENT);
        ui_icon_draw(UI_ICON_LOGO, a.x + 10, a.y + 9, 18, TK_ACCENT, TK_ACCENT_100);
        draw_string_bold("Apps", a.x + 36, kit_ty(a.y, a.h), TK_ACCENT_700);
        draw_vline(a.x + a.w + 4, tb_dock.y + 12, 24, TK_LINE_SUBTLE);
        for (int t = 0; t < tb_n; t++) draw_task_button(t, front);
        if (tb_n) draw_vline(tb_desk.x - 5, tb_dock.y + 12, 24, TK_LINE_SUBTLE);
        Rect d = tb_desk;
        if (taskbar_hover == HOT_DESK) kit_fill(d.x, d.y, d.w, d.h, 6, TK_HOVER, 1);
        ui_icon_draw(UI_ICON_SCREEN, d.x + 7, d.y + 7, 16, TK_ICON, taskbar_hover == HOT_DESK ? TK_HOVER : TK_45);
        return;
    }
    int y = TASKBAR_Y;
    draw_hline(0, y, fb_w, TK_LINE);
    draw_rect(0, y + 1, fb_w, TASKBAR_H - 1, TK_SUNKEN);
    /* Apps button */
    Rect a = tb_apps;
    int hover = taskbar_hover == HOT_APPS;
    if (launcher_on || hover) {
        draw_rect(a.x, a.y, a.w, a.h, launcher_on ? TK_46 : mouse_left ? TK_HOVER : TK_BASE);
        if (launcher_on) draw_rect(a.x, a.y, a.w, 3, TK_ACCENT);
    }
    draw_vline(a.x + a.w - 1, a.y, a.h, TK_LINE_SUBTLE);
    ui_icon_draw(UI_ICON_LOGO, a.x + 12, y + 8, 20, TK_ACCENT, TK_SUNKEN);
    draw_string_bold("Apps", a.x + 38, kit_ty(a.y + 1, a.h - 1), TK_TEXT);
    for (int t = 0; t < tb_n; t++) draw_task_button(t, front);
    /* Tray */
    const char *status = tray_status();
    draw_vline(tb_vol.x - 1, y + 1, TASKBAR_H - 1, TK_LINE);
    if (status) {
        int left = tb_n ? tb_x[tb_n - 1] + tb_w[tb_n - 1] + 10 : 94;
        int sx = tb_vol.x - 12 - ui_string_w(status);
        kit_text(status, sx > left ? sx : left, kit_ty(y + 1, TASKBAR_H - 1),
                 fs_sync_busy() ? TK_TEXT2 : TK_RED, tb_vol.x - 12 - (sx > left ? sx : left), 0);
    }
    draw_volume_icon(tb_vol, taskbar_hover == HOT_VOL);
    if (taskbar_hover == HOT_CLOCK) draw_rect(tb_clock.x, tb_clock.y, tb_clock.w, tb_clock.h, TK_BASE);
    draw_clock(tb_clock.x + 10, kit_ty(y + 1, TASKBAR_H - 1), TK_TEXT);
    Rect d = tb_desk;
    if (taskbar_hover == HOT_DESK) draw_rect(d.x, d.y, d.w, d.h, TK_BASE);
    draw_vline(d.x, d.y, d.h, TK_LINE_SUBTLE);
    ui_icon_draw(UI_ICON_SCREEN, d.x + 8, y + 9, 16, TK_ICON, taskbar_hover == HOT_DESK ? TK_BASE : TK_SUNKEN);
}

/* Modern top bar. */
static void draw_topbar(void) {
    if (!MODERN) return;
    taskbar_layout();
    menu_bar_init();
    draw_rect(0, 0, fb_w, MENUBAR_H - 1, TK_45);
    draw_hline(0, MENUBAR_H - 1, fb_w, TK_LINE);
    int sys_on = open_menu == MENU_CONTEXT && popup_n && popup[0].items == mi_system;
    Rect s = tb_sys;
    uint8_t bg = TK_45;
    if (sys_on || taskbar_hover == HOT_SYS) { bg = sys_on ? TK_ACCENT : TK_HOVER; kit_fill(s.x, s.y, s.w, s.h, 6, bg, 1); }
    ui_icon_draw(UI_ICON_LOGO, s.x + 6, s.y + 3, 16, sys_on ? TK_ON_ACCENT : TK_ACCENT, bg);
    Rect a = tb_appname;
    draw_string_bold(topbar_app_name(), a.x + 9, kit_ty(a.y, a.h), TK_TEXT);
    int rx, ry, rh;
    menu_row_origin(&rx, &ry, &rh);
    int limit = tb_vol.x - 8;
    for (int i = 0; i < bar_n; i++) {
        int x = rx + bar_x[i];
        if (x + bar_w[i] > limit) break;
        int open = (open_menu == i && popup_n) || (open_menu == MENU_BAR && menu_bar_focus == i);
        int hover = taskbar_hover == HOT_MENU - i && open_menu < 0;
        uint8_t ink = TK_TEXT, fill = TK_45;
        if (open) { fill = TK_ACCENT; ink = TK_ON_ACCENT; }
        else if (hover) fill = TK_HOVER;
        if (fill != TK_45) kit_fill(x, ry, bar_w[i], rh, 6, fill, 1);
        draw_mnemonic(bar_menus[i].name, x + 9, kit_ty(ry, rh), ink, bar_w[i] - 12,
                      alt_down || open_menu == MENU_BAR);
    }
    draw_volume_icon(tb_vol, taskbar_hover == HOT_VOL);
    if (taskbar_hover == HOT_CLOCK) kit_fill(tb_clock.x, tb_clock.y, tb_clock.w, tb_clock.h, 6, TK_HOVER, 1);
    draw_clock(tb_clock.x + 9, kit_ty(3, 22), TK_TEXT);
    const char *status = tray_status();
    if (status) {
        int sx = tb_vol.x - 12 - ui_string_w(status);
        if (sx > rx + 200) kit_text(status, sx, kit_ty(3, 22), fs_sync_busy() ? TK_TEXT2 : TK_RED, tb_vol.x - 12 - sx, 0);
    }
}

/* The Classic menu row inside a window. */
static void draw_win_menu_strip(int x, int y, int w, int inactive, int nomenu) {
    if (nomenu || MODERN) return;
    int n;
    const MMenu *menus = menubar_for(chrome_kind, &n);
    int front = chrome_slot >= 0 && chrome_slot == win_front();
    int ry = y + TITLE_BAR, rh = WMENU_H - 2, ix = x + 1 + 3;
    for (int i = 0; i < n; i++) {
        char t[32];
        mi_text(menus[i].name, t, sizeof t, 0);
        int iw = ui_string_w(t) + 16;
        if (ix + iw > x + w - 4) break;
        int open = front && ((open_menu == i && popup_n) || (open_menu == MENU_BAR && menu_bar_focus == i));
        int hover = front && open_menu < 0 && taskbar_hover == HOT_MENU - i;
        uint8_t ink = inactive ? TK_TEXT2 : TK_TEXT;
        if (open) { draw_rect(ix, ry, iw, rh, TK_ACCENT); ink = TK_ON_ACCENT; }
        else if (hover) draw_rect(ix, ry, iw, rh, TK_HOVER);
        draw_mnemonic(menus[i].name, ix + 8, kit_ty(ry, rh), ink, iw - 8,
                      front && (alt_down || open_menu == MENU_BAR));
        ix += iw;
    }
}

/* Volume popup above (Classic) or below (Modern) the tray button. */
#define VOLUME_W 220
#define VOLUME_H 64
static void volume_geom(int *x, int *y) {
    *x = tb_vol.x + tb_vol.w - VOLUME_W;
    if (*x < 4) *x = 4;
    *y = MODERN ? MENUBAR_H + 2 : TASKBAR_Y - VOLUME_H - 2;
}
static void draw_volume_popup(void) {
    if (!volume_on) return;
    taskbar_layout();
    int x, y;
    volume_geom(&x, &y);
    kit_menu_frame(x, y, VOLUME_W, VOLUME_H);
    unsigned v = audio_status()->volume;
    char line[24];
    kstrcpy(line, "Volume  ");
    fmt_uint(line + kstrlen(line), v);
    kstrcpy(line + kstrlen(line), "%");
    draw_string_bold(line, x + 14, y + 10, TK_TEXT);
    kit_slider(x + 14, y + 34, VOLUME_W - 28, (int)v, 100, ctl_state(x + 14, y + 34, VOLUME_W - 28, 18));
}
static int volume_click(void) {
    if (!volume_on) return 0;
    int x, y;
    volume_geom(&x, &y);
    if (!hit(mouse_x, mouse_y, x, y, VOLUME_W, VOLUME_H)) { volume_on = 0; dirty = 1; return rect_hit(tb_vol); }
    if (hit(mouse_x, mouse_y, x + 8, y + 28, VOLUME_W - 16, 30)) {
        int v = (mouse_x - (x + 14) - 6) * 100 / (VOLUME_W - 28 - 12);
        audio_set_volume((unsigned)(v < 0 ? 0 : v > 100 ? 100 : v));
        dirty = 1;
    }
    return 1;
}
static void volume_drag(void) {
    if (!volume_on || !mouse_left) return;
    int x, y;
    volume_geom(&x, &y);
    if (!hit(mouse_x, mouse_y, x, y + 20, VOLUME_W, VOLUME_H - 20)) return;
    int v = (mouse_x - (x + 14) - 6) * 100 / (VOLUME_W - 28 - 12);
    audio_set_volume((unsigned)(v < 0 ? 0 : v > 100 ? 100 : v));
    dirty = 1;
}

static void taskbar_task_click(int id) {
    if (!wins[id].min && win_front() == id) win_minimize(id);
    else win_focus(id);
}

/* Clicks on the taskbar, dock or top bar. Returns 1 when used. */
static int taskbar_hit(void) {
    int h = taskbar_hover_at();
    int on_bar = MODERN ? (mouse_y < MENUBAR_H || rect_hit(tb_dock)) : mouse_y >= TASKBAR_Y;
    if (!on_bar) return 0;
    if (h != HOT_VOL) volume_on = 0;
    if (h == HOT_APPS) { if (launcher_on) launcher_close(); else launcher_open(); }
    else if (h == HOT_DESK) show_desktop();
    else if (h == HOT_VOL) volume_on = !volume_on;
    else if (h == HOT_CLOCK) { cal_reset(); win_open(WK_CAL); }
    else if (h == HOT_SYS) menu_open_context(mi_system, (int)(sizeof mi_system / sizeof mi_system[0]), tb_sys.x, 30);
    else if (h == HOT_APPNAME) { int f = win_front(); if (f >= 0) menu_open_window(f); }
    else if (h <= HOT_MENU) menu_open_bar(HOT_MENU - h);
    else if (h >= 0) taskbar_task_click(h);
    dirty = 1;
    return 1;
}
static int taskbar_rclick(void) {
    int h = taskbar_hover_at();
    if (h < 0) return MODERN ? (mouse_y < MENUBAR_H || rect_hit(tb_dock)) : mouse_y >= TASKBAR_Y;
    win_focus(h);
    int n = (int)(sizeof mi_window / sizeof mi_window[0]), x = mouse_x, y;
    Popup probe = {mi_window, n, 0, 0, 0, 0, -1};
    popup_size(&probe);
    y = MODERN ? tb_dock.y - probe.h - 4 : TASKBAR_Y - probe.h;
    menu_open_context(mi_window, n, x, y);
    return 1;
}

/* ---------- Input handling ---------- */

static int close_hit(int wx, int wy, int ww) {
    /* Open is a modal: no close box, and that corner must not dismiss. */
    if (open_dlg)
        return 0;
    int cx, cy;
    close_box_pos(wx, wy, &cx, &cy);
    return hit(mouse_x, mouse_y, close_box_x(wx, ww), cy, CAP_CLOSE_W, CAP_H);
}

static int min_hit(int wx, int wy, int ww) {
    if (open_dlg)
        return 0;
    int cx, cy;
    min_box_pos(wx, wy, &cx, &cy);
    return hit(mouse_x, mouse_y, min_box_x(wx, ww), cy, CAP_W, CAP_H);
}

static int max_hit(int wx, int wy, int ww) {
    if (open_dlg)
        return 0;
    int cx, cy;
    min_box_pos(wx, wy, &cx, &cy);
    return hit(mouse_x, mouse_y, max_box_x(wx, ww), cy, CAP_W, CAP_H);
}

static int fm_name_hit(int wx, int ww, int iy, int id) {
    /* Label side of the row: past the 12px icon, not the icon itself. */
    int nx = files_list_x(wx, ww) + 20;
    int ny = iy;
    int nh = ROW_H;
    int nw;
    if (fm_renaming && fm_rename_id == id && fs_identity(id)==fm_rename_identity) {
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
        if (fm_ids[i] == fm_rename_id && fm_ids_identity[i]==fm_rename_identity) {
            row = i + up;
            break;
        }
    }
    if (row < 0)
        return 0;
    iy = files_list_y(wy) + (row-fm_first) * ROW_H;
    return fm_name_hit(wx, ww, iy, fm_rename_id);
}

static void handle_files_click(int wx, int wy, int ww, int wh) {
    int side=files_side_w(ww),top=wy+TITLE_H+1;
    fm_history_sync();
    if(side&&mouse_x<wx+side){
        for(int i=0;i<FILES_PLACES;i++){
            int id=files_place_id(i);
            if(id>=0&&hit(mouse_x,mouse_y,wx+8,files_place_y(wy,i),side-16,28)){fm_navigate(id);return;}
        }
        return;
    }
    if(mouse_y<top+FILES_TOOL_H){
        for(int i=0;i<3;i++)if(hit(mouse_x,mouse_y,files_nav_x(wx,ww,i),top+7,30,28)){
            if(i==2){fm_rename_cancel();fm_go_up();}else fm_history_step(i==0?-1:1);
            return;
        }
        int xs[16],widths[16],ids[16],n=files_crumbs(wx,ww,xs,widths,ids);
        for(int k=0;k<n-1;k++)if(hit(mouse_x,mouse_y,xs[k]-2,top+8,widths[k]+4,26)){fm_navigate(ids[k]);return;}
        if(hit(mouse_x,mouse_y,wx+ww-44,top+7,32,28)){fm_filter_show();return;}
        return;
    }
    if(mouse_y>=wy+wh-FILES_FOOT_H)return;
    int x=files_list_x(wx,ww),y=files_header_y(wy),end=wx+ww-12;
    for(int sort=0;sort<FILE_VIEW_SORT_COUNT;sort++){
        if(hit(mouse_x,mouse_y,x,y,files_sort_width[sort],26)){fm_sort_by(sort);return;}
        x+=files_sort_width[sort]+3;
    }
    if(fm_filter_open){
        y=files_filter_y(wy);
        if(hit(mouse_x,mouse_y,end-68,y,32,28)){fm_filter_clear(0);return;}
        if(hit(mouse_x,mouse_y,end-32,y,32,28)){fm_filter_clear(1);return;}
        if(hit(mouse_x,mouse_y,files_list_x(wx,ww),y,end-files_list_x(wx,ww)-76,28)){
            fm_rename_cancel();fm_filter_focus=1;fm_filter_select_all=fm_view.filter[0]!=0;dirty=1;return;
        }
    }
    fm_filter_focus=fm_filter_select_all=0;
    int list_y = files_list_y(wy);
    int rows = files_rows(wy, wh);
    int lw = files_list_w(ww);
    int vis = fm_vis_count();
    int up = fm_has_parent() ? 1 : 0;
    files_scroll(rows);
    for (int i = fm_first; i < vis && i < fm_first+rows; i++) {
        int iy = list_y + (i-fm_first) * ROW_H;
        if (hit(mouse_x, mouse_y, files_list_x(wx, ww), iy, lw, ROW_H)) {
            int id = fm_row_id(i);
            if(id < -1){fm_refresh();dirty=1;return;}
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
            if (already && id >= 0 && mouse_x >= files_list_x(wx, ww) + 20) {
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
            fm_drag_id = id;
            fm_drag_identity=fs_identity(id);
            fm_drag_parent=fm_cwd;
            fm_drag_sx = mouse_x;
            fm_drag_sy = mouse_y;
            return;
        }
    }
    fm_rename_cancel();
}

/* The Files list row under the pointer: an index, -1 for empty list
 * space, or -2 outside the list. */
static int files_row_at(int wx, int wy, int ww, int wh) {
    int x = files_list_x(wx, ww), y = files_list_y(wy), lw = files_list_w(ww);
    int rows = files_rows(wy, wh), vis = fm_vis_count();
    if (!hit(mouse_x, mouse_y, x, y, lw, rows * ROW_H)) return -2;
    int i = fm_first + (mouse_y - y) / ROW_H;
    return i < vis ? i : -1;
}

static void files_drop(void) {
    int id = fm_drag_id;
    int active = fm_drag_active;
    fm_dragging = 0;
    fm_drag_active = 0;
    fm_drag_id = -1;
    if (!active || !file_view_valid(fm_drag_parent,id,fm_drag_identity))
        return;
    if (id == trash_id)
        return;
    if (!icon_hit(ICON_DESK_TRASH, mouse_x, mouse_y))
        return;
    if (trash_id < 0)
        return;
    /* Already in the trash can: drop is a no-op. */
    if (fs_parent(id) == trash_id)
        return;
    if (files_storage_busy()) return;
    if (fs_move(id, trash_id) < 0)
        return;
    kprint_debug("Trash drop\n");
    if (find_open_kind(WK_FILES) >= 0) {
        if (fm_cwd == id) {
            int p = fs_parent(id);
            fm_set_cwd((p >= 0) ? p : fs_root());
        }
        fm_refresh();
    }
    if (open_dlg)
        od_refresh();
    dirty = 1;
}

static void draw_drag_ghost(void) {
    if (!fm_drag_active || !file_view_valid(fm_drag_parent,fm_drag_id,fm_drag_identity))
        return;
    int w = 18, h = 18;
    int x = mouse_x + 12;
    int y = mouse_y + 8;
    if (x + w > fb_w - 2) x = mouse_x - w - 6;
    if (y + h > fb_h - 2) y = mouse_y - h - 6;
    if (x < 2) x = 2;
    if (y < MENUBAR_H) y = MENUBAR_H;
    draw_rect(x, y, w, 1, ui_accent);
    draw_rect(x, y + h - 1, w, 1, ui_accent);
    draw_rect(x, y, 1, h, ui_accent);
    draw_rect(x + w - 1, y, 1, h, ui_accent);
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
    if(edit_search_click(wx,wy,ww,wh))return;
    edit_search.focus=0;edit_manual_scroll=0;
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
    case ICON_DESK_EDIT: open_edit(); break;
    case ICON_CALC: open_calc(); break;
    case ICON_PAINT: open_paint(); break;
    case ICON_VIEW: start_open_dialog(1); break;
    case ICON_SNAKE: open_snake(); break;
    case ICON_WORDLE: open_wordle(); break;
    case ICON_TERM: open_term(); break;
    case ICON_TODO: open_todo(); break;
    case ICON_DESK_SETTINGS: win_open(WK_SETTINGS); break;
    case ICON_CLOCK: win_open(WK_CLOCK); break;
    case ICON_CAL: cal_reset(); win_open(WK_CAL); break;
    case ICON_MINES: win_open(WK_MINES); break;
    case ICON_2048: win_open(WK_2048); break;
    case ICON_BREAKOUT: win_open(WK_BREAKOUT); break;
    case ICON_SYSMON: win_open(WK_SYSMON); break;
    case ICON_BROWSER: open_browser(-1); break;
    case ICON_PLAYER: open_player(-1); break;
    case ICON_WRITER: open_writer(-1); break;
    case ICON_SPREADSHEET: open_spreadsheet(-1); break;
    case ICON_DESK_TRASH: open_files(trash_id >= 0 ? trash_id : fs_root()); break;
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

    if (m == MENU_WINDOW) {
        if (item == 0) win_minimize(win_front());
        else if (item == 1) win_arrange(win_front(), 0);
        else if (item == 2) show_desktop();
        else if (item == 4) win_open(WK_ABOUT);
        else if (item == 5) win_open(WK_HELP);
        return;
    }
    if (m == MENU_FILE) {
        if (item == 8) {
            do_empty_trash();
        } else if (item == 0) {
            open_dlg = 0;
            if (front_kind() == WK_WRITER || front_kind() == WK_SPREADSHEET) {
                document_request(win_front(), DOCUMENT_NEW, -1);
            } else if (front_kind() == WK_PAINT) {
                paint_clear();
            } else if (front_kind() == WK_SNAKE) {
                snake_reset();
            } else if (front_kind() == WK_WORDLE) {
                wordle_new_game(frame_count);
            } else if (front_kind() == WK_CAL) {
                cal_new();dirty=1;
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
            if (front_kind() == WK_CAL && !open_dlg) {
                cal_agenda_retry_save();dirty=1;
            } else if (front_kind() == WK_TODO && !open_dlg) {
                todo_retry_save();dirty=1;
            } else if (front_kind() == WK_SPREADSHEET && !open_dlg) {
                spreadsheet_save_document();
            } else if (front_kind() == WK_WRITER && !open_dlg) {
                writer_save_document();
            } else if (front_kind() == WK_PAINT && !open_dlg) {
                paint_save();
            } else if (front_kind() == WK_EDIT && !open_dlg) {
                edit_save();
                dirty = 1;
            }
        } else if (item == 6) {
            properties_id=fm_row_id(fm_selected);properties_reason="";win_open(WK_PROPERTIES);
        } else if (item == 5) {
            do_duplicate();
        }
        return;
    }
    if (m == MENU_EDITM) {
        if (front_kind() == WK_FILES) {
            files_clipboard_action(item);
        } else if (front_kind() == WK_SPREADSHEET) {
            const int keys[] = {0x2d, 0x2e, 0x2f};
            if (item >= 0 && item < 3) spreadsheet_result(spreadsheet_key(keys[item], 0, SPREADSHEET_MOD_CTRL));
        } else if (front_kind() == WK_WRITER) {
            const int keys[] = {0x2d, 0x2e, 0x2f};
            if (item >= 0 && item < 3) writer_result(writer_key(keys[item], 0, WRITER_MOD_CTRL));
            else if (item == 3 || item == 4)
                writer_result(writer_key(item == 3 ? 0x21 : 0x23, 0, WRITER_MOD_CTRL));
        } else if (front_kind() == WK_CALC) {
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
        else if(item==3||item==4)edit_search_open(item==4);
        dirty = 1;
        return;
    }
}

/* ---------- Menu model ----------
 * Shared actions keep their File / Edit / Window implementations above
 * (LEG codes); the rest are dispatched here. */

static int menu_kind(void) { return front_kind(); }

static const MMenu *menubar_for(int kind, int *n) {
#define BAR(m) do { *n = (int)(sizeof(m) / sizeof(m[0])); return m; } while (0)
    switch (kind) {
    case WK_NONE: BAR(mb_desktop);
    case WK_FILES: BAR(mb_files);
    case WK_EDIT: BAR(mb_editor);
    case WK_WRITER: BAR(mb_writer);
    case WK_SPREADSHEET: BAR(mb_sheet);
    case WK_PAINT: BAR(mb_paint);
    case WK_TERM: BAR(mb_term);
    case WK_SNAKE: case WK_WORDLE: case WK_MINES: case WK_2048: case WK_BREAKOUT: BAR(mb_game);
    case WK_CALC: BAR(mb_calc);
    case WK_VIEW: BAR(mb_viewer);
    case WK_TODO: case WK_CAL: BAR(mb_save);
    default: BAR(mb_plain);
    }
#undef BAR
}

static void recent_add(int id) {
    if (!fs_valid(id) || fs_is_dir(id) || fs_is_app(id)) return;
    unsigned identity = fs_identity(id);
    int at = recent_n < RECENT_N ? recent_n : RECENT_N - 1;
    for (int i = 0; i < recent_n; i++)
        if (recent[i].id == id && recent[i].identity == identity) { at = i; break; }
    for (int i = at; i > 0; i--) recent[i] = recent[i - 1];
    recent[0].id = id;
    recent[0].identity = identity;
    if (at == recent_n && recent_n < RECENT_N) recent_n++;
}
static int recent_valid(int i) {
    return i >= 0 && i < recent_n && fs_valid(recent[i].id) && !fs_is_dir(recent[i].id) &&
           fs_identity(recent[i].id) == recent[i].identity;
}
static void recent_prune(void) {
    int n = 0;
    for (int i = 0; i < recent_n; i++) if (recent_valid(i)) recent[n++] = recent[i];
    recent_n = n;
}
/* The folder name shown beside a recent file. */
static const char *recent_folder(int id) {
    int parent = fs_parent(id);
    return parent < 0 || parent == fs_root() ? "Home" : fs_name(parent);
}
static char recent_label[RECENT_N][FS_NAME_LEN + 1];
static void recent_build_menu(void) {
    recent_prune();
    mi_recent_n = 0;
    for (int i = 0; i < recent_n; i++) {
        /* An '&' in a file name is shown, not taken as a mnemonic. */
        const char *name = fs_name(recent[i].id);
        int n = 0;
        for (int k = 0; name[k] && n < FS_NAME_LEN; k++) if (name[k] != '&') recent_label[i][n++] = name[k];
        recent_label[i][n] = 0;
        mi_recent[mi_recent_n++] = (MItem){recent_label[i], recent_folder(recent[i].id), ACT_RECENT + i, 0};
    }
    if (!recent_n) mi_recent[mi_recent_n++] = (MItem){"No recent files", "", ACT_NONE, 0};
    mi_recent[mi_recent_n++] = (MItem){"-", "", ACT_SEP, 0};
    mi_recent[mi_recent_n++] = (MItem){"&Clear Recent", "", ACT_RECENT_CLEAR, 0};
}

static int place_dir(const char *name) {
    int id = fs_find_child(fs_root(), name);
    return fs_is_dir(id) ? id : -1;
}

static void fm_sort_set(int sort, int descending) {
    fm_rename_cancel();
    fm_filter_focus = fm_filter_select_all = 0;
    fm_view.sort = sort;
    fm_view.descending = descending;
    fm_manual_scroll = 0;
    if (fm_selected < 0) fm_first = 0;
    fm_last_click_item = -1;
    fm_refresh();
    dirty = 1;
}

/* Move the selected Files row to the Trash. */
static void fm_trash_selected(void) {
    int id = fm_row_id(fm_selected);
    if (id < 0 || trash_id < 0 || id == trash_id || id == fs_root() || fs_is_app(id) ||
        fs_parent(id) == trash_id) return;
    if (files_storage_busy()) return;
    fm_rename_cancel();
    if (fs_move(id, trash_id) < 0) { dirty = 1; return; }
    int owner = context_slot;
    for (int i = 0; i < MAX_WIN; i++) if (wins[i].open && wins[i].kind == WK_FILES) {
        context_set(i);
        if (!fm_cwd_valid()) fm_set_cwd(fs_root());
        fm_refresh();
    }
    context_set(owner);
    if (open_dlg) od_refresh();
    dirty = 1;
}

static int files_selected_item(void) {
    if (front_kind() != WK_FILES) return -2;
    int id = fm_row_id(fm_selected);
    return id >= 0 && id != fs_root() ? id : -2;
}

static int act_enabled(int act) {
    if (act == ACT_NONE || act == ACT_SEP) return 0;
    if (act >= ACT_RECENT && act < ACT_RECENT + RECENT_N) return recent_valid(act - ACT_RECENT);
    if (act >= 0x100 && act < 0x200) return menu_item_enabled((act >> 4) & 15, act & 15);
    if (open_dlg) return act == ACT_ABOUT || act == ACT_HELP;
    int fk = front_kind(), f = win_front(), id;
    switch (act) {
    case ACT_NEW_WINDOW: return fk == WK_EDIT || fk == WK_FILES || fk == WK_TERM;
    case ACT_SAVE_AS: return fk == WK_EDIT || fk == WK_WRITER || fk == WK_SPREADSHEET;
    case ACT_REVERT: return fk == WK_EDIT && edit_binding_valid() && !edit_saved_ok;
    case ACT_UNDO: case ACT_REDO:
        return fk == WK_EDIT || fk == WK_PAINT || fk == WK_WRITER || fk == WK_SPREADSHEET;
    case ACT_SELECT_ALL: return fk == WK_EDIT || fk == WK_WRITER || fk == WK_SPREADSHEET;
    case ACT_FIND_NEXT: return fk == WK_EDIT;
    case ACT_OPEN_ITEM: return fk == WK_FILES && fm_row_id(fm_selected) >= -1;
    case ACT_RENAME: case ACT_TRASH:
        id = files_selected_item();
        return id >= 0 && !fs_is_app(id) && id != trash_id && id != prefs_id &&
               (act != ACT_TRASH || (trash_id >= 0 && fs_parent(id) != trash_id));
    case ACT_COPY_PATH: return files_selected_item() >= 0;
    case ACT_NEW_FILE: case ACT_REFRESH: case ACT_FILTER:
    case ACT_SORT_NAME: case ACT_SORT_KIND: case ACT_SORT_SIZE: case ACT_SORT_MODIFIED:
    case ACT_SORT_DESCENDING:
        return fk == WK_FILES;
    case ACT_GO_BACK: return fk == WK_FILES && fm_history_target(-1) >= 0;
    case ACT_GO_FORWARD: return fk == WK_FILES && fm_history_target(1) >= 0;
    case ACT_GO_UP: return fk == WK_FILES && fm_has_parent();
    case ACT_GO_DOCUMENTS: return place_dir("Documents") >= 0;
    case ACT_GO_PICTURES: return place_dir("Pictures") >= 0;
    case ACT_GO_TRASH: return fs_is_dir(trash_id);
    case ACT_LINE_NUMBERS: return fk == WK_EDIT;
    case ACT_RESTORE: return f >= 0 && wins[f].maximized;
    case ACT_MOVE: case ACT_SIZE: return f >= 0 && !wins[f].maximized;
    case ACT_RECENT_CLEAR: return recent_n > 0;
    case ACT_QUIT: return f >= 0;
    case ACT_DESK_OPEN: case ACT_DESK_PROPERTIES: return icon_sel >= 0;
    default: return 1;
    }
}

static int act_checked(int act) {
    if (front_kind() == WK_FILES) {
        if (act == ACT_SORT_NAME) return fm_view.sort == FILE_VIEW_NAME;
        if (act == ACT_SORT_KIND) return fm_view.sort == FILE_VIEW_TYPE;
        if (act == ACT_SORT_SIZE) return fm_view.sort == FILE_VIEW_SIZE;
        if (act == ACT_SORT_MODIFIED) return fm_view.sort == FILE_VIEW_MODIFIED;
        if (act == ACT_SORT_DESCENDING) return fm_view.descending;
    }
    if (act == ACT_LINE_NUMBERS) return !!(win_options & WINOPT_LINE_NUMBERS);
    return 0;
}

static void menu_close(void) {
    open_menu = MENU_NONE;
    menu_sel = -1;
    popup_n = 0;
    menu_bar_focus = -1;
    dirty = 1;
}

static void do_restart(void);
static void windows_save(void);

static void act_run(int act) {
    menu_close();
    if (!act_enabled(act)) return;
    fm_last_click_item = -1;
    if (act >= ACT_RECENT && act < ACT_RECENT + RECENT_N) { open_fs_file(recent[act - ACT_RECENT].id); return; }
    if (act >= 0x100 && act < 0x200) { menu_activate((act >> 4) & 15, act & 15); return; }
    int fk = front_kind(), f = win_front();
    switch (act) {
    case ACT_NEW_WINDOW:
        if (fk == WK_FILES) open_files(fm_cwd);
        else if (fk == WK_TERM) open_term();
        else open_edit();
        break;
    case ACT_SAVE_AS:
        if (fk == WK_EDIT) namedlg_open(0, edit_binding_valid() ? fs_name(edit_file) : "untitled.txt");
        else if (fk == WK_WRITER) namedlg_open(2, writer_file() >= 0 ? fs_name(writer_file()) : "untitled.bwr");
        else namedlg_open(4, spreadsheet_file() >= 0 ? fs_name(spreadsheet_file()) : "untitled.bsh");
        break;
    case ACT_REVERT:
        edit_record();
        edit_load(edit_file);
        break;
    case ACT_UNDO: case ACT_REDO:
        if (fk == WK_EDIT) edit_undo(act == ACT_REDO);
        else if (fk == WK_PAINT) paint_undo(act == ACT_REDO);
        else if (fk == WK_WRITER) writer_result(writer_key(act == ACT_REDO ? 0x15 : 0x2c, 0, WRITER_MOD_CTRL));
        else spreadsheet_result(spreadsheet_key(act == ACT_REDO ? 0x15 : 0x2c, 0, SPREADSHEET_MOD_CTRL));
        break;
    case ACT_SELECT_ALL:
        if (fk == WK_EDIT) { edit_sel_a = 0; edit_sel_b = edit_caret = edit_len; }
        else if (fk == WK_WRITER) writer_result(writer_key(0x1e, 0, WRITER_MOD_CTRL));
        else spreadsheet_result(spreadsheet_key(0x1e, 0, SPREADSHEET_MOD_CTRL));
        break;
    case ACT_FIND_NEXT:
        if (!edit_search.open) edit_search_open(0);
        edit_find_next(1);
        break;
    case ACT_OPEN_ITEM: fm_open_selected(); break;
    case ACT_NEW_FILE: fm_new_file(); break;
    case ACT_RENAME: fm_rename_begin(fm_row_id(fm_selected)); break;
    case ACT_TRASH: fm_trash_selected(); break;
    case ACT_REFRESH: fm_refresh(); break;
    case ACT_FILTER: fm_filter_show(); break;
    case ACT_GO_BACK: fm_history_step(-1); break;
    case ACT_GO_FORWARD: fm_history_step(1); break;
    case ACT_GO_UP: fm_go_up(); break;
    case ACT_GO_HOME: case ACT_GO_DOCUMENTS: case ACT_GO_PICTURES: case ACT_GO_TRASH: {
        int target = act == ACT_GO_HOME ? fs_root() : act == ACT_GO_TRASH ? trash_id :
                     place_dir(act == ACT_GO_DOCUMENTS ? "Documents" : "Pictures");
        if (fk == WK_FILES) fm_navigate(target);
        else open_files(target);
        break;
    }
    case ACT_SORT_NAME: case ACT_SORT_KIND: case ACT_SORT_SIZE: case ACT_SORT_MODIFIED: {
        int sort = act - ACT_SORT_NAME;
        fm_sort_set(sort, fm_view.sort == sort ? fm_view.descending : sort == FILE_VIEW_MODIFIED);
        break;
    }
    case ACT_SORT_DESCENDING: fm_sort_set(fm_view.sort, !fm_view.descending); break;
    case ACT_LINE_NUMBERS: win_options ^= WINOPT_LINE_NUMBERS; windows_save(); break;
    case ACT_RESTORE: win_arrange(f, 0); break;
    case ACT_MOVE: case ACT_SIZE:
        win_kbd_mode = act == ACT_MOVE ? 1 : 2;
        win_kbd_slot = f;
        win_kbd_begin();
        break;
    case ACT_SETTINGS: win_open(WK_SETTINGS); break;
    case ACT_RECENT_CLEAR: recent_n = 0; break;
    case ACT_COPY_PATH: {
        char path[FS_PATH_LEN];
        fs_path(files_selected_item(), path, sizeof path);
        clip_len = kstrlen(path);
        kmemcpy(clip_buf, path, clip_len + 1);
        clipboard_changed();
        break;
    }
    case ACT_QUIT:
        for (int i = 0; i < MAX_WIN; i++)
            if (wins[i].open && wins[i].kind == fk && edit_close_owner < 0) win_request_close(i);
        break;
    case ACT_RESTART: do_restart(); break;
    case ACT_DESK_OPEN: desk_open(icon_sel); break;
    case ACT_DESK_PROPERTIES:
        properties_id = icon_sel == DESK_TRASH && trash_id >= 0 ? trash_id : fs_root();
        properties_reason = "";
        win_open(WK_PROPERTIES);
        break;
    case ACT_SHUTDOWN: do_shutdown(); break;
    default: break;
    }
    dirty = 1;
}

/* ---------- Menu input ---------- */

static int popup_at(int px, int py, int *item) {
    for (int k = popup_n - 1; k >= 0; k--) {
        int i = popup_item_at(&popup[k], px, py);
        if (i >= 0 || hit(px, py, popup[k].x, popup[k].y, popup[k].w, popup[k].h)) {
            *item = i;
            return k;
        }
    }
    *item = -1;
    return -1;
}

static void menu_open_bar(int m) {
    menu_bar_init();
    if (m < 0 || m >= bar_n) return;
    menu_open(m);
    open_menu = m;
    menu_bar_focus = m;
    dirty = 1;
}

static void menu_open_context(const MItem *items, int n, int x, int y) {
    menu_open_popup(items, n, x, y);
    open_menu = MENU_CONTEXT;
    menu_bar_focus = -1;
    dirty = 1;
}

static void menu_open_window(int slot) {
    if (slot < 0) return;
    win_focus(slot);
    Win *w = &wins[slot];
    menu_open_context(mi_window, (int)(sizeof mi_window / sizeof mi_window[0]),
                      w->x + (MODERN ? 8 : 4), w->y + TITLE_BAR - (MODERN ? 4 : 1));
}

/* Pointer moved while a menu is open: track the item and follow submenus
 * and neighbouring menus of the row. */
static void menu_hover(void) {
    int item, k = popup_at(mouse_x, mouse_y, &item);
    if (k >= 0) {
        if (popup[k].sel != item && (item >= 0 || k == popup_n - 1)) {
            popup[k].sel = item;
            if (k == 0 && item >= 0) {
                if (popup[0].items[item].flags & KIT_MENU_SUB) menu_open_sub(item);
                else popup_n = 1;
            }
            dirty = 1;
        }
        return;
    }
    if (open_menu >= 0 && open_menu < MENU_CONTEXT) {
        int bar = win_menu_at(mouse_x, mouse_y);
        if (bar >= 0 && bar != open_menu) menu_open_bar(bar);
    }
}

static void menu_click(void) {
    int item, k = popup_at(mouse_x, mouse_y, &item);
    if (k >= 0) {
        if (item < 0) return;
        const MItem *it = &popup[k].items[item];
        if (it->flags & KIT_MENU_SUB) { popup[0].sel = item; menu_open_sub(item); dirty = 1; return; }
        if (act_enabled(it->act)) act_run(it->act);
        return;
    }
    int bar = win_menu_at(mouse_x, mouse_y);
    if (bar >= 0) {
        if (bar == open_menu) menu_close();
        else menu_open_bar(bar);
        return;
    }
    menu_close();
}

static int menu_item_selectable(const Popup *p, int i) {
    return !mi_is_sep(&p->items[i]) && (act_enabled(p->items[i].act) || (p->items[i].flags & KIT_MENU_SUB));
}

static void menu_step(Popup *p, int dir) {
    for (int n = 0; n < p->n; n++) {
        p->sel = p->sel < 0 ? (dir > 0 ? 0 : p->n - 1) : (p->sel + dir + p->n) % p->n;
        if (menu_item_selectable(p, p->sel)) return;
    }
    p->sel = -1;
}

/* Keys while a menu or the focused menu row is up. Returns 1 when used. */
static int menu_key(void) {
    if (open_menu == MENU_BAR) {
        menu_bar_init();
        if (key_sc == KEY_ESC || key_sc == 0x44) { menu_close(); return 1; }
        if (key_sc == KEY_LEFT || key_sc == KEY_RIGHT) {
            if (bar_n) menu_bar_focus = (menu_bar_focus + (key_sc == KEY_LEFT ? bar_n - 1 : 1)) % bar_n;
            dirty = 1;
            return 1;
        }
        if (key_sc == KEY_DOWN || key_sc == KEY_UP || key_sc == KEY_ENTER || key_sc == KEY_SPACE) {
            int m = menu_bar_focus;
            menu_open_bar(m);
            if (popup_n) menu_step(&popup[0], key_sc == KEY_UP ? -1 : 1);
            return 1;
        }
        char c = key_char >= 'A' && key_char <= 'Z' ? (char)(key_char + 32) : key_char;
        for (int i = 0; c && i < bar_n; i++)
            if (mi_mnemonic(bar_menus[i].name) == c) {
                menu_open_bar(i);
                if (popup_n) menu_step(&popup[0], 1);
                return 1;
            }
        return 1;
    }
    if (open_menu < 0 || !popup_n) return 0;
    Popup *p = &popup[popup_n - 1];
    if (key_sc == KEY_ESC) {
        if (popup_n == 2) popup_n = 1;
        else if (open_menu < MENU_CONTEXT) { popup_n = 0; open_menu = MENU_BAR; }
        else menu_close();
        dirty = 1;
        return 1;
    }
    if (key_sc == KEY_UP || key_sc == KEY_DOWN) { menu_step(p, key_sc == KEY_UP ? -1 : 1); dirty = 1; return 1; }
    if (key_sc == KEY_RIGHT) {
        if (popup_n == 1 && p->sel >= 0 && (p->items[p->sel].flags & KIT_MENU_SUB)) {
            menu_open_sub(p->sel);
            menu_step(&popup[1], 1);
        } else if (open_menu < MENU_CONTEXT) {
            menu_bar_init();
            menu_open_bar((open_menu + 1) % (bar_n ? bar_n : 1));
            if (popup_n) menu_step(&popup[0], 1);
        }
        dirty = 1;
        return 1;
    }
    if (key_sc == KEY_LEFT) {
        if (popup_n == 2) popup_n = 1;
        else if (open_menu < MENU_CONTEXT) {
            menu_bar_init();
            menu_open_bar((open_menu + bar_n - 1) % (bar_n ? bar_n : 1));
            if (popup_n) menu_step(&popup[0], 1);
        }
        dirty = 1;
        return 1;
    }
    if (key_sc == KEY_ENTER || key_sc == KEY_SPACE) {
        if (p->sel < 0) return 1;
        const MItem *it = &p->items[p->sel];
        if (it->flags & KIT_MENU_SUB) { menu_open_sub(p->sel); menu_step(&popup[1], 1); dirty = 1; return 1; }
        if (act_enabled(it->act)) act_run(it->act);
        return 1;
    }
    char c = key_char >= 'A' && key_char <= 'Z' ? (char)(key_char + 32) : key_char;
    for (int i = 0; c && i < p->n; i++) {
        if (mi_is_sep(&p->items[i]) || mi_mnemonic(p->items[i].label) != c) continue;
        if (p->items[i].flags & KIT_MENU_SUB) { p->sel = i; menu_open_sub(i); menu_step(&popup[1], 1); dirty = 1; return 1; }
        if (act_enabled(p->items[i].act)) act_run(p->items[i].act);
        return 1;
    }
    return 1;
}

/* Alt+letter opens a menu of the row by its mnemonic. */
static int menu_mnemonic_open(void) {
    if (!alt_down || ctrl_down || !key_char) return 0;
    if (!MODERN && (win_front() < 0 || !win_has_menu(front_kind()))) return 0;
    menu_bar_init();
    char c = key_char >= 'A' && key_char <= 'Z' ? (char)(key_char + 32) : key_char;
    for (int i = 0; i < bar_n; i++)
        if (mi_mnemonic(bar_menus[i].name) == c) {
            menu_open_bar(i);
            if (popup_n) menu_step(&popup[0], 1);
            return 1;
        }
    return 0;
}

static void menu_focus_row(void) {
    if (!MODERN && (win_front() < 0 || !win_has_menu(front_kind()))) return;
    menu_bar_init();
    if (!bar_n) return;
    popup_n = 0;
    open_menu = MENU_BAR;
    menu_bar_focus = 0;
    dirty = 1;
}

typedef struct {
    char name[FS_NAME_LEN]; /* A result owns its display name and node identity. */
    int icon;
    int file;
    unsigned identity;
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
    for (int i = 0; i < ICON_DESK_TRASH && launch_n < 24; i++) {
        if (str_has(icons[i].label, launch_buf)) {
            kstrcpy(launch_items[launch_n].name, icons[i].label);
            launch_items[launch_n].identity = 0;
            launch_items[launch_n].icon = i;
            launch_items[launch_n].file = -1;
            launch_n++;
        }
    }
    for (int id = 0; id < fs_node_slots() && launch_n < 24; id++) {
        if (!fs_valid(id) || fs_is_app(id) || fs_is_dir(id))
            continue;
        if (launch_len == 0 && launch_n >= 8)
            break;
        if (str_has(fs_name(id), launch_buf)) {
            kstrcpy(launch_items[launch_n].name, fs_name(id));
            launch_items[launch_n].identity = fs_identity(id);
            launch_items[launch_n].icon = -1;
            launch_items[launch_n].file = id;
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
    launch_scroll = 0;
    open_menu = MENU_NONE;
    launcher_refresh();
    dirty = 1;
}

static void launcher_close(void) {
    launcher_on = 0;
    dirty = 1;
}

/* App launcher: a centered glass panel with a search field over a grid of
 * applications. Typing filters applications and files; the grid scrolls by
 * rows when it does not fit. */
#define LP_CELL_W 108
#define LP_CELL_H 92
#define LP_FIELD_H 34
#define LP_PAD 24
#define LP_FOOT 44
#define LP_HEAD (LP_PAD + LP_FIELD_H + 18)
static int launcher_cols(void) {
    int c = (fb_w - 80 - 2 * LP_PAD) / LP_CELL_W;
    return c > 6 ? 6 : c < 3 ? 3 : c;
}
static int launcher_fit(void) {
    int rows = (TASKBAR_Y - 40 - LP_HEAD - 12 - LP_FOOT) / LP_CELL_H;
    return rows < 2 ? 2 : rows;
}
static int launcher_total_rows(void) { int c = launcher_cols(); return (launch_n + c - 1) / c; }
static int launcher_rows(void) {
    int total = launcher_total_rows(), fit = launcher_fit();
    return total < fit ? total : fit;
}
static void launcher_geom(int *x, int *y, int *w, int *h) {
    int rows = launcher_rows();
    *w = launcher_cols() * LP_CELL_W + 2 * LP_PAD;
    *h = LP_HEAD + (rows ? rows * LP_CELL_H : 40) + 12 + LP_FOOT;
    *x = (fb_w - *w) / 2;
    *y = (TASKBAR_Y - *h) / 2;
    if (*y < 16) *y = 16;
}
static void launcher_cell(int x, int y, int i, int *cx, int *cy) {
    int c = launcher_cols(), k = i - launch_scroll * c;
    *cx = x + LP_PAD + (k % c) * LP_CELL_W;
    *cy = y + LP_HEAD + (k / c) * LP_CELL_H;
}
static void launcher_clamp_scroll(void) {
    int rows = launcher_rows(), total = launcher_total_rows();
    if (launch_scroll > total - rows) launch_scroll = total - rows;
    if (launch_scroll < 0) launch_scroll = 0;
}
static void launcher_reveal(void) {
    int c = launcher_cols(), row = launch_sel / c, rows = launcher_rows();
    launcher_clamp_scroll();
    if (row < launch_scroll) launch_scroll = row;
    if (rows && row >= launch_scroll + rows) launch_scroll = row - rows + 1;
}
static void launcher_scroll(int amount) {
    launch_scroll += amount;
    launcher_clamp_scroll();
    dirty = 1;
}
static int launcher_footer_x(int x, int w, int button) { return x + w - 16 - (4 - button) * 36; }

static void launcher_run(int i) {
    if (i < 0 || i >= launch_n)
        return;
    LaunchItem it = launch_items[i];
    if(it.file>=0 && (!it.identity || fs_identity(it.file)!=it.identity ||
       kstrcmp(fs_name(it.file),it.name))){launcher_refresh();dirty=1;return;}
    launcher_close();
    if (it.file >= 0)
        open_fs_file(it.file);
    else
        icon_open(it.icon);
}

static void launcher_key(void) {
    int c = launcher_cols();
    if (key_sc == KEY_ESC) {
        launcher_close();
        return;
    } else if (key_sc == KEY_ENTER) {
        launcher_run(launch_sel);
        return;
    } else if (key_sc == KEY_LEFT) {
        if (launch_sel > 0) launch_sel--;
    } else if (key_sc == KEY_RIGHT) {
        if (launch_sel + 1 < launch_n) launch_sel++;
    } else if (key_sc == KEY_UP) {
        if (launch_sel - c >= 0) launch_sel -= c;
    } else if (key_sc == KEY_DOWN) {
        if (launch_sel + c < launch_n) launch_sel += c;
        else if (launch_sel / c < (launch_n - 1) / c) launch_sel = launch_n - 1;
    } else if (key_sc == KEY_BACKSPACE) {
        if (launch_len > 0)
            launch_len--;
        launcher_refresh();
    } else if (key_char && launch_len < 22) {
        launch_buf[launch_len++] = key_char;
        launcher_refresh();
    } else return;
    launcher_reveal();
    dirty = 1;
}

static int launcher_item_icon(int i) {
    if (launch_items[i].icon >= 0) return app_icon_for(icons[launch_items[i].icon].icon);
    return file_extension(launch_items[i].name, ".bex") ? AI_NATIVE : AI_DOCUMENT;
}

static void draw_launcher(void) {
    if (!launcher_on)
        return;
    int x, y, w, h;
    launcher_geom(&x, &y, &w, &h);
    launcher_clamp_scroll();
    shade_rect(0, 0, fb_w, TASKBAR_Y, 1);
    draw_shadow(x, y, w, h);
    glass_round(x, y, w, h, 14, 1, COLOR_WHITE, 205, 215);
    draw_round_frame(x - 1, y - 1, w + 2, h + 2, 15, INK(14));
    draw_round_frame(x, y, w, h, 14, INK(0));
    int fx = x + LP_PAD, fy = y + LP_PAD, fw = w - 2 * LP_PAD, fh = LP_FIELD_H;
    draw_round_rect(fx, fy, fw, fh, 8, INK(0));
    draw_round_frame(fx, fy, fw, fh, 8, launch_len ? ui_accent : INK(12));
    icon_draw(ICON_SEARCH, fx + 11, fy + (fh - ICON_SIZE) / 2, INK(16));
    launch_buf[launch_len] = 0;
    int tx = fx + 36;
    int ty = fy + (fh - CHAR_H) / 2;
    if (launch_len)
        draw_string(launch_buf, tx, ty, ui_text);
    else
        draw_string("Search apps and files", tx, ty, INK(15));
    int cx = tx + ui_string_w(launch_buf);
    if ((frame_count / 35) & 1)
        draw_rect(cx + 1, fy + 8, 1, fh - 16, INK(28));
    int c = launcher_cols(), rows = launcher_rows();
    int first = launch_scroll * c, last = first + rows * c;
    if (last > launch_n) last = launch_n;
    for (int i = first; i < last; i++) {
        int gx, gy;
        launcher_cell(x, y, i, &gx, &gy);
        int sel = i == launch_sel;
        if (sel) {
            draw_round_rect(gx + 4, gy, LP_CELL_W - 8, LP_CELL_H - 6, 10, INK(3));
            draw_round_frame(gx + 4, gy, LP_CELL_W - 8, LP_CELL_H - 6, 10, TK_ACCENT);
        }
        app_icon_draw(launcher_item_icon(i), gx + (LP_CELL_W - AI_BIG) / 2, gy + 10, AI_BIG);
        const char *label = launch_items[i].name;
        int lw = ui_string_w(label);
        if (lw > LP_CELL_W - 12) lw = LP_CELL_W - 12;
        draw_string_clip(label, gx + (LP_CELL_W - lw) / 2, gy + 10 + AI_BIG + 8, sel ? INK(31) : INK(27), gx + LP_CELL_W - 6);
    }
    if (launch_n == 0)
        draw_string("No matches", fx + 6, y + LP_HEAD + 8, ui_text_dim);
    if (launcher_total_rows() > rows) {
        int track = rows * LP_CELL_H - 8, total = launcher_total_rows();
        int thumb = track * rows / total, top = track * launch_scroll / total;
        draw_round_rect(x + w - 10, y + LP_HEAD, 4, track, 2, INK(5));
        draw_round_rect(x + w - 10, y + LP_HEAD + top, 4, thumb < 16 ? 16 : thumb, 2, INK(14));
    }
    /* Footer: identity on the left; About, Settings, Sleep and Power on the right. */
    int foot = y + h - LP_FOOT;
    draw_hline(x + LP_PAD, foot, w - 2 * LP_PAD, INK(7));
    draw_string_bold("BaseOS", x + LP_PAD + 4, foot + (LP_FOOT - CHAR_H) / 2, INK(18));
    static const int foot_icon[4] = {ICON_APP_ABOUT, ICON_SETTINGS, ICON_MOON, ICON_POWER};
    for (int b = 0; b < 4; b++)
        tool_button(launcher_footer_x(x, w, b), foot + 8, 32, 28, foot_icon[b], 0, TB_FLAT);
}

static int launcher_click(void) {
    int x, y, w, h;
    launcher_geom(&x, &y, &w, &h);
    if (!hit(mouse_x, mouse_y, x, y, w, h)) {
        launcher_close();
        return 1;
    }
    int foot = y + h - LP_FOOT;
    for (int b = 0; b < 4; b++) {
        if (!hit(mouse_x, mouse_y, launcher_footer_x(x, w, b), foot + 8, 32, 28)) continue;
        launcher_close();
        if (b == 0) win_open(WK_ABOUT);
        else if (b == 1) icon_open(ICON_DESK_SETTINGS);
        else if (b == 2) saver_start();
        else do_shutdown();
        return 1;
    }
    int c = launcher_cols(), rows = launcher_rows();
    int first = launch_scroll * c, last = first + rows * c;
    if (last > launch_n) last = launch_n;
    for (int i = first; i < last; i++) {
        int gx, gy;
        launcher_cell(x, y, i, &gx, &gy);
        if (hit(mouse_x, mouse_y, gx + 4, gy, LP_CELL_W - 8, LP_CELL_H - 6)) {
            launcher_run(i);
            return 1;
        }
    }
    return 1;
}

static void menu_activate(int m, int item);
static void saver_start(void) {
    input_saver_buttons=(mouse_left?INPUT_LEFT:0)|(mouse_right?INPUT_RIGHT:0);
    desktop_input_fence();
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
    si->frames = redraw_count;
    si->task_n = 0;
    for(int i=0;i<MAX_WIN&&si->task_n<SYSMON_MAX_TASKS;i++)
        if(wins[i].open&&(wins[i].kind==WK_TERM||wins[i].kind==WK_NATIVE)&&
           app_view_info(i,&si->tasks[si->task_n]))si->task_n++;
    si->program_n = 0;
    for(uint32_t pid=uproc_next(0,0);pid&&si->program_n<SYSMON_MAX_PROGRAMS;pid=uproc_next(pid,0)){
        UprocInfo info;
        if(!uproc_info(pid,&info)||info.state!=UPROC_RUNNING)continue;
        SysmonProgram *program=&si->programs[si->program_n++];
        program->pid=info.pid;program->threads=info.threads;
        program->kib=info.pages*4;program->cpu_ms=info.cpu_ms;
        kmemcpy(program->name,info.name,sizeof program->name);
    }
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
        si->win_name[k] = win_display_title(i,si->win_title[k]);
        si->win_id[k] = i;
        si->win_instance[k] = (unsigned)wins[i].seq;
        si->win_min[k] = wins[i].min;
        si->win_n++;
    }
    si->fs_nodes = fs_node_count();
    si->fs_max = fs_node_limit() <= FS_MAX_NODES ? fs_node_limit() : 0;
    si->fs_bytes = (int)fs_used_bytes();
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
static int name_focus, name_owner, name_owner_seq;
static unsigned name_pdf_paper;

static void namedlg_open(int target, const char *initial) {
    fm_checked_cwd();
    name_dlg = 1;
    name_focus = name_failed = 0;
    name_failure_message=0;
    name_owner = context_slot;
    name_owner_seq = wins[name_owner].seq;
    name_target = target;
    name_pdf_paper = WRITER_PDF_LETTER;
    name_len = 0;
    for (int i = 0; initial && initial[i] && i < FS_NAME_LEN - 1; i++)
        name_buf[name_len++] = initial[i];
    name_buf[name_len] = 0;
    if (target == 3 || target == 6) {
        if (writer_export_retry_name(target == 6 ? DOCUMENT_SAVE_PDF : DOCUMENT_SAVE_RTF,
                                     name_buf, sizeof name_buf, &name_pdf_paper)) name_len = kstrlen(name_buf);
    } else if (target == 5 && spreadsheet_export_retry_name(name_buf, sizeof name_buf)) name_len = kstrlen(name_buf);
    dirty = 1;
}

static void namedlg_hide(void) { name_dlg = 0; dirty = 1; }
static void namedlg_close(void) {
    namedlg_hide();
    edit_close_cancel(); /* Canceling Save As also cancels the close request. */
    dirty = 1;
}

static void namedlg_geom(int *x, int *y, int *w, int *h) {
    *w = 420;
    *h = name_target == 6 ? 260 : 168;
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
    if (name_owner < 0 || name_owner >= MAX_WIN || !wins[name_owner].open ||
        wins[name_owner].seq != name_owner_seq) { namedlg_close(); return; }
    context_set(name_owner);
    name_buf[name_len] = 0;
    if (name_len == 0 && name_target != 6) { name_failed = 1; dirty = 1; return; }
    int close_after = edit_close_valid() && edit_close_owner == name_owner;
    int action = document_action, target = document_target;
    unsigned identity = document_target_identity;
    int parent = fm_checked_cwd();
    int result = name_target == 0 ? edit_write_named(name_buf) :
                 name_target == 1 ? paint_write_named(name_buf) :
                 name_target == 2 ? writer_save_as(parent, name_buf) :
                 name_target == 3 ? writer_export_rtf(parent, name_buf) :
                 name_target == 6 ? writer_export_pdf(parent, name_buf, name_pdf_paper) :
                 name_target == 4 ? spreadsheet_save_as(parent, name_buf) :
                                    spreadsheet_export_csv(parent, name_buf);
    int exporting = name_target == 3 || name_target == 5 || name_target == 6;
    int pending = exporting ? result == DOCUMENT_EXPORT_PENDING :
                  (name_target == 2 || name_target == 4) && result == DOCUMENT_SAVE_PENDING;
    int ok = exporting ? result >= 0 : result == 1;
    if (pending || ok) {
        int owner = name_owner;
        namedlg_hide(); /* Acceptance is distinct from canceling the intent. */
        if (close_after && !exporting) {
            if (pending) {
                if (!document_wait_for_save()) { edit_close_failed = 1; edit_close_dlg = 1; edit_close_focus = 2; }
            } else if (document_destination_valid()) {
                edit_close_cancel(); document_finish(owner, action, target, identity);
            } else { edit_close_failed = 3; edit_close_dlg = 1; edit_close_focus = 2; }
        }
    } else { name_failed = 1; dirty = 1; }
}

static void edit_close_geom(int *x, int *y) {
    *x = (fb_w - 460) / 2;
    *y = (fb_h - 184) / 2;
}

static void draw_edit_close(void) {
    if (!edit_close_dlg) return;
    int x, y;
    edit_close_geom(&x, &y);
    shade_rect(0, 0, fb_w, fb_h, 1);
    draw_shadow(x, y, 460, 184);
    draw_round_rect(x - 1, y - 1, 462, 186, 11, ui_border);
    draw_round_rect(x, y, 460, 184, 10, INK(1));
    draw_string_bold(document_pending_owner ? "Saving changes..." : document_action == DOCUMENT_CLOSE ? "Save changes before closing?"
        : "Save changes before replacing?", x + 22, y + 20, ui_text);
    const Document *doc = &window_state[edit_close_owner].doc;
    const char *name = wins[edit_close_owner].kind == WK_SPREADSHEET ? spreadsheet_title() :
        wins[edit_close_owner].kind == WK_WRITER ? writer_title() :
        doc->file >= 0 && fs_identity(doc->file) == doc->identity
                     ? fs_name(doc->file) : "untitled";
    draw_string_clip(name, x + 22, y + 50, ui_text, x + 438);
    draw_string(edit_close_failed && fs_sync_busy() ? "Disk saving. Retry Save shortly."
                : document_pending_owner ? "Saving to disk. Cancel keeps this document open."
                : edit_close_failed == 3 ? "Requested file changed. Cancel and choose it again."
                : edit_close_failed == 2 ? "Newer changes still need saving."
                : edit_close_failed ? "Save failed. Your document is still open."
                                 : "Your unsaved changes will be lost if discarded.",
                x + 22, y + 82, ui_text_dim);
    const char *labels[] = {"Save", "Discard", "Cancel"};
    for (int i = 0; i < 3; i++) {
        if ((document_pending_owner || edit_close_failed == 3) && i != 2) continue;
        int bx = x + 126 + 106 * i;
        if (i == edit_close_focus) draw_default_button(bx, y + 132, 96, BTN_H, labels[i]);
        else draw_button(bx, y + 132, 96, BTN_H, labels[i]);
    }
}

static void edit_close_click(void) {
    int x, y;
    edit_close_geom(&x, &y);
    for (int i = 0; i < 3; i++)
        if (hit(mouse_x, mouse_y, x + 126 + 106 * i, y + 132, 96, BTN_H)) {
            edit_close_choose(i); return;
        }
    /* Outside clicks cannot silently discard or dismiss a close request. */
}

static void edit_close_key(void) {
    if (key_sc == KEY_ESC) edit_close_cancel();
    else if (key_sc == KEY_TAB) {
        edit_close_focus = document_pending_owner || edit_close_failed == 3 ? 2 : (edit_close_focus + (shift_down ? 2 : 1)) % 3;
        dirty = 1;
    } else if (key_sc == KEY_ENTER) edit_close_choose(edit_close_focus);
}

/* PDF errors must remain readable rather than disappearing beyond one line. */
static void namedlg_pdf_message(const char *message, int x, int y, int width) {
    for (int row = 0; row < 3 && *message; row++) {
        char line[112]; int n = 0, last_space = -1;
        while (message[n] && n < (int)sizeof(line) - 1) {
            line[n] = message[n]; line[n + 1] = 0;
            if (ui_string_w(line) > width) break;
            if (message[n] == ' ') last_space = n;
            n++;
        }
        if (message[n] && last_space > 0) n = last_space;
        if (!n) n = 1;
        line[n] = 0;
        draw_string_clip(line, x, y + row * 18, ui_text_dim, x + width);
        message += n; while (*message == ' ') message++;
    }
}
static void draw_namedlg(void) {
    if (!name_dlg)
        return;
    int x, y, w, h;
    namedlg_geom(&x, &y, &w, &h);
    shade_rect(0, MENUBAR_H, fb_w, TASKBAR_Y - MENUBAR_H, 1);
    draw_shadow(x, y, w, h);
    draw_round_rect(x - 1, y - 1, w + 2, h + 2, 11, ui_border);
    draw_round_rect(x, y, w, h, 10, INK(1));
    int pdf = name_target == 6;
    draw_string_bold(pdf ? "Export PDF pages" : name_target == 5 ? "Export values (CSV)" :
                     name_target == 4 ? "Save spreadsheet as" :
                     name_target == 3 ? "Export rich text (RTF)" :
                     name_target == 1 ? "Save picture as" : "Save document as",
                     x + 22, y + 20, ui_text);
    char location[FS_NAME_LEN + 16];
    kstrcpy(location, "Folder: ");
    kstrcpy(location + 8, fs_is_dir(fm_cwd) && fm_cwd != fs_root() ? fs_name(fm_cwd) : "/");
    const char *message = name_failed ?
        ((name_target == 4 || name_target == 5) ? spreadsheet_status() : name_target >= 2 ? writer_status() : name_failure_message ? name_failure_message :
         "Save failed. Check storage and file name.") :
        name_target == 5 ? "Other apps may run formula-like CSV text." :
        name_target == 1 ? "Saved to the Pictures folder" : location;
    if (pdf) namedlg_pdf_message(message, x + 22, y + 44, w - 44);
    else draw_string_clip(message, x + 22, y + 20 + CHAR_H + 6, ui_text_dim, x + w - 22);
    int fx = x + 22, fy = y + (pdf ? 108 : 64), fw = w - 44, fh = 34;
    draw_round_rect(fx, fy, fw, fh, 7, !pdf || !name_focus ? ui_accent : ui_border);
    draw_round_rect(fx + 1, fy + 1, fw - 2, fh - 2, 6, ui_field);
    name_buf[name_len] = 0;
    draw_string(name_buf, fx + 12, fy + (fh - CHAR_H) / 2, ui_text);
    int cx2 = fx + 12 + ui_string_w(name_buf);
    if ((!pdf || !name_focus) && ((frame_count / 35) & 1))
        draw_rect(cx2 + 1, fy + 8, 2, fh - 16, ui_accent);
    if (pdf) {
        draw_string("Paper:", x + 22, y + 158, ui_text);
        const char *labels[] = {"Letter (US)", "A4"};
        for (int i = 0; i < 2; i++) {
            int bx = x + 88 + i * 156;
            if (name_pdf_paper == (unsigned)i) draw_default_button(bx, y + 152, 148, BTN_H, labels[i]);
            else draw_button(bx, y + 152, 148, BTN_H, labels[i]);
            if (name_focus == i + 1) draw_frame(bx - 3, y + 149, 154, BTN_H + 6, ui_accent);
        }
        draw_string("Tab: focus   Arrows/Space: paper   Esc: cancel", x + 22, y + 186, ui_text_dim);
    }
    int sx, cx, by, bw;
    namedlg_buttons(x, y, w, h, &sx, &cx, &by, &bw);
    draw_button(cx, by, bw, BTN_H, "Cancel");
    draw_default_button(sx, by, bw, BTN_H, pdf ? "Export" : "Save");
    if (name_focus >= (pdf ? 3 : 1)) draw_frame((name_focus==(pdf?3:1)?cx:sx)-3,by-3,bw+6,BTN_H+6,ui_accent);
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
    if (name_target == 6) {
        for (int i = 0; i < 2; i++) if (hit(mouse_x, mouse_y, x + 88 + i * 156, y + 152, 148, BTN_H)) {
            name_pdf_paper = (unsigned)i; name_focus = i + 1; name_failed = 0; dirty = 1; return 1;
        }
        if (hit(mouse_x, mouse_y, x + 22, y + 108, w - 44, 34)) { name_focus = 0; dirty = 1; return 1; }
    }
    if (hit(mouse_x, mouse_y, sx, by, bw, BTN_H))
        namedlg_commit();
    else if (hit(mouse_x, mouse_y, cx, by, bw, BTN_H))
        namedlg_close();
    return 1;
}

static void namedlg_key(void) {
    int pdf = name_target == 6, stops = pdf ? 5 : 3;
    if (key_sc == KEY_TAB) { name_focus=(name_focus+(shift_down?stops-1:1))%stops;dirty=1;return; }
    if (pdf && (name_focus == 1 || name_focus == 2)) {
        if (key_sc == KEY_LEFT || key_sc == KEY_RIGHT || key_sc == KEY_SPACE || key_sc == KEY_ENTER) {
            if (key_sc == KEY_LEFT || key_sc == KEY_RIGHT) name_focus = 3 - name_focus;
            name_pdf_paper = (unsigned)(name_focus - 1); name_failed = 0; dirty = 1; return;
        }
    }
    if (key_sc == KEY_ENTER && name_focus==(pdf?3:1)) { namedlg_close();return; }
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

/* ---------- Window switcher ----------
 * Alt+Tab shows every open window, most recent first, centered on screen.
 * Windows keep their state until Alt is released; Esc cancels. */

static int switcher_ids[MAX_WIN], switcher_n, switcher_sel;

static void switcher_build(void) {
    switcher_n = 0;
    for (int i = 0; i < MAX_WIN; i++)
        if (wins[i].open) switcher_ids[switcher_n++] = i;
    /* Most recent first; minimized windows keep their recency. */
    for (int a = 0; a < switcher_n; a++)
        for (int b = a + 1; b < switcher_n; b++)
            if (wins[switcher_ids[b]].z > wins[switcher_ids[a]].z) {
                int t = switcher_ids[a];
                switcher_ids[a] = switcher_ids[b];
                switcher_ids[b] = t;
            }
}

static int switcher_cell_w(void) { return MODERN ? 104 : 96; }
static int switcher_cell_h(void) { return MODERN ? 96 : 88; }
static int switcher_cols(void) {
    int cw = switcher_cell_w(), cols = (fb_w - 64 + 8) / (cw + 8);
    if (cols < 1) cols = 1;
    return cols < switcher_n ? cols : switcher_n;
}

static void switcher_geom(int *x, int *y, int *w, int *h) {
    int cols = switcher_cols(), rows = (switcher_n + cols - 1) / cols;
    *w = 32 + cols * switcher_cell_w() + (cols - 1) * 8;
    *h = 16 + rows * switcher_cell_h() + (rows - 1) * 8 + (MODERN ? 52 : 60);
    *x = (fb_w - *w) / 2;
    *y = (fb_h - *h) / 2;
}

static void switcher_cell(int k, int *cx, int *cy) {
    int x, y, w, h, cols = switcher_cols();
    switcher_geom(&x, &y, &w, &h);
    *cx = x + 16 + (k % cols) * (switcher_cell_w() + 8);
    *cy = y + 16 + (k / cols) * (switcher_cell_h() + 8);
}

static void switcher_open(int dir) {
    switcher_build();
    if (switcher_n == 0) return;
    if (open_menu >= 0) menu_close();
    switcher_on = 1;
    /* The first step goes to the previous window, not the current one. */
    int f = win_front();
    switcher_sel = (f >= 0 && switcher_ids[0] == f) ? (dir > 0 ? 1 : switcher_n - 1) : 0;
    if (switcher_sel >= switcher_n) switcher_sel = 0;
    dirty = 1;
}

static void switcher_step(int dir) {
    if (!switcher_n) return;
    switcher_sel = (switcher_sel + dir + switcher_n) % switcher_n;
    dirty = 1;
}

static void switcher_commit(void) {
    if (!switcher_on) return;
    switcher_on = 0;
    if (switcher_sel >= 0 && switcher_sel < switcher_n) win_focus(switcher_ids[switcher_sel]);
    dirty = 1;
}

static void switcher_cancel(void) {
    switcher_on = 0;
    dirty = 1;
}

static void switcher_click(void) {
    for (int k = 0; k < switcher_n; k++) {
        int cx, cy;
        switcher_cell(k, &cx, &cy);
        if (hit(mouse_x, mouse_y, cx, cy, switcher_cell_w(), switcher_cell_h())) {
            switcher_sel = k;
            switcher_commit();
            return;
        }
    }
    switcher_cancel();
}

/* "Editor, in Documents" under the cells: the app and, for a file view,
 * the folder it shows. */
static void switcher_detail(int slot, char *out, int cap) {
    const char *app = win_app_name(wins[slot].kind);
    int n = 0;
    while (*app && n < cap - 1) out[n++] = *app++;
    const char *where = 0;
    if (wins[slot].kind == WK_FILES) {
        int cwd = window_state[slot].cwd;
        where = fs_is_dir(cwd) ? (cwd == fs_root() ? "Home" : fs_name(cwd)) : 0;
    }
    if (wins[slot].min) where = 0;
    if (where) {
        const char *sep = ", in ";
        while (*sep && n < cap - 1) out[n++] = *sep++;
        while (*where && n < cap - 1) out[n++] = *where++;
    }
    if (wins[slot].min) {
        const char *m = ", minimized";
        while (*m && n < cap - 1) out[n++] = *m++;
    }
    out[n] = 0;
}

static void draw_switcher(void) {
    if (!switcher_on || !switcher_n) return;
    int x, y, w, h, cw = switcher_cell_w(), ch = switcher_cell_h();
    switcher_geom(&x, &y, &w, &h);
    if (MODERN) {
        kit_shadow(x, y, w, h, 12);
        kit_box(x, y, w, h, 12, TK_46, TK_LINE_STRONG);
    } else {
        kit_shadow(x, y, w, h, 0);
        kit_box(x, y, w, h, KIT_R_SQUARE, TK_RAISED, TK_LINE_STRONG);
    }
    char title[TERM_TASK_TITLE_LEN];
    for (int k = 0; k < switcher_n; k++) {
        int i = switcher_ids[k], cx, cy, on = k == switcher_sel;
        switcher_cell(k, &cx, &cy);
        if (on) {
            if (MODERN) {
                kit_fill(cx, cy, cw, ch, 8, TK_ACCENT_100, 1);
                kit_frame(cx, cy, cw, ch, 8, TK_ACCENT);
                kit_frame(cx + 1, cy + 1, cw - 2, ch - 2, 7, TK_ACCENT);
            } else {
                draw_rect(cx, cy, cw, ch, TK_ACCENT_100);
                draw_frame(cx, cy, cw, ch, TK_ACCENT);
                draw_frame(cx + 1, cy + 1, cw - 2, ch - 2, TK_ACCENT);
            }
        }
        int top = MODERN ? 16 : 14;
        app_icon_draw(app_icon_for(kind_icon(wins[i].kind)), cx + (cw - 32) / 2, cy + top, AI_BIG);
        const char *t = win_display_title(i, title);
        uint8_t ink = on ? (MODERN ? TK_ACCENT_700 : TK_TEXT) : wins[i].min ? TK_TEXT2 : TK_TEXT;
        int tw = kit_text_w(t, on), maxw = cw - 12;
        if (tw > maxw) tw = maxw;
        kit_text(t, cx + (cw - tw) / 2, cy + top + 32 + (MODERN ? 10 : 8), ink, maxw, on);
    }
    int sel = switcher_ids[switcher_sel];
    char detail[64];
    switcher_detail(sel, detail, sizeof detail);
    const char *t = win_display_title(sel, title);
    int ty = y + h - (MODERN ? 36 : 40) + (MODERN ? 10 : 12);
    int tx = x + (MODERN ? 18 : 16);
    int used = kit_text(t, tx, ty, TK_TEXT, w / 2, 1);
    kit_text(detail, tx + used + 6, ty, TK_TEXT2, x + w - 16 - (tx + used + 6), 0);
}

/* The top window under the pointer, or -1. */
static int win_at(int px, int py) {
    int order[MAX_WIN], n = 0;
    wins_by_z(order, &n, 1);
    for (int k = 0; k < n; k++) {
        int i = order[k];
        if (i >= 0 && i < MAX_WIN && hit(px, py, wins[i].x, wins[i].y, wins[i].w, wins[i].h)) return i;
    }
    return -1;
}

/* Caption buttons react to the pointer only when nothing modal is up and
 * the window being drawn is the one under the pointer. */
static int caption_pointer_ok(void) {
    if (open_menu >= 0 || open_dlg || name_dlg || edit_close_dlg || launcher_on || switcher_on ||
        display_pending || dragging_win >= 0 || resizing_win >= 0)
        return 0;
    return chrome_slot >= 0 && win_at(mouse_x, mouse_y) == chrome_slot;
}

static int desk_at(int px, int py) {
    for (int i = 0; i < DESK_COUNT; i++)
        if (icon_hit(i, px, py)) return i;
    return -1;
}

static void desk_open(int i) {
    if (i == DESK_SYSTEM) { properties_id = fs_root(); properties_reason = ""; win_open(WK_PROPERTIES); }
    else if (i == DESK_HOME) open_files(fs_root());
    else if (i == DESK_TRASH) open_files(trash_id >= 0 ? trash_id : fs_root());
    dirty = 1;
}

static const MItem mi_desk_icon[] = {
    {"&Open", "Enter", ACT_DESK_OPEN, 0}, MSEP, {"Propert&ies", "Alt+Enter", ACT_DESK_PROPERTIES, 0},
};
static const MItem mi_trash_icon[] = {
    {"&Open", "Enter", ACT_DESK_OPEN, 0}, MSEP, {"&Empty Trash", "", ACT_EMPTY_TRASH, 0},
};

static void handle_rclick(void) {
    if (display_pending || name_dlg || edit_close_dlg || open_dlg || launcher_on || switcher_on)
        return;
    if (open_menu >= 0) menu_close();
    volume_on = 0;
    if (taskbar_rclick()) return;
    int i = win_at(mouse_x, mouse_y);
    if (i >= 0) {
        Win *w = &wins[i];
        win_focus(i);
        context_set(i);
        if (mouse_y < w->y + TITLE_BAR) {
            menu_open_context(mi_window, (int)(sizeof mi_window / sizeof mi_window[0]), mouse_x, mouse_y);
            return;
        }
        if (w->kind == WK_MINES) {
            if (mines_click(w->x, w->y + TITLE_H + 1, mouse_x, mouse_y, 1)) dirty = 1;
            return;
        }
        if (w->kind == WK_FILES && mouse_y >= w->y + TITLE_H) {
            int row = files_row_at(w->x, w->y, w->w, w->h);
            if (row >= 0 && fm_row_id(row) >= 0) {
                fm_rename_cancel();
                fm_selected = row;
                menu_open_context(mi_files_context, (int)(sizeof mi_files_context / sizeof mi_files_context[0]), mouse_x, mouse_y);
            } else if (row == -1) {
                menu_open_context(mi_folder_context, (int)(sizeof mi_folder_context / sizeof mi_folder_context[0]), mouse_x, mouse_y);
            }
            return;
        }
        if (w->kind == WK_EDIT && mouse_y >= w->y + TITLE_H) {
            menu_open_context(mi_editor_edit, (int)(sizeof mi_editor_edit / sizeof mi_editor_edit[0]), mouse_x, mouse_y);
            return;
        }
        return;
    }
    int d = desk_at(mouse_x, mouse_y);
    if (d >= 0) {
        icon_sel = d;
        if (d == DESK_TRASH) menu_open_context(mi_trash_icon, (int)(sizeof mi_trash_icon / sizeof mi_trash_icon[0]), mouse_x, mouse_y);
        else menu_open_context(mi_desk_icon, (int)(sizeof mi_desk_icon / sizeof mi_desk_icon[0]), mouse_x, mouse_y);
        return;
    }
    icon_sel = -1;
    menu_open_context(mi_desktop_context, (int)(sizeof mi_desktop_context / sizeof mi_desktop_context[0]), mouse_x, mouse_y);
}

/* Press on a window's title bar: drag, or double-click to maximize. A
 * snapped window comes off its snap once the drag starts. */
static void title_press(int i) {
    Win *w = &wins[i];
    if ((win_options & WINOPT_DOUBLE_CLICK) && title_click_window == i &&
        input_gesture_ticks() - title_click_time < dbl_click_ticks()) {
        title_click_window = -1;
        win_arrange(i, w->maximized == SNAP_MAX ? 3 : 0);
        return;
    }
    title_click_window = i;
    title_click_time = input_gesture_ticks();
    dragging_win = i;
    drag_active = 0;
    drag_from_x = mouse_x;
    drag_from_y = mouse_y;
    drag_orig_x = w->x;
    drag_orig_y = w->y;
    drag_outline_x = w->x;
    drag_outline_y = w->y;
    fm_dragging = 0;
    fm_drag_active = 0;
}

static void window_body_click(int i) {
    Win *w = &wins[i];
    if (w->kind == WK_NATIVE) {
        if (hit(mouse_x, mouse_y, w->x + 8, w->y + TITLE_H + 4, 100, NATIVE_TOOLBAR_H - 6)) native_output_toggle(i);
    } else if (w->kind == WK_FILES)
        handle_files_click(w->x, w->y, w->w, w->h);
    else if (w->kind == WK_EDIT)
        handle_edit_click(w->x, w->y, w->w, w->h);
    else if (w->kind == WK_CALC)
        handle_calc_click(w->x, w->y, w->w, w->h);
    else if (w->kind == WK_SETTINGS)
        handle_settings_click(w->x, w->y, w->w, w->h);
    else if (w->kind == WK_ABOUT)
        handle_about_click(w->x, w->y, w->w, w->h);
    else if (w->kind == WK_PAINT)
        handle_paint_click(w->x, w->y, w->w, w->h);
    else if (w->kind == WK_WORDLE) {
        if (wordle_click(w->x, w->y + TITLE_H, mouse_x, mouse_y)) dirty = 1;
    } else if (w->kind == WK_VIEW) {
        if (image_viewer_click(w->x + 1, w->y + TITLE_H + 1, w->w - 2, w->h - TITLE_H - 2, mouse_x, mouse_y)) dirty = 1;
    } else if (w->kind == WK_BROWSER) {
        if (browser_click(w->x + 1, w->y + TITLE_H + 1, w->w - 2, w->h - TITLE_H - 2, mouse_x, mouse_y)) dirty = 1;
    } else if (w->kind == WK_SPREADSHEET) {
        spreadsheet_result(spreadsheet_click(w->x + 1, w->y + TITLE_H + 1, w->w - 2, w->h - TITLE_H - 2,
            mouse_x, mouse_y, (ctrl_down ? SPREADSHEET_MOD_CTRL : 0) | (shift_down ? SPREADSHEET_MOD_SHIFT : 0)));
    } else if (w->kind == WK_WRITER) {
        writer_result(writer_click(w->x + 1, w->y + TITLE_H + 1, w->w - 2, w->h - TITLE_H - 2,
            mouse_x, mouse_y, (ctrl_down ? WRITER_MOD_CTRL : 0) | (shift_down ? WRITER_MOD_SHIFT : 0)));
    } else if (w->kind == WK_PLAYER) {
        if (player_click(w->x + 1, w->y + TITLE_H + 1, w->w - 2, w->h - TITLE_H - 2, mouse_x, mouse_y)) dirty = 1;
    } else if (w->kind == WK_TODO)
        handle_todo_click(w->x, w->y, w->w, w->h);
    else if (w->kind == WK_CAL) {
        if (cal_click(w->x, w->y + TITLE_H + 1, w->w, mouse_x, mouse_y)) dirty = 1;
    } else if (w->kind == WK_MINES) {
        if (mines_click(w->x, w->y + TITLE_H + 1, mouse_x, mouse_y, shift_down)) dirty = 1;
    } else if (w->kind == WK_BREAKOUT) {
        bo_click();
        dirty = 1;
    } else if (w->kind == WK_SYSMON) {
        SysInfo si;
        sysinfo_fill(&si);
        SysmonAction action = sysmon_click(w->x, w->y + TITLE_H + 1, w->w, w->h - TITLE_H - 1, mouse_x, mouse_y, &si);
        if (action.kind == SYSMON_ACTION_REDRAW) dirty = 1;
        else if (action.kind == SYSMON_ACTION_CLOSE_WINDOW) {
            int owner = action.owner;
            if (owner >= 0 && owner < MAX_WIN && wins[owner].open &&
                (unsigned)wins[owner].seq == action.task_instance) win_request_close(owner);
        } else if (action.kind == SYSMON_ACTION_STOP_PROGRAM) {
            /* Its Terminal reports the exit status on its next poll. */
            uproc_kill(action.task_instance);
            dirty = 1;
        } else if (action.kind == SYSMON_ACTION_SHOW_TERMINAL || action.kind == SYSMON_ACTION_STOP_TASK) {
            int owner = action.owner;
            TermTaskInfo live;
            if (owner >= 0 && owner < MAX_WIN && wins[owner].open &&
                (wins[owner].kind == WK_TERM || wins[owner].kind == WK_NATIVE) &&
                app_view_info(owner, &live) && live.instance == action.task_instance) {
                if (action.kind == SYSMON_ACTION_SHOW_TERMINAL) win_focus(owner);
                else { app_view_stop(owner); dirty = 1; }
            }
        }
    }
}

static void handle_click(void) {
    alt_tap = 0;
    if (display_pending) { display_click(); return; }
    if (edit_close_dlg) { edit_close_click(); return; }
    if (name_dlg) { namedlg_click(); return; }
    if (switcher_on) { switcher_click(); return; }
    if (volume_on && volume_click()) return;
    if (launcher_on) {
        /* The Apps button toggles; anything else goes to the launcher. */
        if (taskbar_hover_at() == HOT_APPS) { launcher_close(); return; }
        launcher_click();
        return;
    }
    /* Keep rename if the click is in the Files body; handle_files_click
     * decides. Clicking chrome/menus/desktop cancels (Return commits). */
    if (fm_renaming && !click_on_rename_field()) {
        int wx, wy, ww, wh;
        if (!(win_geom_kind(WK_FILES, &wx, &wy, &ww, &wh) &&
              hit(mouse_x, mouse_y, wx, wy + TITLE_H, ww, wh - TITLE_H)))
            fm_rename_cancel();
    }
    if (win_kbd_mode) { win_kbd_mode = 0; win_kbd_slot = -1; dirty = 1; }
    if (open_menu >= 0) { menu_click(); return; }
    if (open_dlg) { handle_open_dlg_click(); return; }
    if (taskbar_hit()) return;
    volume_on = 0;

    int i = win_at(mouse_x, mouse_y);
    if (i >= 0) {
        Win *w = &wins[i];
        win_focus(i);
        context_set(i);
        int edges = win_resize_edges(w, mouse_x, mouse_y);
        if (edges) {
            resizing_win = i;
            resize_edges = edges;
            drag_from_x = mouse_x;
            drag_from_y = mouse_y;
            drag_orig_x = w->x;
            drag_orig_y = w->y;
            resize_start_w = w->w;
            resize_start_h = w->h;
            return;
        }
        if (close_hit(w->x, w->y, w->w)) { win_request_close(i); return; }
        if (!win_is_dialog(w->kind)) {
            if (max_hit(w->x, w->y, w->w)) { win_arrange(i, w->maximized ? 3 : 0); return; }
            if (min_hit(w->x, w->y, w->w)) { win_minimize(i); return; }
        }
        if (!MODERN && WMENU_H > 0 && win_has_menu(w->kind) &&
            hit(mouse_x, mouse_y, w->x, w->y + TITLE_BAR, w->w, WMENU_H)) {
            int m = win_menu_at(mouse_x, mouse_y);
            if (m >= 0) { fm_last_click_item = -1; menu_open_bar(m); }
            return;
        }
        if (mouse_y < w->y + TITLE_BAR) {
            /* The window icon opens the window menu. */
            int ix = w->x + (MODERN ? 8 : 4);
            if (hit(mouse_x, mouse_y, ix, w->y + 4, 24, TITLE_BAR - 8)) { menu_open_window(i); return; }
            title_press(i);
            return;
        }
        window_body_click(i);
        return;
    }

    int d = desk_at(mouse_x, mouse_y);
    if (d >= 0) {
        int dbl = double_click(d, &icon_last, &icon_last_frame);
        icon_sel = d;
        dirty = 1;
        if (dbl) desk_open(d);
    } else if (icon_sel >= 0) {
        icon_sel = -1;
        dirty = 1;
    }
}

static void handle_wheel(int amount) {
    if(amount&&launcher_on&&!name_dlg&&!open_dlg&&!display_pending){launcher_scroll(amount);return;}
    if(!amount||name_dlg||edit_close_dlg||open_dlg||launcher_on||open_menu>=0||display_pending)return;
    int target=-1;
    for(int i=0;i<MAX_WIN;i++)if(wins[i].open&&!wins[i].min&&
       hit(mouse_x,mouse_y,wins[i].x,wins[i].y+TITLE_H,wins[i].w,wins[i].h-TITLE_H)&&
       (target<0||wins[i].z>wins[target].z))target=i;
    if(target<0)return;
    int original=win_front();context_set(target);Win *w=&wins[target];
    if(w->kind==WK_SPREADSHEET)spreadsheet_scroll(amount*3);
    else if(w->kind==WK_WRITER)writer_scroll(amount*3);
    else if(w->kind==WK_BROWSER)browser_scroll(amount*3);
    else if(w->kind==WK_VIEW)image_viewer_scroll(amount*3);
    else if(w->kind==WK_TERM)term_scroll(-amount*3);
    else if(w->kind==WK_NATIVE)native_output_scroll_by(target,-amount*3);
    else if(w->kind==WK_EDIT){
        int ax,ay,aw,ah,row,col,old=edit_caret;edit_area(w->x,w->y,w->w,w->h,&ax,&ay,&aw,&ah);
        int cols=aw/EDIT_CHAR_W;if(cols<1)cols=1;edit_caret=edit_len;edit_caret_cell(cols,&row,&col);edit_caret=old;
        int max=row+1-ah/EDIT_LINE_H;if(max<0)max=0;
        edit_scroll+=amount*3;if(edit_scroll<0)edit_scroll=0;if(edit_scroll>max)edit_scroll=max;edit_manual_scroll=1;
    }else if(w->kind==WK_FILES){fm_first+=amount*3;fm_manual_scroll=1;files_scroll(files_rows(w->y,w->h));}
    context_set(original);dirty=1;
}

static void launcher_key(void);

/* Shared legacy byte-key policy for both process-backed desktop views. Shell
 * accelerators/modal routing run first; GUI Escape belongs to the app. */
static int native_task_key_dispatch(int slot,int owned){
    int running=app_view_running(slot);
    /* ELF programs take no keyboard input yet, but Ctrl+C stops them. */
    int elf=!owned&&term_elf_running(slot);
    if(!running&&!owned&&!elf)return 0;
    if(ctrl_down&&key_sc==0x2e){if(running||elf)term_task_stop(slot);dirty=1;return 1;}
    if(key_sc==0x49||key_sc==0x51){
        if(owned)native_output_scroll_by(slot,key_sc==0x49?8:-8);
        else {term_scroll(key_sc==0x49?8:-8);dirty=1;}
        return 1;
    }
    if(running){
        int input=key_sc==KEY_ENTER?13:key_sc==KEY_BACKSPACE?8:key_sc==KEY_ESC?27:
                  (!ctrl_down&&!alt_down?(unsigned char)key_char:0);
        if(input)app_view_key(slot,input);
    }
    return 1;
}

/* Super combinations: window states, Show Desktop and Files. Super alone
 * toggles the launcher on release. Returns 1 when used. */
static int super_key(void) {
    if (!super_down) return 0;
    super_used = 1;
    int f = win_front();
    if (key_sc == KEY_UP && f >= 0 && !win_is_dialog(wins[f].kind)) win_arrange(f, 0);
    else if (key_sc == KEY_DOWN && f >= 0) { if (wins[f].maximized) win_arrange(f, 3); else win_minimize(f); }
    else if ((key_sc == KEY_LEFT || key_sc == KEY_RIGHT) && f >= 0 && !win_is_dialog(wins[f].kind))
        win_arrange(f, key_sc == KEY_LEFT ? 1 : 2);
    else if (key_sc == 0x20) show_desktop();
    else if (key_sc == 0x12) open_files(fs_root());
    else return 0;
    dirty = 1;
    return 1;
}

/* Alt+Space Move and Size: arrows step 8px (1px with Shift); Enter or a
 * click ends, Esc puts the window back. Returns 1 when used. */
static int win_kbd_orig_x, win_kbd_orig_y, win_kbd_orig_w, win_kbd_orig_h;
static void win_kbd_begin(void) {
    int s = win_kbd_slot;
    if (s < 0) return;
    win_kbd_orig_x = wins[s].x; win_kbd_orig_y = wins[s].y;
    win_kbd_orig_w = wins[s].w; win_kbd_orig_h = wins[s].h;
}
static int win_kbd_key(void) {
    if (!win_kbd_mode) return 0;
    int s = win_kbd_slot;
    if (s < 0 || s >= MAX_WIN || !wins[s].open) { win_kbd_mode = 0; return 0; }
    Win *w = &wins[s];
    int step = shift_down ? 1 : 8, dx = 0, dy = 0;
    if (key_sc == KEY_LEFT) dx = -step;
    else if (key_sc == KEY_RIGHT) dx = step;
    else if (key_sc == KEY_UP) dy = -step;
    else if (key_sc == KEY_DOWN) dy = step;
    else if (key_sc == KEY_ESC) {
        w->x = win_kbd_orig_x; w->y = win_kbd_orig_y; w->w = win_kbd_orig_w; w->h = win_kbd_orig_h;
        win_kbd_mode = 0;
    } else if (key_sc == KEY_ENTER) win_kbd_mode = 0;
    if (win_kbd_mode == 1) { w->x += dx; w->y += dy; }
    else if (win_kbd_mode == 2) {
        int mw, mh;
        w->w += dx; w->h += dy;
        win_minimum(w, &mw, &mh);
        if (w->w < mw) w->w = mw;
        if (w->h < mh) w->h = mh;
    }
    win_clamp(w);
    dirty = 1;
    return 1;
}

static void handle_key(void) {
    if (display_pending) {
        if (key_sc == KEY_ENTER) display_keep();
        else if (key_sc == KEY_ESC) display_revert();
        return;
    }
    if (edit_close_dlg) { edit_close_key(); return; }
    if (switcher_on) {
        if (key_sc == KEY_TAB) switcher_step(shift_down ? -1 : 1);
        else if (key_sc == KEY_LEFT || key_sc == KEY_RIGHT) switcher_step(key_sc == KEY_LEFT ? -1 : 1);
        else if (key_sc == KEY_ESC) switcher_cancel();
        else if (key_sc == KEY_ENTER) switcher_commit();
        return;
    }
    if (!name_dlg && !open_dlg && !launcher_on && super_key()) return;
    if (win_kbd_key()) return;
    context_set(win_front());
    if (!name_dlg && !open_dlg && !launcher_on) {
        int f = win_front();
        if (alt_down && key_sc == KEY_TAB) { switcher_open(shift_down ? -1 : 1); return; }
        if (alt_down && key_sc == 0x3e) { if (f >= 0) win_request_close(f); return; }
        if (alt_down && key_sc == KEY_SPACE) { if (f >= 0) menu_open_window(f); return; }
        if (open_menu >= 0 && menu_key()) return;
        if (key_sc == 0x44) {
            if (shift_down && f >= 0) {
                Win *w = &wins[f];
                int n, cx = w->x + 24, cy = w->y + TITLE_H + 24;
                const MMenu *bar = menubar_for(w->kind, &n);
                if (w->kind == WK_FILES)
                    menu_open_context(mi_files_context, (int)(sizeof mi_files_context / sizeof mi_files_context[0]), cx, cy);
                else if (n > 1)
                    menu_open_context(bar[1].items, bar[1].n, cx, cy);
            } else menu_focus_row();
            return;
        }
        if (menu_mnemonic_open()) return;
    }
    if(front_kind()==WK_EDIT)edit_manual_scroll=0;
    if(front_kind()==WK_FILES)fm_manual_scroll=0;
    if(front_kind()==WK_FILES&&!name_dlg&&!open_dlg&&!launcher_on&&open_menu<0){
        if(ctrl_down&&key_sc==0x21){fm_filter_show();return;}
        if(ctrl_down&&key_sc>=0x02&&key_sc<=0x05){fm_sort_by(key_sc-0x02);return;}
        if(!fm_renaming&&fm_filter_key())return;
    }
    if(front_kind()==WK_EDIT&&!name_dlg&&!open_dlg&&!launcher_on&&open_menu<0){
        if(ctrl_down&&(key_sc==0x21||key_sc==0x23)){edit_search_open(key_sc==0x23);return;}
        if(key_sc==0x3d){if(!edit_search.open)edit_search_open(0);edit_find_next(shift_down?-1:1);return;}
        int app_shortcut=ctrl_down&&(key_sc==KEY_S||key_sc==KEY_W||key_sc==KEY_M||key_sc==KEY_N||
                        key_sc==KEY_SPACE||key_sc==KEY_TAB||key_sc==0x18||key_sc==0x2c||key_sc==0x15);
        if(edit_search.open&&(edit_search.focus||key_sc==KEY_ESC)&&!app_shortcut){edit_search_key();return;}
    }
    if(front_kind()==WK_BROWSER&&!name_dlg&&!open_dlg&&!launcher_on&&open_menu<0&&
       ((ctrl_down&&(key_sc==0x26||key_sc==0x13||key_sc==0x1e||key_sc==KEY_S))||
        (alt_down&&(key_sc==KEY_LEFT||key_sc==KEY_RIGHT)))){
        if(browser_key(key_sc,key_char,(ctrl_down?BROWSER_MOD_CTRL:0)|
           (shift_down?BROWSER_MOD_SHIFT:0)|(alt_down?BROWSER_MOD_ALT:0)))dirty=1;
        return;
    }
    if (!name_dlg && !open_dlg && alt_down && key_sc == KEY_ENTER) {
        int f = win_front();
        if (front_kind() == WK_FILES) act_run(ACT_PROPERTIES);
        else if (f >= 0 && !win_is_dialog(wins[f].kind)) win_arrange(f, wins[f].maximized == SNAP_MAX ? 3 : 0);
        return;
    }
    if (!name_dlg && !open_dlg && alt_down && front_kind() == WK_FILES &&
        (key_sc == KEY_LEFT || key_sc == KEY_RIGHT || key_sc == KEY_UP)) {
        act_run(key_sc == KEY_LEFT ? ACT_GO_BACK : key_sc == KEY_RIGHT ? ACT_GO_FORWARD : ACT_GO_UP);
        return;
    }
    if (!name_dlg && !open_dlg && alt_down && (key_sc == KEY_LEFT || key_sc == KEY_RIGHT)) {
        int f = win_front();
        if (f >= 0 && !win_is_dialog(wins[f].kind)) win_arrange(f, key_sc == KEY_LEFT ? 1 : 2);
        return;
    }
    if (!name_dlg && !open_dlg && key_sc == 0x3b) { act_run(ACT_HELP); return; }
    if (name_dlg) {
        namedlg_key();
        return;
    }
    if (launcher_on) {
        launcher_key();
        return;
    }
    if (front_kind()==WK_SPREADSHEET && !open_dlg && open_menu<0 &&
        !(alt_down || (ctrl_down && (key_sc==KEY_SPACE || key_sc==KEY_TAB ||
          key_sc==KEY_M || key_sc==KEY_W || key_sc==KEY_N || key_sc==0x18)) || key_sc==0x44)) {
        spreadsheet_result(spreadsheet_key(key_sc,key_char,(ctrl_down?SPREADSHEET_MOD_CTRL:0)|
            (shift_down?SPREADSHEET_MOD_SHIFT:0)));
        return;
    }
    if (front_kind()==WK_WRITER && !open_dlg && open_menu<0 &&
        !(alt_down || (ctrl_down && (key_sc==KEY_SPACE || key_sc==KEY_TAB ||
          key_sc==KEY_M || key_sc==KEY_W || key_sc==KEY_N || key_sc==0x18)) || key_sc==0x44)) {
        writer_result(writer_key(key_sc,key_char,(ctrl_down?WRITER_MOD_CTRL:0)|
            (shift_down?WRITER_MOD_SHIFT:0)));
        return;
    }
    if (ctrl_down && !open_dlg) {
        if(front_kind()==WK_FILES&&(key_sc==0x2d||key_sc==0x2e||key_sc==0x2f)){
            files_clipboard_action(key_sc==0x2d?0:key_sc==0x2e?1:2);return;
        }
        if(key_sc==0x17 && front_kind()==WK_FILES){menu_activate(MENU_FILE,6);return;}
        if(key_sc==0x20 && front_kind()==WK_FILES){act_run(ACT_DUPLICATE);return;}
        if(key_sc==KEY_N && shift_down && front_kind()==WK_FILES){act_run(ACT_NEW_FOLDER);return;}
        if(key_sc==KEY_S && shift_down && act_enabled(ACT_SAVE_AS)){act_run(ACT_SAVE_AS);return;}
        if(key_sc==KEY_Q){act_run(ACT_QUIT);return;}
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
        if (key_sc == KEY_S && (front_kind() == WK_PAINT || front_kind() == WK_EDIT || front_kind() == WK_WRITER || front_kind() == WK_SPREADSHEET || front_kind() == WK_TODO || front_kind() == WK_CAL)) {
            menu_activate(MENU_FILE, 4);
            return;
        }
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
            icon_sel=(icon_sel+1)%DESK_COUNT;
        else if(key_sc==KEY_LEFT || key_sc==KEY_UP)icon_sel=(icon_sel+DESK_COUNT-1)%DESK_COUNT;
        else if(key_sc==KEY_ENTER && icon_sel>=0)desk_open(icon_sel);
        dirty=1;return;
    }

    if(fk==WK_VIEW){
        if(key_sc==KEY_ESC)close_front();
        else if(image_viewer_key(key_sc,key_char))dirty=1;
        return;
    }
    if(fk==WK_BROWSER){
        if(browser_key(key_sc,key_char,(ctrl_down?BROWSER_MOD_CTRL:0)|
           (shift_down?BROWSER_MOD_SHIFT:0)|(alt_down?BROWSER_MOD_ALT:0)))dirty=1;
        return;
    }
    if(fk==WK_PLAYER){
        if(key_sc==KEY_ESC)close_front();
        else if(player_key(key_sc,key_char))dirty=1;
        return;
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
        if (key_sc == 0x3c) { act_run(ACT_RENAME); return; }
        if (key_sc == 0x53) { act_run(ACT_TRASH); return; }
        if (key_sc == 0x3f) { act_run(ACT_REFRESH); return; }
        if (key_sc == KEY_UP) {
            if (vis)
                fm_selected = fm_selected<0?vis-1:(fm_selected - 1 + vis) % vis;
            dirty = 1;
        } else if (key_sc == KEY_DOWN) {
            if (vis)
                fm_selected = fm_selected<0?0:(fm_selected + 1) % vis;
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
        if(key_sc==0x53){edit_record();if(edit_sel_a!=edit_sel_b)edit_delete_sel();else if(edit_caret<edit_len){edit_sel_a=edit_caret;edit_sel_b=edit_caret+1;edit_delete_sel();}dirty=1;return;}
        if(key_sc==0x47||key_sc==0x4f){
            int old=edit_caret;
            if(ctrl_down)edit_caret=key_sc==0x47?0:edit_len;
            else if(key_sc==0x47){while(edit_caret>0&&edit_buf[edit_caret-1]!='\n')edit_caret--;}
            else {while(edit_caret<edit_len&&edit_buf[edit_caret]!='\n')edit_caret++;}
            if(shift_down){if(edit_sel_a==edit_sel_b)edit_sel_a=old;edit_sel_b=edit_caret;}else edit_sel_collapse();dirty=1;return;
        }
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
            int old=edit_caret;
            if (!shift_down && edit_sel_a != edit_sel_b) {
                edit_caret = edit_sel_a < edit_sel_b ? edit_sel_a : edit_sel_b;
            } else if (edit_caret > 0) {
                edit_caret--;
            }
            if(shift_down){if(edit_sel_a==edit_sel_b)edit_sel_a=old;edit_sel_b=edit_caret;}else edit_sel_collapse();
            dirty = 1;
            return;
        }
        if (key_sc == KEY_RIGHT) {
            int old=edit_caret;
            if (!shift_down && edit_sel_a != edit_sel_b) {
                edit_caret = edit_sel_a > edit_sel_b ? edit_sel_a : edit_sel_b;
            } else if (edit_caret < edit_len) {
                edit_caret++;
            }
            if(shift_down){if(edit_sel_a==edit_sel_b)edit_sel_a=old;edit_sel_b=edit_caret;}else edit_sel_collapse();
            dirty = 1;
            return;
        }
        if (key_sc == KEY_UP || key_sc == KEY_DOWN || key_sc==0x49 || key_sc==0x51) {
            int old=edit_caret;
            int wx, wy, ww, wh, ax, ay, aw, ah;
            if (!win_geom_kind(WK_EDIT, &wx, &wy, &ww, &wh))
                return;
            edit_area(wx, wy, ww, wh, &ax, &ay, &aw, &ah);
            int cols = aw / EDIT_CHAR_W;
            if (cols < 1)
                cols = 1;
            int r, c;
            edit_caret_cell(cols, &r, &c);
            if (key_sc == KEY_UP) r--;
            else if(key_sc==KEY_DOWN)r++;
            else {int page=ah/EDIT_LINE_H;if(page<1)page=1;r+=key_sc==0x49?-page:page;}
            if (r < 0)
                r = 0;
            edit_caret = edit_cell_to_index(cols, r, c);
            if(shift_down){if(edit_sel_a==edit_sel_b)edit_sel_a=old;edit_sel_b=edit_caret;}else edit_sel_collapse();
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

    if(fk==WK_SETTINGS){ settings_key(); return; }
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

    if (fk == WK_CAL) {
        int result = cal_key(key_sc, key_char, (ctrl_down ? CAL_MOD_CTRL : 0) |
                             (shift_down ? CAL_MOD_SHIFT : 0) | (alt_down ? CAL_MOD_ALT : 0));
        if (result & CAL_CLOSE) close_front();
        if (result & CAL_CHANGED) dirty = 1;
        return;
    }

    if (fk == WK_2048 || fk == WK_BREAKOUT || fk == WK_MINES ||
        fk == WK_CLOCK || fk == WK_SYSMON) {
        if (key_sc == KEY_ESC) {
            close_front();
            return;
        }
        int changed = 0;
        if (fk == WK_2048)
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
        if(native_task_key_dispatch(context_slot,0))return;
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

    if(fk==WK_NATIVE){
        native_task_key_dispatch(context_slot,1);
        return; /* Escape never falls through to shell Close for a live GUI. */
    }

    if (key_sc == KEY_ESC)
        close_front();
}


/* ---------- Preferences, Settings, logo, boot screen ---------- */

/* Fixed system paths are resolved afresh: cached node IDs can be moved/reused.
 * Only recognized one-byte preferences are replaceable. No pending choice
 * borrows filesystem bytes or changes a leased snapshot. Each preference is
 * one byte, '0' plus its value. */
enum {
    PREF_STYLE, PREF_ACCENT, PREF_DESKTOP, PREF_PATTERN, PREF_WINDOWS, PREF_SNAP,
    PREF_SAVER, PREF_SAVER_DELAY, PREF_DISPLAY, PREF_CLOCK, PREF_MOUSE, PREF_COUNT
};
enum { PREF_IDLE, PREF_QUEUED, PREF_RAM, PREF_SAVED, PREF_WRITE_FAILED, PREF_SYNC_FAILED, PREF_PATH_FAILED, PREF_FULL_FAILED };
enum { PREF_ERR_PATH = -4, PREF_ERR_FULL = -5 };
typedef struct { char choice; int state, file; unsigned identity; } PendingPreference;
static PendingPreference preferences[PREF_COUNT];
static const char *const preference_names[PREF_COUNT] = {
    "style", "accent", "desktop", "pattern", "windows", "snap",
    "saver", "saverdelay", "display", "clock", "mouse"
};
static const int preference_counts[PREF_COUNT] = {
    UI_STYLE_COUNT, UI_ACCENT_N, UI_DESKTOP_N, UI_PATTERN_N, 16, SNAP_DISTANCE_MAX + 1,
    2, SAVER_DELAY_N, DISPLAY_MODE_COUNT, 4, 3
};
static FsSyncTicket preference_sync_ticket;
static int preference_sync_active, preference_retry_requested;
static unsigned preference_retry_after;

static int preference_valid(int which, char value) {
    return value >= '0' && value < '0' + preference_counts[which];
}
static int preference_file(int which) {
    int dir = fs_find_child(fs_root(), "prefs");
    if (!fs_is_dir(dir)) return -1;
    return fs_find_child(dir, preference_names[which]);
}
static int preference_read(int which) {
    int id = preference_file(which);
    if (id < 0 || fs_is_dir(id) || fs_is_app(id) || fs_size(id) != 1) return -1;
    char value = fs_data(id)[0];
    return preference_valid(which, value) ? value - '0' : -1;
}
static void preference_state(int which, int state) {
    if (preferences[which].state != state) { preferences[which].state = state; dirty = 1; }
}
static int preference_write(int which) {
    PendingPreference *p = &preferences[which];
    int dir = fs_find_child(fs_root(), "prefs");
    if (dir >= 0 && !fs_is_dir(dir)) return PREF_ERR_PATH;
    int id = dir < 0 ? -1 : fs_find_child(dir, preference_names[which]);
    if (id >= 0 && (fs_is_dir(id) || fs_is_app(id) || fs_size(id) != 1 ||
                   !preference_valid(which, fs_data(id)[0]))) return PREF_ERR_PATH;
    if (id < 0) {
        /* Preflight before creating even an empty directory/file. */
        unsigned count = (unsigned)fs_node_count() + 1 + (dir < 0);
        if (count > (unsigned)fs_node_limit() ||
            fs_used_bytes() + 1 > fs_capacity_for_nodes(count)) return PREF_ERR_FULL;
        if (dir < 0) dir = fs_mkdir(fs_root(), "prefs");
        if (dir < 0) return dir;
        id = fs_create(dir, preference_names[which]);
        if (id < 0) return id;
        int result = fs_write(id, &p->choice, 1);
        if (result != 1) { (void)fs_delete(id); return result < 0 ? result : -1; }
    } else if (fs_data(id)[0] != p->choice) {
        int result = fs_write(id, &p->choice, 1);
        if (result != 1) return result < 0 ? result : -1;
    }
    prefs_id = dir;
    p->file = id; p->identity = fs_identity(id);
    return 0;
}
static int preference_matches(int which) {
    PendingPreference *p = &preferences[which];
    int id = preference_file(which);
    return id == p->file && fs_identity(id) == p->identity &&
           !fs_is_dir(id) && !fs_is_app(id) && fs_size(id) == 1 && fs_data(id)[0] == p->choice;
}
/* Call after top-level storage progress and before other filesystem mutations.
 * Normal saves share autosync's backoff. Only an explicit Retry requests its
 * own ticket; a retained foreign result is retried at most once per second. */
static void preferences_tick(void) {
    if (preference_sync_active) {
        int result = fs_sync_result(preference_sync_ticket);
        if (result == FS_SYNC_PENDING) return;
        (void)fs_sync_release(preference_sync_ticket);
        preference_sync_active = 0;
    }
    if (fs_sync_busy()) return;
    /* Observe a just-finished clean snapshot before queued writes dirty it. */
    for (int i = 0; i < PREF_COUNT; ++i) {
        int state = preferences[i].state;
        if (state != PREF_RAM && state != PREF_SYNC_FAILED) continue;
        if (!preference_matches(i)) preference_state(i, PREF_WRITE_FAILED);
        else if (fs_storage_status()) preference_state(i, PREF_SYNC_FAILED);
        else if (!fs_needs_sync()) preference_state(i, PREF_SAVED);
    }
    for (int i = 0; i < PREF_COUNT; ++i) if (preferences[i].state == PREF_QUEUED) {
        int result = preference_write(i);
        if (result == FS_ERR_BUSY) return;
        preference_state(i, result == PREF_ERR_PATH ? PREF_PATH_FAILED :
            result == PREF_ERR_FULL ? PREF_FULL_FAILED : result < 0 ? PREF_WRITE_FAILED :
            fs_storage_status() ? PREF_SYNC_FAILED : !fs_needs_sync() ? PREF_SAVED : PREF_RAM);
    }
    if (!preference_retry_requested) return;
    int needs_save = 0;
    for (int i = 0; i < PREF_COUNT; ++i)
        if (preferences[i].state == PREF_RAM || preferences[i].state == PREF_SYNC_FAILED) needs_save = 1;
    if (!needs_save) { preference_retry_requested = 0; return; }
    if ((int32_t)(timer_ticks() - preference_retry_after) < 0) return;
    int result = fs_sync_request(&preference_sync_ticket);
    if (result == FS_ERR_BUSY) { preference_retry_after = timer_ticks() + TIMER_HZ; return; }
    preference_retry_requested = 0;
    if (!result) preference_sync_active = 1;
    else for (int i = 0; i < PREF_COUNT; ++i)
        if (preferences[i].state == PREF_RAM) preference_state(i, PREF_SYNC_FAILED);
}
static void preference_queue(int which, char choice) {
    preferences[which].choice = choice;
    preference_state(which, PREF_QUEUED);
    preferences_tick();
}
static void preferences_retry(void) {
    for (int i = 0; i < PREF_COUNT; ++i)
        if (preferences[i].state == PREF_WRITE_FAILED || preferences[i].state == PREF_SYNC_FAILED ||
            preferences[i].state == PREF_PATH_FAILED || preferences[i].state == PREF_FULL_FAILED)
            preference_state(i, PREF_QUEUED);
    preference_retry_requested = 1;
    preference_retry_after = timer_ticks();
    preferences_tick();
    dirty = 1;
}
/* Shutdown calls this instead of its initial busy join, before session_save
 * and its final fs_sync. Success means ready in RAM, never durable by itself. */
static int preferences_prepare_shutdown(void) {
    preference_retry_requested = 0;
    if (fs_sync_busy() && fs_sync() < 0) { preferences_tick(); return -1; }
    for (int i = 0; i < PREF_COUNT; ++i)
        if (preferences[i].state == PREF_WRITE_FAILED || preferences[i].state == PREF_SYNC_FAILED ||
            preferences[i].state == PREF_PATH_FAILED || preferences[i].state == PREF_FULL_FAILED)
            preference_state(i, PREF_QUEUED);
    preferences_tick();
    for (int i = 0; i < PREF_COUNT; ++i) {
        int state = preferences[i].state;
        if (state == PREF_QUEUED || state == PREF_WRITE_FAILED ||
            state == PREF_PATH_FAILED || state == PREF_FULL_FAILED) return -1;
    }
    return 0;
}
static const char *preference_message(int which) {
    switch (preferences[which].state) {
    case PREF_QUEUED: return "Applied; preference queued until disk is free.";
    case PREF_RAM: return "Applied in RAM; waiting for disk save.";
    case PREF_SAVED: return which == PREF_DISPLAY ? "Resolution saved." : "Preference saved.";
    case PREF_WRITE_FAILED: return "Preference changed or write failed. Retry saving.";
    case PREF_PATH_FAILED: return "Preference path occupied; fix it and retry.";
    case PREF_FULL_FAILED: return "No room for preference; free space and retry.";
    case PREF_SYNC_FAILED: return "RAM only; disk save failed. Use Retry saving.";
    default: return "";
    }
}
static void preference_save(int which, int value) {
    preference_queue(which, (char)('0' + value));
}
static void saver_save(void) {
    preference_save(PREF_SAVER, saver_enabled ? 1 : 0);
}
static void saver_load(void) {
    int choice = preference_read(PREF_SAVER);
    if (choice >= 0) saver_enabled = choice;
    choice = preference_read(PREF_SAVER_DELAY);
    if (choice >= 0) saver_delay = choice;
}
static void windows_save(void) {
    preference_save(PREF_WINDOWS, win_options & 15);
}
/* Appearance, window behavior, clock and mouse preferences. A missing or
 * unrecognized file keeps the default. */
static void theme_load(void) {
    int v;
    if ((v = preference_read(PREF_STYLE)) >= 0) ui_style = v;
    if ((v = preference_read(PREF_ACCENT)) >= 0) ui_accent_id = v;
    if ((v = preference_read(PREF_DESKTOP)) >= 0) ui_desktop_id = v;
    if ((v = preference_read(PREF_PATTERN)) >= 0) ui_pattern = v;
    if ((v = preference_read(PREF_WINDOWS)) >= 0) win_options = v;
    if ((v = preference_read(PREF_SNAP)) >= 0) win_snap_distance = v;
    if ((v = preference_read(PREF_CLOCK)) >= 0) clock_flags = v;
    if ((v = preference_read(PREF_MOUSE)) >= 0) dbl_click_speed = v;
}

/* Switching style changes metrics, so windows are clamped to the new work
 * area and the menu row is rebuilt. */
static void style_set(int style) {
    if (style < 0 || style >= UI_STYLE_COUNT || style == ui_style) return;
    ui_style = style;
    theme_apply();
    for (int i = 0; i < MAX_WIN; i++) if (wins[i].open) win_clamp(&wins[i]);
    icons_init();
    preference_save(PREF_STYLE, style);
    dirty = 1;
}
static void accent_set(int accent) {
    if (accent < 0 || accent >= UI_ACCENT_N) return;
    ui_accent_id = accent;
    ui_theme_set_accent(accent);
    preference_save(PREF_ACCENT, accent);
    dirty = 1;
}
static void desktop_color_set(int desktop) {
    if (desktop < 0 || desktop >= UI_DESKTOP_N) return;
    ui_desktop_id = desktop;
    ui_theme_set_desktop(desktop);
    desk_cache_theme = -1;
    preference_save(PREF_DESKTOP, desktop);
    dirty = 1;
}
static void pattern_set(int pattern) {
    if (pattern < 0 || pattern >= UI_PATTERN_N) return;
    ui_pattern = pattern;
    desk_cache_theme = -1;
    preference_save(PREF_PATTERN, pattern);
    dirty = 1;
}

/* ---------- About ---------- */

static void fmt_cat(char *out, const char *s) { kstrcpy(out + kstrlen(out), s); }
static void fmt_cat_uint(char *out, unsigned v) { char n[12]; fmt_uint(n, v); fmt_cat(out, n); }

static void about_lines(char lines[4][64]) {
    SysInfo si;
    sysinfo_fill(&si);
    kstrcpy(lines[0], "Version 0.9, build " BASEOS_BUILD_LABEL);
    lines[1][0] = 0; fmt_cat_uint(lines[1], (unsigned)fb_w); fmt_cat(lines[1], " x ");
    fmt_cat_uint(lines[1], (unsigned)fb_h); fmt_cat(lines[1], ", 256 colors");
    lines[2][0] = 0; fmt_cat_uint(lines[2], (unsigned)si.mem_mb); fmt_cat(lines[2], " MB memory");
    lines[3][0] = 0; fmt_cat_uint(lines[3], (unsigned)(si.fs_bytes / 1024)); fmt_cat(lines[3], " KB used of ");
    fmt_cat_uint(lines[3], (unsigned)(si.fs_cap / 1024)); fmt_cat(lines[3], " KB");
}

static void draw_about(int wx, int wy, int ww, int wh, int fl) {
    (void)wh;
    gui_draw_window(wx, wy, ww, wh, "About BaseOS", 0, fl | WIN_NOMENU | WIN_DIALOG);
    int top = wy + TITLE_BAR, pad = ui_theme.pad_dialog + 4;
    ui_icon_draw(UI_ICON_LOGO, wx + pad, top + pad, 48, TK_ACCENT, ui_theme.content);
    int tx = wx + pad + 48 + 16, ty = top + pad + 2;
    draw_string_bold("BaseOS", tx, ty, TK_TEXT);
    char lines[4][64];
    about_lines(lines);
    for (int i = 0; i < 4; i++)
        kit_text(lines[i], tx, ty + 24 + i * 20, TK_TEXT2, wx + ww - pad - tx, 0);
    kit_text("(C) 2026 CCG", tx, ty + 24 + 4 * 20 + 8, TK_TEXT2, wx + ww - pad - tx, 0);
    int bw = 88, bh = BTN_H;
    draw_default_button(wx + ww - pad - bw, wy + wh - pad - bh, bw, bh, "OK");
}
static void handle_about_click(int wx, int wy, int ww, int wh) {
    int pad = ui_theme.pad_dialog + 4, bw = 88, bh = BTN_H;
    if (hit(mouse_x, mouse_y, wx + ww - pad - bw, wy + wh - pad - bh, bw, bh)) close_front();
}

/* ---------- Settings ----------
 * A sidebar of pages and a form on the right. Drawing records the hit
 * zones, so clicks use exactly the geometry that was drawn. */

enum { SET_APPEARANCE, SET_DESKTOP, SET_DISPLAY, SET_INPUT, SET_TIME, SET_ABOUT, SET_PAGES };
static const char *const settings_pages[SET_PAGES] = {
    "Appearance", "Desktop", "Display", "Keyboard and Mouse", "Date and Time", "About"
};
static const int settings_page_icon[SET_PAGES] = {
    UI_ICON_A_PAL, UI_ICON_DESK, UI_ICON_SCREEN, UI_ICON_KB, UI_ICON_CLOCK, UI_ICON_INFO
};
static int settings_page;

enum {
    SZ_PAGE = 1, SZ_STYLE, SZ_ACCENT, SZ_DESKTOP, SZ_PATTERN, SZ_WINOPT, SZ_SNAP,
    SZ_SAVER, SZ_SAVER_DELAY, SZ_SAVER_PREVIEW, SZ_DISPLAY, SZ_CLOCK, SZ_MOUSE, SZ_RETRY
};
typedef struct { int x, y, w, h, kind, arg; } SetZone;
static SetZone set_zones[64];
static int set_zone_n, set_slider_drag;
static int set_snap_x, set_snap_w;

static void set_zone(int x, int y, int w, int h, int kind, int arg) {
    if (set_zone_n < (int)(sizeof set_zones / sizeof set_zones[0]))
        set_zones[set_zone_n++] = (SetZone){x, y, w, h, kind, arg};
}
static const SetZone *set_zone_at(int px, int py) {
    for (int i = set_zone_n - 1; i >= 0; i--)
        if (hit(px, py, set_zones[i].x, set_zones[i].y, set_zones[i].w, set_zones[i].h)) return &set_zones[i];
    return 0;
}
static int set_hover(int kind, int arg) {
    const SetZone *z = set_zone_at(mouse_x, mouse_y);
    return z && z->kind == kind && z->arg == arg;
}

static int settings_side_w(void) { return MODERN ? 200 : 192; }
static int settings_window_h(void) { return MODERN ? 540 : 564; }

/* Form layout: a label column and a control column. Classic rows sit on
 * the content with a rule between sections; Modern groups rows in cards. */
typedef struct { int x, y, w, card_y, rows; } SetForm;
static int set_label_w(void) { return MODERN ? 150 : 152; }

static void set_heading(SetForm *f, const char *title, const char *sub) {
    draw_string_bold(title, f->x, f->y, TK_TEXT);
    f->y += 20;
    if (sub) { draw_string(sub, f->x, f->y, TK_TEXT2); f->y += 20; }
    f->y += MODERN ? 8 : 10;
}
static void set_rule(SetForm *f) {
    if (MODERN) { f->y += 16; return; }
    draw_hline(f->x, f->y + 10, f->w, TK_LINE_SUBTLE);
    f->y += 24;
}
static void set_card_begin(SetForm *f) {
    f->card_y = f->y;
    f->rows = 0;
}
/* Starts a row of height h (Classic) or at least 48 (Modern). Draws the
 * label and returns the y of the row; *cx gets the control column x. */
static int set_row(SetForm *f, const char *label, int h, int *cx) {
    int y;
    if (MODERN) {
        int rh = h + 16 < 48 ? 48 : h + 16;
        if (f->rows) draw_hline(f->x + 1, f->y, f->w - 2, TK_45);
        y = f->y + (rh - h) / 2;
        if (label) kit_text(label, f->x + 16, kit_ty(f->y, rh), TK_TEXT, set_label_w(), 0);
        *cx = f->x + 16 + set_label_w() + 16;
        f->y += rh;
    } else {
        y = f->y;
        if (label) kit_text(label, f->x, kit_ty(y, h), TK_TEXT2, set_label_w(), 0);
        *cx = f->x + set_label_w() + 16;
        f->y += h + 14;
    }
    f->rows++;
    return y;
}
static void set_card_end(SetForm *f) {
    if (!MODERN) { f->y -= 14; return; }
    kit_frame(f->x, f->card_y, f->w, f->y - f->card_y, 10, TK_LINE_SUBTLE);
}

static uint8_t pal_of(uint32_t rgb) { return gfx_rgb((int)(rgb >> 16) & 255, (int)(rgb >> 8) & 255, (int)rgb & 255); }

static void set_swatches(int x, int y, int n, int sel, int kind, int accent) {
    for (int i = 0; i < n; i++) {
        uint8_t c = pal_of(accent ? ui_accent_rgb(ui_style, i) : ui_desktop_rgb(ui_style, i));
        if (MODERN) {
            int sx = x + i * 32, sy = y + 2;
            if (i == sel) { kit_fill(sx - 4, sy - 4, 30, 30, 15, TK_ACCENT, 1); kit_fill(sx - 2, sy - 2, 26, 26, 13, ui_theme.content, 1); }
            kit_fill(sx, sy, 22, 22, 11, c, 1);
            set_zone(sx - 4, sy - 4, 30, 30, kind, i);
        } else {
            int sx = x + i * 36;
            if (i == sel) { draw_frame(sx, y, 26, 26, TK_TEXT); draw_frame(sx + 1, y + 1, 24, 24, TK_TEXT); }
            draw_rect(sx + 3, y + 3, 20, 20, c);
            draw_frame(sx + 3, y + 3, 20, 20, TK_LINE_STRONG);
            set_zone(sx, y, 26, 26, kind, i);
        }
    }
}
static void set_value_label(SetForm *f, int y, int h, const char *text, int pill, int after_x) {
    if (MODERN) {
        int right = f->x + f->w - 16;
        if (pill) {
            int pw = kit_text_w(text, 1) + 18;
            kit_fill(right - pw, y + (h - 22) / 2, pw, 22, 11, TK_ACCENT_100, 1);
            kit_text(text, right - pw + 9, kit_ty(y + (h - 22) / 2, 22), TK_ACCENT_700, pw, 1);
        } else kit_text_right(text, right, kit_ty(y, h), TK_TEXT2, 0);
    } else kit_text(text, after_x, kit_ty(y, h), TK_TEXT, 200, 0);
}
static void set_segmented(int x, int y, const char *const *labels, int n, int sel, int kind) {
    int xs[8], ws[8], hover = -1;
    for (int i = 0; i < n; i++) if (set_hover(kind, i)) hover = i;
    kit_segmented(x, y, labels, n, sel, hover, xs, ws);
    for (int i = 0; i < n; i++) set_zone(xs[i], y, ws[i], kit_segmented_h(), kind, i);
}
/* A check (Classic) or a toggle at the right of the row (Modern). */
static void set_switch(SetForm *f, const char *label, const char *hint, int on, int kind, int arg) {
    int cx, h = hint ? 40 : (MODERN ? 20 : 24);
    int y = set_row(f, 0, h, &cx);
    if (MODERN) {
        int lx = f->x + 16;
        kit_text(label, lx, hint ? y + 2 : kit_ty(y, h), TK_TEXT, f->w - 80, 0);
        if (hint) kit_text(hint, lx, y + 22, TK_TEXT2, f->w - 80, 0);
        int tx = f->x + f->w - 16 - KIT_TOGGLE_W, ty = y + (h - KIT_TOGGLE_H) / 2;
        kit_toggle(tx, ty, (on ? KIT_ON : 0) | (set_hover(kind, arg) ? KIT_HOVER : 0));
        set_zone(f->x, y - 8, f->w, h + 16, kind, arg);
    } else {
        int w = kit_check_row(cx, y, label, (on ? KIT_ON : 0) | (set_hover(kind, arg) ? KIT_HOVER : 0), 0);
        if (hint) kit_text(hint, cx + ui_theme.check + 8, y + 24, TK_TEXT2, f->x + f->w - cx - 30, 0);
        set_zone(cx, y, w, 24, kind, arg);
    }
}
/* Classic draws the row label in the label column next to the check. */
static void set_switch_labeled(SetForm *f, const char *row_label, const char *label, const char *hint,
                               int on, int kind, int arg) {
    if (!MODERN) {
        int save = f->y;
        kit_text(row_label, f->x, kit_ty(save, 24), TK_TEXT2, set_label_w(), 0);
    }
    set_switch(f, label, hint, on, kind, arg);
}

static const int snap_distance_max = SNAP_DISTANCE_MAX;
static void set_slider_row(SetForm *f, const char *label, int value, int max, const char *unit, int kind) {
    int cx, y = set_row(f, 0, 18, &cx);
    int lx = MODERN ? f->x + 32 : cx;
    kit_text(label, lx, kit_ty(y, 18), TK_TEXT2, 140, 0);
    int sx = MODERN ? f->x + 16 + set_label_w() + 16 + 32 : cx + 100;
    int right = f->x + f->w - (MODERN ? 16 : 0);
    int sw = right - 60 - sx;
    if (sw > 240) sw = 240;
    kit_slider(sx, y, sw, value, max, set_slider_drag ? KIT_PRESSED : 0);
    char v[16];
    fmt_uint(v, (unsigned)value);
    kstrcpy(v + kstrlen(v), unit);
    if (MODERN) kit_text_right(v, right, kit_ty(y, 18), TK_TEXT, 0);
    else kit_text(v, sx + sw + 12, kit_ty(y, 18), TK_TEXT, 60, 0);
    set_snap_x = sx;
    set_snap_w = sw;
    set_zone(sx - 4, y - 4, sw + 8, 26, kind, 0);
}

static const char *const pattern_names[UI_PATTERN_N] = {"None", "Dots", "Grid"};
static const char *const style_names[UI_STYLE_COUNT] = {"Classic", "Modern"};

static void settings_appearance(SetForm *f) {
    int cx, y;
    set_heading(f, "Appearance", "Changes apply right away.");
    if (!MODERN) set_rule(f), f->y -= 10;
    set_card_begin(f);
    y = set_row(f, "Style", kit_segmented_h(), &cx);
    set_segmented(cx, y, style_names, UI_STYLE_COUNT, ui_style, SZ_STYLE);
    y = set_row(f, "Accent color", MODERN ? 30 : 26, &cx);
    set_swatches(cx, y, UI_ACCENT_N, ui_accent_id, SZ_ACCENT, 1);
    set_value_label(f, y, MODERN ? 30 : 26, ui_accent_name(ui_style, ui_accent_id), 1, cx + UI_ACCENT_N * 36 + 2);
    y = set_row(f, "Desktop color", MODERN ? 30 : 26, &cx);
    set_swatches(cx, y, UI_DESKTOP_N, ui_desktop_id, SZ_DESKTOP, 0);
    set_value_label(f, y, MODERN ? 30 : 26, ui_desktop_name(ui_style, ui_desktop_id), 0, cx + UI_DESKTOP_N * 36 + 2);
    y = set_row(f, "Desktop pattern", kit_segmented_h(), &cx);
    set_segmented(cx, y, pattern_names, UI_PATTERN_N, ui_pattern, SZ_PATTERN);
    set_card_end(f);
    set_rule(f);
    draw_string_bold("Windows", f->x, f->y, TK_TEXT);
    f->y += MODERN ? 24 : 32;
    set_card_begin(f);
    if (MODERN) {
        set_switch(f, "Show window contents while dragging",
                   "When off, BaseOS draws an outline instead. Faster on slow machines.",
                   win_options & WINOPT_DRAG_CONTENTS, SZ_WINOPT, WINOPT_DRAG_CONTENTS);
        set_switch(f, "Double-click a title bar to maximize", 0, win_options & WINOPT_DOUBLE_CLICK, SZ_WINOPT, WINOPT_DOUBLE_CLICK);
        set_switch(f, "Snap windows to screen edges", 0, win_options & WINOPT_SNAP, SZ_WINOPT, WINOPT_SNAP);
    } else {
        set_switch_labeled(f, "While dragging", "Show window contents",
                           "When off, BaseOS draws a 1px outline instead. Faster on slow machines.",
                           win_options & WINOPT_DRAG_CONTENTS, SZ_WINOPT, WINOPT_DRAG_CONTENTS);
        set_switch_labeled(f, "Title bars", "Double-click to maximize", 0, win_options & WINOPT_DOUBLE_CLICK, SZ_WINOPT, WINOPT_DOUBLE_CLICK);
        set_switch_labeled(f, "Screen edges", "Snap windows to edges", 0, win_options & WINOPT_SNAP, SZ_WINOPT, WINOPT_SNAP);
    }
    set_slider_row(f, "Snap distance", win_snap_distance, snap_distance_max, " px", SZ_SNAP);
    set_card_end(f);
}

static const char *const saver_delay_names[SAVER_DELAY_N] = {"90 s", "3 min", "5 min", "10 min", "30 min"};

static void settings_desktop(SetForm *f) {
    int cx, y;
    set_heading(f, "Desktop", "The screen saver starts when the computer sits idle.");
    if (!MODERN) set_rule(f), f->y -= 10;
    set_card_begin(f);
    if (MODERN) set_switch(f, "Screen saver", 0, saver_enabled, SZ_SAVER, 0);
    else set_switch_labeled(f, "Screen saver", "Start the screen saver when idle", 0, saver_enabled, SZ_SAVER, 0);
    y = set_row(f, "Start after", kit_segmented_h(), &cx);
    set_segmented(cx, y, saver_delay_names, SAVER_DELAY_N, saver_delay, SZ_SAVER_DELAY);
    y = set_row(f, "Preview", BTN_H, &cx);
    {
        int bw = kit_button_w("Preview", -1);
        kit_button(cx, y, bw, BTN_H, "Preview", -1, KB_DEFAULT, set_hover(SZ_SAVER_PREVIEW, 0) ? KIT_HOVER : 0);
        set_zone(cx, y, bw, BTN_H, SZ_SAVER_PREVIEW, 0);
    }
    set_card_end(f);
    set_rule(f);
    draw_string_bold("Desktop icons", f->x, f->y, TK_TEXT);
    f->y += MODERN ? 24 : 28;
    draw_string("System opens the volume's properties. Home and Trash open in Files.", f->x, f->y, TK_TEXT2);
    f->y += 20;
}

static void settings_display(SetForm *f) {
    int cx, y;
    set_heading(f, "Display", "A new resolution waits 15 seconds for you to keep it.");
    if (!MODERN) set_rule(f), f->y -= 10;
    set_card_begin(f);
    for (int i = 0; i < DISPLAY_MODE_COUNT; ++i) {
        y = set_row(f, i == 0 ? "Resolution" : 0, ui_theme.row_h, &cx);
        int on = display_current_mode() == i;
        int w = kit_check_row(cx, y, display_mode(i)->name, (on ? KIT_ON : 0) | (set_hover(SZ_DISPLAY, i) ? KIT_HOVER : 0), 1);
        set_zone(cx, y, w, ui_theme.row_h, SZ_DISPLAY, i);
        if (!MODERN) f->y -= 8;
    }
    if (!MODERN) f->y += 8;
    y = set_row(f, "Colors", 20, &cx);
    kit_text("256 colors", cx, kit_ty(y, 20), TK_TEXT, 200, 0);
    set_card_end(f);
    f->y += 12;
    kit_text(display_message[0] ? display_message :
             preferences[PREF_DISPLAY].state ? preference_message(PREF_DISPLAY) :
             "Keys 1-4 select a mode. Enter keeps it; Escape reverts.",
             f->x, f->y, TK_TEXT2, f->w, 0);
    f->y += 20;
}

static const char *const mouse_speed_names[3] = {"Slow", "Normal", "Fast"};
static const char *const shortcut_keys[][2] = {
    {"Super", "Apps"}, {"Ctrl+Space", "Apps"}, {"Alt+Tab", "Switch windows"},
    {"Alt+F4", "Close window"}, {"Super+Arrow", "Snap or maximize"}, {"Super+D", "Show desktop"},
    {"Alt or F10", "Menu row"}, {"Alt+Space", "Window menu"},
};

static void settings_input(SetForm *f) {
    int cx, y;
    set_heading(f, "Keyboard and Mouse", 0);
    if (!MODERN) set_rule(f), f->y -= 10;
    set_card_begin(f);
    y = set_row(f, "Double-click speed", kit_segmented_h(), &cx);
    set_segmented(cx, y, mouse_speed_names, 3, dbl_click_speed, SZ_MOUSE);
    set_card_end(f);
    set_rule(f);
    draw_string_bold("Shortcuts", f->x, f->y, TK_TEXT);
    f->y += MODERN ? 24 : 28;
    set_card_begin(f);
    int n = (int)(sizeof shortcut_keys / sizeof shortcut_keys[0]);
    for (int i = 0; i < n; i++) {
        y = set_row(f, shortcut_keys[i][1], 22, &cx);
        kit_kbd(cx, y, shortcut_keys[i][0]);
        if (!MODERN) f->y -= 6;
    }
    if (!MODERN) f->y += 6;
    set_card_end(f);
}

static void settings_time(SetForm *f) {
    int cx, y;
    set_heading(f, "Date and Time", "The clock reads the computer's real-time clock.");
    if (!MODERN) set_rule(f), f->y -= 10;
    char d[24], t[12];
    date_time_text(d, t);
    draw_logo_string(t, f->x, f->y, TK_TEXT);
    f->y += LOGO_FONT_H + 4;
    draw_string(d, f->x, f->y, TK_TEXT2);
    f->y += 32;
    set_card_begin(f);
    if (MODERN) {
        set_switch(f, "Use 24-hour time", 0, !(clock_flags & CLOCK_12H), SZ_CLOCK, CLOCK_12H);
        set_switch(f, "Show the date in the taskbar", 0, !(clock_flags & CLOCK_NO_DATE), SZ_CLOCK, CLOCK_NO_DATE);
    } else {
        set_switch_labeled(f, "Time format", "Use 24-hour time", 0, !(clock_flags & CLOCK_12H), SZ_CLOCK, CLOCK_12H);
        set_switch_labeled(f, "Taskbar", "Show the date", 0, !(clock_flags & CLOCK_NO_DATE), SZ_CLOCK, CLOCK_NO_DATE);
    }
    set_card_end(f);
    (void)y; (void)cx;
}

static void settings_about(SetForm *f) {
    ui_icon_draw(UI_ICON_LOGO, f->x, f->y, 48, TK_ACCENT, ui_theme.content);
    draw_logo_string("BaseOS", f->x + 64, f->y + 6, TK_TEXT);
    f->y += 64;
    char lines[4][64];
    about_lines(lines);
    for (int i = 0; i < 4; i++) { kit_text(lines[i], f->x, f->y, TK_TEXT, f->w, 0); f->y += 20; }
    f->y += 8;
    kit_text("(C) 2026 CCG", f->x, f->y, TK_TEXT2, f->w, 0);
    f->y += 20;
}

/* The worst preference state of the page's preferences, for the footer. */
static int settings_failed_pref(void) {
    for (int i = 0; i < PREF_COUNT; ++i) {
        int s = preferences[i].state;
        if (s == PREF_WRITE_FAILED || s == PREF_SYNC_FAILED || s == PREF_PATH_FAILED || s == PREF_FULL_FAILED) return i;
    }
    return -1;
}

static void draw_settings(int wx, int wy, int ww, int wh, int fl) {
    gui_draw_window(wx, wy, ww, wh, settings_pages[settings_page], 0, fl);
    set_zone_n = 0;
    int top = wy + TITLE_H, side = settings_side_w(), bottom = wy + wh - 1;
    /* Sidebar */
    draw_rect(wx + 1, top, side, bottom - top, MODERN ? ui_theme.sidebar : TK_BASE);
    draw_vline(wx + 1 + side, top, bottom - top, MODERN ? TK_LINE_SUBTLE : TK_LINE_SUBTLE);
    int iy = top + (MODERN ? 10 : 8), ih = MODERN ? 32 : 24, im = MODERN ? 8 : 4;
    for (int i = 0; i < SET_PAGES; i++) {
        int x = wx + 1 + im, w = side - 2 * im, on = i == settings_page;
        int hov = !on && set_hover(SZ_PAGE, i);
        if (on) kit_fill(x, iy, w, ih, MODERN ? 6 : KIT_R_SQUARE, TK_ACCENT, 1);
        else if (hov) kit_fill(x, iy, w, ih, MODERN ? 6 : KIT_R_SQUARE, TK_HOVER, 1);
        uint8_t ink = on ? TK_ON_ACCENT : TK_TEXT, bg = on ? TK_ACCENT : hov ? TK_HOVER : (MODERN ? ui_theme.sidebar : TK_BASE);
        int pad = MODERN ? 10 : 8;
        ui_icon_draw(settings_page_icon[i], x + pad, iy + (ih - 16) / 2, 16, ink, bg);
        kit_text(settings_pages[i], x + pad + 16 + (MODERN ? 10 : 8), kit_ty(iy, ih), ink, w - 40, on);
        set_zone(x, iy, w, ih, SZ_PAGE, i);
        iy += ih + (MODERN ? 2 : 0);
    }
    /* Page */
    int cx = wx + 1 + side + 1, pad = MODERN ? 20 : 24;
    SetForm f = {cx + pad, top + (MODERN ? 20 : 28), wx + ww - 1 - (MODERN ? 0 : SB) - cx - 2 * pad, 0, 0};
    if (!MODERN) {
        draw_rect(wx + ww - 1 - SB, top, SB, bottom - top, TK_BASE);
        draw_vline(wx + ww - 1 - SB, top, bottom - top, TK_LINE_SUBTLE);
    }
    switch (settings_page) {
    case SET_APPEARANCE: settings_appearance(&f); break;
    case SET_DESKTOP: settings_desktop(&f); break;
    case SET_DISPLAY: settings_display(&f); break;
    case SET_INPUT: settings_input(&f); break;
    case SET_TIME: settings_time(&f); break;
    default: settings_about(&f); break;
    }
    int failed = settings_failed_pref();
    if (failed >= 0) {
        int by = bottom - 16 - BTN_H, bw = kit_button_w("Retry saving", -1);
        kit_text(preference_message(failed), f.x, kit_ty(by, BTN_H), TK_RED_700, f.w - bw - 12, 0);
        kit_button(f.x + f.w - bw, by, bw, BTN_H, "Retry saving", -1, KB_DEFAULT, set_hover(SZ_RETRY, 0) ? KIT_HOVER : 0);
        set_zone(f.x + f.w - bw, by, bw, BTN_H, SZ_RETRY, 0);
    }
}

static void settings_slider_set(void) {
    if (set_snap_w <= 0) return;
    int v = (mouse_x - set_snap_x) * snap_distance_max / set_snap_w;
    if (v < 0) v = 0;
    if (v > snap_distance_max) v = snap_distance_max;
    if (v != win_snap_distance) { win_snap_distance = v; dirty = 1; }
}
static void settings_drag(void) {
    if (!set_slider_drag) return;
    if (!mouse_left) {
        set_slider_drag = 0;
        preference_save(PREF_SNAP, win_snap_distance);
        dirty = 1;
        return;
    }
    settings_slider_set();
}

static void handle_settings_click(int wx, int wy, int ww, int wh) {
    (void)wx; (void)wy; (void)ww; (void)wh;
    const SetZone *z = set_zone_at(mouse_x, mouse_y);
    if (!z) return;
    switch (z->kind) {
    case SZ_PAGE: settings_page = z->arg; break;
    case SZ_STYLE: style_set(z->arg); break;
    case SZ_ACCENT: accent_set(z->arg); break;
    case SZ_DESKTOP: desktop_color_set(z->arg); break;
    case SZ_PATTERN: pattern_set(z->arg); break;
    case SZ_WINOPT: win_options ^= z->arg; windows_save(); break;
    case SZ_SNAP: set_slider_drag = 1; settings_slider_set(); break;
    case SZ_SAVER: saver_enabled = !saver_enabled; saver_save(); break;
    case SZ_SAVER_DELAY: saver_delay = z->arg; preference_save(PREF_SAVER_DELAY, saver_delay); break;
    case SZ_SAVER_PREVIEW: saver_start(); break;
    case SZ_DISPLAY: display_request(z->arg); break;
    case SZ_CLOCK: clock_flags ^= z->arg; preference_save(PREF_CLOCK, clock_flags); break;
    case SZ_MOUSE: dbl_click_speed = z->arg; preference_save(PREF_MOUSE, dbl_click_speed); break;
    case SZ_RETRY: preferences_retry(); break;
    default: break;
    }
    dirty = 1;
}

/* Settings keys: Tab and the arrows move between pages, 1-4 pick a display
 * mode on the Display page, R retries saving. */
static void settings_key(void) {
    if (key_sc == KEY_ESC) { close_front(); return; }
    if (key_sc == KEY_TAB || key_sc == KEY_DOWN) settings_page = (settings_page + (shift_down ? SET_PAGES - 1 : 1)) % SET_PAGES;
    else if (key_sc == KEY_UP) settings_page = (settings_page + SET_PAGES - 1) % SET_PAGES;
    else if (key_char >= '1' && key_char <= '0' + DISPLAY_MODE_COUNT) { display_request(key_char - '1'); return; }
    else if (key_char == 'r' || key_char == 'R') preferences_retry();
    else if (key_sc == KEY_SPACE && settings_page == SET_DESKTOP) { saver_enabled = !saver_enabled; saver_save(); }
    dirty = 1;
}

/* ---------- Logo and boot screen ---------- */

static void draw_logo(int x, int y, int size) {
    ui_icon_draw(UI_ICON_LOGO, x, y, size, TK_ACCENT, get_pixel(x, y));
}

/* Esc in the input queue during the boot screen. Earlier typeahead is
 * dropped with it. */
static int boot_esc_pressed(void) {
    drain_8042();
    for (unsigned i = 0; i < device_input.count; i++) {
        const InputSample *s = &device_input.queue[(device_input.head + i) % INPUT_CAPACITY];
        if (s->kind == INPUT_KEY && s->scancode == 1 && (s->flags & INPUT_MAKE)) {
            input_wake_serial = s->serial;
            return 1;
        }
    }
    return 0;
}

/* The boot screen borrows tokens for its dark colors; theme_apply puts the
 * desktop palette back when it ends. Esc shows the kernel log. */
static void boot_colors(void) {
    int m = ui_style == UI_STYLE_MODERN;
    gfx_set_color(TK_DESKTOP, m ? 0x1E2330 : 0x14213A);
    gfx_set_color(TK_LINE, m ? 0x2E3546 : 0x2A3A57);
    gfx_set_color(TK_TEXT2, m ? 0xA9B3C4 : 0x9DB0CC);
    gfx_set_color(TK_LINE_STRONG, m ? 0x8E99AA : 0x6F86A8);
    gfx_set_color(TK_47, 0x4A5568);
    gfx_set_color(TK_46, 0xD8DDE6);
    gfx_pal_commit();
}

static const char *boot_stage(int prog, int bw) {
    if (prog < bw / 3) return "Loading drivers";
    if (prog < bw * 2 / 3) return "Mounting volumes";
    return "Starting window manager";
}

static void boot_messages(void) {
    static char text[8192];
    platform_log_copy(text, sizeof text);
    draw_rect(0, 0, fb_w, fb_h, TK_DESKTOP);
    /* Show the last lines that fit, newest at the bottom. */
    int line_h = CHAR_H + 2, rows = (fb_h - 48) / line_h, n = kstrlen(text), lines = 0, at = n;
    while (at > 0 && lines < rows) { at--; if (text[at] == '\n' && at != n - 1) lines++; }
    if (at > 0) at++;
    int y = 24;
    char line[160];
    while (at < n && y < fb_h - 24) {
        int k = 0;
        while (at < n && text[at] != '\n' && k < (int)sizeof line - 1) line[k++] = text[at++];
        line[k] = 0;
        if (at < n && text[at] == '\n') at++;
        draw_string_clip(line, 24, y, TK_TEXT2, fb_w - 24);
        y += line_h;
    }
    flip_vga();
}

static void boot_splash(void) {
    boot_colors();
    draw_rect(0, 0, fb_w, fb_h, TK_DESKTOP);
    int size = 128;
    int lx = (fb_w - size) / 2;
    int group = size + 20 + LOGO_FONT_H + 36 + 6 + 12 + CHAR_H;
    int ly = (fb_h - group) / 2 - 40;
    draw_logo(lx, ly, size);
    int ty = ly + size + 20;
    int lw = logo_string_w("BaseOS");
    draw_logo_string("BaseOS", (fb_w - lw) / 2, ty, COLOR_WHITE);
    int bw = 240, bx = (fb_w - bw) / 2, by = ty + LOGO_FONT_H + 36;
    int sy = by + 6 + 12;
    int r = MODERN ? 3 : KIT_R_SQUARE;
    kit_fill(bx, by, bw, 6, r, TK_LINE, 1);
    int hy = fb_h - 32 - CHAR_H;
    if (MODERN) {
        int kw = kit_text_w("Esc", 0) + 12, tw = ui_string_w("show boot messages"), x0 = (fb_w - kw - 8 - tw) / 2;
        kit_box(x0, hy - 2, kw, 20, 4, TK_DESKTOP, TK_47);
        draw_string("Esc", x0 + 6, hy, TK_46);
        draw_string("show boot messages", x0 + kw + 8, hy, TK_LINE_STRONG);
    } else draw_string_centered("Press Esc to show boot messages", hy, TK_LINE_STRONG);
    flip_vga();
    kprint_debug("SPLASH\n");

    uint32_t start = frame_count;
    int shown = -1, messages = 0;
    const char *stage = 0;
    while (1) {
        poll_time();
        kernel_services_poll();
        if(kernel_events_take(KERNEL_EVENT_DOWNLOADS)){
            int original=context_slot;
            for(int i=0;i<MAX_WIN;i++)if(wins[i].open&&wins[i].kind==WK_FILES){context_set(i);fm_refresh();}
            context_set(original);dirty=1;
        }
        if(browser_tick()&&find_open_kind(WK_BROWSER)>=0)dirty=1;
        if(player_tick()&&find_open_kind(WK_PLAYER)>=0)dirty=1;
        AppViewUpdate update=app_view_poll_update();
        native_window_complete(update);
        if(update.flags)dirty=1;
        if (display_pending && (int32_t)(timer_ticks() - display_deadline) >= 0) display_revert();
        context_set(win_front());
        if (boot_esc_pressed() && !messages) { messages = 1; boot_messages(); }
        int prog = (int)((frame_count - start) * bw / 150);
        if (prog > bw)
            prog = bw;
        if (!messages && prog - shown >= 8) {
            shown = prog;
            kit_fill(bx, by, bw, 6, r, TK_LINE, 1);
            if (prog > 0) kit_fill(bx, by, prog, 6, r, TK_ACCENT, 1);
            flip_rect(bx, by, bw, 6);
            const char *s = boot_stage(prog, bw);
            if (s != stage) {
                stage = s;
                draw_rect(0, sy, fb_w, CHAR_H + 2, TK_DESKTOP);
                draw_string_centered(s, sy, TK_TEXT2);
                flip_rect(0, sy, fb_w, CHAR_H + 2);
            }
        }
        if ((frame_count - start) >= (messages ? 450u : 150u))
            break;
    }
    theme_apply();
    kprint_debug("DESKTOP\n");
    dirty = 1;
}


typedef struct { int open,kind,x,y,w,h,min,z,caret; char path[FS_PATH_LEN]; } SavedWindow;
typedef struct { unsigned magic,version; SavedWindow win[MAX_WIN]; } SavedSession;
typedef struct { unsigned magic,version,valid; WriterBinding source; } SavedWriterBinding;
#define WRITER_BINDING_MAGIC 0x31425257u
_Static_assert(sizeof(SavedWriterBinding)==24,"Writer recovery binding ABI changed");
typedef struct { EditorSource binding; EditorBinding draft; } SavedEditorSource;
typedef struct {
    unsigned magic,version,count;
    EditorBinding session;
    SavedEditorSource win[MAX_WIN];
} SavedEditorBindings;
#define EDITOR_BINDING_MAGIC 0x31424445u
_Static_assert(sizeof(SavedEditorBindings)==248,"Editor recovery binding ABI changed");
/* Paired to the exact v1 session and native draft. Runtime node identities
 * are never persisted or trusted across reboot. */
typedef struct {
    unsigned magic, version, valid;
    SpreadsheetBinding source;
    EditorBinding session, draft;
    unsigned anchor;
} SavedSheetBinding;
#define SHEET_BINDING_MAGIC 0x31425342u
_Static_assert(sizeof(SavedSheetBinding)==52,"Spreadsheet recovery binding ABI changed");
static int session_ready;

static int session_put(int dir,const char *name,const void *data,int size){
    if(size<0||(unsigned)size>fs_file_limit())return -1;
    int id=fs_find_child(dir,name);
    if(id>=0&&(fs_is_dir(id)||fs_is_app(id)))return -1;
    if(id>=0&&fs_size(id)==size){const unsigned char *a=(const unsigned char *)fs_data(id),*b=data;int i=0;while(i<size&&a[i]==b[i])i++;if(i==size)return 0;}
    int created=id<0;
    if(created)id=fs_create(dir,name);
    if(id<0)return -1;
    int result=fs_write(id,data,size);
    if(result!=size){if(created)fs_delete(id);return -1;}
    return 0;
}
static void session_save(void){
    if(fs_sync_busy())return;
    if(!session_ready)return;
    int dir=fs_find_child(fs_root(),"prefs");
    unsigned writer_size=0;
    const unsigned char *writer_draft=0;
    int writer_slot=find_open_kind(WK_WRITER);
    unsigned sheet_size=0;
    const unsigned char *sheet_draft=0;
    int sheet_slot=find_open_kind(WK_SPREADSHEET);
    SavedSheetBinding sheet_binding;kmemset(&sheet_binding,0,sizeof sheet_binding);
    sheet_binding.magic=SHEET_BINDING_MAGIC;sheet_binding.version=1;
    SavedWriterBinding binding={WRITER_BINDING_MAGIC,1,0,{0,0,0}};
    SavedEditorBindings editors;kmemset(&editors,0,sizeof editors);
    editors.magic=EDITOR_BINDING_MAGIC;editors.version=1;editors.count=MAX_WIN;
    int editor_slot=find_open_kind(WK_EDIT);
    if(writer_slot>=0){
        writer_draft=writer_snapshot(&writer_size);
        if(!writer_draft){session_status="Writer recovery could not be prepared.";return;}
        binding.valid=(unsigned)writer_binding(&binding.source);
    }
    if(sheet_slot>=0){
        sheet_draft=spreadsheet_snapshot(&sheet_size);
        if(!sheet_draft){session_status="Spreadsheet recovery could not be prepared.";return;}
        /* Only non-mutating getters follow until the borrowed bytes are copied. */
        sheet_binding.valid=(unsigned)spreadsheet_binding(&sheet_binding.source);
        sheet_binding.anchor=spreadsheet_anchor();
        edit_fingerprint(sheet_draft,sheet_size,&sheet_binding.draft);
    }
    int needed=dir<0?1:0,projected=(int)fs_used_bytes();
    if(dir>=0&&!fs_is_dir(dir))goto failure;
    for(int i=-7;i<MAX_WIN;i++){
        char draft[]="draft0.txt";const char *name;
        if(i==-7){if(sheet_slot<0)continue;name="sheet-binding";}
        else if(i==-6){if(sheet_slot<0)continue;name="sheet-draft.bsh";}
        else if(i==-5){if(editor_slot<0)continue;name="editor-bindings";}
        else if(i==-4){if(writer_slot<0)continue;name="writer-binding";}
        else if(i==-3){if(writer_slot<0)continue;name="writer-draft.bwr";}
        else if(i==-2)name="session";
        else if(i==-1){if(!paint_ready)continue;name="paint-draft";}
        else {if(!wins[i].open||wins[i].kind!=WK_EDIT)continue;draft[5]+=(char)i;name=draft;}
        int size=i==-7?(int)sizeof(sheet_binding):i==-6?(int)sheet_size:i==-5?(int)sizeof(editors):i==-4?(int)sizeof(binding):i==-3?(int)writer_size:i==-2?(int)sizeof(SavedSession):i==-1?PAINT_W*PAINT_H:window_state[i].doc.len;
        if(size<0||(unsigned)size>fs_file_limit())goto failure;
        int id=fs_find_child(dir,name);
        if(id<0)needed++;
        else {
            if(fs_is_dir(id)||fs_is_app(id))goto failure;
            int old_size=fs_size(id);
            /* Small metadata is committed last, so do not spend space
             * that an unusually large older metadata file might free later. */
            projected-=(i==-7||i==-5||i==-4||i==-2||i==-1)&&old_size>size?size:old_size;
        }
        projected+=size;
    }
    /* No draft is changed until every new node and replacement byte fits. */
    if(needed>fs_node_limit()-fs_node_count()||projected>(int)fs_capacity_for_nodes((unsigned)(fs_node_count()+needed)))goto failure;
    if(dir<0){dir=fs_mkdir(fs_root(),"prefs");if(dir<0)goto failure;}
    SavedSession snap;kmemset(&snap,0,sizeof snap);snap.magic=0x53534542;snap.version=1;
    for(int i=0;i<MAX_WIN;i++){
        Win *w=&wins[i];SavedWindow *v=&snap.win[i];
        if(!w->open||!session_window_kind(w->kind))continue;
        context_set(i);v->open=1;v->kind=w->kind;v->x=w->x;v->y=w->y;v->w=w->w;v->h=w->h;v->min=w->min;v->z=w->z;
        int id=w->kind==WK_EDIT?edit_file:w->kind==WK_SPREADSHEET?spreadsheet_file():w->kind==WK_WRITER?writer_file():w->kind==WK_FILES?fm_cwd:w->kind==WK_TERM?term_cwd():-1;
        if(w->kind==WK_EDIT && !edit_binding_valid())id=-1;
        if(w->kind==WK_WRITER && fs_identity(id)!=writer_file_identity())id=-1;
        if(w->kind==WK_SPREADSHEET && fs_identity(id)!=spreadsheet_file_identity())id=-1;
        if(fs_valid(id))fs_path(id,v->path,sizeof v->path);
        if(w->kind==WK_EDIT){
            v->caret=edit_caret;
            if(id>=0&&edit_source.valid)editors.win[i].binding=edit_source;
            edit_fingerprint(edit_buf,(unsigned)edit_len,&editors.win[i].draft);
        }
        else if(w->kind==WK_WRITER)v->caret=(int)writer_caret();
        else if(w->kind==WK_SPREADSHEET)v->caret=(int)spreadsheet_caret();
    }
    /* Pair the sidecar with both v1 metadata and each complete draft. If a
     * future write failure leaves mixed recovery generations, they cannot
     * authorize saving another slot's draft over a formerly bound source. */
    edit_fingerprint(&snap,sizeof snap,&editors.session);
    edit_fingerprint(&snap,sizeof snap,&sheet_binding.session);
    /* Reclaim smaller drafts first, so the preflight's total-space promise
     * also holds when one document grows while another becomes shorter. */
    for(int growing=0;growing<2;growing++)for(int i=-2;i<MAX_WIN;i++){
        if(i==-2){
            if(sheet_slot<0)continue;
            int old=fs_find_child(dir,"sheet-draft.bsh"),old_size=old<0?0:fs_size(old);
            if((old<0||(int)sheet_size>=old_size)!=growing)continue;
            if(session_put(dir,"sheet-draft.bsh",sheet_draft,(int)sheet_size)<0)goto failure;
            continue;
        }
        if(i==-1){
            if(writer_slot<0)continue;
            int old=fs_find_child(dir,"writer-draft.bwr"),old_size=old<0?0:fs_size(old);
            if((old<0||(int)writer_size>=old_size)!=growing)continue;
            if(session_put(dir,"writer-draft.bwr",writer_draft,(int)writer_size)<0)goto failure;
            continue;
        }
        if(!wins[i].open||wins[i].kind!=WK_EDIT)continue;
        context_set(i);char name[]="draft0.txt";name[5]+=(char)i;
        int old=fs_find_child(dir,name),old_size=old<0?0:fs_size(old);
        if((old<0||edit_len>=old_size)!=growing)continue;
        if(session_put(dir,name,edit_buf,edit_len)<0)goto failure;
    }
    if(sheet_slot>=0 && session_put(dir,"sheet-binding",&sheet_binding,sizeof sheet_binding)<0)goto failure;
    if(editor_slot>=0 && session_put(dir,"editor-bindings",&editors,sizeof editors)<0)goto failure;
    if(writer_slot>=0 && session_put(dir,"writer-binding",&binding,sizeof binding)<0)goto failure;
    if(paint_ready && session_put(dir,"paint-draft",paint_pix,PAINT_W*PAINT_H)<0)goto failure;
    if(session_put(dir,"session",&snap,sizeof snap)<0)goto failure;
    session_status="";context_set(win_front());return;
failure:
    session_status="Session not saved: storage is full.";context_set(win_front());
}
static void session_restore(void){
    int dir=fs_find_child(fs_root(),"prefs"),id=fs_find_child(dir,"session");SavedSession snap;
    if(id>=0 && fs_size(id)==sizeof snap){kmemcpy(&snap,fs_data(id),sizeof snap);
        SavedEditorBindings editors;int editor_bindings_valid=0;
        int metadata=fs_find_child(dir,"editor-bindings");
        if(metadata>=0&&!fs_is_dir(metadata)&&!fs_is_app(metadata)&&fs_size(metadata)==sizeof editors){
            kmemcpy(&editors,fs_data(metadata),sizeof editors);
            editor_bindings_valid=editors.magic==EDITOR_BINDING_MAGIC&&editors.version==1&&editors.count==MAX_WIN&&
                edit_fingerprint_matches(&snap,sizeof snap,&editors.session);
        }
        if(snap.magic==0x53534542&&snap.version==1)for(int i=0;i<MAX_WIN;i++){
            SavedWindow *v=&snap.win[i];v->path[FS_PATH_LEN-1]=0;
            if(!v->open||!session_window_kind(v->kind)||v->w<1||v->h<1||v->w>4096||v->h>4096||v->x<0||v->x>4096||v->y<0||v->y>4096)continue;
            int slot=win_open(v->kind);if(slot<0)break;Win *w=&wins[slot];w->x=v->x;w->y=v->y;w->w=v->w;w->h=v->h;w->min=!!v->min;w->z=v->z>=0&&v->z<100000?v->z:slot;win_clamp(w);
            if(w->z>wm_z)wm_z=w->z;
            int target=v->path[0]?fs_resolve(fs_root(),v->path):-1;
            if(v->kind==WK_FILES){fm_set_cwd(fs_is_dir(target)?target:fs_root());fm_refresh();}
            if(v->kind==WK_TERM)term_set_cwd(target);
            if(v->kind==WK_SPREADSHEET){
                if(fs_valid(target)&&!fs_is_dir(target)&&!fs_is_app(target))spreadsheet_open_file(target);
                int draft=fs_find_child(dir,"sheet-draft.bsh");
                if(draft>=0&&!fs_is_dir(draft)&&!fs_is_app(draft)){
                    unsigned caret=v->caret>=0?(unsigned)v->caret:0,anchor=caret;
                    int source=-1,metadata=fs_find_child(dir,"sheet-binding");
                    SavedSheetBinding binding;
                    if(metadata>=0&&!fs_is_dir(metadata)&&!fs_is_app(metadata)&&fs_size(metadata)==sizeof binding){
                        kmemcpy(&binding,fs_data(metadata),sizeof binding);
                        if(binding.magic==SHEET_BINDING_MAGIC&&binding.version==1&&
                           edit_fingerprint_matches(&snap,sizeof snap,&binding.session)&&
                           edit_fingerprint_matches(fs_data(draft),(unsigned)fs_size(draft),&binding.draft)){
                            anchor=binding.anchor;
                            if(binding.valid==1&&spreadsheet_binding_matches(target,&binding.source))source=target;
                        }
                    }
                    if(!spreadsheet_restore((const unsigned char *)fs_data(draft),(unsigned)fs_size(draft),
                        source,fs_identity(source),1,caret,anchor))
                        session_status="Spreadsheet recovery could not be restored.";
                }
                int docs=fs_find_child(fs_root(),"Documents");
                fm_set_cwd(fs_valid(target)?fs_parent(target):fs_is_dir(docs)?docs:fs_root());
            }
            if(v->kind==WK_WRITER){
                if(fs_valid(target)&&!fs_is_dir(target)&&!fs_is_app(target))writer_open_file(target);
                int draft=fs_find_child(dir,"writer-draft.bwr");
                if(draft>=0&&!fs_is_dir(draft)&&!fs_is_app(draft)){
                    unsigned caret=v->caret>=0?(unsigned)v->caret:0;
                    int source=-1,metadata=fs_find_child(dir,"writer-binding");
                    SavedWriterBinding binding;
                    if(metadata>=0&&!fs_is_dir(metadata)&&!fs_is_app(metadata)&&fs_size(metadata)==sizeof binding){
                        kmemcpy(&binding,fs_data(metadata),sizeof binding);
                        if(binding.magic==WRITER_BINDING_MAGIC&&binding.version==1&&binding.valid==1&&
                           writer_binding_matches(target,&binding.source))source=target;
                    }
                    /* A path alone cannot identify a file replaced while offline. */
                    if(!writer_restore((const unsigned char *)fs_data(draft),(unsigned)fs_size(draft),
                        source,fs_identity(source),1,caret,caret))
                        session_status="Writer recovery could not be restored.";
                }
                int docs=fs_find_child(fs_root(),"Documents");
                fm_set_cwd(fs_valid(target)?fs_parent(target):fs_is_dir(docs)?docs:fs_root());
            }
            if(v->kind==WK_EDIT){edit_clear();if(fs_valid(target)&&!fs_is_dir(target)){
                    fm_set_cwd(fs_parent(target));edit_load(target);
                }
                char name[]="draft0.txt";name[5]+=(char)i;int draft=fs_find_child(dir,name);
                if(draft>=0&&!fs_is_dir(draft)&&!fs_is_app(draft)){
                    if(fs_size(draft)<EDIT_BUF_SIZE){
                        int length=fs_read(draft,edit_scratch,EDIT_BUF_SIZE);
                        if(length!=fs_size(draft)){session_status="Editor recovery could not be restored.";continue;}
                        kmemcpy(edit_buf,edit_scratch,length+1);edit_len=length;edit_saved_ok=0;
                        if(!editor_bindings_valid||editors.win[i].binding.valid!=1||
                           !edit_fingerprint_matches(edit_buf,(unsigned)edit_len,&editors.win[i].draft)||
                           !edit_binding_matches(target,&editors.win[i].binding.source)){
                            edit_file=-1;edit_identity=0;edit_source.valid=0;
                        }
                    }else session_status="Recovery draft exceeds the Editor limit.";
                }
                edit_caret=v->caret>=0&&v->caret<=edit_len?v->caret:edit_len;edit_sel_a=edit_sel_b=edit_caret;
            }
        }
    }
    id=fs_find_child(dir,"paint-draft");if(id>=0&&fs_size(id)==PAINT_W*PAINT_H){paint_init();kmemcpy(paint_pix,fs_data(id),PAINT_W*PAINT_H);}
    session_ready=1;context_set(win_front());dirty=1;
}
int program_key(void){ return desktop_program_key(); }
void program_present(void){cursor_restore();draw_ui();flip_vga();cursor_on=0;dirty=1;}
static void render_scene(void) {
                int isel = icon_sel;
                if (fm_drag_active && icon_hit(DESK_TRASH, mouse_x, mouse_y))
                    isel = DESK_TRASH;
                draw_desktop();
                draw_desktop_icons(isel);
                draw_ui();
                if (open_dlg)
                    draw_open_dialog();
                draw_taskbar();
                if (MODERN)
                    draw_topbar();
                draw_volume_popup();
                draw_pulldown(open_menu, menu_sel);
                draw_drag_ghost();
                draw_rename_overlay();
                draw_launcher();
                draw_switcher();
                draw_namedlg();
                draw_edit_close();
}
/* The outline drawn instead of the window while it is dragged with
 * "Show window contents" off. */
static void draw_drag_outline(void) {
    if (dragging_win < 0 || !drag_active) return;
    Win *w = &wins[dragging_win];
    draw_frame(drag_outline_x, drag_outline_y, w->w, w->h, TK_TEXT);
}
static void draw_display_confirmation(void) {
    if (!display_pending) return;
    int x = (fb_w - 420) / 2, y = (fb_h - 150) / 2;
    shade_rect(0, 0, fb_w, fb_h, 1);
    draw_round_rect(x, y, 420, 150, 10, INK(1));
    draw_round_frame(x, y, 420, 150, 10, ui_border);
    draw_string_bold("Keep this display resolution?", x + 24, y + 20, ui_text);
    char seconds[16], message[80];
    unsigned ticks_left = display_deadline - timer_ticks();
    fmt_uint(seconds, (ticks_left + TIMER_HZ - 1) / TIMER_HZ);
    kstrcpy(message, "Reverting automatically in ");
    kstrcpy(message + kstrlen(message), seconds);
    kstrcpy(message + kstrlen(message), " seconds.");
    draw_string(message, x + 24, y + 54, ui_text_dim);
    draw_default_button(x + 34, y + 98, 164, 30, "Keep (Enter)");
    draw_button(x + 222, y + 98, 164, 30, "Revert (Esc)");
}

static void display_refresh_layout(void) {
    desktop_input_fence();
    input_resize(&device_input, fb_w, fb_h);
    cursor_on = 0;
    dragging_win = resizing_win = -1;
    drag_cached = -1;
    open_menu = MENU_NONE;
    launcher_on = 0;
    theme_apply();
    icons_init();
    for (int i = 0; i < MAX_WIN; ++i) if (wins[i].open) win_clamp(&wins[i]);
    if (mouse_x >= fb_w) mouse_x = fb_w - 1;
    if (mouse_y >= fb_h) mouse_y = fb_h - 1;
    dirty = 1;
}
static int display_request(int mode) {
    if (display_pending || mode == display_current_mode()) return 0;
    int old = display_current_mode();
    if (old < 0) { display_message = "Current mode cannot be restored safely."; return 0; }
    kmemcpy(display_saved_windows, wins, sizeof wins);
    cursor_restore();
    if (!display_set_mode(mode)) {
        display_message = "Resolution switching requires QEMU standard VGA.";
        dirty = 1; return 0;
    }
    display_previous = old;
    display_pending = 1;
    display_deadline = timer_ticks() + 15 * TIMER_HZ;
    display_message = "";
    display_refresh_layout();
    return 1;
}
static void display_revert(void);
static void display_keep(void);
static void display_click(void) {
    int x = (fb_w - 420) / 2, y = (fb_h - 150) / 2;
    if (hit(mouse_x, mouse_y, x + 34, y + 98, 164, 30)) display_keep();
    else if (hit(mouse_x, mouse_y, x + 222, y + 98, 164, 30)) display_revert();
}
static void display_revert(void) {
    if (!display_pending) return;
    cursor_on = 0;
    if (!display_set_mode(display_previous)) {
        display_message = "Could not restore the previous display mode.";
        display_pending = 0; dirty = 1; return;
    }
    kmemcpy(wins, display_saved_windows, sizeof wins);
    display_pending = 0;
    display_refresh_layout();
    platform_log("DISPLAY-REVERTED\n");
}
static void display_keep(void) {
    if (!display_pending) return;
    display_pending = 0;
    preference_queue(PREF_DISPLAY, (char)('0' + display_current_mode()));
    display_message = "";
    dirty = 1;
}

static void display_load(void) {
    int mode = preference_read(PREF_DISPLAY);
    if (mode >= 0) display_set_mode(mode);
}

static void render_desktop_frame(void) {
    int moving=dragging_win>=0 && dragging_win==win_front() && drag_active && !open_dlg && !name_dlg && !edit_close_dlg && !launcher_on && open_menu<0;
    int outline=!(win_options&WINOPT_DRAG_CONTENTS);
    if(moving){
        if(drag_cached!=dragging_win){
            render_skip=outline?-1:dragging_win;render_scene();render_skip=-1;
            kmemcpy((void *)DRAG_CACHE,fb,fb_w*fb_h);drag_cached=dragging_win;
        }else kmemcpy(fb,(void *)DRAG_CACHE,fb_w*fb_h);
        if(outline)draw_drag_outline();
        else {draw_one_window(&wins[dragging_win],0);context_set(win_front());}
    }else {drag_cached=-1;render_scene();}
}

/* Partial repaint and canvas-only occlusion both need unchanged composition.
 * In particular, a drag cache may contain pixels hidden in the current scene. */
static int client_scene_stable(void) {
    return !dirty&&!name_dlg&&!edit_close_dlg&&!open_dlg&&
        !launcher_on&&open_menu<0&&!display_pending&&!saver_on&&
        dragging_win<0&&resizing_win<0&&drag_cached<0&&!fm_dragging&&!fm_drag_active&&!fm_renaming&&
        !edit_dragging&&!paint_dragging&&!paint_shape_drag&&!mouse_left;
}
/* Only an unchanged front client can be repainted without rebuilding the
 * scene. Keep video, audio progress and native canvas guards in one place. */
static int partial_client_ready(int slot) {
    return slot>=0&&slot<MAX_WIN&&wins[slot].open&&!wins[slot].min&&
        slot==win_front()&&client_scene_stable();
}
/* Suppress buffer updates only when the whole terminal, including its border,
 * fits in a higher window's opaque client interior. The eight-pixel inset
 * excludes every rounded corner; outer-window containment alone is unsafe. */
static int window_content_hidden(int slot) {
    const Win *w=&wins[slot];
    if(!w->open||w->min)return 1;
    for(int i=0;i<MAX_WIN;i++){
        const Win *cover=&wins[i];
        if(!cover->open||cover->min||cover->z<=w->z)continue;
        if(w->x-1>=cover->x+8&&w->y-1>=cover->y+TITLE_H+1&&
           w->x+w->w+1<=cover->x+cover->w-8&&
           w->y+w->h+1<=cover->y+cover->h-8)return 1;
    }
    return 0;
}
/* CANVAS-only updates cannot change text, layout, chrome or taskbar labels.
 * Test their exact scaled damage rectangle, using the owner's published frame
 * rather than the selected Terminal. Retain the same conservative opaque
 * client insets as whole-window occlusion; never trust rounded edge pixels. */
static int term_canvas_hidden(int slot) {
    const Win *w=&wins[slot];
    int source_w,source_h,target_w,target_h;
    if(!term_canvas_size(slot,&source_w,&source_h))return 0;
    term_canvas_geometry(w->w,w->h,source_w,source_h,&target_w,&target_h);
    if(target_w<=0||target_h<=0)return 0;
    int x=w->x+1+TERM_PAD,y=w->y+TITLE_H+1+TERM_PAD;
    for(int i=0;i<MAX_WIN;i++){
        const Win *cover=&wins[i];
        if(!cover->open||cover->min||cover->z<=w->z)continue;
        if(x>=cover->x+8&&y>=cover->y+TITLE_H+1&&
           x+target_w<=cover->x+cover->w-8&&
           y+target_h<=cover->y+cover->h-8)return 1;
    }
    return 0;
}
enum { TERM_RENDER_NONE, TERM_RENDER_FULL, TERM_RENDER_CANVAS };
static int term_task_render_action(TermTaskUpdate update) {
    int slot=update.slot;
    if(!update.flags||slot<0||slot>=MAX_WIN||!wins[slot].open)
        return TERM_RENDER_NONE;
    if(wins[slot].kind==WK_NATIVE){
        if(update.flags&APP_VIEW_LIFECYCLE)return TERM_RENDER_FULL;
        if(window_content_hidden(slot))return TERM_RENDER_NONE;
        if(update.flags==APP_VIEW_OUTPUT&&!app_view_output_visible(slot))return TERM_RENDER_NONE;
        return TERM_RENDER_FULL; /* Owned updates initially use full composition. */
    }
    if(wins[slot].kind!=WK_TERM)return TERM_RENDER_NONE;
    /* A hidden task can still change its visible taskbar label on exit. */
    if(update.flags&TERM_TASK_LIFECYCLE)return TERM_RENDER_FULL;
    if(window_content_hidden(slot))return TERM_RENDER_NONE;
    if(update.flags==TERM_TASK_CANVAS&&client_scene_stable()&&term_canvas_hidden(slot))
        return TERM_RENDER_NONE;
    if(update.flags==TERM_TASK_CANVAS&&partial_client_ready(slot))return TERM_RENDER_CANVAS;
    return TERM_RENDER_FULL;
}

/* Trusted native-host adapter. Every snapshot independently checks process,
 * Terminal incarnation and published geometry; it never changes selection. */
static int native_host_snapshot(const ProcessBinding *binding,NativeUiHost *out) {
    if(!binding||binding->slot>=MAX_WIN||!process_binding_live(binding)||!app_view_binding_matches(binding))return 0;
    const Win *w=&wins[binding->slot];
    if(!w->open||(w->kind!=WK_TERM&&w->kind!=WK_NATIVE))return 0;
    int owned=w->kind==WK_NATIVE;
    if(owned!=app_view_owned_binding(binding))return 0;
    *out=(NativeUiHost){0};
    out->kind=owned?BOS_UI_KIND_OWNED_WINDOW:BOS_UI_KIND_HOSTED_CANVAS;
    out->capabilities=owned?NATIVE_UI_CAPABILITIES_OWNED:NATIVE_UI_CAPABILITIES_HOSTED;
    out->view=native_canvas_geometry((int)binding->slot);
    int output=owned&&app_view_output_visible((int)binding->slot);
    if(w->min)out->state|=BOS_UI_STATE_MINIMIZED;
    if(desktop_input_blocked()||output)out->state|=BOS_UI_STATE_BLOCKED;
    if(!w->min&&!output&&out->view.viewport_w>0&&out->view.viewport_h>0)out->state|=BOS_UI_STATE_AVAILABLE;
    if(!w->min&&!output&&!desktop_input_blocked()&&(int)binding->slot==win_front())out->state|=BOS_UI_STATE_FOCUSED;
    return 1;
}
static void native_host_acquired(NativeUiAcquired *out) {
    unsigned partial=device_input.packet_n?device_input.packet[0]&(INPUT_LEFT|INPUT_RIGHT):0;
    unsigned unknown=input_unknown_buttons;
    if(device_input.loss&INPUT_LOSS_DEVICE)unknown=INPUT_LEFT|INPUT_RIGHT;
    *out=(NativeUiAcquired){device_input.serial,device_input.buttons|partial|unknown,timer_ticks()};
}
static void native_host_focus(const ProcessBinding *binding) {
    NativeUiHost host;
    if(native_host_snapshot(binding,&host)&&(host.state&BOS_UI_STATE_AVAILABLE)&&
       !(host.state&BOS_UI_STATE_BLOCKED))win_focus((int)binding->slot);
}
static unsigned desktop_native_pointer(const InputSample *sample) {
    native_ui_refresh(sample->ticks,sample->buttons);
    BosHandle target=0;
    /* Existing shell drags retain their own gesture. A later chord over a
     * native canvas cannot steal an in-progress title/built-in interaction. */
    if(!mouse_left&&!mouse_right&&!desktop_input_blocked()&&sample->x>=0&&sample->x<fb_w&&
       sample->y>=MENUBAR_H&&sample->y<TASKBAR_Y){
        int slot=-1;
        for(int i=0;i<MAX_WIN;i++)if(wins[i].open&&!wins[i].min&&
           hit(sample->x,sample->y,wins[i].x,wins[i].y,wins[i].w,wins[i].h)&&
           (slot<0||wins[i].z>wins[slot].z))slot=i;
        if(slot>=0&&(wins[slot].kind==WK_TERM||wins[slot].kind==WK_NATIVE)&&
           !(wins[slot].kind==WK_NATIVE&&app_view_output_visible(slot))){
            CanvasView view=native_canvas_geometry(slot);
            if(canvas_view_contains(&view,sample->x,sample->y))target=native_ui_target_at((unsigned)slot);
        }
    }
    return native_ui_route(sample,target);
}

/* Desktop-only compatibility adapter for the one ordered device stream. */
typedef struct {
    Win win[MAX_WIN];
    int menu, picker, name, close, launcher, saver, display;
    int valid;
} InputScene;
static InputScene input_scene;
static int desktop_input_scene_changed(void) {
    if (!input_scene.valid || input_scene.menu != open_menu ||
        input_scene.picker != open_dlg || input_scene.name != name_dlg ||
        input_scene.close != edit_close_dlg || input_scene.launcher != launcher_on ||
        input_scene.saver != saver_on || input_scene.display != display_pending) return 1;
    for (int i=0;i<MAX_WIN;++i) {
        Win *a=&input_scene.win[i], *b=&wins[i];
        if (a->open!=b->open || (b->open && (a->seq!=b->seq || a->kind!=b->kind ||
            a->min!=b->min || a->z!=b->z || a->x!=b->x || a->y!=b->y ||
            a->w!=b->w || a->h!=b->h))) return 1;
    }
    return 0;
}
static void desktop_input_remember_scene(void) {
    kmemcpy(input_scene.win,wins,sizeof wins);
    input_scene.menu=open_menu; input_scene.picker=open_dlg; input_scene.name=name_dlg;
    input_scene.close=edit_close_dlg; input_scene.launcher=launcher_on;
    input_scene.saver=saver_on; input_scene.display=display_pending; input_scene.valid=1;
}
static int desktop_input_blocked(void) {
    return open_menu>=0 || open_dlg || name_dlg || edit_close_dlg || launcher_on || saver_on || display_pending;
}
static void desktop_input_modifiers(unsigned modifiers) {
    shift_down=!!(modifiers&INPUT_SHIFT); ctrl_down=!!(modifiers&INPUT_CTRL); alt_down=!!(modifiers&INPUT_ALT);
}
static void desktop_input_cancel(unsigned buttons) {
    native_ui_cancel_all(BOS_UI_REASON_SCENE,input_routing?input_sample_ticks:timer_ticks(),buttons);
    /* Cancellation never commits a file drop, snapped window or Paint shape. */
    dragging_win=resizing_win=-1; drag_active=fm_dragging=fm_drag_active=0;
    for(int i=0;i<MAX_WIN;++i) window_state[i].doc.dragging=0;
    writer_release(); spreadsheet_release();
    if(paint_shape_drag)dirty=1;
    paint_dragging=paint_shape_drag=0;
    mouse_left=mouse_right=0;
    input_suppressed=buttons;
}
static void desktop_input_clear_clicks(void) {
    title_click_window=icon_last=pick_last_click_item=-1;
    for(int i=0;i<MAX_WIN;++i)window_state[i].last_click_item=-1;
}
static void desktop_input_fence(void) {
    input_pointer_fence(&device_input);
    unsigned partial=device_input.packet_n ? device_input.packet[0]&(INPUT_LEFT|INPUT_RIGHT) : 0;
    desktop_input_cancel(device_input.buttons|partial);
    desktop_input_clear_clicks();
}
static void desktop_pointer_motion(void) {
        win_resize_tick();
        if (dragging_win >= 0 && mouse_left && !open_dlg) {
            Win *dw = &wins[dragging_win];
            int dx = mouse_x - drag_from_x;
            int dy = mouse_y - drag_from_y;
            int adx = dx < 0 ? -dx : dx;
            int ady = dy < 0 ? -dy : dy;
            if (!drag_active && (adx > 3 || ady > 3))
                drag_active = 1;
            if (drag_active && dw->maximized) {
                /* Pull a snapped window off its snap: restore it with the
                 * pointer at the same relative spot of the title bar. */
                int rel = dw->w > 0 ? (drag_from_x - dw->x) * dw->old_w / dw->w : 0;
                win_arrange(dragging_win, 3);
                drag_orig_x = drag_from_x - rel;
                drag_orig_y = drag_from_y - TITLE_BAR / 2;
            }
            if (drag_active) {
                int nx = drag_orig_x + dx;
                int ny = drag_orig_y + dy;
                win_snap_edges(&nx, &ny, dw->w, dw->h);
                if (win_options & WINOPT_DRAG_CONTENTS) {
                    if (nx != dw->x || ny != dw->y) {
                        dw->x = nx;
                        dw->y = ny;
                        win_clamp(dw);
                        dirty = 1;
                    }
                } else {
                    Win probe = *dw;
                    probe.x = nx;
                    probe.y = ny;
                    win_clamp(&probe);
                    if (probe.x != drag_outline_x || probe.y != drag_outline_y) {
                        drag_outline_x = probe.x;
                        drag_outline_y = probe.y;
                        dirty = 1;
                    }
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

        if (front_kind() == WK_WRITER && mouse_left && mouse_moved && !open_dlg &&
            !name_dlg && !edit_close_dlg && !launcher_on && open_menu < 0 && dragging_win < 0) {
            Win *w = &wins[win_front()];
            if (writer_drag(w->x+1,w->y+TITLE_H+1,w->w-2,w->h-TITLE_H-2,mouse_x,mouse_y)) dirty=1;
        }
        if (front_kind() == WK_SPREADSHEET && mouse_left && mouse_moved && !open_dlg &&
            !name_dlg && !edit_close_dlg && !launcher_on && open_menu < 0 && dragging_win < 0) {
            Win *w = &wins[win_front()];
            if (spreadsheet_drag(w->x+1,w->y+TITLE_H+1,w->w-2,w->h-TITLE_H-2,mouse_x,mouse_y)) dirty=1;
        }
        if (paint_dragging || paint_shape_drag) paint_drag_tick();
        if (set_slider_drag) settings_drag();
}
static void desktop_pointer_release(void) {
            resizing_win=-1;
            if (dragging_win >= 0 && drag_active) {
                Win *dw = &wins[dragging_win];
                if (!(win_options & WINOPT_DRAG_CONTENTS)) {
                    dw->x = drag_outline_x;
                    dw->y = drag_outline_y;
                    win_clamp(dw);
                    dirty = 1;
                }
                if (win_options & WINOPT_SNAP) {
                    if (mouse_x < 2) win_arrange(dragging_win,1);
                    else if (mouse_x >= fb_w-2) win_arrange(dragging_win,2);
                    else if (mouse_y <= (MODERN ? MENUBAR_H : 1)) win_arrange(dragging_win,0);
                }
            }
            if (dragging_win >= 0 && !drag_active && !open_dlg &&
                wins[dragging_win].kind == WK_FILES && fm_cwd != fs_root())
                fm_go_up();
            dragging_win = -1;
            drag_active = 0;
            if (fm_dragging)
                files_drop();
            edit_dragging = 0;
            if (set_slider_drag) settings_drag();
            writer_release();
            spreadsheet_release();
            paint_mouse_up();
}
static void desktop_pointer_sample(const InputSample *sample) {
    if(sample->epoch!=device_input.epoch)return;
    unsigned prior=(mouse_left?INPUT_LEFT:0)|(mouse_right?INPUT_RIGHT:0);
    input_suppressed &= sample->buttons;
    input_unknown_buttons &= sample->buttons;
    unsigned buttons=sample->buttons & ~(input_suppressed|input_unknown_buttons);
    mouse_moved=sample->x!=mouse_x || sample->y!=mouse_y;
    input_cursor_moved |= mouse_moved;
    mouse_x=sample->x; mouse_y=sample->y;
    unsigned native=desktop_native_pointer(sample);
    if(native&NATIVE_UI_CONSUMED_POINTER){
        mouse_left=mouse_right=0;
        if(sample->wheel&&!(native&NATIVE_UI_CONSUMED_WHEEL))handle_wheel(sample->wheel);
        if(mouse_moved&&dragging_win<0){int h=taskbar_hover_at();if(h!=taskbar_hover){taskbar_hover=h;dirty=1;}}
        return;
    }
    /* Final release coordinates belong to the gesture being released. */
    if(mouse_moved && (prior&INPUT_LEFT)) desktop_pointer_motion();
    mouse_left=!!(buttons&INPUT_LEFT); mouse_right=!!(buttons&INPUT_RIGHT);
    if(open_menu>=0 && open_menu!=MENU_BAR && mouse_moved) menu_hover();
    if(volume_on && mouse_left && mouse_moved) volume_drag();
    if((buttons&INPUT_RIGHT) && !(prior&INPUT_RIGHT))handle_rclick();
    if((buttons&INPUT_LEFT) && !(prior&INPUT_LEFT))handle_click();
    if(sample->epoch!=device_input.epoch)return;
    /* Existing Paint stamps its selected brush once on DOWN, even with no
     * motion; the old per-turn drag tick supplied this initial application. */
    if((buttons&INPUT_LEFT) && !(prior&INPUT_LEFT) && mouse_left)desktop_pointer_motion();
    if((prior&INPUT_LEFT) && !(buttons&INPUT_LEFT))desktop_pointer_release();
    if(sample->wheel&&!(native&NATIVE_UI_CONSUMED_WHEEL))handle_wheel(sample->wheel);
    /* A dismissal click is already consumed; held gestures cannot click through
     * a newly opened menu/modal or continue behind it. Focus by DOWN is ordered
     * normally, so the same queued DOWN/UP survives without an epoch change. */
    if(desktop_input_blocked())desktop_input_cancel(sample->buttons);
    native_ui_refresh(sample->ticks,sample->buttons);
    if(mouse_moved && dragging_win<0){int h=taskbar_hover_at();if(h!=taskbar_hover){taskbar_hover=h;dirty=1;}}
}
static void desktop_keyboard_loss(void) {
    /* Lost focus shortcuts make the intended Terminal owner unknowable. Keep
     * every open Terminal draft, but require a complete line before execution. */
    for(int i=0;i<MAX_WIN;++i)if(wins[i].open&&wins[i].kind==WK_TERM)term_input_lost(i);
    dirty=1;
}
static void desktop_input_reset(const InputSample *sample) {
    if(sample->kind==INPUT_RESET)desktop_keyboard_loss();
    if(sample->reason&INPUT_LOSS_DEVICE)input_unknown_buttons=INPUT_LEFT|INPUT_RIGHT;
    desktop_input_cancel(sample->buttons);
    native_ui_input_loss(sample->ticks,sample->buttons|input_unknown_buttons,sample->dropped);
    desktop_input_clear_clicks();
    input_cursor_moved |= mouse_x!=sample->x || mouse_y!=sample->y;
    mouse_x=sample->x;mouse_y=sample->y;
    desktop_input_modifiers(sample->modifiers);
}
/* Alt tapped alone focuses the menu row; releasing Alt commits the
 * window switcher. */
static void desktop_modifier_sample(const InputSample *sample, int was_alt) {
    if (sample->kind != INPUT_MODIFIER) return;
    int alt_key = sample->scancode == 0x38;
    if (alt_key && (sample->flags & INPUT_MAKE)) { if (!was_alt) alt_tap = 1; return; }
    if (!alt_key) { alt_tap = 0; return; }
    if (alt_down) return;
    if (switcher_on) { switcher_commit(); alt_tap = 0; return; }
    if (alt_tap && !mouse_left && !mouse_right && !name_dlg && !open_dlg && !launcher_on &&
        !edit_close_dlg && !display_pending) {
        if (open_menu >= 0) menu_close();
        else menu_focus_row();
    }
    alt_tap = 0;
}
/* Super (E0 5B/5C): held for shortcuts, tapped alone for the launcher.
 * Returns 1 when the sample was the Super key itself. */
static int desktop_super_sample(const InputSample *sample) {
    if (!(sample->flags & INPUT_EXTENDED) || (sample->scancode != 0x5b && sample->scancode != 0x5c)) return 0;
    if (sample->flags & INPUT_MAKE) {
        if (!super_down) super_used = 0;
        super_down = 1;
        return 1;
    }
    if (super_down && !super_used && !name_dlg && !open_dlg && !edit_close_dlg && !display_pending) {
        if (launcher_on) launcher_close();
        else { if (open_menu >= 0) menu_close(); launcher_open(); }
    }
    super_down = 0;
    return 1;
}
static unsigned desktop_input_turn(void) {
    if(input_tail_pending) {
        /* An unusually full controller may need more than the exclusive end
         * budget. Finish draining across ordinary turns, never replay its tail. */
        input_tail_pending=input_acquire(32)==32;
        if(device_input.loss)desktop_keyboard_loss();
        if(device_input.loss&INPUT_LOSS_DEVICE)input_unknown_buttons=INPUT_LEFT|INPUT_RIGHT;
        if(device_input.loss)native_ui_input_loss(device_input.ticks,device_input.buttons|input_unknown_buttons,device_input.dropped);
        input_discard(&device_input);desktop_input_fence();
        desktop_input_modifiers(device_input.modifiers);
        mouse_moved |= mouse_x!=device_input.x || mouse_y!=device_input.y;
        mouse_x=device_input.x;mouse_y=device_input.y;
        last_input_frame=frame_count;desktop_input_remember_scene();
        return INPUT_BATCH;
    }
    /* Changes made by a routed record are remembered immediately. Only an
     * autonomous scene change fences backlog, including same-slot reuse. */
    if(desktop_input_scene_changed())desktop_input_fence();
    native_ui_refresh(timer_ticks(),device_input.buttons);
    desktop_input_remember_scene();
    input_cursor_moved=mouse_moved;
    unsigned count=0;
    InputSample sample;
    while(count<INPUT_BATCH && input_pop(&device_input,&sample)) {
        ++count;last_input_frame=frame_count;
        input_routing=1;input_sample_ticks=sample.ticks;
        if(saver_on) {
            int wake=(sample.kind==INPUT_KEY || sample.kind==INPUT_MODIFIER) && (sample.flags&INPUT_MAKE);
            if(sample.kind==INPUT_POINTER) {
                wake=sample.x!=mouse_x || sample.y!=mouse_y || sample.wheel || (sample.buttons&~input_saver_buttons);
                input_saver_buttons=sample.buttons;
                mouse_x=sample.x;mouse_y=sample.y;
            }
            if(sample.kind==INPUT_RESET)desktop_input_reset(&sample);
            desktop_input_modifiers(sample.modifiers);
            if(wake) {
                /* Bytes already collected belong to the wake backlog too. */
                input_acquire_backlog();
                input_wake_serial=device_input.serial;
                saver_stop();desktop_input_fence();
                input_cursor_moved=1;
                mouse_x=device_input.x;mouse_y=device_input.y;
            }
        }
        else if(sample.kind==INPUT_RESET)desktop_input_reset(&sample);
        else if(sample.serial<=input_wake_serial) {
            /* Consume the complete wake backlog across bounded turns. */
            desktop_input_modifiers(sample.modifiers);
        }
        else if(sample.kind==INPUT_POINTER) {
            if(sample.epoch==device_input.epoch) {
                desktop_input_modifiers(sample.modifiers);
                desktop_pointer_sample(&sample);
            }
        } else {
            int was_alt=alt_down;
            desktop_input_modifiers(sample.modifiers);
            desktop_modifier_sample(&sample,was_alt);
            if(sample.kind==INPUT_KEY && (sample.flags&INPUT_MAKE) && !desktop_super_sample(&sample)) {
                key_sc=(uint8_t)sample.scancode;key_char=(char)sample.character;
                key_ext=!!(sample.flags&INPUT_EXTENDED);
                alt_tap=0;
                handle_key();
                if(desktop_input_scene_changed())desktop_input_cancel(sample.buttons);
            } else if(sample.kind==INPUT_KEY) desktop_super_sample(&sample);
        }
        native_ui_refresh(sample.ticks,sample.buttons);
        input_routing=0;
        desktop_input_remember_scene();
    }
    mouse_moved=input_cursor_moved;
    return count;
}
static void desktop_program_input(int active) {
    /* Preserve legacy keyboard typeahead when the synchronous owner returns.
     * Pointer backlog is fenced; it cannot click the resumed desktop. */
    if(active)input_sync_loss=0;
    else {
        input_tail_pending=input_acquire(1024)==1024;
        if(input_sync_loss)desktop_keyboard_loss();
        input_sync_loss=0;
    }
    desktop_input_fence();
    if(!active) {
        if(device_input.loss&INPUT_LOSS_DEVICE)input_unknown_buttons=INPUT_LEFT|INPUT_RIGHT;
        desktop_input_modifiers(device_input.modifiers);
        input_cursor_moved |= mouse_x!=device_input.x || mouse_y!=device_input.y;
        mouse_x=device_input.x;mouse_y=device_input.y;
    }
}
static int desktop_program_key(void) {
    /* Synchronous BASIC/exec temporarily owns this stream. Its old INKEY
     * last-character behavior stays, but no pointer click is saved for replay
     * into whatever desktop window happens to be present when it returns. */
    drain_8042();
    InputSample sample;int result=0;
    unsigned available=device_input.count+(device_input.loss!=0);
    for(unsigned n=0;n<available && input_pop(&device_input,&sample);++n) {
        desktop_input_modifiers(sample.modifiers);
        if(sample.kind==INPUT_KEY && (sample.flags&INPUT_MAKE)) {
            if(sample.character)result=(int)sample.character;
            else if(sample.scancode==KEY_ESC)result=27;
        } else if(sample.kind==INPUT_POINTER || sample.kind==INPUT_RESET) {
            /* A legacy KEY syscall consumes samples, never re-enters another
             * UI handler. The begin hook already cancelled desktop gestures. */
            if(sample.kind==INPUT_RESET) {
                input_sync_loss|=sample.reason;
                if(sample.reason&INPUT_LOSS_DEVICE)input_unknown_buttons=INPUT_LEFT|INPUT_RIGHT;
            } else input_unknown_buttons&=sample.buttons;
            input_suppressed=sample.buttons;
            input_cursor_moved |= mouse_x!=sample.x || mouse_y!=sample.y;
            mouse_x=sample.x;mouse_y=sample.y;
        }
    }
    return result;
}

/* ---------- Shell contract (kernel/shell.h) ---------- */

static uint32_t last_blink, last_clock, last_session;

/* Storage is mounted and input is not live yet. display_load may change the
 * video mode, so the kernel sizes the pointer after this returns. */
static void desktop_prepare(void) {
    kmemset(window_state, 0, sizeof(WindowState) * MAX_WIN);
    poll_time();
    trash_id = fs_find_child(fs_root(), "trash");
    prefs_id = fs_find_child(fs_root(), "prefs");
    provision_system_files(edit_scratch, EDIT_BUF_SIZE);
    theme_load();
    saver_load();
    display_load();
    theme_apply();
    term_reset();
    term_set_program_input(desktop_program_input);
    term_set_native_launch(terminal_native_launch);
    term_set_program_io(program_key, program_present);

    menu_bar_init();
    icons_init();
    mouse_x = fb_w / 2;
    mouse_y = fb_h / 2;
}

static void desktop_start(void) {
    if(kernel_pointer_ready()){const NativeUiHooks native_hooks={native_host_snapshot,native_host_acquired,native_host_focus};native_ui_init(&native_hooks);}
    boot_splash();
    session_restore();
#ifdef FEATURE_TEST
    extern void feature_test(void);feature_test();
#endif
    last_input_frame = frame_count;
    last_session = frame_count;
}

/* One cooperative frame. The kernel has already pumped storage and devices. */
static int desktop_turn(void) {
    if (kernel_events_take(KERNEL_EVENT_STORAGE)) dirty = 1;
    document_persistence_poll();
    preferences_tick();
    if(todo_tick()&&find_open_kind(WK_TODO)>=0)dirty=1;
    if(cal_agenda_tick()&&find_open_kind(WK_CAL)>=0)dirty=1;
    poll_time();
    if(kernel_events_take(KERNEL_EVENT_DOWNLOADS)){
        int original=context_slot;
        for(int i=0;i<MAX_WIN;i++)if(wins[i].open&&wins[i].kind==WK_FILES){context_set(i);fm_refresh();}
        context_set(original);dirty=1;
    }
    if(browser_tick()&&find_open_kind(WK_BROWSER)>=0)dirty=1;
    if(writer_tick()&&find_open_kind(WK_WRITER)>=0)dirty=1;
    if(spreadsheet_tick()&&find_open_kind(WK_SPREADSHEET)>=0)dirty=1;
    int player_update=player_tick();
    int player_slot=find_open_kind(WK_PLAYER);
    if(player_update==PLAYER_CHANGED&&player_slot>=0&&!wins[player_slot].min)dirty=1;
    term_elf_poll();
    TermTaskUpdate term_update=app_view_poll_update();
    native_window_complete(term_update);
    /* Publication/clear changes become visible to input before the next
     * routed sample, including when rendering is occluded or deferred. */
    native_ui_refresh(timer_ticks(),device_input.buttons);
    if (display_pending && (int32_t)(timer_ticks() - display_deadline) >= 0) display_revert();
    context_set(win_front());
    drain_8042();

    unsigned input_count = desktop_input_turn();
    if (saver_on) {
        if(fs_needs_sync())fs_autosync();
        saver_frame();
        return 1;
    }
    if (!input_count && saver_enabled && !display_pending &&
        video_status()->state!=VIDEO_PLAYING && video_status()->state!=VIDEO_LOADING &&
        frame_count-last_input_frame>70u*saver_delay_secs[saver_delay] && !open_dlg && !name_dlg && !edit_close_dlg) {
        saver_start();
    }

    /* Flush changed files once the mouse is up, so a save or a drag
     * lands on the floppy without waiting for Shutdown. */
    if (!mouse_left && !fm_renaming && fs_needs_sync()) {
        const char *before = fs_storage_status();
        fs_autosync();
        if (before != fs_storage_status()) dirty = 1;
    }
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

    if(!fs_sync_busy()&&!display_pending&&frame_count-last_session>=5*TIMER_HZ&&frame_count-last_input_frame>=TIMER_HZ&&!mouse_left&&!name_dlg&&!edit_close_dlg&&!open_dlg){session_save();last_session=frame_count;}
    if(drag_cached>=0 && dragging_win<0)dirty=1;
    if(mouse_moved&&dragging_win<0){int h=taskbar_hover_at();if(h!=taskbar_hover){taskbar_hover=h;dirty=1;}}
    int moved = mouse_moved;
    mouse_moved = 0;
    int term_action=term_task_render_action(term_update);
    if(term_action==TERM_RENDER_FULL)dirty=1;
    int playback_update=player_update==PLAYER_VIDEO_FRAME||player_update==PLAYER_AUDIO_PROGRESS;
    int playback_only=playback_update&&partial_client_ready(player_slot);
    if(playback_update&&player_slot>=0&&!wins[player_slot].min&&!playback_only)dirty=1;
    int canvas_only=term_action==TERM_RENDER_CANVAS&&!dirty;
    if (dirty || playback_only || canvas_only || moved || !cursor_on) {
        int ox = cursor_sx, oy = cursor_sy, oon = cursor_on;
        cursor_restore();
        if (dirty || playback_only || canvas_only) {
            if(dirty){
                render_desktop_frame();
                draw_display_confirmation();
            }else if(canvas_only){
                Win *w=&wins[term_update.slot];
                context_set(term_update.slot);
                draw_term_canvas(w->x,w->y,w->w,w->h);
            }else{
                Win *w=&wins[player_slot];
                context_set(player_slot);
                if(player_update==PLAYER_AUDIO_PROGRESS)
                    player_draw_audio_progress(w->x+1,w->y+TITLE_H+1,w->w-2,w->h-TITLE_H-2);
                else player_draw_playback(w->x+1,w->y+TITLE_H+1,w->w-2,w->h-TITLE_H-2);
            }
            redraw_count++;
            dirty = 0;
            cursor_save_draw();
            gfx_present();
            if(redraw_count==1)platform_log("DESKTOP-READY\n");
        } else {
            cursor_save_draw();
            if (oon)
                flip_rect(ox, oy, CURSOR_W, CURSOR_H);
            flip_rect(cursor_sx, cursor_sy, CURSOR_W, CURSOR_H);
        }
    }
    return dirty||mouse_left||mouse_moved;
}

const Shell baseos_shell = {"desktop", desktop_prepare, desktop_start, desktop_turn};
