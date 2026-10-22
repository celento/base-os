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
#include "apps/pane_chrome.h"
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
static char launch_buf[FS_NAME_LEN];
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

/* App icon (AI_*) shown in the title bar of the window being drawn, or -1,
 * and the kind whose menu model the window row shows. */
static int chrome_icon = -1;
static int chrome_dirty;     /* draw an unsaved-changes dot after the title */
static const char *chrome_subtitle; /* dim text after the title */
static int chrome_kind = -1;
static int chrome_slot = -1;
static int chrome_flags;  /* WIN_MAXED and WIN_DIALOG for the window being drawn */

/* Hover and press state for a control under the pointer. */
static int ctl_state(int x, int y, int w, int h) {
    if (!hit(mouse_x, mouse_y, x, y, w, h)) return 0;
    return mouse_left ? KIT_HOVER | KIT_PRESSED : KIT_HOVER;
}

typedef struct { int x, y, w, h; } Rect;

/* Hover targets: drawing registers each control that reacts to the
 * pointer, and pointer motion repaints only when the target under it
 * changes. */
#define HOT_MAX 96
static Rect hot_rect[HOT_MAX];
static int hot_n, hot_last = -1;
static int hot(int x, int y, int w, int h) {
    if (hot_n < HOT_MAX) hot_rect[hot_n++] = (Rect){x, y, w, h};
    return ctl_state(x, y, w, h);
}
static int hot_index(void) {
    for (int i = hot_n - 1; i >= 0; i--)
        if (hit(mouse_x, mouse_y, hot_rect[i].x, hot_rect[i].y, hot_rect[i].w, hot_rect[i].h)) return i;
    return -1;
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
        /* The unsaved dot: a small bullet one space after the name. */
        if (chrome_dirty) xmax -= 10;
        kit_text(title, tx, ty, title_ink, xmax - tx, 1);
        if (chrome_dirty) {
            int dx = tx + kit_text_w(title, 1) + ui_advance(' ') + 1;
            if (dx > xmax + 4) dx = xmax + 4;
            kit_fill(dx, ty + UI_FONT_H / 2 - 2, 5, 5, 2, MODERN ? TK_TEXT2 : title_ink, 0);
        }
        if (chrome_subtitle) {
            int sx = tx + kit_text_w(title, 1) + 6;
            if (sx < xmax - 24) kit_text(chrome_subtitle, sx, ty, MODERN ? TK_TEXT2 : active ? TK_ACCENT_100 : TK_LINE_STRONG, xmax - sx, 0);
        }
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
    ICON_HELP,
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
    icon_set(ICON_HELP, "Help", ICON_APP_HELP);
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
    ACT_RECENT_MENU, ACT_RECENT_CLEAR, ACT_COPY_PATH, ACT_NEW_GAME, ACT_REVERT, ACT_QUIT, ACT_DOC_PROPERTIES, ACT_TERM_CLEAR, ACT_TERM_BOTTOM,
    ACT_RESTART, ACT_SHUTDOWN, ACT_DESK_OPEN, ACT_DESK_PROPERTIES, ACT_VIEW_ICONS, ACT_VIEW_DETAILS,
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
    {"&Icons", "", ACT_VIEW_ICONS, KIT_MENU_RADIO}, {"De&tails", "", ACT_VIEW_DETAILS, KIT_MENU_RADIO}, MSEP,
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
    {"&New", "Ctrl+N", ACT_NEW, 0}, {"New &Window", "Ctrl+Shift+N", ACT_NEW_WINDOW, 0},
    {"&Open...", "Ctrl+O", ACT_OPEN, 0}, {"Open &Recent", "", ACT_RECENT_MENU, KIT_MENU_SUB}, MSEP,
    {"&Save", "Ctrl+S", ACT_SAVE, 0}, {"Save &As...", "Ctrl+Shift+S", ACT_SAVE_AS, 0},
    {"Re&vert to Saved", "", ACT_REVERT, 0}, MSEP,
    {"Propert&ies", "Alt+Enter", ACT_DOC_PROPERTIES, 0}, MSEP,
    {"&Close Window", "Ctrl+W", ACT_CLOSE, 0}, {"&Quit Editor", "Ctrl+Q", ACT_QUIT, 0},
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
static const MItem mi_term_view[] = {
    {"&Clear", "", ACT_TERM_CLEAR, 0}, {"Scroll to &Bottom", "", ACT_TERM_BOTTOM, 0},
};
static const MMenu mb_term[] = {
    MN("&Shell", mi_term_shell), MN("&Edit", mi_term_edit), MN("&View", mi_term_view), MN("&Help", mi_help),
};
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
static void switcher_cancel(void);
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
static int properties_h(void);
static void handle_properties_click(int wx, int wy, int ww, int wh);
static void properties_key(void);
static void handle_about_click(int wx, int wy, int ww, int wh);
static void draw_settings(int wx, int wy, int ww, int wh, int fl);
static int settings_window_h(void);
static void settings_scroll_by(int dy);
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
    int open,replace_mode,focus,field,exact,whole,position[2],selected[2];
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
    int files_details;       /* detail rows instead of icon tiles */
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
#define fm_details (window_state[context_slot].files_details)
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
#define TERM_BG      ((uint8_t)(PAL_ANSI + 0))
#define TERM_FG      ((uint8_t)(PAL_ANSI + 7))
#define TERM_ANSI(i) ((uint8_t)(PAL_ANSI + (i)))

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
    {'C', 0, 0}, {'E', 0, 1}, {'B', 0, 2}, {'N', 0, 3},
    {'7', 1, 0}, {'8', 1, 1}, {'9', 1, 2}, {'/', 1, 3},
    {'4', 2, 0}, {'5', 2, 1}, {'6', 2, 2}, {'*', 2, 3},
    {'1', 3, 0}, {'2', 3, 1}, {'3', 3, 2}, {'-', 3, 3},
    {'0', 4, 0}, {'.', 4, 1}, {'=', 4, 2}, {'+', 4, 3},
};
#define CALC_NKEYS ((int)(sizeof(calc_keys) / sizeof(calc_keys[0])))

static int open_dlg = 0;
static int name_dlg = 0;
static int name_failed;
static int name_target = 0;
static char name_buf[FS_NAME_LEN];
static int name_len = 0, name_sel;
static int name_focus, name_owner, name_owner_seq;
static unsigned name_pdf_paper;
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
    return kind != WK_ABOUT && kind != WK_PROPERTIES;
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
    else if (w->kind == WK_SETTINGS) { *mw = 600; *mh = 320; }  /* the page scrolls */
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
    return fm_count;
}

static int fm_row_id(int row) {
    if(!fm_cwd_valid())return -2;
    int i = row;
    if (i < 0 || i >= fm_count ||
        !file_view_valid(fm_cwd,fm_ids[i],fm_ids_identity[i])) return -2;
    return fm_ids[i];
}

static void fm_refresh(void) {
    /* A row position is not a selection identity. Preserve only the same live
     * object; filtering, deletion or node-slot reuse clears the selection. */
    int selected_id=fm_row_id(fm_selected);
    unsigned selected_identity=selected_id>=0?fs_identity(selected_id):0;
    fm_checked_cwd();
    fm_view.hidden[0]=fm_cwd==fs_root()?trash_id:-1;
    fm_view.hidden[1]=fm_cwd==fs_root()?prefs_id:-1;
    int capacity=window_state_reserve(&window_state[context_slot],fs_child_count(fm_cwd));
    fm_count=file_view_build(fm_cwd,&fm_view,fm_ids,fm_ids_identity,capacity,&fm_total);
    int index=file_view_find(fm_ids,fm_ids_identity,fm_count,selected_id,selected_identity);
    fm_selected=index;
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
        if(fm_selected<0&&fm_count){fm_selected=key_sc==KEY_UP?fm_count-1:0;return 1;}
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

static int edit_search_flags(void){return (edit_search.exact?TEXT_MATCH_CASE:0)|(edit_search.whole?TEXT_WHOLE_WORD:0);}
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
    int found=text_find(edit_buf,edit_len,q->text[0],start,direction,edit_search_flags());
    if(found<0){kstrcpy(q->message,"Text not found.");dirty=1;return 0;}
    edit_sel_a=found;edit_sel_b=found+size;edit_caret=edit_sel_b;
    kstrcpy(q->message,"Match selected.");dirty=1;return 1;
}
/* Find as you type: the first hit at or after the selection start becomes
 * the current match, wrapping to the top. With no hit or no query the
 * selection collapses to its start, so the caret stays where it was. */
static void edit_find_incremental(void){
    EditorSearch *q=&edit_search;int size=kstrlen(q->text[0]);
    int lo=edit_sel_a<edit_sel_b?edit_sel_a:edit_sel_b;
    int found=size?text_find(edit_buf,edit_len,q->text[0],lo,1,edit_search_flags()):-1;
    if(found<0){edit_caret=lo;edit_sel_collapse();}
    else {edit_sel_a=found;edit_sel_b=found+size;edit_caret=edit_sel_b;}
    dirty=1;
}
static void edit_replace_one(void){
    EditorSearch *q=&edit_search;int lo=edit_sel_a<edit_sel_b?edit_sel_a:edit_sel_b;
    int hi=edit_sel_a>edit_sel_b?edit_sel_a:edit_sel_b;
    int n=kstrlen(q->text[0]),r=kstrlen(q->text[1]);
    if(!n||hi-lo!=n||!text_match(edit_buf,edit_len,lo,q->text[0],edit_search_flags())){
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
    int length=text_replace_all(edit_scratch,EDIT_BUF_SIZE,edit_buf,edit_len,q->text[0],q->text[1],edit_search_flags(),&count);
    if(length<0){kstrcpy(q->message,"Empty search or replacement exceeds limit.");dirty=1;return;}
    if(count){edit_record();kmemcpy(edit_buf,edit_scratch,length+1);edit_len=length;edit_caret=0;edit_sel_collapse();edit_saved_ok=0;}
    fmt_uint(q->message,count);kstrcpy(q->message+kstrlen(q->message)," replacements.");dirty=1;
}
static void edit_search_key(void){
    EditorSearch *q=&edit_search;int f=q->field;char *text=q->text[f];int length=kstrlen(text),pos=q->position[f];
    char before[64];kstrcpy(before,text);
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
        q->position[f]=pos;dirty=1;
        if(f==0)edit_find_incremental();
        return;
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
    if(f==0&&kstrcmp(before,text))edit_find_incremental();
}
static int nav_size(void);
/* Find bar. Classic docks it above the status bar; Modern floats it at the
 * top right of the text. Drawing and clicks share edit_find_layout. */
typedef struct { Rect panel, field[2], prev, next, exact, whole, close, replace, all; int count_x, rows; } FindLayout;
static int edit_status_h(void) { return MODERN ? 28 : 24; }
static int edit_search_height(void) {
    if (!edit_search.open || MODERN) return 0;
    return edit_search.replace_mode ? 2 * BTN_H + 20 : BTN_H + 12;
}
static int edit_text_top(int wy) { return wy + TITLE_H; }
static int edit_text_bottom(int wy, int wh) { return wy + wh - 1 - edit_status_h() - edit_search_height(); }

static void edit_find_layout(int wx, int wy, int ww, int wh, FindLayout *L) {
    int rows = edit_search.replace_mode ? 2 : 1, s = nav_size(), fw;
    L->rows = rows;
    if (MODERN) {
        int w = ww - 2 * 24 - SB < 500 ? ww - 2 * 24 - SB : 500;
        L->panel = (Rect){wx + ww - 1 - SB - 12 - w, edit_text_top(wy) + 10, w, rows * (BTN_H + 8) + 8};
    } else
        L->panel = (Rect){wx + 1, edit_text_bottom(wy, wh), ww - 2, edit_search_height()};
    int x = L->panel.x + 8, y = L->panel.y + (MODERN ? 8 : 6);
    int right = L->panel.x + L->panel.w - 8;
    L->close = (Rect){right - s, y + (BTN_H - s) / 2, s, s};
    fw = MODERN ? 210 : 240;
    if (x + fw + 260 > right) fw = right - x - 260 > 120 ? right - x - 260 : 120;
    L->field[0] = (Rect){x, y, fw, BTN_H};
    L->count_x = x + fw + 10;
    /* The count slot fits the longest text it shows. */
    int cw = kit_text_w("9999 of 9999", 0), mw = kit_text_w("9999 matches", 0), nw = kit_text_w("No results", 0);
    if (mw > cw) cw = mw;
    if (nw > cw) cw = nw;
    int nx = L->count_x + cw + 6;
    L->prev = (Rect){nx, y + (BTN_H - s) / 2, s, s};
    L->next = (Rect){nx + s + 2, y + (BTN_H - s) / 2, s, s};
    int ex = L->next.x + s + 12;
    L->exact = (Rect){ex, y + (BTN_H - s) / 2, MODERN ? s + 4 : kit_text_w("Match case", 0) + 24, s};
    L->whole = (Rect){L->exact.x + L->exact.w + (MODERN ? 4 : 12), L->exact.y, MODERN ? s + 4 : kit_text_w("Whole word", 0) + 24, s};
    /* Drop the whole-word toggle when the bar is too narrow for it. */
    if (L->whole.x + L->whole.w > L->close.x - 8) L->whole.w = 0;
    int y2 = y + BTN_H + 8;
    L->field[1] = (Rect){x, y2, fw, BTN_H};
    int rw = kit_button_w("Replace", -1);
    L->replace = (Rect){L->count_x, y2, rw, BTN_H};
    L->all = (Rect){L->count_x + rw + 8, y2, kit_button_w("Replace All", -1), BTN_H};
}

/* Matches in the document and which one the selection is. */
static void edit_find_count(int *count, int *current) {
    EditorSearch *q = &edit_search;
    int n = kstrlen(q->text[0]), lo = edit_sel_a < edit_sel_b ? edit_sel_a : edit_sel_b;
    *count = *current = 0;
    if (!n) return;
    for (int at = text_find(edit_buf, edit_len, q->text[0], 0, 1, edit_search_flags()); at >= 0 && *count < 9999;) {
        (*count)++;
        if (at == lo && edit_sel_a != edit_sel_b) *current = *count;
        if (at + 1 > edit_len - n) break;
        int next = text_find(edit_buf, edit_len, q->text[0], at + 1, 1, edit_search_flags());
        if (next <= at) break;
        at = next;
    }
}

static void edit_find_field(Rect r, int f, int placeholder_search) {
    EditorSearch *q = &edit_search;
    int focus = q->focus && q->field == f, tx, ty = kit_ty(r.y, r.h);
    if (placeholder_search) tx = kit_search(r.x, r.y, r.w, r.h, "", q->text[f][0] ? "" : "Find", focus ? KIT_FOCUS : 0);
    else {
        tx = kit_field(r.x, r.y, r.w, r.h, focus ? KIT_FOCUS : 0);
        if (!q->text[f][0] && !focus) kit_text("Replace with", tx, ty, TK_TEXT2, r.w - (tx - r.x) - 8, 0);
    }
    int lim = r.x + r.w - 8 - tx;
    if (focus && q->selected[f] && q->text[f][0]) {
        int w = kit_text_w(q->text[f], 0);
        draw_rect(tx - 1, ty, (w < lim ? w : lim) + 2, UI_FONT_H, TK_ACCENT);
        kit_text(q->text[f], tx, ty, TK_ON_ACCENT, lim, 0);
    } else kit_text(q->text[f], tx, ty, TK_TEXT, lim, 0);
    if (focus && !q->selected[f] && ((frame_count / 35) & 1) == 0) {
        char prefix[64];
        kmemcpy(prefix, q->text[f], q->position[f]);
        prefix[q->position[f]] = 0;
        int cx = tx + kit_text_w(prefix, 0);
        if (cx < r.x + r.w - 4) kit_caret(cx, ty);
    }
}

static void draw_edit_search(int wx, int wy, int ww, int wh) {
    if (!edit_search.open) return;
    EditorSearch *q = &edit_search;
    FindLayout L;
    edit_find_layout(wx, wy, ww, wh, &L);
    Rect p = L.panel;
    if (MODERN) { kit_shadow(p.x, p.y, p.w, p.h, 10); kit_box(p.x, p.y, p.w, p.h, 10, ui_theme.overlay, TK_LINE_STRONG); }
    else { draw_hline(p.x, p.y, p.w, TK_LINE_SUBTLE); draw_rect(p.x, p.y + 1, p.w, p.h - 1, TK_BASE); }
    uint8_t paper = MODERN ? ui_theme.overlay : TK_BASE;
    edit_find_field(L.field[0], 0, 1);
    int count, current;
    edit_find_count(&count, &current);
    char text[32];
    if (!q->text[0][0]) text[0] = 0;
    else if (!count) kstrcpy(text, "No results");
    else if (!current) {
        fmt_uint(text, (unsigned)count);
        kstrcpy(text + kstrlen(text), count == 1 ? " match" : " matches");
    } else {
        fmt_uint(text, (unsigned)current);
        kstrcpy(text + kstrlen(text), " of ");
        fmt_uint(text + kstrlen(text), (unsigned)count);
    }
    kit_text(text, L.count_x, kit_ty(L.field[0].y, BTN_H), TK_TEXT2, L.prev.x - L.count_x - 4, 0);
    kit_icon_button(L.prev.x, L.prev.y, L.prev.w, UI_ICON_UP, hot(L.prev.x, L.prev.y, L.prev.w, L.prev.h));
    kit_icon_button(L.next.x, L.next.y, L.next.w, UI_ICON_DOWN, hot(L.next.x, L.next.y, L.next.w, L.next.h));
    kit_vseparator(L.exact.x - 6, L.field[0].y + 4, BTN_H - 8);
    for (int k = 0; k < 2; k++) {
        Rect b = k ? L.whole : L.exact;
        int set = k ? q->whole : q->exact;
        if (!b.w) continue;
        if (MODERN) {
            int on = set ? KIT_ON : hot(b.x, b.y, b.w, b.h);
            if (on) kit_fill(b.x, b.y, b.w, b.h, 6, set ? TK_ACCENT_100 : TK_HOVER, 1);
            uint8_t ink = set ? TK_ACCENT_700 : TK_TEXT;
            int ty = kit_ty(b.y, b.h);
            kit_text_center(k ? "ab" : "Aa", b.x, ty, b.w, ink, 0);
            if (k) draw_hline(b.x + b.w / 2 - kit_text_w("ab", 0) / 2, ty + UI_FONT_H, kit_text_w("ab", 0), ink);
        } else kit_check_row(b.x, b.y + (b.h - ui_theme.check) / 2 - 1, k ? "Whole word" : "Match case",
                             (set ? KIT_ON : 0) | hot(b.x, b.y, b.w, b.h), 0);
    }
    if (MODERN) kit_vseparator(L.close.x - 6, L.field[0].y + 4, BTN_H - 8);
    kit_icon_button(L.close.x, L.close.y, L.close.w, UI_ICON_X, hot(L.close.x, L.close.y, L.close.w, L.close.h));
    if (L.rows > 1) {
        edit_find_field(L.field[1], 1, 0);
        kit_button(L.replace.x, L.replace.y, L.replace.w, BTN_H, "Replace", -1, KB_DEFAULT,
                   hot(L.replace.x, L.replace.y, L.replace.w, BTN_H));
        kit_button(L.all.x, L.all.y, L.all.w, BTN_H, "Replace All", -1, KB_DEFAULT, hot(L.all.x, L.all.y, L.all.w, BTN_H));
    }
    (void)paper;
}

static int rect_has(Rect r) { return hit(mouse_x, mouse_y, r.x, r.y, r.w, r.h); }
static int edit_search_click(int wx, int wy, int ww, int wh) {
    if (!edit_search.open) return 0;
    EditorSearch *q = &edit_search;
    FindLayout L;
    edit_find_layout(wx, wy, ww, wh, &L);
    if (!rect_has(L.panel)) return 0;
    q->focus = 1;
    for (int f = 0; f < L.rows; f++)
        if (rect_has(L.field[f])) { q->field = f; q->position[f] = kstrlen(q->text[f]); q->selected[f] = 1; dirty = 1; return 1; }
    if (rect_has(L.next)) edit_find_next(1);
    else if (rect_has(L.prev)) edit_find_next(-1);
    else if (rect_has(L.exact)) { q->exact = !q->exact; edit_find_incremental(); }
    else if (L.whole.w && rect_has(L.whole)) { q->whole = !q->whole; edit_find_incremental(); }
    else if (rect_has(L.close)) q->open = q->focus = 0;
    else if (L.rows > 1 && rect_has(L.replace)) edit_replace_one();
    else if (L.rows > 1 && rect_has(L.all)) edit_replace_everywhere();
    dirty = 1;
    return 1;
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
/* Writer and Spreadsheet client area: under the menu row in Classic; in
 * Modern the app toolbar starts on the header's bottom rule and continues
 * the header. Drawing, clicks and drags share it. */
static void doc_client(const Win *w, int *x, int *y, int *cw, int *ch) {
    *x = w->x + 1;
    *y = MODERN ? w->y + TITLE_BAR - 1 : w->y + TITLE_H;
    *cw = w->w - 2;
    *ch = w->y + w->h - 1 - *y;
}
/* Their toolbar and find-bar controls hover through the shell's targets. */
int writer_hover(int x, int y, int w, int h) { return hot(x, y, w, h); }
int spreadsheet_hover(int x, int y, int w, int h) { return hot(x, y, w, h); }
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
    if (ch == 'E') {                      /* clear the entry, keep the operation */
        kstrcpy(calc_entry, "0");
        calc_entry_len = 1;
        calc_has_dot = 0;
        calc_fresh = 1;
        return;
    }
    if (ch == 'B') {                      /* backspace in the entry being typed */
        if (calc_fresh) return;
        if (calc_entry_len > 0 && calc_entry[calc_entry_len - 1] == '.') calc_has_dot = 0;
        if (calc_entry_len > 0) calc_entry[--calc_entry_len] = 0;
        if (!calc_entry_len || (calc_entry_len == 1 && calc_entry[0] == '-')) {
            kstrcpy(calc_entry, "0");
            calc_entry_len = 1;
            calc_fresh = 1;
        }
        return;
    }
    if (ch == 'N') {                      /* change sign */
        if (calc_entry[0] == '-') {
            for (int i = 0; i < calc_entry_len; i++) calc_entry[i] = calc_entry[i + 1];
            calc_entry_len--;
        } else if (!(calc_entry_len == 1 && calc_entry[0] == '0') && calc_entry_len < (int)sizeof calc_entry - 1) {
            for (int i = calc_entry_len; i >= 0; i--) calc_entry[i + 1] = calc_entry[i];
            calc_entry[0] = '-';
            calc_entry_len++;
        }
        return;
    }
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

/* Multiply, divide and backspace glyphs, which the ASCII UI font lacks,
 * centered on (cx, cy). Returns 0 for other keys. */
static int calc_glyph(char ch, int cx, int cy, uint8_t ink) {
    if (ch == '*') {
        for (int i = -4; i <= 4; i++) {
            draw_rect(cx + i, cy + i, 2, 1, ink);
            draw_rect(cx + i, cy - i, 2, 1, ink);
        }
    } else if (ch == '/') {
        draw_rect(cx - 5, cy, 11, 2, ink);
        kit_fill(cx - 1, cy - 5, 3, 3, 1, ink, 1);
        kit_fill(cx - 1, cy + 4, 3, 3, 1, ink, 1);
    } else if (ch == 'B') {
        /* A key cap pointing left with an x inside. */
        int l = cx - 9, r = cx + 8, t = cy - 6, b = cy + 6;
        draw_hline(l + 6, t, r - l - 5, ink);
        draw_hline(l + 6, b, r - l - 5, ink);
        draw_vline(r, t, b - t + 1, ink);
        for (int i = 0; i <= 6; i++) { put_pixel(l + i, cy - i, ink); put_pixel(l + i, cy + i, ink); }
        for (int i = -2; i <= 2; i++) { put_pixel(cx + 1 + i, cy + i, ink); put_pixel(cx + 1 + i, cy - i, ink); }
    } else return 0;
    return 1;
}

static void draw_calc(int wx, int wy, int ww, int wh, int inactive) {
    (void)ww;
    gui_draw_window(wx, wy, CALC_W, wh, "Calculator", 0,
                    inactive ? WIN_INACTIVE : 0);
    int dx = wx + CALC_MARGIN;
    int dy = wy + TITLE_H + 12;
    int dw = CALC_W - 2 * CALC_MARGIN;
    int dh = CALC_DISP_H + 10;
    /* Display: a sunken readout with the pending operator at the left. */
    if (MODERN) kit_fill(dx, dy, dw, dh, 8, TK_SUNKEN, 1);
    else kit_box(dx, dy, dw, dh, 0, TK_RAISED, TK_LINE_STRONG);
    if (calc_op && !calc_error) {
        char op[2] = { calc_op == '*' ? 'x' : (char)calc_op, 0 };
        kit_text(op, dx + 10, dy + 6, TK_ACCENT_700, 20, 1);
    }

    const char *s = calc_entry;
    uint8_t ink = calc_error ? TK_RED_700 : TK_TEXT;
    int tw = logo_string_w(s);
    int use_logo = 1;
    for (const char *q = s; *q; q++) {
        if ((*q < '0' || *q > '9') && *q != '.' && *q != '-') {
            use_logo = 0;
            break;
        }
    }
    if (use_logo && tw <= dw - 36) {
        int tx = dx + dw - 12 - tw;
        draw_logo_string(s, tx, dy + (dh - LOGO_FONT_H) / 2, ink);
    } else {
        tw = kit_text_w(s, 1);
        int tx = dx + dw - 12 - tw;
        if (tx < dx + 24)
            tx = dx + 24;
        kit_text(s, tx, kit_ty(dy, dh), ink, dx + dw - 12 - tx, 1);
    }

    int kx = wx + CALC_MARGIN;
    int ky = dy + dh + 12;
    for (int i = 0; i < CALC_NKEYS; i++) {
        int x, y, w, h;
        calc_key_rect(kx, ky, i, &x, &y, &w, &h);
        char lab[4];
        lab[0] = calc_keys[i].ch;
        lab[1] = 0;
        char ch = calc_keys[i].ch;
        if (ch == 'E') kstrcpy(lab, "CE");
        else if (ch == 'B') kstrcpy(lab, "<");
        else if (ch == 'N') kstrcpy(lab, "+/-");
        else if (ch == '*') kstrcpy(lab, "x");
        int digit = (ch >= '0' && ch <= '9') || ch == '.';
        int st = hot(x, y, w, h);
        /* Digits are plain keys, operators and Clear are tinted, = is primary. */
        if (ch == '=') kit_button(x, y, w, h, lab, -1, KB_PRIMARY, st);
        else if (digit) kit_button(x, y, w, h, lab, -1, KB_DEFAULT, st);
        else {
            int on = calc_op == ch && calc_fresh;
            uint8_t fill = on ? TK_ACCENT_100 : (st & KIT_PRESSED) ? TK_HOVER : (st & KIT_HOVER) ? TK_HOVER : MODERN ? TK_SUNKEN : TK_BASE;
            kit_box(x, y, w, h, ui_theme.r_control, fill, MODERN ? 0 : TK_LINE);
            uint8_t ink = ch == 'C' || ch == 'E' ? TK_RED_700 : ch == 'B' || ch == 'N' ? TK_TEXT : TK_ACCENT_700;
            if (!calc_glyph(ch, x + w / 2, y + h / 2, ink)) kit_text_center(lab, x, kit_ty(y, h), w, ink, 1);
        }
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
    0xFF00AF, 0xFFAFD7, 0x870000, 0xAF5F00, 0xFFAF00, 0xFFFF00, 0xD7FF00, 0x00FF00,
    0x00FFAF, 0x00FFFF, 0x87D7FF, 0x0087FF, 0x5F00FF, 0xAF00FF, 0xD7AFFF, 0xFFD7AF
};
static uint8_t paint_well2[PAINT_NWELL2];

/* The status bar under the color well. */
static int paint_status_h(void) { return MODERN ? 28 : 24; }
static int paint_hover_x = -1, paint_hover_y = -1;  /* canvas cell under the pointer */

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
    *ph = wh - TITLE_H - PAINT_TOOL_H - PAINT_WELL_H - paint_status_h();
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
    *y = wy + wh - paint_status_h() - 24;
}

static void paint_well2_cell(int wx, int wy, int wh, int i, int *x, int *y) {
    *x = wx + 12 + i * (22 + 4);
    *y = wy + wh - paint_status_h() - 48;
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

/* A tool cell: flat until hovered, accent-tinted while selected. */
static void paint_cell_bg(int x, int y, int on) {
    int st = hot(x, y, PAINT_CELL, PAINT_CELL);
    if (on) kit_box(x, y, PAINT_CELL, PAINT_CELL, ui_theme.r_control, TK_ACCENT_100, MODERN ? 0 : TK_ACCENT_300);
    else if (st & (KIT_HOVER | KIT_PRESSED)) kit_box(x, y, PAINT_CELL, PAINT_CELL, ui_theme.r_control, TK_HOVER, MODERN ? 0 : TK_LINE);
}

static void draw_paint(int wx, int wy, int ww, int wh, int inactive) {
    paint_init();
    gui_draw_window(wx, wy, ww, wh, "Paint", 0,
                    inactive ? WIN_INACTIVE : 0);

    /* Tool strip: tools, brush sizes, then the current color. */
    int strip_y = wy + TITLE_H;
    kit_toolbar(wx + 1, strip_y, ww - 2, PAINT_TOOL_H + 1);
    for (int i = 0; i < PAINT_NTOOLS; i++) {
        int tx, ty;
        paint_tool_cell(wx, wy, i, &tx, &ty);
        paint_cell_bg(tx, ty, i == paint_tool);
        doodle_ink = i == paint_tool ? TK_ACCENT_700 : TK_ICON;
        draw_tool_doodle(i, tx, ty);
    }

    {
        int dx, dy;
        paint_brush_cell(wx, wy, 0, &dx, &dy);
        kit_vseparator(dx - 9, dy + 4, PAINT_CELL - 8);
        for (int i = 0; i < 3; i++) {
            int bx, by;
            paint_brush_cell(wx, wy, i, &bx, &by);
            int on = (paint_brush == i + 1);
            paint_cell_bg(bx, by, on);
            int rad = i + 1;
            uint8_t dotc = on ? TK_ACCENT_700 : TK_ICON;
            draw_round_rect(bx + PAINT_CELL / 2 - rad, by + PAINT_CELL / 2 - rad,
                            rad * 2, rad * 2, rad, dotc);
        }
        int cx, cy;
        paint_chip_cell(wx, wy, ww, &cx, &cy);
        kit_box(cx - 2, cy - 2, 44, 28, ui_theme.r_control, TK_RAISED, TK_LINE_STRONG);
        kit_fill(cx + 2, cy + 2, 36, 20, MODERN ? 3 : 0, paint_color, 0);
    }

    /* Canvas on a workspace, with the window shadow under it. */
    int px, py, pw, ph, cx, cy;
    paint_canvas_geom(wx, wy, ww, wh, &px, &py, &pw, &ph, &cx, &cy);
    draw_rect(wx + 1, strip_y + PAINT_TOOL_H + 1, ww - 2, ph - 1, MODERN ? TK_SUNKEN : TK_HOVER);
    kit_shadow(cx, cy, PAINT_W * PAINT_SCALE, PAINT_H * PAINT_SCALE, 0);
    draw_frame(cx - 1, cy - 1, PAINT_W * PAINT_SCALE + 2, PAINT_H * PAINT_SCALE + 2, TK_LINE_STRONG);
    (void)px; (void)py; (void)pw; (void)ph;

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

    /* Color well, then the status bar: the tool's hint on the left, the
     * pointer's canvas position and the canvas size on the right. */
    int sh = paint_status_h(), well_top = wy + wh - sh - PAINT_WELL_H, status_y = wy + wh - 1 - sh;
    draw_hline(wx + 1, well_top, ww - 2, TK_LINE_SUBTLE);
    draw_rect(wx + 1, well_top + 1, ww - 2, status_y - well_top - 1, MODERN ? ui_theme.content : TK_BASE);
    if (MODERN) {
        draw_hline(wx + 1, status_y, ww - 2, TK_LINE_SUBTLE);
        kit_fill_bottom(wx + 1, status_y + 1, ww - 2, sh - 1, ui_theme.r_window - 1, ui_theme.statusbar);
    } else kit_statusbar(wx + 1, status_y, ww - 2, sh);
    {
        static const char *const hints[PAINT_NTOOLS] = {
            "Pencil: drag to draw", "Fill: click an area", "Line: drag from end to end",
            "Rectangle: drag a corner", "Ellipse: drag its bounds", "Text: click, then type",
            "Eraser: drag to erase",
        };
        char right[48], n[12];
        right[0] = 0;
        if (paint_hover_x >= 0) {
            fmt_uint(n, (unsigned)paint_hover_x); kstrcpy(right, n); kstrcpy(right + kstrlen(right), ", ");
            fmt_uint(n, (unsigned)paint_hover_y); kstrcpy(right + kstrlen(right), n);
            kstrcpy(right + kstrlen(right), "      ");
        }
        fmt_uint(n, PAINT_W); kstrcpy(right + kstrlen(right), n); kstrcpy(right + kstrlen(right), " x ");
        fmt_uint(n, PAINT_H); kstrcpy(right + kstrlen(right), n); kstrcpy(right + kstrlen(right), " px");
        int ty = kit_ty(status_y + 1, sh - 1), pad = MODERN ? 12 : 8, rw = kit_text_w(right, 0);
        kit_text_right(right, wx + ww - 1 - pad, ty, TK_TEXT2, 0);
        kit_text(hints[paint_tool], wx + 1 + pad, ty, TK_TEXT2, ww - 2 - 2 * pad - rw - 16, 0);
    }
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
        int r = MODERN ? 4 : 0;
        if (cur) kit_frame(sx - 2, sy - 2, 26, 22, r ? r + 2 : 0, TK_ACCENT);
        kit_box(sx, sy, 22, 18, r, c, cur ? TK_ACCENT_700 : TK_LINE);
        (void)hot(sx, sy, 22, 18);
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

/* Save the picture in parent, or in Pictures (made when missing) when
 * parent is not a folder. */
static int paint_write_in(int parent, const char *name) {
    name_failure_message=0;
    if (fs_sync_busy()) {
        name_failure_message="Disk saving. Retry Save shortly.";
        return 0;
    }
    paint_init();
    int pics = fs_is_dir(parent) ? parent : fs_find_child(fs_root(), "Pictures");
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
    return fs_valid(id)&&!fs_is_dir(id)&&!fs_is_app(id)&&!fs_streamed(id)&&
           image_probe(fs_data(id),(unsigned)fs_size(id))!=IMAGE_FORMAT_NONE;
}

static void open_view(int file_id) {
    if(!fs_valid(file_id)||fs_is_dir(file_id)||fs_is_app(file_id)||fs_streamed(file_id))return;
    if(win_open(WK_VIEW)<0)return;
    image_viewer_open(fs_data(file_id),(unsigned)fs_size(file_id),fs_name(file_id));
    open_dlg=0;dirty=1;
}

/* Client rectangle of Browser, Image Viewer, Media Player and System
 * Monitor, shared by drawing, clicks and the player's fast paths. Classic
 * starts under the menu row; in Modern an app with a toolbar starts on the
 * header rule, so its toolbar continues the header. */
static void app_client(int wx, int wy, int ww, int wh, int toolbar, int *x, int *y, int *cw, int *ch) {
    int top = MODERN ? wy + TITLE_BAR - (toolbar ? 1 : 0) : wy + TITLE_H;
    *x = wx + 1; *y = top; *cw = ww - 2; *ch = wy + wh - 1 - top;
}

static void draw_view(int wx,int wy,int ww,int wh,int inactive) {
    int cx, cy, cw, ch;
    gui_draw_window(wx,wy,ww,wh,image_viewer_title(),0,inactive?WIN_INACTIVE:0);
    app_client(wx, wy, ww, wh, 1, &cx, &cy, &cw, &ch);
    image_viewer_set_hover(inactive ? 0 : hot);
    image_viewer_draw(cx, cy, cw, ch);
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

/* Tracks the canvas cell under the pointer for the status bar. */
static void paint_hover_tick(void) {
    int wx, wy, ww, wh, lx = -1, ly = -1;
    if (front_kind() != WK_PAINT || !win_geom_kind(WK_PAINT, &wx, &wy, &ww, &wh)) return;
    if (!paint_mouse_logical(wx, wy, ww, wh, mouse_x, mouse_y, &lx, &ly)) lx = ly = -1;
    if (lx != paint_hover_x || ly != paint_hover_y) { paint_hover_x = lx; paint_hover_y = ly; dirty = 1; }
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
static int snk_best;   /* best score since boot */
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
        if (snk_score > snk_best)
            snk_best = snk_score;
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
    if (snk_over && (key_sc == KEY_ENTER || key_sc == KEY_SPACE)) {
        snake_reset();
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

/* Snake layout: a toolbar with the score and New game, the playfield, and
 * a status bar. Drawing and clicks share snake_layout. */
#define SNK_MARGIN 16
typedef struct {
    PaneRect field, again, card, card_button;
    int cx, cy, cw, ch;
} SnakeLayout;

static void snake_layout(int wx, int wy, int ww, int wh, SnakeLayout *L) {
    L->cx = wx + 1; L->cy = wy + TITLE_H; L->cw = ww - 2; L->ch = wh - TITLE_H - 1;
    int body = pane_body_y(L->cy), bottom = pane_status_y(L->cy, L->ch);
    int fw = SNK_COLS * SNK_CELL, fh = SNK_ROWS * SNK_CELL;
    L->field = (PaneRect){L->cx + (L->cw - fw) / 2, body + (bottom - body - fh) / 2, fw, fh};
    int bw = pane_tool_w("New game");
    L->again = (PaneRect){L->cx + L->cw - pane_pad() - bw, pane_ctl_y(L->cy), bw, BTN_H};
    pane_card_layout(L->field.x, L->field.y, fw, fh, &L->card, &L->card_button);
}

static int snake_window_h(void) {
    return TITLE_H + pane_tool_h() - (MODERN ? 1 : 0) + 2 * SNK_MARGIN + SNK_ROWS * SNK_CELL +
           pane_status_h() + 1;
}

static void draw_snake(int wx, int wy, int ww, int wh, int inactive) {
    gui_draw_window(wx, wy, ww, wh, "Snake", 0, inactive ? WIN_INACTIVE : 0);
    SnakeLayout L;
    snake_layout(wx, wy, ww, wh, &L);
    PaneHot hp = inactive ? 0 : hot;
    /* Toolbar: score, length and best, then New game. */
    pane_toolbar(L.cx, L.cy, L.cw);
    char num[12];
    int x = L.cx + pane_pad() + 4, ty = pane_ctl_y(L.cy);
    utoa((unsigned)snk_score, num);
    x += pane_inline_stat(x, ty, BTN_H, "Score", num) + 20;
    utoa((unsigned)snk_len, num);
    x += pane_inline_stat(x, ty, BTN_H, "Length", num) + 20;
    utoa((unsigned)snk_best, num);
    pane_inline_stat(x, ty, BTN_H, "Best", num);
    pane_tool(&L.again, ICON_REFRESH, "New game", KB_DEFAULT,
              pane_hot(hp, L.again.x, L.again.y, L.again.w, L.again.h));
    /* Playfield art: a dark board with a faint grid. */
    PaneRect *f = &L.field;
    uint8_t field = gfx_rgb(0x16, 0x1A, 0x24);
    uint8_t grid = gfx_rgb(0x1E, 0x23, 0x30);
    kit_fill(f->x - 1, f->y - 1, f->w + 2, f->h + 2, MODERN ? 8 : KIT_R_SQUARE, TK_LINE, 1);
    kit_fill(f->x, f->y, f->w, f->h, MODERN ? 7 : KIT_R_SQUARE, field, 1);
    int gx = f->x, gy = f->y;
    for (int r = 1; r < SNK_ROWS; r++)
        draw_hline(gx, gy + r * SNK_CELL, SNK_COLS * SNK_CELL, grid);
    for (int c = 1; c < SNK_COLS; c++)
        draw_vline(gx + c * SNK_CELL, gy, SNK_ROWS * SNK_CELL, grid);
    for (int i = 0; i < snk_len; i++) {
        uint8_t body = (i == 0) ? TK_ACCENT_700 : ui_accent;
        draw_round_rect(gx + snk_x[i] * SNK_CELL + 1, gy + snk_y[i] * SNK_CELL + 1,
                        SNK_CELL - 2, SNK_CELL - 2, 3, body);
    }
    draw_round_rect(gx + snk_food_x * SNK_CELL + 2, gy + snk_food_y * SNK_CELL + 2,
                    SNK_CELL - 4, SNK_CELL - 4, (SNK_CELL - 4) / 2, gfx_rgb(0xF0, 0x50, 0x48));
    if (snk_over) {
        shade_rect(f->x, f->y, f->w, f->h, 1);
        char text[40];
        utoa((unsigned)snk_score, num);
        pane_join(text, sizeof text, "You scored ", num, snk_score == 1 ? " point." : " points.");
        pane_card(&L.card, &L.card_button, "Game over", text, "Play again",
                  pane_hot(hp, L.card_button.x, L.card_button.y, L.card_button.w, L.card_button.h));
    }
    pane_statusbar(L.cx, L.cy, L.cw, L.ch,
                   snk_over ? "Enter or Space plays again; Esc closes" : "Arrow keys steer; Esc closes",
                   TK_TEXT2, snk_over ? "Game over" : "Playing", snk_over ? TK_RED_700 : TK_TEXT2);
}

static void handle_snake_click(int wx, int wy, int ww, int wh) {
    SnakeLayout L;
    snake_layout(wx, wy, ww, wh, &L);
    if (pane_hit(mouse_x, mouse_y, &L.again) || (snk_over && pane_hit(mouse_x, mouse_y, &L.card_button)))
        snake_reset();
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

/* Output lines are plain text; colors come from their shape. A prompt line
 * shows its folder in blue, errors are red, usage lines amber, and words that
 * end in a slash (folders in ls) blue. */
enum { TL_PLAIN, TL_PROMPT, TL_ERROR, TL_USAGE, TL_NOTE };
static int term_line_kind(const char *s, int index) {
    static const char *const errors[] = { "Cannot ", "Error", "No manual", "Command exceeds", "Input was lost",
                                          "Program stopped", "Stop this terminal", "Disk is saving", "Network is busy" };
    for (unsigned k = 0; k < sizeof errors / sizeof errors[0]; k++) {
        int n = kstrlen(errors[k]), i = 0;
        while (i < n && s[i] == errors[k][i]) i++;
        if (i == n) return TL_ERROR;
    }
    if (s[0] == 'U' && s[1] == 's' && s[2] == 'a' && s[3] == 'g' && s[4] == 'e' && s[5] == ':') return TL_USAGE;
    return index < 2 ? TL_NOTE : TL_PLAIN;
}
/* The prompt (folder and marker) and folder names are bold. */
static uint8_t term_char_ink(const char *s, int len, int at, int kind, int *prompt_end, int *bold) {
    *bold = 0;
    if (kind == TL_ERROR) return TERM_ANSI(9);
    if (kind == TL_USAGE) return TERM_ANSI(11);
    if (kind == TL_NOTE) return TERM_ANSI(8);
    if (kind == TL_PROMPT) {
        if (*prompt_end < 0) { int i = 0; while (s[i] && !(s[i] == '>' && s[i + 1] == ' ')) i++; *prompt_end = i; }
        *bold = at <= *prompt_end;
        if (at < *prompt_end) return TERM_ANSI(12);
        if (at <= *prompt_end + 1) return TERM_ANSI(10);
        *bold = 0;
        return TERM_ANSI(15);
    }
    /* A word that ends in a slash. */
    int a = at, b = at;
    if (s[at] == ' ') return TERM_FG;
    while (a > 0 && s[a - 1] != ' ') a--;
    while (b < len && s[b] != ' ') b++;
    *bold = b > a + 1 && s[b - 1] == '/';
    return *bold ? TERM_ANSI(12) : TERM_FG;
}
/* One wrapped chunk of a line: characters [from, from + cols) of s. The
 * monospace font has no bold face, so bold is the glyph drawn twice, one
 * pixel apart. */
static void term_line_chunk(const char *s, int len, int kind, int from, int cols, int x, int y) {
    int prompt_end = -1, bold;
    for (int c = 0; c < cols && from + c < len; c++) {
        uint8_t ink = term_char_ink(s, len, from + c, kind, &prompt_end, &bold);
        draw_edit_char(s[from + c], x + c * EDIT_CHAR_W, y, ink);
        if (bold) draw_edit_char(s[from + c], x + c * EDIT_CHAR_W + 1, y, ink);
    }
}
/* Terminal and native-task bodies: dark in both styles, rounded at the
 * bottom in Modern. */
static void term_body(int wx, int top, int ww, int h) {
    if (MODERN && !(chrome_flags & WIN_MAXED)) kit_fill_bottom(wx + 1, top, ww - 2, h, ui_theme.r_window - 1, TERM_BG);
    else draw_rect(wx + 1, top, ww - 2, h, TERM_BG);
}
/* Scroll position along the right edge; the wheel and Page keys scroll. */
static void term_scroll_marker(int x, int y, int h, int first, int rows, int total) {
    if (total <= rows || h < 16) return;
    int at, size;
    kit_scroll_thumb(h, first, rows, total, &at, &size);
    kit_fill(x, y + at, 6, size, 3, TERM_ANSI(8), 0);
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
    term_body(w->x,top-1,w->w,w->h-TITLE_H-1);
    kit_toolbar(w->x+1,top-1,w->w-2,NATIVE_TOOLBAR_H+1);
    int output=app_view_output_visible(slot);
    tool_button(w->x+8,top+2,76,NATIVE_TOOLBAR_H-4,ICON_LIST,app_view_output_dropped(slot)?"Output*":"Output",
                output?TB_ACTIVE:TB_FLAT);
    AppViewResult result;
    int ended=app_view_result(slot,&result);
    const char *status=ended?
        (result.reason==PROCESS_EXIT_STOP?"Stopped":"Ended; see Output"):
        "Ctrl+C stops; Close ends task";
    if(output&&app_view_output_dropped(slot))status="Older output discarded";
    kit_text(status,w->x+96,kit_ty(top,NATIVE_TOOLBAR_H-1),TK_TEXT2,w->w-108,0);
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
        else kit_text(ended?"No published frame; see Output":"Starting...",
                      w->x+16,top+NATIVE_TOOLBAR_H+16,TERM_ANSI(8),w->w-32,0);
    }
}

static void draw_term(int wx, int wy, int ww, int wh, int inactive) {
    char title[TERM_TASK_TITLE_LEN], size[24];
    int tcols = (ww - 2 - TERM_PAD * 2) / EDIT_CHAR_W, trows = (wh - TITLE_H - 2 - TERM_PAD * 2) / EDIT_LINE_H;
    kstrcpy(size, "~ ");
    fmt_uint(size + 2, (unsigned)(tcols > 0 ? tcols : 0));
    kstrcpy(size + kstrlen(size), "x");
    fmt_uint(size + kstrlen(size), (unsigned)(trows > 0 ? trows : 0));
    chrome_subtitle = size;
    gui_draw_window(wx, wy, ww, wh, win_display_title(context_slot, title), 0, inactive ? WIN_INACTIVE : 0);
    chrome_subtitle = 0;
    term_body(wx, wy + TITLE_H, ww, wh - TITLE_H - 1);

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
        int kind=i==count?(term_task_running(context_slot)?TL_NOTE:TL_PROMPT):term_is_echo(i)?TL_PROMPT:term_line_kind(text,i);
        for(int chunk=0;chunk<chunks;chunk++,visual++){
            int row=visual-first;
            if(row<0||row>=rows)continue;
            int y=ay+row*EDIT_LINE_H;
            term_line_chunk(text,len,kind,chunk*cols,cols,ax,y);
            /* Block cursor; hollow while another window has focus. */
            if(i==count&&chunk==chunks-1){
                int cx=ax+(len%cols)*EDIT_CHAR_W;
                if(inactive)draw_frame(cx,y,EDIT_CHAR_W,EDIT_FONT_H,TERM_ANSI(8));
                else if((frame_count/35)%2==0)draw_rect(cx,y,EDIT_CHAR_W,EDIT_FONT_H,TERM_ANSI(15));
            }
        }
    }
    term_scroll_marker(wx+ww-1-10,wy+TITLE_H+6,wh-TITLE_H-14,first,rows,total);
}

static void open_term(void) {
    open_dlg = 0;
    win_open(WK_TERM);
    dirty = 1;
}

/* ---------- Todo ---------- */

/* Todo layout: a toolbar with the new-task field, Add and Save, the task
 * rows with check boxes, and a status bar with the counts and the save
 * state. Drawing and clicks share todo_layout. */
typedef struct {
    PaneRect field, add, save;
    int cx, cw, list_y, rows;
} TodoLayout;

static void todo_layout(int wx, int wy, int ww, int wh, TodoLayout *L) {
    int cx = wx + 1, cy = wy + TITLE_H, cw = ww - 2, ch = wh - TITLE_H - 1;
    int pad = pane_pad(), y = pane_ctl_y(cy), sw = pane_tool_w("Save"), aw = kit_button_w("Add", -1);
    L->cx = cx; L->cw = cw;
    L->save = (PaneRect){cx + cw - pad - sw, y, sw, BTN_H};
    L->add = (PaneRect){L->save.x - 10 - aw, y, aw, BTN_H};
    L->field = (PaneRect){cx + pad, y, L->add.x - 8 - cx - pad, BTN_H};
    L->list_y = pane_body_y(cy) + (MODERN ? 6 : 4);
    int n = (pane_status_y(cy, ch) - 4 - L->list_y) / ROW_H;
    L->rows = n < 0 ? 0 : n > TODO_MAX ? TODO_MAX : n;
}

/* Default height: every task row fits. */
static int todo_window_h(void) {
    return TITLE_H + pane_tool_h() - (MODERN ? 1 : 0) + (MODERN ? 6 : 4) + TODO_MAX * ROW_H + 4 +
           pane_status_h() + 1;
}

static void draw_todo(int wx, int wy, int ww, int wh, int inactive) {
    gui_draw_window(wx, wy, ww, wh, "Todo", 0, inactive ? WIN_INACTIVE : 0);
    TodoLayout L;
    todo_layout(wx, wy, ww, wh, &L);
    int cy = wy + TITLE_H, ch = wh - TITLE_H - 1, n = todo_count();
    PaneHot hp = inactive ? 0 : hot;
    /* Toolbar: field, Add, Save. */
    pane_toolbar(L.cx, cy, L.cw);
    int active = todo_field_active() && !inactive, full = n >= TODO_MAX;
    const char *txt = todo_field();
    PaneRect *f = &L.field;
    int tx = kit_field(f->x, f->y, f->w, f->h, active ? KIT_FOCUS : 0), ty = kit_ty(f->y, f->h);
    int room = f->x + f->w - (MODERN ? 10 : 6) - tx, start = 0;
    while (txt[start] && kit_text_w(txt + start, 0) > room - 2) start++;
    if (txt[0]) kit_text(txt + start, tx, ty, TK_TEXT, room, 0);
    else kit_text(full ? "The list is full" : "Add a task", tx, ty, TK_TEXT2, room, 0);
    if (active && ((frame_count / 35) & 1) == 0) kit_caret(tx + kit_text_w(txt + start, 0), ty);
    int can_add = txt[0] && !full;
    kit_button(L.add.x, L.add.y, L.add.w, L.add.h, "Add", -1, KB_PRIMARY,
               can_add ? pane_hot(hp, L.add.x, L.add.y, L.add.w, L.add.h) : KIT_DISABLED);
    kit_vseparator(L.save.x - 5, L.save.y + 4, L.save.h - 8);
    pane_tool(&L.save, ICON_SAVE, "Save", KB_DEFAULT, pane_hot(hp, L.save.x, L.save.y, L.save.w, L.save.h));
    /* Rows */
    int shown = n < L.rows ? n : L.rows, done = 0, pad = pane_pad();
    for (int i = 0; i < n; i++) done += todo_done(i) != 0;
    for (int i = 0; i < shown; i++) {
        int ry = L.list_y + i * ROW_H, d = todo_done(i);
        int state = pane_hot(hp, L.cx, ry, L.cw, ROW_H);
        uint8_t ink = kit_row(L.cx, ry, L.cw, ROW_H, state);
        int bx = L.cx + pad + (MODERN ? 6 : 0), lx = bx + ui_theme.check + 10;
        kit_check(bx, ry + (ROW_H - ui_theme.check) / 2, d ? KIT_ON : 0);
        const char *s = todo_text(i);
        int lw = kit_text(s, lx, kit_ty(ry, ROW_H), d ? TK_TEXT2 : ink, L.cx + L.cw - pad - lx, 0);
        if (d) draw_hline(lx, ry + ROW_H / 2, lw, TK_TEXT2);
    }
    if (!n) {
        int my = L.list_y + (pane_status_y(cy, ch) - L.list_y) / 2 - UI_FONT_H;
        kit_text_center("No tasks yet", L.cx, my, L.cw, TK_TEXT, 1);
        kit_text_center("Type a task above and press Enter.", L.cx, my + UI_FONT_H + 4, L.cw, TK_TEXT2, 0);
    }
    /* Status bar: counts, then the save state. */
    char left[48], a[12], b[12];
    pane_num(a, (unsigned)n);
    pane_num(b, (unsigned)done);
    pane_join(left, sizeof left, a, n == 1 ? " task, " : " tasks, ", b);
    pane_append(left, sizeof left, full ? " done (list full)" : " done");
    pane_statusbar(L.cx, cy, L.cw, ch, left, TK_TEXT2, todo_status(), TK_TEXT2);
}

static void handle_todo_click(int wx, int wy, int ww, int wh) {
    TodoLayout L;
    todo_layout(wx, wy, ww, wh, &L);
    if (pane_hit(mouse_x, mouse_y, &L.save)) {
        todo_retry_save();dirty=1;return;
    }
    if (pane_hit(mouse_x, mouse_y, &L.add)) {
        todo_focus_field();
        todo_field_enter();
        dirty = 1;
        return;
    }
    if (pane_hit(mouse_x, mouse_y, &L.field)) {
        todo_focus_field();
        dirty = 1;
        return;
    }
    int n = todo_count();
    if (n > L.rows)
        n = L.rows;
    for (int i = 0; i < n; i++) {
        int ry = L.list_y + i * ROW_H;
        if (hit(mouse_x, mouse_y, L.cx, ry, L.cw, ROW_H)) {
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
    if(!identity||!fs_valid(id)||fs_is_dir(id)||fs_is_app(id)||fs_streamed(id)||fs_identity(id)!=identity)
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




static int help_window_h(void);
static void layout_window(int kind, int *x, int *y, int *w, int *h) {
    switch (kind) {
    case WK_NATIVE:
        *w=640;*h=480;break;
    case WK_SPREADSHEET:
        { /* The client starts below the title (Modern) or menu row (Classic). */
            int co=MODERN?TITLE_BAR-1:TITLE_H;
            *w=SPREADSHEET_W+2;*h=spreadsheet_fit_height(SPREADSHEET_W,SPREADSHEET_H+TITLE_H+1-co)+1+co;
        }
        break;
    case WK_WRITER:
        *w=WRITER_W+2;*h=WRITER_H+TITLE_H+2;break;
    case WK_BROWSER:
        *w=BROWSER_W+2;*h=BROWSER_H+TITLE_H+2;break;
    case WK_PLAYER:
        *w=PLAYER_W+2;*h=PLAYER_H+TITLE_H+2;break;
    case WK_PROPERTIES:
        *w=380;*h=properties_h();break;
    case WK_HELLO:
        *w = 520;
        *h = TITLE_H + 140;
        break;
    case WK_HELP:
        *w = 560;
        *h = help_window_h();
        break;
    case WK_ABOUT:
        *w = 460;
        *h = 260;
        break;
    case WK_SETTINGS:
        *w = 800;
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
        *h = 500 + paint_status_h();
        break;
    case WK_VIEW:
        *w=IMAGE_VIEWER_W+2;*h=IMAGE_VIEWER_H+TITLE_H+2;break;
    case WK_SNAKE:
        *w = 520;
        *h = snake_window_h();
        break;
    case WK_WORDLE:
        *w = WORDLE_W + 2;
        *h = TITLE_H + wordle_client_h() + 1;
        break;
    case WK_TERM:
        *w = TERM_WIN_W;
        *h = TERM_WIN_H;
        break;
    case WK_TODO:
        *w = TODO_W;
        *h = todo_window_h();
        break;
    case WK_CLOCK:
        *w = CLOCK_W + 2;
        *h = CLOCK_H + TITLE_H + 1;
        break;
    case WK_CAL:
        *w = CAL_W + 2;
        *h = CAL_H + TITLE_H + 1;
        break;
    case WK_MINES:
        *w = MINES_W + 2;
        *h = TITLE_H + mines_client_h() + 1;
        break;
    case WK_2048:
        *w = G2048_W + 2;
        *h = TITLE_H + g2048_client_h() + 1;
        break;
    case WK_BREAKOUT:
        *w = BO_CLIENT_W + 2;
        *h = TITLE_H + bo_client_h() + 1;
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
    if (kind == WK_CALC || kind == WK_TERM || kind == WK_PAINT)
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

/* Line number gutter: wide enough for the last line's number. */
static int edit_gutter_w(void) {
    if (!(win_options & WINOPT_LINE_NUMBERS)) return 0;
    int lines = 1, digits = 1;
    for (int i = 0; i < edit_len; i++) if (edit_buf[i] == '\n') lines++;
    for (int n = lines; n >= 10; n /= 10) digits++;
    if (digits < 2) digits = 2;
    return digits * EDIT_CHAR_W + 20;
}

static void edit_area(int wx, int wy, int ww, int wh,
                      int *ax, int *ay, int *aw, int *ah) {
    *ax = wx + 1 + edit_gutter_w() + (MODERN ? 12 : 8);
    *ay = edit_text_top(wy) + (MODERN ? 10 : 6);
    *aw = wx + ww - 1 - SB - 4 - *ax;
    *ah = edit_text_bottom(wy, wh) - 4 - *ay;
    if (*aw < EDIT_CHAR_W) *aw = EDIT_CHAR_W;
    if (*ah < EDIT_LINE_H) *ah = EDIT_LINE_H;
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
    fm_selected = -1;
    fm_refresh();
    dirty = 1;
}

static void fm_open_selected(void) {
    int vis = fm_vis_count();
    if (vis <= 0 || fm_selected < 0 || fm_selected >= vis)
        return;
    int id = fm_row_id(fm_selected);
    if (id < 0) {fm_refresh();dirty=1;return;}
    int index=fm_selected;
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
    fm_selected=index;
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

/* ---------- Drawing ---------- */

static int file_extension(const char *name,const char *suffix);
/* Application nodes are named after their desktop icon; a few have longer names. */
static int app_icon_by_name(const char *name) {
    for (int i = 0; i < ICON_DESK_COUNT; i++)
        if (kstrcmp(icons[i].label, name) == 0) return icons[i].icon;
    if (kstrcmp(name, "Image Viewer") == 0) return ICON_APP_VIEWER;
    if (kstrcmp(name, "Minesweeper") == 0) return ICON_APP_MINES;
    if (kstrcmp(name, "Hello") == 0) return ICON_APP_HELLO;
    return ICON_WINDOW;
}
/* ---------- File browser parts ----------
 * The Places sidebar, the path bar and the detail list. Files, Open and
 * Save As draw with these and hit test the rectangles they were drawn in. */

#define PLACE_MAX 10
typedef struct { const char *name; int id, icon; } Place;
static const char *const place_folders[] = {"Desktop", "Documents", "Downloads", "Pictures", "Media", "Programs"};

/* Folders first, then devices from *devices on. */
static int places_build(Place *p, int *devices, int with_trash) {
    int n = 0;
    p[n++] = (Place){"Home", fs_root(), UI_ICON_HOME};
    for (unsigned i = 0; i < sizeof place_folders / sizeof *place_folders; i++) {
        int id = fs_find_child(fs_root(), place_folders[i]);
        if (fs_is_dir(id)) p[n++] = (Place){place_folders[i], id, i == 0 ? UI_ICON_DESK : UI_ICON_FOLDER};
    }
    *devices = n;
    p[n++] = (Place){"System", fs_root(), UI_ICON_DISK};
    if (with_trash && fs_is_dir(trash_id)) p[n++] = (Place){"Trash", trash_id, UI_ICON_TRASH};
    return n;
}

static int places_head_h(void) { return MODERN ? 28 : 26; }
static int places_row_y(int y, int k, int devices) {
    int ry = y + 4 + places_head_h() + k * ROW_H;
    return k >= devices ? ry + 6 + places_head_h() : ry;
}

static void places_draw(int x, int y, int w, int h, int cwd, int with_trash) {
    Place p[PLACE_MAX];
    int dev, n = places_build(p, &dev, with_trash), lit = -1;
    for (int k = 0; k < n && lit < 0; k++) if (p[k].id == cwd) lit = k;
    int hh = places_head_h(), hx = x + (MODERN ? 18 : 12);
    int ix = x + (MODERN ? 10 : 12), tx = ix + (MODERN ? 26 : 20);
    draw_rect(x, y, w - 1, h, ui_theme.sidebar);
    draw_vline(x + w - 1, y, h, TK_LINE_SUBTLE);
    for (int k = 0; k < n; k++) {
        int ry = places_row_y(y, k, dev);
        if (ry + ROW_H > y + h) break;
        if (k == 0 || k == dev)
            kit_text(k ? "Devices" : "Places", hx, kit_ty(ry - hh + 2, hh), TK_TEXT2, w - 24, 1);
        uint8_t paper = ui_theme.sidebar, ink = TK_TEXT;
        int on = k == lit;
        if (on) {
            paper = TK_ACCENT_100;
            if (MODERN) { kit_fill(x + 4, ry, w - 20, ROW_H, 6, paper, 1); ink = TK_ACCENT_700; }
            else draw_rect(x + 1, ry, w - 9, ROW_H, paper);
        } else if (hot(x, ry, w - 8, ROW_H) & KIT_HOVER) {
            paper = TK_HOVER;
            if (MODERN) kit_fill(x + 4, ry, w - 20, ROW_H, 6, paper, 1);
            else draw_rect(x + 1, ry, w - 9, ROW_H, paper);
        }
        ui_icon_draw(p[k].icon, ix, ry + (ROW_H - 16) / 2, 16, TK_ICON, paper);
        kit_text(p[k].name, tx, kit_ty(ry, ROW_H), ink, x + w - 10 - tx, on);
    }
}

static int places_hit(int x, int y, int w, int h, int with_trash) {
    if (!hit(mouse_x, mouse_y, x, y, w, h)) return -1;
    Place p[PLACE_MAX];
    int dev, n = places_build(p, &dev, with_trash);
    for (int k = 0; k < n; k++) {
        int ry = places_row_y(y, k, dev);
        if (ry + ROW_H > y + h) break;
        if (hit(mouse_x, mouse_y, x, ry, w, ROW_H)) return p[k].id;
    }
    return -1;
}

/* Path bar: a Home icon and the folder names split by chevrons. Classic
 * draws it in a field, Modern flat on the toolbar. Leading folders drop out
 * when the path does not fit. */
#define CRUMB_MAX 16
static const char *crumb_name(int id) { return id == fs_root() ? "Home" : fs_name(id); }
static int crumbs_layout(int x, int w, int cwd, int *xs, int *ws, int *ids) {
    int chain[CRUMB_MAX], cw[CRUMB_MAX], n = 0, total = 0;
    for (int id = cwd; fs_valid(id) && n < CRUMB_MAX; id = fs_parent(id)) {
        chain[n++] = id;
        if (id == fs_root()) break;
    }
    for (int k = 0; k < n; k++) {
        cw[k] = kit_text_w(crumb_name(chain[k]), 0) + 8 + (chain[k] == fs_root() ? 20 : 0);
        total += cw[k] + (k ? 16 : 0);
    }
    int first = n - 1, count = 0, cx = x;
    while (first > 0 && total > w) { total -= cw[first] + 16; first--; }
    for (int k = first; k >= 0; k--) {
        int width = cw[k];
        if (cx + width > x + w) width = x + w - cx;
        if (width < 16) break;
        xs[count] = cx; ws[count] = width; ids[count++] = chain[k];
        cx += width + 16;
    }
    return count;
}

static int crumbs_inner(int *x, int *w) {
    int pad = MODERN ? 4 : 6;
    *x += pad; *w -= pad * 2;
    return pad;
}

static void crumbs_draw(int x, int y, int w, int h, int cwd) {
    uint8_t paper = MODERN ? ui_theme.chrome : TK_RAISED;
    if (!MODERN) kit_field(x, y, w, h, 0);
    int ix = x, iw = w, xs[CRUMB_MAX], ws[CRUMB_MAX], ids[CRUMB_MAX];
    crumbs_inner(&ix, &iw);
    int n = crumbs_layout(ix, iw, cwd, xs, ws, ids), ty = kit_ty(y, h);
    for (int k = 0; k < n; k++) {
        int tx = xs[k] + 4;
        if (ids[k] == fs_root()) {
            ui_icon_draw(UI_ICON_HOME, tx - 2, y + (h - 16) / 2, 16, TK_ICON, paper);
            tx += 20;
        }
        kit_text(crumb_name(ids[k]), tx, ty, TK_TEXT, xs[k] + ws[k] - tx, 0);
        if (k < n - 1) ui_icon_draw(UI_ICON_RIGHT, xs[k] + ws[k], y + (h - 16) / 2, 16, TK_TEXT2, paper);
    }
}

static int crumbs_hit(int x, int y, int w, int h, int cwd) {
    if (!hit(mouse_x, mouse_y, x, y, w, h)) return -1;
    int ix = x, iw = w, xs[CRUMB_MAX], ws[CRUMB_MAX], ids[CRUMB_MAX];
    crumbs_inner(&ix, &iw);
    int n = crumbs_layout(ix, iw, cwd, xs, ws, ids);
    for (int k = 0; k < n; k++)
        if (mouse_x >= xs[k] && mouse_x < xs[k] + ws[k]) return ids[k];
    return -1;
}

/* Back, forward and up, each a square icon button. */
static int nav_size(void) { return MODERN ? 30 : 26; }
static int nav_x(int x, int i) { return x + (MODERN ? 12 : 8) + i * (nav_size() + (MODERN ? 4 : 2)); }
static int nav_end(int x) { return nav_x(x, 3) + 4; }
static void nav_draw(int x, int y, int h, const int *enabled) {
    static const int icon[3] = {UI_ICON_BACK, UI_ICON_FWD, UI_ICON_UP};
    int s = nav_size(), by = y + (h - s) / 2;
    for (int i = 0; i < 3; i++)
        kit_icon_button(nav_x(x, i), by, s, icon[i], enabled[i] ? hot(nav_x(x, i), by, s, s) : KIT_DISABLED);
}
static int nav_hit(int x, int y, int h) {
    int s = nav_size(), by = y + (h - s) / 2;
    for (int i = 0; i < 3; i++) if (hit(mouse_x, mouse_y, nav_x(x, i), by, s, s)) return i;
    return -1;
}

/* Node icons: the app's own icon for applications, else by kind. */
static int code_name(const char *nm) {
    static const char *const ext[] = {".c", ".h", ".asm", ".s", ".py", ".js", ".sh", ".ld", ".mk"};
    for (unsigned i = 0; i < sizeof ext / sizeof *ext; i++) if (file_extension(nm, ext[i])) return 1;
    return 0;
}
static void node_icon_draw(int id, int x, int y, int size, uint8_t paper) {
    const char *nm = fs_name(id);
    int big = size >= 32;
    if (fs_is_app(id)) { app_icon_draw(app_icon_for(app_icon_by_name(nm)), x, y, big ? AI_BIG : AI_SMALL); return; }
    if (!fs_is_dir(id) && file_extension(nm, ".bex")) { app_icon_draw(AI_NATIVE, x, y, big ? AI_BIG : AI_SMALL); return; }
    int icon = big ? UI_ICON_A_DOC : UI_ICON_DOC;
    if (id == trash_id) icon = big ? UI_ICON_A_TRASH : UI_ICON_TRASH;
    else if (fs_is_dir(id)) icon = big ? UI_ICON_A_FILES : UI_ICON_FOLDER;
    else if (is_image_file(id)) icon = big ? UI_ICON_A_IMG : UI_ICON_IMG;
    else if (code_name(nm)) icon = big ? UI_ICON_A_CODE : UI_ICON_CODE;
    ui_icon_draw(icon, x, y, size, TK_ICON, paper);
}

static void size_text(unsigned bytes, char *out) {
    if (bytes < 1024u * 1024u) {
        fmt_uint(out, (bytes + 1023) / 1024);
        kstrcpy(out + kstrlen(out), " KB");
        return;
    }
    unsigned tenths = (unsigned)(((unsigned long long)bytes * 10 + 524288) / 1048576);
    fmt_uint(out, tenths / 10);
    int n = kstrlen(out);
    if (tenths < 1000) { out[n++] = '.'; out[n++] = (char)('0' + tenths % 10); }
    kstrcpy(out + n, " MB");
}

/* "4 KB", "3 items" or "App". */
static void node_size_text(int id, char *out) {
    if (fs_is_dir(id)) {
        int n = fs_child_count(id);
        fmt_uint(out, (unsigned)n);
        kstrcpy(out + kstrlen(out), n == 1 ? " item" : " items");
    } else if (fs_is_app(id)) kstrcpy(out, "App");
    else size_text((unsigned)fs_size(id), out);
}

static int leap_year(unsigned y) { return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0); }
/* Timestamps count seconds from 2000-01-01. */
static unsigned day_number(int year, int month, int day) {
    unsigned n = 0;
    for (int y = 2000; y < year; y++) n += leap_year((unsigned)y) ? 366 : 365;
    for (int m = 1; m < month; m++) n += (unsigned)rtc_days_in_month(year, m);
    return n + (unsigned)day - 1;
}
static void day_date(unsigned days, int *year, int *month, int *day) {
    unsigned y = 2000;
    for (;;) {
        unsigned yd = leap_year(y) ? 366 : 365;
        if (days < yd) break;
        days -= yd; y++;
    }
    int m = 1;
    while (m < 12 && days >= (unsigned)rtc_days_in_month((int)y, m)) days -= (unsigned)rtc_days_in_month((int)y, m++);
    *year = (int)y; *month = m; *day = (int)days + 1;
}
/* "14:30" or "2:30 PM", following the clock setting. */
static void clock_text(int hour, int min, char *out) {
    int n = 0;
    if (clock_flags & CLOCK_12H) {
        int h = hour % 12 ? hour % 12 : 12;
        fmt_uint(out, (unsigned)h);
        n = kstrlen(out);
    } else { fmt_pad2(out, hour); n = 2; }
    out[n++] = ':';
    fmt_pad2(out + n, min); n += 2;
    if (clock_flags & CLOCK_12H) { kstrcpy(out + n, hour < 12 ? " AM" : " PM"); n += 3; }
    out[n] = 0;
}
/* "Today 14:30", "Yesterday 18:02" or "18 Oct 2026". */
static void stamp_text(unsigned stamp, char *out) {
    if (!stamp) { kstrcpy(out, "Unknown"); return; }
    RtcTime now;
    rtc_read(&now);
    unsigned today = day_number(now.year, now.month, now.day), day = stamp / 86400;
    if (day == today || day + 1 == today) {
        kstrcpy(out, day == today ? "Today " : "Yesterday ");
        clock_text((int)(stamp / 3600 % 24), (int)(stamp / 60 % 60), out + kstrlen(out));
        return;
    }
    int y, m, d;
    day_date(day, &y, &m, &d);
    fmt_uint(out, (unsigned)d);
    int n = kstrlen(out);
    out[n++] = ' ';
    for (int i = 0; i < 3; i++) out[n++] = rtc_month_name[m - 1][i];
    out[n++] = ' ';
    fmt_uint(out + n, (unsigned)y);
}

/* Detail list: Name, then Size and Modified at the right when there is
 * room. Size and date columns start at the returned x values (0: hidden). */
enum { FL_SIZE = 1, FL_DATE = 2 };
static int flist_head_h(void) { return MODERN ? 32 : 24; }
static void flist_columns(int x, int w, int cols, int *name_x, int *size_x, int *date_x) {
    int dw = (cols & FL_DATE) && w >= 360 ? (MODERN ? 128 : 124) : 0;
    int sw = (cols & FL_SIZE) && w >= 420 ? 72 : 0;
    *name_x = x + (MODERN ? 16 : 8);
    *date_x = dw ? x + w - dw : 0;
    *size_x = sw ? (dw ? *date_x : x + w) - sw : 0;
}
static int flist_name_end(int x, int w, int cols) {
    int nx, sx, dx;
    flist_columns(x, w, cols, &nx, &sx, &dx);
    return (sx ? sx : dx ? dx : x + w) - 8;
}

static void sort_arrow(int x, int y, int up) {
    for (int r = 0; r < 4; r++) draw_hline(x + r, up ? y + r : y + 3 - r, 7 - 2 * r, TK_TEXT2);
}

static void flist_header(int x, int y, int w, int cols, const FileViewOptions *v) {
    int h = flist_head_h(), nx, sx, dx, ty = kit_ty(y, h);
    flist_columns(x, w, cols, &nx, &sx, &dx);
    kit_list_header(x, y, w, h);
    static const int sorts[3] = {FILE_VIEW_NAME, FILE_VIEW_SIZE, FILE_VIEW_MODIFIED};
    static const char *const names[3] = {"Name", "Size", "Modified"};
    int at[3] = {nx, sx ? sx + 8 : 0, dx ? dx + 8 : 0};
    for (int c = 0; c < 3; c++) {
        if (!at[c]) continue;
        if (!MODERN && c) draw_vline(at[c] - 8, y + 4, h - 8, TK_LINE_SUBTLE);
        int tw = kit_text(names[c], at[c], ty, TK_TEXT, x + w - at[c], 1);
        if (v && v->sort == sorts[c]) sort_arrow(at[c] + tw + 6, ty + 7, !v->descending);
    }
}

/* FILE_VIEW_* for a header click, or -1. */
static int flist_header_hit(int x, int y, int w, int cols) {
    int nx, sx, dx;
    if (!hit(mouse_x, mouse_y, x, y, w, flist_head_h())) return -1;
    flist_columns(x, w, cols, &nx, &sx, &dx);
    if (dx && mouse_x >= dx) return FILE_VIEW_MODIFIED;
    if (sx && mouse_x >= sx) return FILE_VIEW_SIZE;
    return FILE_VIEW_NAME;
}

/* One row. warn tints it amber (a name that would be replaced). */
static void flist_row(int x, int y, int w, int cols, int id, int state, int warn) {
    int h = ROW_H, nx, sx, dx;
    flist_columns(x, w, cols, &nx, &sx, &dx);
    if (warn && !(state & KIT_ON)) kit_fill(MODERN ? x + 6 : x, y, MODERN ? w - 12 : w, h, MODERN ? 6 : 0, TK_AMBER_50, 1);
    uint8_t ink = kit_row(x, y, w, h, state), dim = kit_row_dim(state);
    uint8_t paper = (state & KIT_ON) ? ((state & KIT_UNFOCUSED) ? TK_ACCENT_100 : TK_ACCENT) :
                    warn ? TK_AMBER_50 : ui_theme.content;
    int ty = kit_ty(y, h), tx = nx + (MODERN ? 24 : 22);
    char text[32];
    node_icon_draw(id, nx, y + (h - 16) / 2, 16, paper);
    kit_text(fs_name(id), tx, ty, ink, flist_name_end(x, w, cols) - tx, 0);
    if (sx) {
        node_size_text(id, text);
        kit_text_right(text, (dx ? dx : x + w) - 16, ty, dim, 0);
    }
    if (dx) {
        stamp_text(fs_modified(id), text);
        kit_text(text, dx + 8, ty, dim, x + w - dx - 12, 0);
    }
}

static void files_title(char *title, int max) {
    const char *nm = fm_cwd == fs_root() ? "Home" : fs_name(fm_cwd);
    int i = 0;
    while (nm[i] && i < max - 1) {
        title[i] = nm[i];
        i++;
    }
    title[i] = 0;
}

/* Status bar: a pending file action's message, else the item count and
 * the selection; the free space goes at the right. */
static void files_status(char *left, int max, char *right) {
    int n = 0;
    if((files_action_busy&&(int32_t)(files_message_until-frame_count)>0)||
       (file_clipboard_status()[0]&&((int32_t)(files_message_until-frame_count)>0||file_clipboard_pending_sync()))){
        const char *message=files_action_busy&&(int32_t)(files_message_until-frame_count)>0?
            "Disk saving. Retry the file action.":file_clipboard_status();
        while(n+1<max&&message[n]){left[n]=message[n];n++;}
        left[n]=0;
    } else {
        char number[16];
        fmt_uint(number, (unsigned)fm_count);
        kstrcpy(left, number);
        if (fm_view.filter[0]) {
            kstrcpy(left + kstrlen(left), " of ");
            fmt_uint(number, (unsigned)fm_total);
            kstrcpy(left + kstrlen(left), number);
        }
        kstrcpy(left + kstrlen(left), fm_count == 1 && !fm_view.filter[0] ? " item" : " items");
        int id = fm_row_id(fm_selected);
        if (id >= 0) {
            kstrcpy(left + kstrlen(left), ", 1 selected");
            if (!fs_is_dir(id) && !fs_is_app(id)) {
                kstrcpy(left + kstrlen(left), " (");
                size_text((unsigned)fs_size(id), left + kstrlen(left));
                kstrcpy(left + kstrlen(left), ")");
            }
        }
    }
    unsigned cap = fs_capacity(), used = fs_used_bytes();
    size_text(cap > used ? cap - used : 0, right);
    kstrcpy(right + kstrlen(right), " free");
}

/* Files layout: Places sidebar at the left, a toolbar across the top (back,
 * forward, up, path, view buttons and search), the items as icon tiles or
 * detail rows, and a status bar. fm_first is the first item shown; in the
 * icon view it starts a row. */
#define FILES_TOOL_H (MODERN ? 44 : 34)
#define FILES_STATUS_H (MODERN ? 28 : 24)
#define FILES_TILE_W (MODERN ? 96 : 92)
#define FILES_TILE_H (MODERN ? 92 : 84)
#define FILES_GAP 4
#define FILES_PAD 12
static int files_side_w(int ww) { return ww >= 560 ? (MODERN ? 176 : 160) : 0; }
static int files_tool_y(int wy) { return MODERN ? wy + TITLE_BAR - 1 : wy + TITLE_H; }
static int files_body_y(int wy) { return files_tool_y(wy) + FILES_TOOL_H; }
static int files_status_y(int wy, int wh) { return wy + wh - 1 - FILES_STATUS_H; }
static int files_content_x(int wx, int ww) { return wx + 1 + files_side_w(ww); }
static int files_content_w(int ww) { return ww - 2 - files_side_w(ww); }
static int files_header_h(void) { return MODERN ? 32 : 24; }
static int files_list_y(int wy) { return files_body_y(wy) + (fm_details ? files_header_h() : FILES_PAD); }
static int files_list_bottom(int wy, int wh) { return files_status_y(wy, wh) - (fm_details ? 0 : 4); }
static int files_cols(int ww) {
    if (fm_details) return 1;
    int c = (files_content_w(ww) - 2 * FILES_PAD - SB + FILES_GAP) / (FILES_TILE_W + FILES_GAP);
    return c < 1 ? 1 : c;
}
static int files_item_h(void) { return fm_details ? ROW_H : FILES_TILE_H + FILES_GAP; }
static int files_rows(int wy, int wh) {
    int r = (files_list_bottom(wy, wh) - files_list_y(wy)) / files_item_h();
    return r < 1 ? 1 : r;
}
static int files_scrolls(int ww, int wy, int wh) { return fm_vis_count() > files_rows(wy, wh) * files_cols(ww); }
/* The rectangle of item i, when it is on screen. */
static int files_item_rect(int wx, int wy, int ww, int wh, int i, int *x, int *y, int *w, int *h) {
    int cols = files_cols(ww), k = i - fm_first;
    if (i < 0 || i >= fm_vis_count() || k < 0 || k >= files_rows(wy, wh) * cols) return 0;
    if (fm_details) {
        *x = files_content_x(wx, ww);
        *y = files_list_y(wy) + k * ROW_H;
        *w = files_content_w(ww) - (files_scrolls(ww, wy, wh) ? SB : 0);
        *h = ROW_H;
    } else {
        *x = files_content_x(wx, ww) + FILES_PAD + (k % cols) * (FILES_TILE_W + FILES_GAP);
        *y = files_list_y(wy) + (k / cols) * (FILES_TILE_H + FILES_GAP);
        *w = FILES_TILE_W;
        *h = FILES_TILE_H;
    }
    return 1;
}
/* The item under the pointer: an index, -1 for empty space among the
 * items, or -2 outside them. */
static int files_item_at(int wx, int wy, int ww, int wh) {
    int ax = files_content_x(wx, ww), ay = files_list_y(wy) - (fm_details ? 0 : FILES_PAD);
    int aw = files_content_w(ww) - (files_scrolls(ww, wy, wh) ? SB : 0);
    if (!hit(mouse_x, mouse_y, ax, ay, aw, files_list_bottom(wy, wh) - ay)) return -2;
    int cols = files_cols(ww), page = files_rows(wy, wh) * cols;
    for (int i = fm_first; i < fm_vis_count() && i < fm_first + page; i++) {
        int x, y, w, h;
        if (files_item_rect(wx, wy, ww, wh, i, &x, &y, &w, &h) && hit(mouse_x, mouse_y, x, y, w, h)) return i;
    }
    return -1;
}
/* The label part of an item, where a click on the selection renames. */
static int files_label_hit(int x, int y, int w, int h) {
    if (fm_details) return mouse_x >= x + (MODERN ? 40 : 30) && hit(mouse_x, mouse_y, x, y, w, h);
    return hit(mouse_x, mouse_y, x, y + 46, w, h - 46);
}

static void files_scroll(int rows, int cols) {
    if (rows < 1) rows = 1;
    if (cols < 1) cols = 1;
    int page = rows * cols, vis = fm_vis_count();
    if (!fm_manual_scroll && fm_selected >= 0) {
        if (fm_selected < fm_first) fm_first = fm_selected / cols * cols;
        if (fm_selected >= fm_first + page) fm_first = (fm_selected / cols - rows + 1) * cols;
    }
    int max = (vis + cols - 1) / cols - rows;
    if (max < 0) max = 0;
    if (fm_first > max * cols) fm_first = max * cols;
    if (fm_first < 0) fm_first = 0;
    fm_first -= fm_first % cols;
}

static void tile_label(const char *s, int x, int y, int w, uint8_t ink, int bold);

/* An icon tile: the 32px icon and up to two lines of name. */
static void draw_file_tile(int x, int y, int w, int h, int id, int state, int cut) {
    uint8_t bg = 0;
    if (state & KIT_ON) bg = TK_ACCENT_100;
    else if (state & KIT_HOVER) bg = MODERN ? TK_SUNKEN : TK_ACCENT_50;
    if (bg) kit_fill(x, y, w, h, MODERN ? 8 : 0, bg, 1);
    if ((state & KIT_ON) && !(state & KIT_UNFOCUSED) && !MODERN) kit_frame(x, y, w, h, 0, TK_ACCENT);
    node_icon_draw(id, x + (w - 32) / 2, y + 10, 32, bg ? bg : ui_theme.content);
    tile_label(fs_name(id), x + 4, y + 48, w - 8, cut ? TK_TEXT2 : TK_TEXT, 0);
}

static int files_is_cut(int id) {
    return file_clipboard_mode() == FILE_CLIPBOARD_CUT && file_clipboard_source() == id &&
           file_clipboard_identity() == fs_identity(id);
}

/* Toolbar pieces, shared by drawing and clicks. */
static int files_search_w(int ww) { return ww >= 640 ? 200 : ww >= 480 ? 150 : 0; }
static int files_search_x(int wx, int ww) { return wx + ww - (MODERN ? 12 : 8) - files_search_w(ww); }
static int files_view_x(int wx, int ww) {
    int right = files_search_w(ww) ? files_search_x(wx, ww) - 8 : wx + ww - 8;
    return right - 2 * nav_size();
}
static int files_ctl_y(int wy) { return files_tool_y(wy) + (FILES_TOOL_H - 1 - BTN_H) / 2; }

static void draw_files(int wx, int wy, int ww, int wh, int inactive) {
    char title[FS_NAME_LEN], left[FS_PATH_LEN], right[24];
    fm_history_sync();
    files_title(title, sizeof title);
    gui_draw_window(wx, wy, ww, wh, title, 0, inactive ? WIN_INACTIVE : 0);
    int ty = files_tool_y(wy), by = files_body_y(wy), sy = files_status_y(wy, wh), side = files_side_w(ww);
    /* Toolbar */
    kit_toolbar(wx + 1, ty, ww - 2, FILES_TOOL_H);
    int enabled[3] = {fm_history_target(-1) >= 0, fm_history_target(1) >= 0, fm_has_parent()};
    nav_draw(wx, ty, FILES_TOOL_H - 1, enabled);
    int cy = files_ctl_y(wy), vx = files_view_x(wx, ww), vs = nav_size(), vy = ty + (FILES_TOOL_H - 1 - vs) / 2;
    crumbs_draw(nav_end(wx), cy, vx - 8 - nav_end(wx), BTN_H, fm_cwd);
    if (MODERN) kit_box(vx - 2, vy - 2, 2 * vs + 4, vs + 4, 8, TK_SUNKEN, 0);
    kit_icon_button(vx, vy, vs, UI_ICON_LIST, fm_details ? KIT_ON : hot(vx, vy, vs, vs));
    kit_icon_button(vx + vs, vy, vs, UI_ICON_GRID, !fm_details ? KIT_ON : hot(vx + vs, vy, vs, vs));
    int sw = files_search_w(ww);
    if (sw) {
        int sx = files_search_x(wx, ww), focus = fm_filter_focus && !inactive;
        char placeholder[FS_NAME_LEN + 8];
        kstrcpy(placeholder, "Search ");
        kstrcpy(placeholder + 7, title);
        int tx = kit_search(sx, cy, sw, BTN_H, fm_view.filter, placeholder, focus ? KIT_FOCUS : 0);
        if (focus && fm_filter_select_all && fm_view.filter[0]) {
            int fw = kit_text_w(fm_view.filter, 0), lim = kit_search_clear_x(sx, sw) - tx - 4;
            draw_rect(tx - 1, kit_ty(cy, BTN_H), (fw < lim ? fw : lim) + 2, UI_FONT_H, TK_ACCENT);
            kit_text(fm_view.filter, tx, kit_ty(cy, BTN_H), TK_ON_ACCENT, lim, 0);
        } else if (focus && ((frame_count / 35) & 1) == 0)
            kit_caret(tx + kit_text_w(fm_view.filter, 0), kit_ty(cy, BTN_H));
    }
    /* Sidebar */
    if (side) places_draw(wx + 1, by, side, sy - by, fm_cwd, 1);
    /* Items */
    int cx = files_content_x(wx, ww), cw = files_content_w(ww), cols = files_cols(ww), rows = files_rows(wy, wh);
    int vis = fm_vis_count(), unfocused = inactive || fm_filter_focus ? KIT_UNFOCUSED : 0;
    files_scroll(rows, cols);
    if (fm_details) flist_header(cx, by, cw, FL_SIZE | FL_DATE, &fm_view);
    for (int i = fm_first; i < vis && i < fm_first + rows * cols; i++) {
        int x, y, w, h, id = fm_row_id(i);
        if (id < 0 || !files_item_rect(wx, wy, ww, wh, i, &x, &y, &w, &h)) continue;
        int renaming = fm_renaming && id == fm_rename_id && fs_identity(id) == fm_rename_identity;
        int state = i == fm_selected ? KIT_ON | unfocused : renaming || inactive ? 0 : hot(x, y, w, h);
        if (fm_details) {
            flist_row(x, y, w, FL_SIZE | FL_DATE, id, state, 0);
        } else draw_file_tile(x, y, w, h, id, state, files_is_cut(id));
    }
    if (!fm_count) {
        const char *empty = fm_view.filter[0] ? "No matching names" : "This folder is empty";
        int ey = files_list_y(wy) + (fm_details ? 8 : 24);
        if (ey + UI_FONT_H <= sy) kit_text_center(empty, cx, ey, cw, TK_TEXT2, 0);
    }
    if (files_scrolls(ww, wy, wh)) {
        int top = fm_details ? files_list_y(wy) : by + 2, len = sy - top - 2;
        int total_rows = (vis + cols - 1) / cols;
        kit_vscroll(cx + cw - SB, top, len, fm_first / cols, rows, total_rows, 0);
    }
    /* Status bar */
    files_status(left, sizeof left, right);
    if (MODERN) {
        draw_hline(wx + 1, sy, ww - 2, TK_LINE_SUBTLE);
        kit_fill_bottom(wx + 1, sy + 1, ww - 2, FILES_STATUS_H, ui_theme.r_window - 1, ui_theme.statusbar);
    } else kit_statusbar(wx + 1, sy, ww - 2, FILES_STATUS_H);
    int tty = kit_ty(sy + 1, FILES_STATUS_H - 1), pad = MODERN ? 12 : 8;
    int rw = kit_text_w(right, 0);
    kit_text(left, wx + pad, tty, TK_TEXT2, ww - pad * 3 - rw, 0);
    kit_text_right(right, wx + ww - pad, tty, TK_TEXT2, 0);
}

/* The rename field sits over the name: in the row, or under the icon. */
static int files_rename_rect(int wx, int wy, int ww, int wh, int *fx, int *fy, int *fw, int *fh) {
    int row = -1;
    for (int i = 0; i < fm_count; i++)
        if (fm_ids[i] == fm_rename_id && fm_ids_identity[i] == fm_rename_identity) { row = i; break; }
    int x, y, w, h;
    if (row < 0 || !files_item_rect(wx, wy, ww, wh, row, &x, &y, &w, &h)) return 0;
    int tw = kit_text_w(fm_rename_buf, 0) + 16;
    if (fm_details) {
        *fx = x + (MODERN ? 36 : 26); *fy = y + 1; *fh = h - 2;
        *fw = tw < 120 ? 120 : tw;
        int end = flist_name_end(x, w, FL_SIZE | FL_DATE);
        if (*fx + *fw > end) *fw = end - *fx;
    } else {
        *fw = tw < w ? w : tw;
        if (*fw > w + 40) *fw = w + 40;
        *fx = x + (w - *fw) / 2; *fy = y + 46; *fh = 22;
    }
    if (*fw < 24) *fw = 24;
    return 1;
}

static void draw_rename_overlay(void) {
    int wx, wy, ww, wh, fx, fy, fw, fh;
    if (!fm_renaming || !win_geom_kind(WK_FILES, &wx, &wy, &ww, &wh)) return;
    if (!files_rename_rect(wx, wy, ww, wh, &fx, &fy, &fw, &fh)) return;
    int tx = kit_field(fx, fy, fw, fh, KIT_FOCUS), ty = kit_ty(fy, fh);
    int used = kit_text(fm_rename_buf, tx, ty, TK_TEXT, fw - (tx - fx) - 4, 0);
    if ((frame_count / 35) % 2 == 0) kit_caret(tx + used, ty);
}

/* ---------- Open and Save As ----------
 * One browser serves both: Open hides what the app cannot open, Save As
 * shows everything and dims other types. Both keep their own back and
 * forward history. */

static FileViewOptions pick_view;
static int pick_hidden, pick_show_all, pick_save, pick_manual;
static int pick_hist[16], pick_hist_n, pick_hist_pos;
static char pick_typed[FS_NAME_LEN];
static int pick_typed_len;
static const char *pick_message;

static const char *const saveas_exts[7] = {".txt", ".pbm", ".bwr", ".rtf", ".bsh", ".csv", ".pdf"};
static const char *const saveas_types[7] = {
    "Plain text (.txt)", "Picture (.pbm)", "Writer document (.bwr)", "Rich text (.rtf)",
    "Spreadsheet (.bsh)", "CSV values (.csv)", "PDF document (.pdf)"
};
static int name_target_valid(void) { return name_target >= 0 && name_target < 7; }

static int od_row_enabled(int id) {
    if (pick_save) return fs_is_dir(id) || (!fs_is_app(id) && name_target_valid() &&
                                           file_extension(fs_name(id), saveas_exts[name_target]));
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
    if (!fs_is_dir(pick_cwd)) pick_cwd = fs_root();
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
    pick_view.hidden[0] = pick_cwd == fs_root() ? trash_id : -1;
    pick_view.hidden[1] = pick_cwd == fs_root() ? prefs_id : -1;
    int total = 0, n = pick_ids ? file_view_build(pick_cwd, &pick_view, pick_ids, pick_identity, pick_capacity, &total) : 0;
    pick_count = pick_hidden = 0;
    for (int i = 0; i < n; i++) {
        if (!pick_save && !pick_show_all && !od_row_enabled(pick_ids[i])) { pick_hidden++; continue; }
        pick_ids[pick_count] = pick_ids[i];
        pick_identity[pick_count++] = pick_identity[i];
    }
    if (pick_selected >= pick_count)
        pick_selected = pick_count - 1;
    if (pick_selected < 0)
        pick_selected = pick_save ? -1 : 0;
    if (pick_selected >= 0 && pick_count && !od_row_enabled(pick_ids[pick_selected]))
        pick_selected = pick_save ? -1 : od_next_enabled(pick_selected, 1);
}

static void pick_go(int id, int record) {
    if (!fs_is_dir(id)) return;
    if (record && id != pick_cwd) {
        if (pick_hist_n == 0) { pick_hist[0] = pick_cwd; pick_hist_n = 1; pick_hist_pos = 0; }
        pick_hist_n = pick_hist_pos + 1;
        if (pick_hist_n == 16) {
            for (int i = 1; i < 16; i++) pick_hist[i - 1] = pick_hist[i];
            pick_hist_n--;
        }
        pick_hist[pick_hist_n++] = id;
        pick_hist_pos = pick_hist_n - 1;
    }
    pick_cwd = id;
    pick_first = pick_manual = 0;
    pick_selected = pick_save ? -1 : 0;
    pick_view.filter[0] = 0;
    pick_typed_len = 0; pick_typed[0] = 0;
    pick_message = 0;
    od_refresh();
    dirty = 1;
}

static void pick_history_step(int dir) {
    int p = pick_hist_pos + dir;
    if (p < 0 || p >= pick_hist_n || !fs_is_dir(pick_hist[p])) return;
    pick_hist_pos = p;
    pick_go(pick_hist[p], 0);
}

static void pick_reset(int cwd, int save) {
    pick_save = save;
    pick_manual = 0;
    pick_focus = pick_first = 0;
    pick_show_all = 0;
    pick_view.filter[0] = 0;
    pick_view.sort = FILE_VIEW_NAME;
    pick_view.descending = 0;
    pick_hist_n = pick_hist_pos = 0;
    pick_last_click_item = -1;
    pick_cwd = fs_is_dir(cwd) ? cwd : fs_root();
    pick_selected = save ? -1 : 0;
    pick_typed_len = 0; pick_typed[0] = 0;
    pick_message = 0;
    od_refresh();
}

static int pick_row_id(int row) {
    if (row < 0 || row >= pick_count) return -1;
    return fs_identity(pick_ids[row]) == pick_identity[row] ? pick_ids[row] : -1;
}

static void pick_sort(int sort) {
    if (pick_view.sort == sort) pick_view.descending = !pick_view.descending;
    else { pick_view.sort = sort; pick_view.descending = sort == FILE_VIEW_MODIFIED; }
    int id = pick_row_id(pick_selected);
    od_refresh();
    for (int i = 0; i < pick_count; i++) if (pick_ids[i] == id) pick_selected = i;
    dirty = 1;
}

static void pick_scroll(int rows, int visible) {
    int max = pick_count - visible;
    if (max < 0) max = 0;
    pick_first += rows;
    pick_manual = 1;
    if (pick_first > max) pick_first = max;
    if (pick_first < 0) pick_first = 0;
    dirty = 1;
}

/* Shared layout of the two dialogs. band is the height above the toolbar
 * (Save As puts its name field there), foot the height of the footer. */
typedef struct {
    int x, y, w, h, band_y, tool_y, tool_h, body_y, body_h, side_w, list_x, list_w, rows_y, rows, foot_y, foot_h;
} Chooser;

static void chooser_layout(Chooser *c, int x, int y, int w, int h, int band, int foot) {
    c->x = x; c->y = y; c->w = w; c->h = h;
    c->band_y = y + TITLE_BAR - (MODERN ? 1 : 0);
    c->tool_y = c->band_y + band;
    c->tool_h = MODERN ? 44 : 34;
    c->body_y = c->tool_y + c->tool_h;
    c->foot_h = foot;
    c->foot_y = y + h - foot;
    c->body_h = c->foot_y - c->body_y;
    c->side_w = w >= 520 ? (MODERN ? 156 : 148) : 0;
    c->list_x = x + 1 + c->side_w;
    c->list_w = w - 2 - c->side_w;
    c->rows_y = c->body_y + flist_head_h();
    c->rows = (c->foot_y - c->rows_y - 4) / ROW_H;
    if (c->rows < 1) c->rows = 1;
}

static int chooser_cols(void) { return pick_save ? FL_DATE : FL_SIZE | FL_DATE; }
static int chooser_row_w(const Chooser *c) { return c->list_w - (pick_count > c->rows ? SB : 0); }
static int chooser_right_w(void) { return pick_save ? kit_button_w("New Folder", UI_ICON_NEWF) : 168; }
static int chooser_right_x(const Chooser *c) { return c->x + c->w - (MODERN ? 12 : 8) - chooser_right_w(); }
static int chooser_ctl_y(const Chooser *c) { return c->tool_y + (c->tool_h - 1 - BTN_H) / 2; }

/* Footer band: Classic paints the base color, Modern the chrome color with
 * the window's rounded bottom. */
static void dlg_footer(int x, int w, int fy, int fh) {
    if (MODERN) {
        draw_hline(x + 1, fy, w - 2, TK_LINE);
        kit_fill_bottom(x + 1, fy + 1, w - 2, fh - 2, ui_theme.r_window - 1, ui_theme.chrome);
    } else {
        draw_hline(x + 1, fy, w - 2, TK_LINE_SUBTLE);
        draw_rect(x + 1, fy + 1, w - 2, fh - 2, TK_BASE);
    }
}

/* The name of a row that would be replaced, for Save As. */
static int saveas_existing(void) {
    if (!pick_save || !name_len) return -1;
    for (int i = 0; i < pick_count; i++)
        if (kstrcmp(fs_name(pick_ids[i]), name_buf) == 0) return i;
    return -1;
}

static void chooser_draw(const Chooser *c) {
    int x = c->x, w = c->w;
    kit_toolbar(x + 1, c->tool_y, w - 2, c->tool_h);
    int enabled[3] = {pick_hist_pos > 0, pick_hist_pos < pick_hist_n - 1, pick_cwd != fs_root()};
    nav_draw(x, c->tool_y, c->tool_h - 1, enabled);
    int cy = chooser_ctl_y(c), rx = chooser_right_x(c);
    crumbs_draw(nav_end(x), cy, rx - 8 - nav_end(x), BTN_H, pick_cwd);
    if (pick_save) kit_button(rx, cy, chooser_right_w(), BTN_H, "New Folder", UI_ICON_NEWF, KB_DEFAULT,
                              hot(rx, cy, chooser_right_w(), BTN_H));
    else kit_search(rx, cy, chooser_right_w(), BTN_H, pick_view.filter, "Search",
                    pick_focus == 3 ? KIT_FOCUS : 0);
    if (!pick_save && pick_focus == 3 && ((frame_count / 35) & 1) == 0)
        kit_caret(rx + (MODERN ? 11 : 7) + 21 + kit_text_w(pick_view.filter, 0), kit_ty(cy, BTN_H));
    if (c->side_w) places_draw(x + 1, c->body_y, c->side_w, c->body_h, pick_cwd, 0);
    int cols = chooser_cols(), rw = chooser_row_w(c);
    flist_header(c->list_x, c->body_y, c->list_w, cols, &pick_view);
    if (pick_selected >= 0 && !pick_manual) {
        if (pick_selected < pick_first) pick_first = pick_selected;
        if (pick_selected >= pick_first + c->rows) pick_first = pick_selected - c->rows + 1;
    }
    if (pick_first > pick_count - c->rows) pick_first = pick_count - c->rows;
    if (pick_first < 0) pick_first = 0;
    int warn = saveas_existing(), y = c->rows_y;
    for (int i = pick_first; i < pick_count && i < pick_first + c->rows; i++, y += ROW_H) {
        int id = pick_ids[i], state = 0;
        if (!od_row_enabled(id)) state = KIT_DISABLED;
        else if (i == pick_selected && i != warn) state = KIT_ON | (pick_focus ? KIT_UNFOCUSED : 0);
        flist_row(c->list_x, y, rw, cols, id, state, i == warn);
    }
    int nx, sx, dx;
    flist_columns(c->list_x, rw, cols, &nx, &sx, &dx);
    if (!pick_count && !pick_hidden)
        kit_text(pick_view.filter[0] ? "No matching names" : "This folder is empty", nx, kit_ty(y, ROW_H),
                 TK_TEXT2, rw - 24, 0);
    if (pick_hidden && y + ROW_H <= c->foot_y) {
        char note[64];
        fmt_uint(note, (unsigned)pick_hidden);
        kstrcpy(note + kstrlen(note), pick_hidden == 1 ? " file hidden by the filter" : " files hidden by the filter");
        kit_text(note, nx, kit_ty(y + 4, ROW_H), TK_TEXT2, rw - 24, 0);
    }
    if (pick_count > c->rows)
        kit_vscroll(c->list_x + c->list_w - SB, c->rows_y, c->rows * ROW_H, pick_first, c->rows, pick_count, 0);
}

static int saveas_click_row(int i, int dbl);
static void files_refresh_all(void);
static void od_open_selected(void);

/* Clicks in the toolbar, sidebar and list. Returns 1 when handled. */
static int chooser_click(const Chooser *c) {
    int n = nav_hit(c->x, c->tool_y, c->tool_h - 1);
    if (n == 0) { pick_history_step(-1); return 1; }
    if (n == 1) { pick_history_step(1); return 1; }
    if (n == 2) { if (pick_cwd != fs_root()) pick_go(fs_parent(pick_cwd), 1); return 1; }
    int cy = chooser_ctl_y(c), rx = chooser_right_x(c), id;
    if ((id = crumbs_hit(nav_end(c->x), cy, rx - 8 - nav_end(c->x), BTN_H, pick_cwd)) >= 0) {
        if (id != pick_cwd) pick_go(id, 1);
        return 1;
    }
    if (hit(mouse_x, mouse_y, rx, cy, chooser_right_w(), BTN_H)) {
        if (!pick_save) {
            if (pick_view.filter[0] && mouse_x >= kit_search_clear_x(rx, chooser_right_w())) {
                pick_view.filter[0] = 0; od_refresh();
            }
            pick_focus = 3; dirty = 1;
            return 1;
        }
        if (fs_sync_busy()) { pick_message = "Disk saving. Retry New Folder shortly."; dirty = 1; return 1; }
        char name[FS_NAME_LEN];
        int made = fs_unique_dir(pick_cwd, name) < 0 ? -1 : fs_mkdir(pick_cwd, name);
        if (made < 0) { pick_message = "Could not create a folder here."; dirty = 1; return 1; }
        pick_message = 0;
        od_refresh();
        for (int i = 0; i < pick_count; i++) if (pick_ids[i] == made) pick_selected = i;
        files_refresh_all();
        dirty = 1;
        return 1;
    }
    if (c->side_w && (id = places_hit(c->x + 1, c->body_y, c->side_w, c->body_h, 0)) >= 0) {
        if (id != pick_cwd) pick_go(id, 1);
        return 1;
    }
    int cols = chooser_cols(), rw = chooser_row_w(c), sort = flist_header_hit(c->list_x, c->body_y, c->list_w, cols);
    if (sort >= 0) { pick_sort(sort); return 1; }
    if (pick_count > c->rows && hit(mouse_x, mouse_y, c->list_x + c->list_w - SB, c->rows_y, SB, c->rows * ROW_H)) {
        int at, size;
        kit_scroll_thumb(c->rows * ROW_H, pick_first, c->rows, pick_count, &at, &size);
        pick_scroll(mouse_y < c->rows_y + at ? -c->rows : c->rows, c->rows);
        return 1;
    }
    if (!hit(mouse_x, mouse_y, c->list_x, c->rows_y, rw, c->foot_y - c->rows_y)) return 0;
    int i = pick_first + (mouse_y - c->rows_y) / ROW_H;
    if (i >= pick_count || i >= pick_first + c->rows) {
        if (pick_save) pick_selected = -1;
        dirty = 1;
        return 1;
    }
    if (fs_identity(pick_ids[i]) != pick_identity[i]) { od_refresh(); dirty = 1; return 1; }
    if (!od_row_enabled(pick_ids[i])) return 1;
    int dbl = double_click(i, &pick_last_click_item, &pick_last_click_frame);
    pick_selected = i;
    pick_typed_len = 0; pick_typed[0] = 0;
    if (pick_save) saveas_click_row(i, dbl);
    else {
        pick_focus = 0;
        if (dbl) od_open_selected();
    }
    dirty = 1;
    return 1;
}

static void open_title(char *out) {
    kstrcpy(out, pick_pics_only ? "Open Picture" : "Open");
}
static const char *open_filter_name(void) {
    if (pick_pics_only) return "Image files";
    if (pick_sheet) return "Spreadsheets (.bsh, .csv)";
    if (pick_writer) return "Documents";
    return "All files";
}
static int open_has_filter(void) { return pick_pics_only || pick_sheet || pick_writer; }

static void pick_geom(int *px, int *py, int *pw, int *ph) {
    int top = MENUBAR_H, avail = TASKBAR_Y - top - 16;
    *pw = fb_w - 16 < 640 ? fb_w - 16 : 640;
    *ph = avail < 460 ? avail : 460;
    *px = (fb_w - *pw) / 2;
    *py = top + (TASKBAR_Y - top - *ph) / 2;
}

static int open_foot_h(void) { return (MODERN ? 28 : 24) + BTN_H * 2 + 10; }

static void open_layout(Chooser *c) {
    int x, y, w, h;
    pick_geom(&x, &y, &w, &h);
    chooser_layout(c, x, y, w, h, 0, open_foot_h());
}

/* Footer: file name and filter on the first row, buttons on the second. */
static void od_footer(const Chooser *c, int *lx, int *fx, int *fw, int *dx, int *dw, int *row1, int *row2) {
    int pad = MODERN ? 16 : 12;
    *lx = c->x + pad;
    *fx = c->x + 92;
    *dw = 176;
    *dx = c->x + c->w - pad - *dw;
    *fw = *dx - 8 - *fx;
    *row1 = c->foot_y + (MODERN ? 14 : 12);
    *row2 = *row1 + BTN_H + 10;
}

static void od_buttons(int px, int py, int pw, int ph, int *ox, int *oy, int *ow,
                       int *cx, int *cy, int *cw) {
    (void)py;
    *ow = *cw = MODERN ? 84 : 80;
    *oy = *cy = py + ph - (MODERN ? 14 : 12) - BTN_H;
    *ox = px + pw - (MODERN ? 16 : 12) - *ow;
    *cx = *ox - 8 - *cw;
}

static void draw_open_dialog(void) {
    Chooser c;
    open_layout(&c);
    char title[24];
    open_title(title);
    shade_rect(0, 0, fb_w, fb_h, 1);
    chrome_icon = AI_FILES;
    gui_draw_window(c.x, c.y, c.w, c.h, title, 0, WIN_NOMENU | WIN_DIALOG);
    chrome_icon = -1;
    hot(close_box_x(c.x, c.w), cap_y(c.y), CAP_CLOSE_W, CAP_H);
    chooser_draw(&c);
    dlg_footer(c.x, c.w, c.foot_y, c.foot_h);
    int lx, fx, fw, dx, dw, r1, r2;
    od_footer(&c, &lx, &fx, &fw, &dx, &dw, &r1, &r2);
    kit_text("File name", lx, kit_ty(r1, BTN_H), TK_TEXT, fx - lx - 8, 0);
    int tx = kit_field(fx, r1, fw, BTN_H, pick_focus == 0 ? KIT_FOCUS : 0);
    int id = pick_row_id(pick_selected);
    const char *shown = pick_typed_len ? pick_typed : id >= 0 && !fs_is_dir(id) ? fs_name(id) : "";
    int used = kit_text(shown, tx, kit_ty(r1, BTN_H), TK_TEXT, fw - (tx - fx) - 8, 0);
    if (pick_focus == 0 && pick_typed_len && ((frame_count / 35) & 1) == 0) kit_caret(tx + used, kit_ty(r1, BTN_H));
    const char *filter = open_has_filter() && pick_show_all ? "All files" : open_filter_name();
    kit_dropdown(dx, r1, dw, BTN_H, filter, open_has_filter() ? hot(dx, r1, dw, BTN_H) : KIT_DISABLED);
    int ox, oy, ow, cx, cy, cw;
    od_buttons(c.x, c.y, c.w, c.h, &ox, &oy, &ow, &cx, &cy, &cw);
    int can = id >= 0;
    kit_button(cx, oy, cw, BTN_H, "Cancel", -1, KB_DEFAULT, hot(cx, oy, cw, BTN_H) | (pick_focus == 1 ? KIT_FOCUS : 0));
    kit_button(ox, oy, ow, BTN_H, "Open", -1, KB_PRIMARY,
               can ? hot(ox, oy, ow, BTN_H) | (pick_focus == 2 ? KIT_FOCUS : 0) : KIT_DISABLED);
}

static void start_open_dialog(int pics_only) {
    open_dlg = 1;
    pick_pics_only = pics_only;
    pick_owner = win_front();
    pick_writer = pick_owner >= 0 && wins[pick_owner].kind == WK_WRITER && !pics_only;
    pick_sheet = pick_owner >= 0 && wins[pick_owner].kind == WK_SPREADSHEET && !pics_only;
    pick_owner_seq = pick_owner >= 0 ? wins[pick_owner].seq : 0;
    pick_reset(fs_root(), 0);
    dirty = 1;
}

static void files_refresh_all(void) {
    int original = context_slot;
    for (int i = 0; i < MAX_WIN; i++)
        if (wins[i].open && wins[i].kind == WK_FILES) { context_set(i); fm_refresh(); }
    context_set(original);
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
        pick_go(id, 1);
        return;
    }
    open_dlg = 0;
    if (pick_writer || pick_sheet) {
        if (pick_owner >= 0 && wins[pick_owner].open &&
            wins[pick_owner].kind == (pick_sheet ? WK_SPREADSHEET : WK_WRITER) &&
            wins[pick_owner].seq == pick_owner_seq) document_request(pick_owner, DOCUMENT_OPEN, id);
    } else open_fs_file(id);
}


#define EDIT_MARKS 128
static void fmt_grouped(char *out, unsigned v);
static const char *kind_text(int id);
static void draw_editor(int wx, int wy, int ww, int wh, int inactive) {
    char title[FS_NAME_LEN];
    kstrcpy(title, edit_binding_valid() ? fs_name(edit_file) : "untitled");
    chrome_dirty = !edit_saved_ok;
    gui_draw_window(wx, wy, ww, wh, title, 0, inactive ? WIN_INACTIVE : 0);
    chrome_dirty = 0;

    int ax, ay, aw, ah;
    edit_area(wx, wy, ww, wh, &ax, &ay, &aw, &ah);
    int top = edit_text_top(wy), bottom = edit_text_bottom(wy, wh), g = edit_gutter_w();
    int cols = aw / EDIT_CHAR_W;
    int rows = ah / EDIT_LINE_H;
    if (cols < 1)
        cols = 1;
    if (rows < 1)
        rows = 1;
    if(!edit_manual_scroll)edit_ensure_caret_visible(rows, cols);

    int sel_lo = edit_sel_a < edit_sel_b ? edit_sel_a : edit_sel_b;
    int sel_hi = edit_sel_a > edit_sel_b ? edit_sel_a : edit_sel_b;
    int has_sel = sel_lo != sel_hi;
    int caret_row, caret_col;
    edit_caret_cell(cols, &caret_row, &caret_col);
    if (g && !MODERN) draw_vline(wx + g, top, bottom - top, TK_LINE_SUBTLE);
    /* The current line, while nothing is selected. */
    int cur = caret_row - edit_scroll;
    if (!has_sel && !inactive && cur >= 0 && cur < rows) {
        int hx = MODERN ? wx + 1 : wx + 1 + g + (g ? 1 : 0);
        draw_rect(hx, ay + cur * EDIT_LINE_H, wx + ww - 1 - SB - hx, EDIT_LINE_H, TK_ACCENT_50);
    }
    const char *needle = edit_search.open ? edit_search.text[0] : "";
    int needle_n = kstrlen(needle), match_end = -1;

    int r = 0, c = 0, line = 1, caret_line = 1, marks = 0, mark_row[EDIT_MARKS];
    for (int ei = 0; ei <= edit_len; ei++) {
        int vis_row = r - edit_scroll;
        if (ei == edit_caret) caret_line = line;
        /* A row that starts a line gets its number. */
        if (g && c == 0 && (ei == 0 || edit_buf[ei - 1] == '\n') && vis_row >= 0 && vis_row < rows) {
            char num[12];
            fmt_uint(num, (unsigned)line);
            int on = r == caret_row, nw = kstrlen(num) * EDIT_CHAR_W;
            int nx = wx + 1 + g - 10 - nw;
            for (int k = 0; num[k]; k++)
                draw_edit_char(num[k], nx + k * EDIT_CHAR_W, ay + vis_row * EDIT_LINE_H, on ? TK_TEXT : TK_LINE_STRONG);
        }
        int selected = has_sel && ei >= sel_lo && ei < sel_hi;
        if (needle_n && ei >= match_end && text_match(edit_buf, edit_len, ei, needle, edit_search_flags())) {
            match_end = ei + needle_n;
            if (marks < EDIT_MARKS && (!marks || mark_row[marks - 1] != r)) mark_row[marks++] = r;
        }
        if (vis_row >= 0 && vis_row < rows && ei < edit_len) {
            int cw = edit_buf[ei] == '\n' ? 4 : EDIT_CHAR_W;
            if (selected)
                draw_rect(ax + c * EDIT_CHAR_W, ay + vis_row * EDIT_LINE_H, cw, EDIT_LINE_H, inactive ? TK_ACCENT_100 : TK_ACCENT);
            else if (ei < match_end && edit_buf[ei] != '\n')
                draw_rect(ax + c * EDIT_CHAR_W, ay + vis_row * EDIT_LINE_H, cw, EDIT_LINE_H, TK_FIND);
        }
        if (!has_sel && !inactive && ei == edit_caret && vis_row >= 0 && vis_row < rows) {
            int cx = ax + c * EDIT_CHAR_W;
            int cy = ay + vis_row * EDIT_LINE_H;
            if ((frame_count / 35) % 2 == 0)
                draw_rect(cx, cy, 2, EDIT_FONT_H, TK_TEXT);
        }
        if (ei == edit_len)
            break;
        if (edit_buf[ei] == '\n') {
            r++;
            c = 0;
            line++;
            continue;
        }
        if (vis_row >= 0 && vis_row < rows)
            draw_edit_char(edit_buf[ei], ax + c * EDIT_CHAR_W,
                           ay + vis_row * EDIT_LINE_H,
                           selected && !inactive ? TK_ON_ACCENT : TK_TEXT);
        c++;
        if (c >= cols) {
            r++;
            c = 0;
        }
    }
    kit_vscroll(wx + ww - 1 - SB, top, bottom - top, edit_scroll, rows, r + 1, 0);
    /* Overview marks on the scrollbar: matches in amber, the caret in accent. */
    if (r + 1 > rows) {
        int mx = wx + ww - 1 - 5, track = bottom - top - 8;
        for (int i = 0; i < marks; i++) draw_rect(mx, top + 4 + mark_row[i] * track / (r + 1), 4, 2, TK_AMBER);
        draw_rect(mx, top + 4 + caret_row * track / (r + 1), 4, 2, TK_ACCENT);
    }
    draw_edit_search(wx, wy, ww, wh);

    /* Status bar: position, kind, size, line endings, tab, then the state. */
    int sy = wy + wh - 1 - edit_status_h(), pad = MODERN ? 12 : 8;
    if (MODERN) {
        draw_hline(wx + 1, sy, ww - 2, TK_LINE_SUBTLE);
        kit_fill_bottom(wx + 1, sy + 1, ww - 2, edit_status_h(), ui_theme.r_window - 1, ui_theme.statusbar);
    } else kit_statusbar(wx + 1, sy, ww - 2, edit_status_h());
    int col = 1;
    for (int i = edit_caret - 1; i >= 0 && edit_buf[i] != '\n'; i--) col++;
    char pos[32], size[24];
    kstrcpy(pos, "Ln ");
    fmt_uint(pos + 3, (unsigned)caret_line);
    kstrcpy(pos + kstrlen(pos), ", Col ");
    fmt_uint(pos + kstrlen(pos), (unsigned)col);
    fmt_grouped(size, (unsigned)edit_len);
    kstrcpy(size + kstrlen(size), edit_len == 1 ? " byte" : " bytes");
    const char *kind = edit_binding_valid() ? kind_text(edit_file) : "Plain text";
    const char *items[5] = {pos, kind, size, "LF", "Tab: 2 spaces"};
    const char *state = edit_save_busy ? "Save not started; retry" :
                        edit_search.open && edit_search.message[0] ? edit_search.message :
                        edit_saved_ok ? (edit_binding_valid() ? "Saved" : "") : "Modified";
    int ty = kit_ty(sy + 1, edit_status_h() - 1), x = wx + pad;
    int right = wx + ww - pad - kit_text_w(state, 0) - 16;
    for (int i = 0; i < 5; i++) {
        int w = kit_text_w(items[i], 0);
        if (x + w > right) break;
        kit_text(items[i], x, ty, TK_TEXT2, w, 0);
        x += w + 18;
    }
    kit_text_right(state, wx + ww - pad, ty, edit_save_busy ? TK_RED_700 : TK_TEXT2, 0);
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

/* ---------- Properties ----------
 * The item's icon and name (editable for ordinary files and folders), its
 * details, the app that opens it, and for the volume root its storage. */

static char prop_buf[FS_NAME_LEN];
static int prop_len, prop_for = -1, prop_failed;
static unsigned prop_identity;

static int prop_editable(int id) {
    return fs_valid(id) && id != fs_root() && !fs_is_app(id) && id != trash_id && id != prefs_id;
}

static void prop_sync(void) {
    int id = properties_id;
    if (prop_for == id && prop_identity == fs_identity(id)) return;
    prop_for = id;
    prop_identity = fs_identity(id);
    prop_failed = 0;
    prop_len = 0;
    const char *nm = id == fs_root() ? "System" : fs_name(id);
    while (nm[prop_len] && prop_len < FS_NAME_LEN - 1) { prop_buf[prop_len] = nm[prop_len]; prop_len++; }
    prop_buf[prop_len] = 0;
}

/* 4198 as "4,198". */
static void fmt_grouped(char *out, unsigned v) {
    char digits[12];
    fmt_uint(digits, v);
    int n = kstrlen(digits), o = 0;
    for (int i = 0; i < n; i++) {
        if (i && (n - i) % 3 == 0) out[o++] = ',';
        out[o++] = digits[i];
    }
    out[o] = 0;
}

static const char *kind_text(int id) {
    const char *n = fs_name(id);
    static const struct { const char *ext, *kind; } kinds[] = {
        {".txt", "Plain text"}, {".bwr", "Writer document"}, {".rtf", "Rich text"},
        {".bsh", "Spreadsheet"}, {".csv", "CSV values"}, {".pdf", "PDF document"},
        {".bex", "Program"}, {".c", "C source"}, {".h", "C header"}, {".html", "Web page"},
        {".htm", "Web page"}, {".wav", "Sound"}, {".mp3", "Sound"}, {".mpg", "Video"},
        {".mpeg", "Video"}, {".log", "Log file"},
    };
    if (id == fs_root()) return "Volume";
    if (fs_is_dir(id)) return "Folder";
    if (fs_is_app(id)) return "Application";
    for (unsigned i = 0; i < sizeof kinds / sizeof *kinds; i++) if (file_extension(n, kinds[i].ext)) return kinds[i].kind;
    if (is_image_file(id)) return "Picture";
    return "File";
}

/* The app open_fs_file would use; 0 when nothing opens it. */
static const char *opens_with(int id, int *ai) {
    const char *n = fs_name(id);
    *ai = AI_DOCUMENT;
    if (fs_is_dir(id)) { *ai = AI_FILES; return "Files"; }
    if (fs_is_app(id)) return 0;
    if (file_extension(n, ".bex")) { *ai = AI_NATIVE; return "Runs as a program"; }
    if (file_extension(n, ".bwr")) { *ai = AI_WRITER; return "Writer"; }
    if (file_extension(n, ".bsh") || file_extension(n, ".csv")) { *ai = AI_SHEET; return "Spreadsheet"; }
    if (file_extension(n, ".html") || file_extension(n, ".htm")) { *ai = AI_BROWSER; return "Browser"; }
    if (file_extension(n, ".wav") || file_extension(n, ".wave") || file_extension(n, ".mp3") ||
        file_extension(n, ".mpg") || file_extension(n, ".mpeg")) { *ai = AI_PLAYER; return "Media Player"; }
    if (is_image_file(id) || file_extension(n, ".jpg") || file_extension(n, ".jpeg") || file_extension(n, ".png") ||
        file_extension(n, ".bmp") || file_extension(n, ".gif")) { *ai = AI_VIEWER; return "Image Viewer"; }
    if (fs_size(id) >= EDIT_BUF_SIZE) return 0;
    *ai = AI_EDITOR;
    return "Editor";
}

static int prop_root(void) { return properties_id == fs_root(); }
static int prop_rows(void) {
    if (!fs_valid(properties_id)) return 1;
    return prop_root() ? 6 : 4;
}
static int prop_pad(void) { return MODERN ? 20 : 16; }
static int prop_foot_h(void) { return MODERN ? 58 : BTN_H + 28; }
static int properties_h(void) {
    int h = TITLE_BAR + prop_pad() + 32 + 16 + prop_rows() * (MODERN ? 26 : 24) + (MODERN ? 24 : 12);
    if (fs_valid(properties_id) && !prop_root() && !fs_is_app(properties_id)) h += 16 + BTN_H;
    if (properties_reason[0]) h += 28;
    return h + 12 + prop_foot_h();
}

static void prop_buttons(int wx, int wy, int ww, int wh, int *ok, int *cancel, int *by, int *bw) {
    (void)wy;
    *bw = MODERN ? 84 : 80;
    *by = wy + wh - (prop_foot_h() + BTN_H) / 2 - (MODERN ? 0 : 2);
    *ok = wx + ww - prop_pad() - *bw;
    *cancel = *ok - 8 - *bw;
}
static void prop_field(int wx, int wy, int ww, int *fx, int *fy, int *fw) {
    *fx = wx + prop_pad() + 32 + 12;
    *fy = wy + TITLE_BAR + prop_pad() + (32 - BTN_H) / 2;
    *fw = ww - (*fx - wx) - prop_pad();
}

static void prop_row(int lx, int vx, int *y, int right, const char *label, const char *value) {
    int rh = MODERN ? 26 : 24;
    kit_text(label, lx, kit_ty(*y, rh), TK_TEXT2, vx - lx - 8, 0);
    kit_text(value, vx, kit_ty(*y, rh), TK_TEXT, right - vx, 0);
    *y += rh;
}

static void draw_properties(int wx, int wy, int ww, int wh, int fl) {
    prop_sync();
    int id = properties_id, pad = prop_pad(), valid = fs_valid(id);
    char title[FS_NAME_LEN + 16];
    kstrcpy(title, valid ? (prop_root() ? "System" : fs_name(id)) : "Item");
    kstrcpy(title + kstrlen(title), " Properties");
    if (valid) chrome_icon = fs_is_dir(id) ? AI_FILES : fs_is_app(id) ? app_icon_for(app_icon_by_name(fs_name(id))) : AI_DOCUMENT;
    gui_draw_window(wx, wy, ww, wh, title, 0, fl | WIN_NOMENU | WIN_DIALOG);
    int top = wy + TITLE_BAR, y = top + pad, right = wx + ww - pad;
    if (valid) {
        if (prop_root()) ui_icon_draw(UI_ICON_A_DISK, wx + pad, y, 32, TK_ICON, ui_theme.content);
        else node_icon_draw(id, wx + pad, y, 32, ui_theme.content);
    }
    int fx, fy, fw;
    prop_field(wx, wy, ww, &fx, &fy, &fw);
    if (valid && prop_editable(id)) {
        int focus = !(fl & WIN_INACTIVE);
        int tx = kit_field(fx, fy, fw, BTN_H, (focus ? KIT_FOCUS : 0) | (prop_failed ? KIT_ERROR : 0));
        int used = kit_text(prop_buf, tx, kit_ty(fy, BTN_H), TK_TEXT, fw - (tx - fx) - 8, 0);
        if (focus && ((frame_count / 35) & 1) == 0) kit_caret(tx + used, kit_ty(fy, BTN_H));
    } else kit_text(valid ? prop_buf : "This item is no longer available.", fx, kit_ty(fy, BTN_H), TK_TEXT, fw, 1);
    y += 32 + 12;
    int lx = wx + pad, vx = wx + pad + 108;
    char value[FS_PATH_LEN], number[16];
    if (MODERN) {
        int rows = prop_rows();
        kit_box(wx + pad, y, ww - pad * 2, rows * 26 + 16, 8, TK_RAISED, TK_LINE_SUBTLE);
        lx += 14; vx += 14; right -= 14; y += 8;
    } else { kit_separator(wx + pad, y, ww - pad * 2); y += 8; }
    if (valid && !prop_root()) {
        prop_row(lx, vx, &y, right, "Kind", kind_text(id));
        int parent = fs_parent(id);
        if (fs_valid(parent)) fs_path(parent, value, sizeof value);
        else kstrcpy(value, "/");
        prop_row(lx, vx, &y, right, "Location", value);
        if (fs_is_dir(id)) node_size_text(id, value);
        else {
            size_text((unsigned)fs_size(id), value);
            kstrcpy(value + kstrlen(value), " (");
            fmt_grouped(number, (unsigned)fs_size(id));
            kstrcpy(value + kstrlen(value), number);
            kstrcpy(value + kstrlen(value), fs_size(id) == 1 ? " byte)" : " bytes)");
        }
        prop_row(lx, vx, &y, right, "Size", value);
        stamp_text(fs_modified(id), value);
        prop_row(lx, vx, &y, right, "Modified", value);
    } else if (valid) {
        unsigned cap = fs_capacity(), used = fs_used_bytes();
        prop_row(lx, vx, &y, right, "Volume", fs_storage_name());
        size_text(used, value);
        prop_row(lx, vx, &y, right, "Used", value);
        size_text(cap > used ? cap - used : 0, value);
        prop_row(lx, vx, &y, right, "Free", value);
        if (fs_node_limit() <= FS_MAX_NODES) fmt_uint(value, (unsigned)(fs_node_limit() - fs_node_count()));
        else kstrcpy(value, "No limit");
        prop_row(lx, vx, &y, right, "Free slots", value);
        size_text(fs_file_limit(), value);
        prop_row(lx, vx, &y, right, "Largest file", value);
        prop_row(lx, vx, &y, right, "Status", fs_storage_status() ? fs_storage_status() :
                 fs_needs_sync() ? "Changes waiting to be saved" : "Saved to disk");
    } else prop_row(lx, vx, &y, right, "Status", "Removed or renamed elsewhere");
    if (MODERN) { y += 8; lx -= 14; vx -= 14; right += 14; }
    y += MODERN ? 16 : 4;
    if (valid && !prop_root() && !fs_is_app(id)) {
        if (!MODERN) { kit_separator(wx + pad, y, ww - pad * 2); y += 12; }
        int ai;
        const char *app = opens_with(id, &ai);
        kit_text("Opens with", lx, kit_ty(y, BTN_H), TK_TEXT2, vx - lx - 8, 0);
        int tx = kit_field(vx, y, right - vx, BTN_H, KIT_DISABLED);
        if (app) {
            app_icon_draw(ai, tx, y + (BTN_H - 16) / 2, AI_SMALL);
            kit_text(app, tx + 22, kit_ty(y, BTN_H), TK_TEXT, right - tx - 30, 0);
        } else kit_text("No app opens this file", tx, kit_ty(y, BTN_H), TK_TEXT2, right - tx - 8, 0);
        y += BTN_H + 16;
    }
    if (properties_reason[0]) {
        ui_icon_draw(UI_ICON_WARN, lx, y + 2, 16, TK_ICON, ui_theme.content);
        kit_text(properties_reason, lx + 22, y + 2, TK_AMBER_700, right - lx - 22, 0);
        y += 28;
    }
    if (prop_failed) kit_text(fs_sync_busy() ? "Disk saving. Try again shortly." : "That name cannot be used here.",
                              wx + pad, wy + wh - prop_foot_h() - 22, TK_RED_700, ww - pad * 2, 0);
    if (MODERN) dlg_footer(wx, ww, wy + wh - prop_foot_h(), prop_foot_h());
    int ok, cancel, by, bw;
    prop_buttons(wx, wy, ww, wh, &ok, &cancel, &by, &bw);
    if (valid && prop_editable(id))
        kit_button(cancel, by, bw, BTN_H, "Cancel", -1, KB_DEFAULT, hot(cancel, by, bw, BTN_H));
    kit_button(ok, by, bw, BTN_H, "OK", -1, KB_PRIMARY, hot(ok, by, bw, BTN_H));
}

/* OK renames when the name changed; the window stays open on failure. */
static void properties_commit(void) {
    int id = properties_id;
    prop_buf[prop_len] = 0;
    if (prop_editable(id) && prop_for == id && prop_identity == fs_identity(id) &&
        kstrcmp(prop_buf, fs_name(id)) != 0) {
        if (!prop_len || fs_rename(id, prop_buf) < 0) { prop_failed = 1; dirty = 1; return; }
        files_refresh_all();
        if (open_dlg || name_dlg) od_refresh();
    }
    prop_for = -1;
    close_front();
}

static void handle_properties_click(int wx, int wy, int ww, int wh) {
    int ok, cancel, by, bw;
    prop_buttons(wx, wy, ww, wh, &ok, &cancel, &by, &bw);
    if (hit(mouse_x, mouse_y, ok, by, bw, BTN_H)) properties_commit();
    else if (prop_editable(properties_id) && hit(mouse_x, mouse_y, cancel, by, bw, BTN_H)) { prop_for = -1; close_front(); }
}

static void properties_key(void) {
    if (key_sc == KEY_ESC) { prop_for = -1; close_front(); return; }
    if (key_sc == KEY_ENTER) { properties_commit(); return; }
    if (!prop_editable(properties_id)) return;
    prop_sync();
    if (key_sc == KEY_BACKSPACE) { if (prop_len) prop_buf[--prop_len] = 0; }
    else if (key_char >= 32 && key_char != '/' && prop_len < FS_NAME_LEN - 1) { prop_buf[prop_len++] = key_char; prop_buf[prop_len] = 0; }
    else return;
    prop_failed = 0;
    dirty = 1;
}

/* ---------- Hello and Help ---------- */

static void draw_hello(int wx, int wy, int ww, int wh, int fl) {
    gui_draw_window(wx, wy, ww, wh, "Hello", 0, fl);
    int top = wy + TITLE_H, lw = logo_string_w("BaseOS"), gap = 14;
    int x = wx + (ww - (40 + gap + lw)) / 2, y = top + 26;
    ui_icon_draw(UI_ICON_LOGO, x, y + (LOGO_FONT_H - 40) / 2, 40, TK_ACCENT, ui_theme.content);
    draw_logo_string("BaseOS", x + 40 + gap, y, TK_ACCENT_700);
    kit_text_center("Hello, world. Everything you see is drawn from scratch.", wx, y + LOGO_FONT_H + 14, ww, TK_TEXT2, 0);
}

/* Keyboard reference: sections of shortcut chips and what they do. */
typedef struct { const char *keys, *text; } HelpRow;
static const HelpRow help_rows[] = {
    {0, "Getting around"},
    {"Ctrl+Space", "Search apps and files"},
    {"Alt+Tab", "Switch between windows"},
    {"Ctrl+C / X / V", "Copy, cut and paste files or selected text"},
    {0, "Files"},
    {"Ctrl+F", "Filter the current folder"},
    {"Ctrl+1 to Ctrl+4", "Choose the sort column"},
    {0, "Writer"},
    {"Ctrl+S", "Save the document as BWR"},
    {"Ctrl+Shift+P", "Export the document as PDF"},
    {0, "Programs"},
    {".bex", "Open a .bex file to run a native app"},
    {"Ctrl+C", "Stop the native task in Terminal"},
};
#define HELP_ROWS ((int)(sizeof help_rows / sizeof help_rows[0]))
static int help_window_h(void) {
    int h = TITLE_H + 20 + 24;
    for (int i = 0; i < HELP_ROWS; i++) h += help_rows[i].keys ? 28 : (i ? 38 : 28);
    return h + 24 + 20;
}
static void draw_help(int wx, int wy, int ww, int wh, int fl) {
    gui_draw_window(wx, wy, ww, wh, "Help", 0, fl);
    int pad = ui_theme.pad_dialog + 4, x = wx + pad, y = wy + TITLE_H + 20, kx = x + 170;
    kit_text("Keyboard shortcuts", x, y, TK_TEXT, ww - 2 * pad, 1);
    y += 24;
    for (int i = 0; i < HELP_ROWS; i++) {
        const HelpRow *r = &help_rows[i];
        if (!r->keys) {
            if (i) { kit_separator(x, y + 12, ww - 2 * pad); y += 10; }
            kit_text(r->text, x, y + 8, TK_TEXT2, ww - 2 * pad, 1);
            y += 28;
            continue;
        }
        kit_kbd(x, y + 3, r->keys);
        kit_text(r->text, kx, kit_ty(y, 28), TK_TEXT, wx + ww - pad - kx, 0);
        y += 28;
    }
    kit_text("Documents has Writer and Spreadsheet quick starts.", x, y + 8, TK_TEXT2, ww - 2 * pad, 0);
}

static void draw_window_contents(Win *w, int inactive) {
    context_set((int)(w - wins));
    chrome_icon = app_icon_for(kind_icon(w->kind));
    int wx = w->x, wy = w->y, ww = w->w, wh = w->h;
    int fl = inactive ? WIN_INACTIVE : 0;
    if (w->kind == WK_PROPERTIES) {
        draw_properties(wx, wy, ww, wh, fl);
    } else if (w->kind == WK_HELLO) {
        draw_hello(wx, wy, ww, wh, fl);
    } else if (w->kind == WK_HELP) {
        draw_help(wx, wy, ww, wh, fl);
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
        wordle_draw(wx + 1, wy + TITLE_H, ww - 2, wh - TITLE_H - 1, inactive ? 0 : hot);
    } else if (w->kind == WK_TERM) {
        draw_term(wx, wy, ww, wh, inactive);
    } else if (w->kind == WK_NATIVE) {
        draw_native_window((int)(w-wins),inactive);
    } else if (w->kind == WK_TODO) {
        draw_todo(wx, wy, ww, wh, inactive);
    } else if (w->kind == WK_CLOCK) {
        gui_draw_window(wx, wy, ww, wh, "Clock", 0, fl);
        clock_draw(wx + 1, wy + TITLE_H, ww - 2, wh - TITLE_H - 1);
    } else if (w->kind == WK_CAL) {
        gui_draw_window(wx, wy, ww, wh, "Calendar", 0, fl);
        cal_draw(wx + 1, wy + TITLE_H, ww - 2, wh - TITLE_H - 1, inactive ? 0 : hot);
    } else if (w->kind == WK_MINES) {
        gui_draw_window(wx, wy, ww, wh, "Minesweeper", 0, fl);
        mines_draw(wx + 1, wy + TITLE_H, ww - 2, wh - TITLE_H - 1, inactive ? 0 : hot);
    } else if (w->kind == WK_2048) {
        gui_draw_window(wx, wy, ww, wh, "2048", 0, fl);
        g2048_draw(wx + 1, wy + TITLE_H, ww - 2, wh - TITLE_H - 1, inactive ? 0 : hot);
    } else if (w->kind == WK_BREAKOUT) {
        gui_draw_window(wx, wy, ww, wh, "Breakout", 0, fl);
        bo_draw(wx + 1, wy + TITLE_H, ww - 2, wh - TITLE_H - 1, inactive ? 0 : hot);
    } else if (w->kind == WK_SPREADSHEET || w->kind == WK_WRITER) {
        int sheet = w->kind == WK_SPREADSHEET, cx, cy, cw, ch;
        chrome_dirty = sheet ? spreadsheet_dirty() : writer_dirty();
        gui_draw_window(wx, wy, ww, wh, sheet ? spreadsheet_title() : writer_title(), 0, fl);
        chrome_dirty = 0;
        doc_client(w, &cx, &cy, &cw, &ch);
        if (sheet) { spreadsheet_set_active(!inactive); spreadsheet_draw(cx, cy, cw, ch); }
        else { writer_set_active(!inactive); writer_draw(cx, cy, cw, ch); }
    } else if (w->kind == WK_BROWSER) {
        int cx, cy, cw, ch;
        gui_draw_window(wx,wy,ww,wh,"Browser",0,fl);
        app_client(wx, wy, ww, wh, 1, &cx, &cy, &cw, &ch);
        browser_set_hover(inactive ? 0 : hot);
        browser_draw(cx, cy, cw, ch);
    } else if (w->kind == WK_PLAYER) {
        int cx, cy, cw, ch;
        gui_draw_window(wx,wy,ww,wh,"Media Player",0,fl);
        app_client(wx, wy, ww, wh, 0, &cx, &cy, &cw, &ch);
        player_set_hover(inactive ? 0 : hot);
        player_draw(cx, cy, cw, ch);
    } else if (w->kind == WK_SYSMON) {
        int cx, cy, cw, ch;
        gui_draw_window(wx, wy, ww, wh, "System Monitor", 0, fl);
        SysInfo si;
        sysinfo_fill(&si);
        app_client(wx, wy, ww, wh, 1, &cx, &cy, &cw, &ch);
        sysmon_set_hover(inactive ? 0 : hot);
        sysmon_draw(cx, cy, cw, ch, &si);
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
static Rect tb_apps, tb_vol, tb_clock, tb_desk, tb_dock, tb_sys, tb_appname;
static int tb_task_y, tb_task_h;
static int volume_on;  /* volume popup */
enum { HOT_NONE = -2, HOT_APPS = -1, HOT_VOL = -3, HOT_CLOCK = -4, HOT_DESK = -5,
       HOT_SYS = -6, HOT_APPNAME = -7, HOT_MENU = -100 /* - index */ };
static int taskbar_hover = HOT_NONE;

/* Taskbar date and time, following the clock settings: no date when it is
 * turned off, and 12- or 24-hour time. */
static void date_time_text(char *date, char *time) {
    RtcTime now;
    rtc_read(&now);
    static const char *dshort[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    char t[4];
    date[0] = 0;
    if (!(clock_flags & CLOCK_NO_DATE)) {
        kstrcpy(date, dshort[now.wday]);
        kstrcpy(date + kstrlen(date), " ");
        fmt_uint(t, (unsigned)now.day);
        kstrcpy(date + kstrlen(date), t);
        kstrcpy(date + kstrlen(date), " ");
        int n = kstrlen(date);
        const char *mn = rtc_month_name[now.month - 1];
        for (int i = 0; i < 3 && mn[i]; i++) date[n++] = mn[i];
        date[n] = 0;
    }
    clock_text(now.hour, now.min, time);
}
static int clock_w(void) {
    char d[24], t[12];
    date_time_text(d, t);
    return (d[0] ? ui_string_w(d) + 8 : 0) + uib_string_w(t);
}
static void draw_clock(int x, int y, uint8_t ink) {
    char d[24], t[12];
    date_time_text(d, t);
    if (d[0]) draw_string(d, x, y, ink);
    draw_string_bold(t, x + (d[0] ? ui_string_w(d) + 8 : 0), y, ink);
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

static int click_on_rename_field(void) {
    int wx, wy, ww, wh, fx, fy, fw, fh;
    if (!fm_renaming || !win_geom_kind(WK_FILES, &wx, &wy, &ww, &wh)) return 0;
    return files_rename_rect(wx, wy, ww, wh, &fx, &fy, &fw, &fh) && hit(mouse_x, mouse_y, fx, fy, fw, fh);
}

/* Toolbar, sidebar, column headers and scrollbar. Returns 1 when the click
 * was one of these. */
static int files_chrome_click(int wx, int wy, int ww, int wh) {
    int ty = files_tool_y(wy), by = files_body_y(wy), sy = files_status_y(wy, wh), side = files_side_w(ww);
    if (mouse_y >= sy) return 1;
    if (mouse_y < by) {
        if (mouse_y < ty) return 1;
        int n = nav_hit(wx, ty, FILES_TOOL_H - 1);
        if (n == 0 || n == 1) { fm_history_step(n == 0 ? -1 : 1); return 1; }
        if (n == 2) { fm_rename_cancel(); fm_go_up(); return 1; }
        int cy = files_ctl_y(wy), vx = files_view_x(wx, ww), vs = nav_size(), vy = ty + (FILES_TOOL_H - 1 - vs) / 2;
        int id = crumbs_hit(nav_end(wx), cy, vx - 8 - nav_end(wx), BTN_H, fm_cwd);
        if (id >= 0) { if (id != fm_cwd) fm_navigate(id); return 1; }
        if (hit(mouse_x, mouse_y, vx, vy, 2 * vs, vs)) {
            int details = mouse_x < vx + vs;
            if (details != fm_details) { fm_rename_cancel(); fm_details = details; fm_first = fm_manual_scroll = 0; }
            dirty = 1;
            return 1;
        }
        int sw = files_search_w(ww), sx = files_search_x(wx, ww);
        if (sw && hit(mouse_x, mouse_y, sx, cy, sw, BTN_H)) {
            if (fm_view.filter[0] && mouse_x >= kit_search_clear_x(sx, sw)) fm_filter_clear(0);
            else fm_filter_show();
            dirty = 1;
        }
        return 1;
    }
    if (side && mouse_x < wx + 1 + side) {
        int id = places_hit(wx + 1, by, side, sy - by, 1);
        if (id >= 0 && id != fm_cwd) fm_navigate(id);
        return 1;
    }
    int cx = files_content_x(wx, ww), cw = files_content_w(ww);
    if (fm_details) {
        int sort = flist_header_hit(cx, by, cw, FL_SIZE | FL_DATE);
        if (sort >= 0) { fm_sort_by(sort); return 1; }
    }
    if (files_scrolls(ww, wy, wh) && mouse_x >= cx + cw - SB) {
        int rows = files_rows(wy, wh), cols = files_cols(ww), at, size;
        int top = fm_details ? files_list_y(wy) : by + 2, len = sy - top - 2;
        kit_scroll_thumb(len, fm_first / cols, rows, (fm_vis_count() + cols - 1) / cols, &at, &size);
        fm_first += (mouse_y < top + at ? -rows : rows) * cols;
        fm_manual_scroll = 1;
        files_scroll(rows, cols);
        dirty = 1;
        return 1;
    }
    return 0;
}

static void handle_files_click(int wx, int wy, int ww, int wh) {
    fm_history_sync();
    if (files_chrome_click(wx, wy, ww, wh)) return;
    fm_filter_focus = fm_filter_select_all = 0;
    files_scroll(files_rows(wy, wh), files_cols(ww));
    int i = files_item_at(wx, wy, ww, wh);
    if (i == -2) return;
    dirty = 1;
    if (i < 0) {
        fm_rename_cancel();
        fm_selected = -1;
        return;
    }
    int id = fm_row_id(i);
    if (id < 0) { fm_refresh(); return; }
    int x, y, w, h;
    files_item_rect(wx, wy, ww, wh, i, &x, &y, &w, &h);
    int already = i == fm_selected;
    int dbl = double_click(i, &fm_last_click_item, &fm_last_click_frame);
    fm_dragging = fm_drag_active = 0;
    fm_drag_id = -1;
    if (dbl) {
        fm_rename_cancel();
        fm_selected = i;
        fm_open_selected();
        return;
    }
    /* A click on the name of the selected item renames it in place. */
    if (already && files_label_hit(x, y, w, h)) {
        fm_rename_begin(id);
        return;
    }
    if (fm_renaming && id == fm_rename_id) return;
    fm_rename_cancel();
    fm_selected = i;
    fm_dragging = 1;
    fm_drag_id = id;
    fm_drag_identity = fs_identity(id);
    fm_drag_parent = fm_cwd;
    fm_drag_sx = mouse_x;
    fm_drag_sy = mouse_y;
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
    Chooser c;
    open_layout(&c);
    if (hit(mouse_x, mouse_y, close_box_x(c.x, c.w), cap_y(c.y), CAP_CLOSE_W, CAP_H)) {
        close_open_dialog();
        return;
    }
    int ox, oy, ow, cx, cy, cw;
    od_buttons(c.x, c.y, c.w, c.h, &ox, &oy, &ow, &cx, &cy, &cw);
    if (hit(mouse_x, mouse_y, cx, cy, cw, BTN_H)) {
        close_open_dialog();
        return;
    }
    if (hit(mouse_x, mouse_y, ox, oy, ow, BTN_H)) {
        od_open_selected();
        return;
    }
    int lx, fx, fw, dx, dw, r1, r2;
    od_footer(&c, &lx, &fx, &fw, &dx, &dw, &r1, &r2);
    if (hit(mouse_x, mouse_y, fx, r1, fw, BTN_H)) { pick_focus = 0; dirty = 1; return; }
    if (hit(mouse_x, mouse_y, dx, r1, dw, BTN_H)) {
        if (open_has_filter()) { pick_show_all = !pick_show_all; od_refresh(); dirty = 1; }
        return;
    }
    /* Modal: clicks outside the dialog are swallowed. */
    if (hit(mouse_x, mouse_y, c.x, c.y, c.w, c.h)) chooser_click(&c);
}

/* Case-insensitive prefix test for type-ahead. */
static int name_has_prefix(const char *name, const char *prefix) {
    for (; *prefix; name++, prefix++) {
        char a = *name, b = *prefix;
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (!a || a != b) return 0;
    }
    return 1;
}

static void pick_type_select(void) {
    if (!pick_typed_len) return;
    for (int i = 0; i < pick_count; i++)
        if (od_row_enabled(pick_ids[i]) && name_has_prefix(fs_name(pick_ids[i]), pick_typed)) {
            pick_selected = i;
            return;
        }
}

static void open_dlg_key(void) {
    static const int order[4] = {0, 3, 1, 2};
    if (key_sc == KEY_TAB) {
        int k = 0;
        while (k < 3 && order[k] != pick_focus) k++;
        pick_focus = order[(k + (shift_down ? 3 : 1)) % 4];
        dirty = 1;
        return;
    }
    if (alt_down && (key_sc == KEY_LEFT || key_sc == KEY_RIGHT)) { pick_history_step(key_sc == KEY_LEFT ? -1 : 1); return; }
    if (alt_down && key_sc == KEY_UP) { if (pick_cwd != fs_root()) pick_go(fs_parent(pick_cwd), 1); return; }
    if (key_sc == KEY_ESC) {
        if (pick_focus == 3 && pick_view.filter[0]) { pick_view.filter[0] = 0; od_refresh(); dirty = 1; return; }
        close_open_dialog();
        return;
    }
    if (key_sc == KEY_ENTER) {
        if (pick_focus == 1) close_open_dialog();
        else od_open_selected();
        return;
    }
    if (key_sc == KEY_UP || key_sc == KEY_DOWN) {
        if (pick_count) {
            int from = pick_selected < 0 ? (key_sc == KEY_UP ? 0 : pick_count - 1) : pick_selected;
            int next = od_next_enabled(from, key_sc == KEY_UP ? -1 : 1);
            if (next >= 0) pick_selected = next;
        }
        pick_typed_len = 0; pick_typed[0] = 0;
        pick_manual = 0;
        dirty = 1;
        return;
    }
    char *text = pick_focus == 3 ? pick_view.filter : pick_focus == 0 ? pick_typed : 0;
    if (!text) return;
    int n = kstrlen(text);
    if (key_sc == KEY_BACKSPACE) {
        if (n) text[n - 1] = 0;
        else if (pick_focus == 0 && pick_cwd != fs_root()) { pick_go(fs_parent(pick_cwd), 1); return; }
    } else if (key_char >= 32 && key_char != '/' && n < FS_NAME_LEN - 1) {
        text[n] = key_char;
        text[n + 1] = 0;
    } else return;
    if (pick_focus == 3) { od_refresh(); pick_first = 0; }
    else { pick_typed_len = kstrlen(pick_typed); pick_type_select(); pick_manual = 0; }
    dirty = 1;
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
    case ICON_HELP: win_open(WK_HELP); break;
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
            } else if (front_kind() == WK_MINES) {
                mines_new(frame_count);
            } else if (front_kind() == WK_2048) {
                g2048_new(frame_count);
            } else if (front_kind() == WK_BREAKOUT) {
                bo_new();
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
    case ACT_SORT_DESCENDING: case ACT_VIEW_ICONS: case ACT_VIEW_DETAILS:
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
    case ACT_DOC_PROPERTIES: return fk == WK_EDIT && edit_binding_valid();
    case ACT_TERM_CLEAR: return fk == WK_TERM && !term_task_running(f);
    case ACT_TERM_BOTTOM: return fk == WK_TERM && term_scroll_offset() > 0;
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
        if (act == ACT_VIEW_ICONS) return !fm_details;
        if (act == ACT_VIEW_DETAILS) return fm_details;
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
    case ACT_VIEW_ICONS: case ACT_VIEW_DETAILS:
        fm_rename_cancel(); fm_details = act == ACT_VIEW_DETAILS; fm_first = fm_manual_scroll = 0; break;
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
    case ACT_TERM_CLEAR: term_clear(); break;
    case ACT_TERM_BOTTOM: term_scroll(-term_scroll_offset()); break;
    case ACT_DOC_PROPERTIES:
        properties_id = edit_file;
        properties_reason = "";
        win_open(WK_PROPERTIES);
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

/* Every app plus up to 24 matching files. */
#define LAUNCH_MAX (ICON_DESK_COUNT + 24)
static LaunchItem launch_items[LAUNCH_MAX];
static int launch_n = 0, launch_apps = 0;
static int launch_recent_view;  /* the Recent files list is showing */
static int launch_recent_sel;

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

static int name_before(const char *a, const char *b) {
    for (;; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a;
        char y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (x != y || !x) return x < y;
    }
}

/* Apps in name order, then, while searching, matching files. */
static void launcher_refresh(void) {
    launch_n = 0;
    launch_buf[launch_len] = 0;
    for (int i = 0; i < ICON_DESK_TRASH && launch_n < LAUNCH_MAX; i++) {
        if (!str_has(icons[i].label, launch_buf)) continue;
        LaunchItem it = {{0}, i, -1, 0};
        kstrcpy(it.name, icons[i].label);
        int at = launch_n++;
        while (at > 0 && name_before(it.name, launch_items[at - 1].name)) { launch_items[at] = launch_items[at - 1]; at--; }
        launch_items[at] = it;
    }
    launch_apps = launch_n;
    for (int id = 0; launch_len && id < fs_node_slots() && launch_n < LAUNCH_MAX; id++) {
        if (!fs_valid(id) || fs_is_app(id) || fs_is_dir(id) || !str_has(fs_name(id), launch_buf))
            continue;
        kstrcpy(launch_items[launch_n].name, fs_name(id));
        launch_items[launch_n].identity = fs_identity(id);
        launch_items[launch_n].icon = -1;
        launch_items[launch_n].file = id;
        launch_n++;
    }
    if (launch_sel >= launch_n)
        launch_sel = launch_n ? launch_n - 1 : 0;
    if (launch_sel < 0 && launch_len)
        launch_sel = 0;
}

static void launcher_open(void) {
    if (switcher_on) switcher_cancel();
    launcher_on = 1;
    launch_len = 0;
    launch_sel = 0;
    launch_scroll = 0;
    launch_recent_view = 0;
    volume_on = 0;
    open_menu = MENU_NONE;
    popup_n = 0;
    recent_prune();
    launcher_refresh();
    dirty = 1;
}

static void launcher_close(void) {
    launcher_on = 0;
    dirty = 1;
}

/* Launcher panel. Classic: 400 wide, 4px above the taskbar at the left.
 * Modern: 480 wide, centered 8px above the dock. A search field, the
 * Recent files row, the app grid (or matching files while searching), and
 * a footer with Settings, Restart and Shut down. */
static int lp_pad(void) { return MODERN ? 14 : 12; }
static int lp_field_h(void) { return MODERN ? 34 : 26; }
static int lp_cols(void) { return MODERN ? 5 : 4; }
static int lp_tile_h(void) { return MODERN ? 88 : 84; }
static int lp_foot_h(void) { return MODERN ? 49 : 41; }
static int lp_row_h(void) { return MODERN ? 30 : 24; }
static int lp_section_h(void) { return MODERN ? 30 : 28; }

static void launcher_geom(int *x, int *y, int *w, int *h) {
    int work = TASKBAR_Y - MENUBAR_H;
    if (MODERN) {
        *w = 480;
        *h = work - 16 < 480 ? work - 16 : 480;
        *x = (fb_w - *w) / 2;
        *y = tb_dock.y - 8 - *h;
    } else {
        *w = 400;
        *h = work - 8 < 460 ? work - 8 : 460;
        *x = 4;
        *y = TASKBAR_Y - 4 - *h;
    }
}
static int lp_field_y(int y) { return y + lp_pad(); }
/* Top of the Recent files row (no search) or of the results. */
static int lp_body_y(int y) { return lp_field_y(y) + lp_field_h() + (MODERN ? 10 : 8); }
static int lp_recent_h(void) { return MODERN ? 40 : 36; }
static int lp_grid_y(int y) {
    int top = lp_body_y(y);
    if (!launch_len) top += lp_recent_h() + (MODERN ? 6 : 0);
    return top + lp_section_h();
}
static int lp_grid_bottom(int y, int h) { return y + h - lp_foot_h() - 4; }
static int lp_grid_x(int x) { return x + (MODERN ? 12 : 8); }
static int lp_grid_w(int w) { return w - (MODERN ? 12 + 4 + 10 : 8 + 8 + 12); }
static int lp_cell_w(int w) { return (lp_grid_w(w) - (lp_cols() - 1) * 4) / lp_cols(); }

static int launcher_visible_rows(void) {
    int x, y, w, h;
    launcher_geom(&x, &y, &w, &h);
    int rows = (lp_grid_bottom(y, h) - lp_grid_y(y)) / (lp_tile_h() + 4);
    return rows < 1 ? 1 : rows;
}
static int launcher_total_rows(void) { int c = lp_cols(); return (launch_apps + c - 1) / c; }
static void launcher_clamp_scroll(void) {
    int rows = launcher_visible_rows(), total = launcher_total_rows();
    if (launch_scroll > total - rows) launch_scroll = total - rows;
    if (launch_scroll < 0) launch_scroll = 0;
}
static void launcher_reveal(void) {
    if (launch_sel < 0 || launch_sel >= launch_apps) return;
    int row = launch_sel / lp_cols(), rows = launcher_visible_rows();
    launcher_clamp_scroll();
    if (row < launch_scroll) launch_scroll = row;
    if (row >= launch_scroll + rows) launch_scroll = row - rows + 1;
}
static void launcher_scroll(int amount) {
    launch_scroll += amount;
    launcher_clamp_scroll();
    dirty = 1;
}
static void launcher_cell(int x, int y, int w, int i, int *cx, int *cy) {
    int c = lp_cols(), k = i - launch_scroll * c, cw = lp_cell_w(w);
    *cx = lp_grid_x(x) + (k % c) * (cw + 4);
    *cy = lp_grid_y(y) + (k / c) * (lp_tile_h() + 4);
}
/* Files matching a search are listed under the app grid rows that fit. */
static int lp_files_y(int x, int y, int w) {
    int rows = (launch_apps + lp_cols() - 1) / lp_cols();
    int vis = launcher_visible_rows();
    (void)x; (void)w;
    if (rows > vis) rows = vis;
    return lp_grid_y(y) + rows * (lp_tile_h() + 4) + (launch_apps ? 4 : 0) + lp_section_h();
}

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
static void launcher_run_recent(int i) {
    if (!recent_valid(i)) { recent_prune(); dirty = 1; return; }
    int id = recent[i].id;
    launcher_close();
    open_fs_file(id);
}

static void launcher_key(void) {
    int c = lp_cols();
    if (launch_recent_view) {
        if (key_sc == KEY_ESC || key_sc == KEY_LEFT || key_sc == KEY_BACKSPACE) launch_recent_view = 0;
        else if (key_sc == KEY_UP && launch_recent_sel > 0) launch_recent_sel--;
        else if (key_sc == KEY_DOWN && launch_recent_sel + 1 < recent_n) launch_recent_sel++;
        else if (key_sc == KEY_ENTER) { launcher_run_recent(launch_recent_sel); return; }
        dirty = 1;
        return;
    }
    if (key_sc == KEY_ESC) {
        if (launch_len) { launch_len = 0; launch_sel = 0; launcher_refresh(); dirty = 1; return; }
        launcher_close();
        return;
    } else if (key_sc == KEY_ENTER) {
        if (launch_sel < 0) { launch_recent_view = 1; launch_recent_sel = 0; dirty = 1; return; }
        launcher_run(launch_sel);
        return;
    } else if (key_sc == KEY_LEFT) {
        if (launch_sel > 0) launch_sel--;
    } else if (key_sc == KEY_RIGHT) {
        if (launch_sel < 0) { launch_recent_view = 1; launch_recent_sel = 0; }
        else if (launch_sel + 1 < launch_n) launch_sel++;
    } else if (key_sc == KEY_UP) {
        if (launch_sel >= launch_apps) launch_sel = launch_sel > launch_apps ? launch_sel - 1 : launch_apps - 1;
        else if (launch_sel - c >= 0) launch_sel -= c;
        else if (!launch_len) launch_sel = -1;
    } else if (key_sc == KEY_DOWN) {
        if (launch_sel < 0) launch_sel = 0;
        else if (launch_sel >= launch_apps) { if (launch_sel + 1 < launch_n) launch_sel++; }
        else if (launch_sel + c < launch_apps) launch_sel += c;
        else if (launch_apps < launch_n) launch_sel = launch_apps;
        else if (launch_sel / c < (launch_apps - 1) / c) launch_sel = launch_apps - 1;
    } else if (key_sc == KEY_BACKSPACE) {
        if (launch_len > 0)
            launch_len--;
        launcher_refresh();
    } else if (key_char && launch_len < (int)sizeof launch_buf - 2) {
        launch_buf[launch_len++] = key_char;
        launch_sel = 0;
        launcher_refresh();
    } else return;
    launcher_reveal();
    dirty = 1;
}

static int launcher_item_icon(int i) {
    if (launch_items[i].icon >= 0) return app_icon_for(icons[launch_items[i].icon].icon);
    return file_extension(launch_items[i].name, ".bex") ? AI_NATIVE : AI_DOCUMENT;
}

/* A tile label on up to two centered lines, split at the last space that
 * fits the first line. */
static void tile_label(const char *s, int x, int y, int w, uint8_t ink, int bold) {
    int n = kstrlen(s);
    if (kit_text_w(s, bold) <= w) { kit_text_center(s, x, y, w, ink, bold); return; }
    char first[FS_NAME_LEN];
    int cut = -1;
    for (int i = 0; i < n && i < FS_NAME_LEN - 1; i++) {
        if (s[i] != ' ') continue;
        kmemcpy(first, s, i); first[i] = 0;
        if (kit_text_w(first, bold) <= w) cut = i;
    }
    if (cut < 0) {
        int tw = kit_text_w(s, bold) < w ? kit_text_w(s, bold) : w;
        kit_text(s, x + (w - tw) / 2, y, ink, w, bold);
        return;
    }
    kmemcpy(first, s, cut); first[cut] = 0;
    kit_text_center(first, x, y, w, ink, bold);
    const char *rest = s + cut + 1;
    int tw = kit_text_w(rest, bold) < w ? kit_text_w(rest, bold) : w;
    kit_text(rest, x + (w - tw) / 2, y + CHAR_H + 1, ink, w, bold);
}

static int lp_hover(int x, int y, int w, int h) { return hit(mouse_x, mouse_y, x, y, w, h); }

static void lp_list_item(int x, int y, int w, int icon, const char *name, const char *detail, int sel) {
    int h = lp_row_h(), hov = lp_hover(x, y, w, h);
    if (sel || hov) kit_fill(x, y, w, h, MODERN ? 6 : KIT_R_SQUARE, sel ? TK_ACCENT_100 : (MODERN ? TK_HOVER : TK_ACCENT_50), 1);
    app_icon_draw(icon, x + (MODERN ? 10 : 8), y + (h - 16) / 2, AI_SMALL);
    int tx = x + (MODERN ? 10 : 8) + 16 + (MODERN ? 8 : 6);
    int dw = detail ? kit_text_w(detail, 0) : 0;
    kit_text(name, tx, kit_ty(y, h), TK_TEXT, x + w - tx - dw - 16, 0);
    if (detail) kit_text_right(detail, x + w - (MODERN ? 10 : 8), kit_ty(y, h), TK_TEXT2, 0);
}

static const char *const lp_foot_labels[3] = {"Settings", "Restart", "Shut down"};
static const int lp_foot_icons[3] = {UI_ICON_GEAR, UI_ICON_RESTART, UI_ICON_POWER};
static void lp_foot_geom(int x, int y, int w, int h, int b, int *bx, int *by, int *bw, int *bh) {
    *bh = MODERN ? 30 : 26;
    *by = y + h - lp_foot_h() + (lp_foot_h() - *bh) / 2 + (MODERN ? 0 : 1);
    int ws[3];
    for (int i = 0; i < 3; i++) ws[i] = kit_text_w(lp_foot_labels[i], 0) + 16 + 6 + (MODERN ? 20 : 24);
    int pad = MODERN ? 10 : 8;
    if (b == 0) *bx = x + pad;
    else if (b == 2) *bx = x + w - pad - ws[2];
    else *bx = x + w - pad - ws[2] - (MODERN ? 4 : 6) - ws[1];
    *bw = ws[b];
}

static void draw_launcher(void) {
    if (!launcher_on)
        return;
    int x, y, w, h;
    launcher_geom(&x, &y, &w, &h);
    launcher_clamp_scroll();
    uint8_t panel = MODERN ? TK_46 : TK_RAISED;
    if (MODERN) {
        kit_shadow(x, y, w, h, 12);
        kit_box(x, y, w, h, 12, panel, TK_LINE_STRONG);
    } else {
        kit_shadow(x, y, w, h, 0);
        kit_box(x, y, w, h, KIT_R_SQUARE, panel, TK_TEXT2);
    }
    /* Search */
    int pad = lp_pad(), fx = x + pad, fy = lp_field_y(y), fw = w - 2 * pad, fh = lp_field_h();
    launch_buf[launch_len] = 0;
    int tx = kit_search(fx, fy, fw, fh, launch_buf, "Search apps and files", KIT_FOCUS);
    if (!launch_recent_view && ((frame_count / 35) & 1) == 0)
        kit_caret(tx + kit_text_w(launch_buf, 0) + (launch_len ? 1 : -2), fy + (fh - 17) / 2);
    int body = lp_body_y(y);
    char count[12];
    if (launch_recent_view) {
        /* Recent files list */
        int rh = MODERN ? 40 : 37, bx = x + (MODERN ? 12 : 8);
        kit_icon_button(bx, body + (rh - (MODERN ? 30 : 26)) / 2 - (MODERN ? 0 : 2), MODERN ? 30 : 26, UI_ICON_BACK,
                        lp_hover(bx, body, 30, rh) ? KIT_HOVER : 0);
        draw_string_bold("Recent files", bx + (MODERN ? 36 : 30), kit_ty(body, rh - 4), TK_TEXT);
        int cw = kit_text_w("Clear", 0) + 20, cx = x + w - (MODERN ? 12 : 8) - cw;
        kit_button(cx, body + (rh - 26) / 2 - (MODERN ? 0 : 2), cw, MODERN ? 30 : 26, "Clear", -1, KB_FLAT,
                   lp_hover(cx, body, cw, rh) ? KIT_HOVER : 0);
        if (!MODERN) draw_hline(x + 1, body + rh - 1, w - 2, TK_LINE_SUBTLE);
        int ly = body + rh + 4, lx = x + (MODERN ? 8 : 4), lw = w - 2 * (MODERN ? 8 : 4);
        recent_prune();
        if (!recent_n) kit_text("No recent files", lx + 10, ly + 6, TK_TEXT2, lw, 0);
        for (int i = 0; i < recent_n && ly + lp_row_h() < y + h - lp_foot_h(); i++, ly += lp_row_h())
            lp_list_item(lx, ly, lw, AI_DOCUMENT, fs_name(recent[i].id), recent_folder(recent[i].id), i == launch_recent_sel);
    } else {
        if (!launch_len) {
            /* Recent files row */
            int rx = x + (MODERN ? 12 : 4), rw = w - 2 * (MODERN ? 12 : 4), rh = MODERN ? 40 : 32;
            int sel = launch_sel < 0, hov = lp_hover(rx, body, rw, rh);
            if (MODERN) kit_box(rx, body, rw, rh, 8, sel ? TK_ACCENT_50 : hov ? TK_HOVER : ui_theme.content, sel ? TK_ACCENT : TK_LINE_SUBTLE);
            else if (sel || hov) draw_rect(rx, body, rw, rh, sel ? TK_ACCENT_100 : TK_ACCENT_50);
            ui_icon_draw(UI_ICON_RECENT, rx + (MODERN ? 12 : 8), body + (rh - 16) / 2, 16, TK_ICON, MODERN ? ui_theme.content : panel);
            kit_text("Recent files", rx + (MODERN ? 12 : 8) + 16 + (MODERN ? 10 : 8), kit_ty(body, rh), TK_TEXT, rw - 100, MODERN);
            fmt_uint(count, (unsigned)recent_n);
            int cxr = rx + rw - (MODERN ? 12 : 8) - 16;
            ui_icon_draw(UI_ICON_RIGHT, cxr, body + (rh - 16) / 2, 16, TK_ICON, MODERN ? ui_theme.content : panel);
            if (MODERN) {
                int pw = kit_text_w(count, 0) + 14, px = cxr - 10 - pw;
                kit_fill(px, body + (rh - 20) / 2, pw, 20, 10, TK_HOVER, 1);
                kit_text_center(count, px, kit_ty(body + (rh - 20) / 2, 20), pw, TK_ICON, 0);
            } else kit_text_right(count, cxr - 12, kit_ty(body, rh), TK_TEXT2, 0);
            if (!MODERN) draw_hline(x + 1, body + lp_recent_h() - 1, w - 2, TK_LINE_SUBTLE);
        }
        /* All apps */
        int sy = lp_grid_y(y) - lp_section_h(), sx = x + (MODERN ? 18 : 12), sr = x + w - (MODERN ? 18 : 12);
        kit_text(launch_len ? "Apps" : "All apps", sx, kit_ty(sy, lp_section_h()), TK_TEXT2, 200, 1);
        fmt_uint(count, (unsigned)launch_apps);
        kit_text_right(count, sr, kit_ty(sy, lp_section_h()), TK_TEXT2, 0);
        int c = lp_cols(), rows = launcher_visible_rows(), cw = lp_cell_w(w), th = lp_tile_h();
        int first = launch_scroll * c, last = first + rows * c;
        if (last > launch_apps) last = launch_apps;
        for (int i = first; i < last; i++) {
            int gx, gy;
            launcher_cell(x, y, w, i, &gx, &gy);
            int sel = i == launch_sel, hov = !sel && lp_hover(gx, gy, cw, th);
            if (sel) {
                if (MODERN) {
                    kit_fill(gx, gy, cw, th, 8, TK_ACCENT_50, 1);
                    kit_frame(gx, gy, cw, th, 8, TK_ACCENT);
                    kit_frame(gx + 1, gy + 1, cw - 2, th - 2, 7, TK_ACCENT);
                } else {
                    draw_rect(gx, gy, cw, th, TK_ACCENT_100);
                    draw_frame(gx, gy, cw, th, TK_ACCENT);
                }
            } else if (hov) kit_fill(gx, gy, cw, th, MODERN ? 8 : KIT_R_SQUARE, MODERN ? TK_HOVER : TK_ACCENT_50, 1);
            int top = MODERN ? 12 : 10;
            app_icon_draw(launcher_item_icon(i), gx + (cw - AI_BIG) / 2, gy + top, AI_BIG);
            tile_label(launch_items[i].name, gx + 3, gy + top + AI_BIG + (MODERN ? 6 : 4), cw - 6, TK_TEXT, 0);
        }
        if (!launch_apps && launch_len)
            kit_text("No apps match", sx, lp_grid_y(y) + 6, TK_TEXT2, w, 0);
        if (launcher_total_rows() > rows) {
            int gy = lp_grid_y(y), len = lp_grid_bottom(y, h) - gy;
            kit_vscroll(x + w - 1 - (MODERN ? 10 : 12), gy, len, launch_scroll, rows, launcher_total_rows(), 0);
        }
        /* Matching files */
        if (launch_len && launch_n > launch_apps) {
            int fy2 = lp_files_y(x, y, w);
            kit_text("Files", sx, kit_ty(fy2 - lp_section_h(), lp_section_h()), TK_TEXT2, 200, 1);
            fmt_uint(count, (unsigned)(launch_n - launch_apps));
            kit_text_right(count, sr, kit_ty(fy2 - lp_section_h(), lp_section_h()), TK_TEXT2, 0);
            int lx = x + (MODERN ? 8 : 4), lw = w - 2 * (MODERN ? 8 : 4);
            for (int i = launch_apps; i < launch_n && fy2 + lp_row_h() <= y + h - lp_foot_h() - 4; i++, fy2 += lp_row_h())
                lp_list_item(lx, fy2, lw, launcher_item_icon(i), launch_items[i].name,
                             recent_folder(launch_items[i].file), i == launch_sel);
        }
    }
    /* Footer */
    int foot = y + h - lp_foot_h();
    if (MODERN) {
        kit_fill_bottom(x + 1, foot, w - 2, lp_foot_h() - 1, 11, TK_45);
        draw_hline(x + 1, foot, w - 2, TK_LINE_SUBTLE);
    } else {
        draw_rect(x + 1, foot, w - 2, lp_foot_h() - 1, TK_BASE);
        draw_hline(x + 1, foot, w - 2, TK_LINE_SUBTLE);
    }
    for (int b = 0; b < 3; b++) {
        int bx, by, bw, bh;
        lp_foot_geom(x, y, w, h, b, &bx, &by, &bw, &bh);
        int kind = MODERN || b == 0 ? KB_FLAT : KB_DEFAULT;
        kit_button(bx, by, bw, bh, lp_foot_labels[b], lp_foot_icons[b], kind, lp_hover(bx, by, bw, bh) ? KIT_HOVER : 0);
    }
}

static int launcher_click(void) {
    int x, y, w, h;
    launcher_geom(&x, &y, &w, &h);
    if (!hit(mouse_x, mouse_y, x, y, w, h)) {
        launcher_close();
        return 1;
    }
    for (int b = 0; b < 3; b++) {
        int bx, by, bw, bh;
        lp_foot_geom(x, y, w, h, b, &bx, &by, &bw, &bh);
        if (!hit(mouse_x, mouse_y, bx, by, bw, bh)) continue;
        launcher_close();
        if (b == 0) win_open(WK_SETTINGS);
        else if (b == 1) do_restart();
        else do_shutdown();
        return 1;
    }
    int pad = lp_pad(), fy = lp_field_y(y), fh = lp_field_h();
    if (launch_len && hit(mouse_x, mouse_y, kit_search_clear_x(x + pad, w - 2 * pad), fy, 24, fh)) {
        launch_len = 0; launch_sel = 0; launcher_refresh(); dirty = 1; return 1;
    }
    int body = lp_body_y(y);
    if (launch_recent_view) {
        int rh = MODERN ? 40 : 37;
        if (hit(mouse_x, mouse_y, x + (MODERN ? 12 : 8), body, 30, rh)) { launch_recent_view = 0; dirty = 1; return 1; }
        int cw = kit_text_w("Clear", 0) + 20, cx = x + w - (MODERN ? 12 : 8) - cw;
        if (hit(mouse_x, mouse_y, cx, body, cw, rh)) { recent_n = 0; dirty = 1; return 1; }
        int ly = body + rh + 4;
        for (int i = 0; i < recent_n; i++, ly += lp_row_h())
            if (hit(mouse_x, mouse_y, x, ly, w, lp_row_h())) { launcher_run_recent(i); return 1; }
        return 1;
    }
    if (!launch_len && hit(mouse_x, mouse_y, x, body, w, lp_recent_h())) {
        launch_recent_view = 1;
        launch_recent_sel = 0;
        dirty = 1;
        return 1;
    }
    int c = lp_cols(), rows = launcher_visible_rows(), cw = lp_cell_w(w);
    int first = launch_scroll * c, last = first + rows * c;
    if (last > launch_apps) last = launch_apps;
    for (int i = first; i < last; i++) {
        int gx, gy;
        launcher_cell(x, y, w, i, &gx, &gy);
        if (hit(mouse_x, mouse_y, gx, gy, cw, lp_tile_h())) {
            launcher_run(i);
            return 1;
        }
    }
    if (launch_len && launch_n > launch_apps) {
        int fy2 = lp_files_y(x, y, w);
        for (int i = launch_apps; i < launch_n; i++, fy2 += lp_row_h())
            if (hit(mouse_x, mouse_y, x, fy2, w, lp_row_h())) { launcher_run(i); return 1; }
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


static void saveas_reset(void);
static int saveas_dir(void);
static int saveas_click(int x, int y, int w, int h);
static int saveas_family_next(int target);

static void namedlg_open(int target, const char *initial) {
    fm_checked_cwd();
    name_dlg = 1;
    name_focus = name_failed = 0;
    name_failure_message=0;
    name_owner = context_slot;
    name_owner_seq = wins[name_owner].seq;
    name_target = target;
    name_pdf_paper = WRITER_PDF_LETTER;
    name_len = name_sel = 0;
    for (int i = 0; initial && initial[i] && i < FS_NAME_LEN - 1; i++)
        name_buf[name_len++] = initial[i];
    name_buf[name_len] = 0;
    if (target == 3 || target == 6) {
        if (writer_export_retry_name(target == 6 ? DOCUMENT_SAVE_PDF : DOCUMENT_SAVE_RTF,
                                     name_buf, sizeof name_buf, &name_pdf_paper)) name_len = kstrlen(name_buf);
    } else if (target == 5 && spreadsheet_export_retry_name(name_buf, sizeof name_buf)) name_len = kstrlen(name_buf);
    saveas_reset();
    dirty = 1;
}

static void namedlg_hide(void) { name_dlg = 0; dirty = 1; }
static void namedlg_close(void) {
    namedlg_hide();
    edit_close_cancel(); /* Canceling Save As also cancels the close request. */
    dirty = 1;
}

/* Save As: a name band (with paper choice for PDF), the folder browser and
 * a footer with the type and the buttons. */
static void namedlg_geom(int *x, int *y, int *w, int *h) {
    int avail = fb_h - MENUBAR_H - 64;
    *w = fb_w - 16 < 640 ? fb_w - 16 : 640;
    *h = 500 + (name_target == 6 ? 34 : 0);
    if (*h > avail) *h = avail;
    *x = (fb_w - *w) / 2;
    *y = MENUBAR_H + 8 + (avail - *h) / 2;
}

static int namedlg_pad(void) { return MODERN ? 16 : 12; }
static void namedlg_field(int x, int y, int w, int *fx, int *fy, int *fw, int *fh) {
    *fx = x + 92;
    *fy = y + TITLE_BAR + (MODERN ? 9 : 10);
    *fw = w - 92 - namedlg_pad();
    *fh = BTN_H;
}
/* The message line under the field, then the paper row for PDF. */
static int namedlg_message_y(int x, int y, int w) {
    int fx, fy, fw, fh;
    namedlg_field(x, y, w, &fx, &fy, &fw, &fh);
    return fy + fh + 6;
}
static int namedlg_message_h(void) { return MODERN ? 30 : 22; }
static void namedlg_paper(int x, int y, int w, int i, int *px, int *py, int *pw, int *ph) {
    *px = x + 92 + i * 140;
    *py = namedlg_message_y(x, y, w) + namedlg_message_h() + 6;
    *pw = 132;
    *ph = 24;
}
static int namedlg_band_h(void) {
    return (MODERN ? 9 : 10) + BTN_H + 6 + namedlg_message_h() + (name_target == 6 ? 30 : 0) + (MODERN ? 10 : 8);
}
static int namedlg_foot_h(void) { return MODERN ? 56 : 48; }

static void namedlg_buttons(int x, int y, int w, int h, int *sx, int *cx, int *by, int *bw) {
    int fh = namedlg_foot_h();
    *bw = MODERN ? 84 : 80;
    *by = y + h - fh + (fh - BTN_H) / 2;
    *sx = x + w - namedlg_pad() - *bw;
    *cx = *sx - 8 - *bw;
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
    /* The chosen folder becomes the document window's folder. */
    int chosen = saveas_dir();
    if (chosen >= 0 && name_target != 1) fm_set_cwd(chosen);
    int parent = fm_checked_cwd();
    int result = name_target == 0 ? edit_write_named(name_buf) :
                 name_target == 1 ? paint_write_in(chosen, name_buf) :
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

/* ---------- Alerts ----------
 * A 32px icon, a bold question and a line of detail; the buttons sit on the
 * content in Classic and on a footer band in Modern. */

static int alert_w(void) { return 440; }
static int alert_body_h(void) { return MODERN ? 76 : 64; }
static int alert_foot_h(void) { return MODERN ? 58 : BTN_H + 16; }
static int alert_h(void) { return TITLE_BAR + alert_body_h() + alert_foot_h(); }
static void alert_buttons_y(int y, int *by) { *by = y + alert_h() - (alert_foot_h() + BTN_H) / 2 - (MODERN ? 0 : 4); }

static void draw_alert(int x, int y, int w, int h, const char *title, int app_icon, int icon,
                       const char *heading, const char *detail) {
    shade_rect(0, 0, fb_w, fb_h, 1);
    chrome_icon = app_icon;
    gui_draw_window(x, y, w, h, title, 0, WIN_NOMENU | WIN_DIALOG);
    chrome_icon = -1;
    hot(close_box_x(x, w), cap_y(y), CAP_CLOSE_W, CAP_H);
    int top = y + TITLE_BAR, ix = x + (MODERN ? 20 : 16), tx = ix + 32 + (MODERN ? 14 : 16);
    ui_icon_draw(icon, ix, top + (MODERN ? 16 : 14), 32, TK_ICON, ui_theme.content);
    kit_text(heading, tx, top + (MODERN ? 18 : 16), TK_TEXT, x + w - 16 - tx, 1);
    kit_text(detail, tx, top + (MODERN ? 40 : 37), TK_TEXT2, x + w - 16 - tx, 0);
    if (MODERN) dlg_footer(x, w, y + h - alert_foot_h(), alert_foot_h());
}

static void edit_close_geom(int *x, int *y) {
    *x = (fb_w - alert_w()) / 2;
    *y = (fb_h - alert_h()) / 2;
}

/* Don't Save at the left; Cancel and Save at the right. */
static void edit_close_button(int x, int i, int *bx, int *bw) {
    int pad = MODERN ? 16 : 16;
    *bw = i == 1 ? kit_button_w("Don't Save", -1) : MODERN ? 84 : 80;
    if (i == 1) *bx = x + pad;
    else if (i == 0) *bx = x + alert_w() - pad - *bw;
    else *bx = x + alert_w() - pad - *bw - 8 - (MODERN ? 84 : 80);
}

static const char *edit_close_doc_name(void) {
    const Document *doc = &window_state[edit_close_owner].doc;
    return wins[edit_close_owner].kind == WK_SPREADSHEET ? spreadsheet_title() :
        wins[edit_close_owner].kind == WK_WRITER ? writer_title() :
        doc->file >= 0 && fs_identity(doc->file) == doc->identity ? fs_name(doc->file) : "untitled";
}

static void draw_edit_close(void) {
    if (!edit_close_dlg) return;
    int x, y, w = alert_w(), h = alert_h();
    edit_close_geom(&x, &y);
    char heading[FS_NAME_LEN + 48];
    if (document_pending_owner) kstrcpy(heading, "Saving changes...");
    else {
        kstrcpy(heading, "Save changes to \"");
        kstrcpy(heading + kstrlen(heading), edit_close_doc_name());
        kstrcpy(heading + kstrlen(heading), document_action == DOCUMENT_CLOSE ? "\" before closing?" : "\" before replacing?");
    }
    const char *detail = edit_close_failed && fs_sync_busy() ? "Disk saving. Retry Save shortly."
                : document_pending_owner ? "Saving to disk. Cancel keeps this document open."
                : edit_close_failed == 3 ? "Requested file changed. Cancel and choose it again."
                : edit_close_failed == 2 ? "Newer changes still need saving."
                : edit_close_failed ? "Save failed. Your document is still open."
                : "If you don't save, everything since your last save is lost.";
    int kind = wins[edit_close_owner].kind;
    draw_alert(x, y, w, h, win_app_name(kind), app_icon_for(kind_icon(kind)),
               edit_close_failed ? UI_ICON_A_ERR : UI_ICON_A_WARN, heading, detail);
    static const char *const labels[] = {"Save", "Don't Save", "Cancel"};
    int by;
    alert_buttons_y(y, &by);
    for (int i = 0; i < 3; i++) {
        if ((document_pending_owner || edit_close_failed == 3) && i != 2) continue;
        int bx, bw;
        edit_close_button(x, i, &bx, &bw);
        kit_button(bx, by, bw, BTN_H, labels[i], -1, i == 0 ? KB_PRIMARY : KB_DEFAULT,
                   hot(bx, by, bw, BTN_H) | (i == edit_close_focus ? KIT_FOCUS : 0));
    }
}

static void edit_close_click(void) {
    int x, y, by;
    edit_close_geom(&x, &y);
    alert_buttons_y(y, &by);
    if (hit(mouse_x, mouse_y, close_box_x(x, alert_w()), cap_y(y), CAP_CLOSE_W, CAP_H)) { edit_close_choose(2); return; }
    for (int i = 0; i < 3; i++) {
        if ((document_pending_owner || edit_close_failed == 3) && i != 2) continue;
        int bx, bw;
        edit_close_button(x, i, &bx, &bw);
        if (hit(mouse_x, mouse_y, bx, by, bw, BTN_H)) { edit_close_choose(i); return; }
    }
    /* Outside clicks cannot silently discard or dismiss a close request. */
}

static void edit_close_key(void) {
    if (key_sc == KEY_ESC) edit_close_cancel();
    else if (key_sc == KEY_TAB) {
        edit_close_focus = document_pending_owner || edit_close_failed == 3 ? 2 : (edit_close_focus + (shift_down ? 2 : 1)) % 3;
        dirty = 1;
    } else if (key_sc == KEY_ENTER) edit_close_choose(edit_close_focus);
    else if (key_char == 'd' || key_char == 'D') {
        if (!document_pending_owner && edit_close_failed != 3) edit_close_choose(1);
    }
}

/* ---------- Save As ---------- */

static int saveas_dir(void) { return name_dlg && fs_is_dir(pick_cwd) ? pick_cwd : -1; }

/* The folder a document would be saved next to: its own folder, else the
 * window's folder (Paint defaults to Pictures). */
static void saveas_reset(void) {
    int dir = fm_checked_cwd(), file = -1;
    if (name_target == 0 && edit_binding_valid()) file = edit_file;
    else if (name_target == 2 || name_target == 3 || name_target == 6) file = writer_file();
    else if (name_target == 4 || name_target == 5) file = spreadsheet_file();
    if (file >= 0 && fs_is_dir(fs_parent(file))) dir = fs_parent(file);
    if (name_target == 1) {
        int pics = fs_find_child(fs_root(), "Pictures");
        if (fs_is_dir(pics)) dir = pics;
    }
    pick_reset(dir, 1);
    /* The name without its extension starts selected. */
    name_sel = 0;
    for (int i = 0; i < name_len; i++) if (name_buf[i] == '.') name_sel = i;
}

static Chooser saveas_layout_of(int x, int y, int w, int h) {
    Chooser c;
    chooser_layout(&c, x, y, w, h, namedlg_band_h(), namedlg_foot_h());
    return c;
}

/* The file this window already owns, which Save As may replace. */
static int saveas_own_file(void) {
    if (name_owner < 0 || name_owner >= MAX_WIN) return -1;
    if (name_target == 0) {
        const Document *doc = &window_state[name_owner].doc;
        return doc->file >= 0 && fs_identity(doc->file) == doc->identity ? doc->file : -1;
    }
    if (name_target == 2) return writer_file();
    if (name_target == 4) return spreadsheet_file();
    return -1;
}

/* Writer saves BWR or exports RTF and PDF; Spreadsheet saves BSH or
 * exports CSV. A close request keeps the plain save. */
static int saveas_family_next(int target) {
    if (edit_close_valid() && edit_close_owner == name_owner) return target;
    if (target == 2) return 3;
    if (target == 3) return 6;
    if (target == 6) return 2;
    if (target == 4) return 5;
    if (target == 5) return 4;
    return target;
}

static void saveas_set_target(int target) {
    if (target == name_target) return;
    const char *ext = saveas_exts[target];
    int dot = -1;
    for (int i = 0; i < name_len; i++) if (name_buf[i] == '.') dot = i;
    if (dot < 0) dot = name_len;
    int elen = kstrlen(ext);
    if (dot + elen < FS_NAME_LEN) { kstrcpy(name_buf + dot, ext); name_len = dot + elen; }
    name_target = target;
    name_sel = 0;
    name_failed = 0;
    name_focus = 0;
    od_refresh();
    dirty = 1;
}

static int saveas_click_row(int i, int dbl) {
    int id = pick_ids[i];
    if (fs_is_dir(id)) {
        if (dbl) pick_go(id, 1);
        return 0;
    }
    const char *nm = fs_name(id);
    name_len = 0;
    while (nm[name_len] && name_len < FS_NAME_LEN - 1) { name_buf[name_len] = nm[name_len]; name_len++; }
    name_buf[name_len] = 0;
    name_sel = 0;
    name_failed = 0;
    name_focus = 0;
    if (dbl) namedlg_commit();
    return 0;
}

static void saveas_type_rect(const Chooser *c, int *tx, int *ty, int *tw) {
    *tx = c->x + 92;
    *ty = c->foot_y + (c->foot_h - BTN_H) / 2;
    *tw = 220;
}

/* Returns 1 for the close box. */
static int saveas_click(int x, int y, int w, int h) {
    if (hit(mouse_x, mouse_y, close_box_x(x, w), cap_y(y), CAP_CLOSE_W, CAP_H)) return 1;
    Chooser c = saveas_layout_of(x, y, w, h);
    int tx, ty, tw;
    saveas_type_rect(&c, &tx, &ty, &tw);
    if (hit(mouse_x, mouse_y, tx, ty, tw, BTN_H)) { saveas_set_target(saveas_family_next(name_target)); return 0; }
    chooser_click(&c);
    return 0;
}

static const char *saveas_title(void) {
    return name_target == 6 ? "Export PDF" : name_target == 5 ? "Export CSV" :
           name_target == 3 ? "Export Rich Text" : name_target == 1 ? "Save Picture As" : "Save As";
}

/* Message line: errors, a name that exists, or a note about the type. */
static void saveas_message(int x, int y, int w, int existing) {
    char text[FS_NAME_LEN + 64];
    const char *message = 0;
    int kind = KIT_INFO;
    if (name_failed) {
        message = (name_target == 4 || name_target == 5) ? spreadsheet_status() : name_target >= 2 ? writer_status() :
                  name_failure_message ? name_failure_message : "Save failed. Check storage and file name.";
        kind = KIT_ERROR_MSG;
    } else if (pick_message) {
        message = pick_message; kind = KIT_ERROR_MSG;
    } else if (existing >= 0) {
        kstrcpy(text, name_buf);
        kstrcpy(text + kstrlen(text), pick_ids[existing] == saveas_own_file() ?
                " already exists here. Saving will replace it." : " already exists here. Choose another name.");
        message = text; kind = KIT_WARN;
    } else if (name_target == 5) message = "Other apps may run formula-like CSV text.";
    if (!message || !message[0]) return;
    static const uint8_t fills[] = {TK_ACCENT_50, TK_AMBER_50, TK_RED_50, TK_GREEN_50};
    static const uint8_t inks[] = {TK_ACCENT_700, TK_AMBER_700, TK_RED_700, TK_GREEN_700};
    static const int icons[] = {UI_ICON_INFO_C, UI_ICON_WARN, UI_ICON_ERR, UI_ICON_OK};
    int h = namedlg_message_h(), ix = x;
    uint8_t paper = MODERN ? ui_theme.chrome : TK_BASE;
    if (MODERN) { kit_fill(x, y, w, h, 6, fills[kind], 1); ix += 10; paper = fills[kind]; }
    ui_icon_draw(icons[kind], ix, y + (h - 16) / 2, 16, TK_ICON, paper);
    kit_text(message, ix + 22, kit_ty(y, h), inks[kind], x + w - ix - 30, 0);
}

static void draw_namedlg(void) {
    if (!name_dlg)
        return;
    int x, y, w, h;
    namedlg_geom(&x, &y, &w, &h);
    Chooser c = saveas_layout_of(x, y, w, h);
    shade_rect(0, 0, fb_w, fb_h, 1);
    int owner_kind = name_owner >= 0 && name_owner < MAX_WIN ? wins[name_owner].kind : WK_EDIT;
    chrome_icon = app_icon_for(kind_icon(owner_kind));
    gui_draw_window(x, y, w, h, saveas_title(), 0, WIN_NOMENU | WIN_DIALOG);
    chrome_icon = -1;
    hot(close_box_x(x, w), cap_y(y), CAP_CLOSE_W, CAP_H);
    int band = namedlg_band_h(), pad = namedlg_pad();
    if (MODERN) {
        draw_rect(x + 1, c.band_y, w - 2, band - 1, ui_theme.chrome);
        draw_hline(x + 1, c.band_y + band - 1, w - 2, TK_LINE);
    } else {
        draw_rect(x + 1, c.band_y, w - 2, band - 1, TK_BASE);
        draw_hline(x + 1, c.band_y + band - 1, w - 2, TK_LINE_SUBTLE);
    }
    int pdf = name_target == 6;
    int fx, fy, fw, fh;
    namedlg_field(x, y, w, &fx, &fy, &fw, &fh);
    kit_text("Name", x + pad, kit_ty(fy, fh), TK_TEXT, fx - x - pad - 8, 0);
    int focused = !pdf || !name_focus;
    int tx = kit_field(fx, fy, fw, fh, focused ? KIT_FOCUS : 0), ty = kit_ty(fy, fh);
    name_buf[name_len] = 0;
    int caret = tx;
    if (name_sel > 0 && name_sel <= name_len && focused) {
        char stem[FS_NAME_LEN];
        for (int i = 0; i < name_sel; i++) stem[i] = name_buf[i];
        stem[name_sel] = 0;
        int sw = kit_text_w(stem, 0);
        draw_rect(tx - 1, ty, sw + 2, UI_FONT_H, TK_ACCENT);
        kit_text(stem, tx, ty, TK_ON_ACCENT, fw - 16, 0);
        caret = tx + sw + kit_text(name_buf + name_sel, tx + sw, ty, TK_TEXT, fw - 16 - sw, 0);
    } else caret = tx + kit_text(name_buf, tx, ty, TK_TEXT, fw - 16, 0);
    if (focused && name_sel == 0 && ((frame_count / 35) & 1) == 0) kit_caret(caret, ty);
    int existing = saveas_existing();
    saveas_message(fx, namedlg_message_y(x, y, w), fw, existing);
    if (pdf) {
        static const char *const papers[] = {"Letter (US)", "A4"};
        int px, py, pw, ph;
        namedlg_paper(x, y, w, 0, &px, &py, &pw, &ph);
        kit_text("Paper", x + pad, kit_ty(py, ph), TK_TEXT, fx - x - pad - 8, 0);
        for (int i = 0; i < 2; i++) {
            namedlg_paper(x, y, w, i, &px, &py, &pw, &ph);
            int state = (name_pdf_paper == (unsigned)i ? KIT_ON : 0) | (name_focus == i + 1 ? KIT_FOCUS : 0);
            kit_check_row(px, py + (ph - ui_theme.check) / 2 - 1, papers[i], state | hot(px, py, pw, ph), 1);
        }
    }
    chooser_draw(&c);
    dlg_footer(x, w, c.foot_y, c.foot_h);
    int ttx, tty, ttw;
    saveas_type_rect(&c, &ttx, &tty, &ttw);
    kit_text("Type", x + pad, kit_ty(tty, BTN_H), TK_TEXT, ttx - x - pad - 8, 0);
    int switchable = saveas_family_next(name_target) != name_target;
    kit_dropdown(ttx, tty, ttw, BTN_H, saveas_types[name_target_valid() ? name_target : 0],
                 switchable ? hot(ttx, tty, ttw, BTN_H) : KIT_DISABLED);
    int sx, cx, by, bw;
    namedlg_buttons(x, y, w, h, &sx, &cx, &by, &bw);
    const char *action = name_target == 3 || name_target == 5 || pdf ? "Export" :
                         existing >= 0 && pick_ids[existing] == saveas_own_file() ? "Replace" : "Save";
    int stop = pdf ? 3 : 1;
    kit_button(cx, by, bw, BTN_H, "Cancel", -1, KB_DEFAULT, hot(cx, by, bw, BTN_H) | (name_focus == stop ? KIT_FOCUS : 0));
    kit_button(sx, by, bw, BTN_H, action, -1, KB_PRIMARY, hot(sx, by, bw, BTN_H) | (name_focus == stop + 1 ? KIT_FOCUS : 0));
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
        for (int i = 0; i < 2; i++) {
            int px, py, pw, ph;
            namedlg_paper(x, y, w, i, &px, &py, &pw, &ph);
            if (hit(mouse_x, mouse_y, px, py, pw, ph)) {
                name_pdf_paper = (unsigned)i; name_focus = i + 1; name_failed = 0; dirty = 1; return 1;
            }
        }
    }
    int fx, fy, fw, fh;
    namedlg_field(x, y, w, &fx, &fy, &fw, &fh);
    if (hit(mouse_x, mouse_y, fx, fy, fw, fh)) { name_focus = 0; name_sel = 0; dirty = 1; return 1; }
    if (hit(mouse_x, mouse_y, sx, by, bw, BTN_H))
        namedlg_commit();
    else if (hit(mouse_x, mouse_y, cx, by, bw, BTN_H))
        namedlg_close();
    else if (saveas_click(x, y, w, h))
        namedlg_close();
    return 1;
}

/* Typing replaces a selected name stem; Backspace removes it. */
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
    else if (key_sc == KEY_LEFT || key_sc == KEY_RIGHT) {
        name_sel = 0;
        dirty = 1;
    } else if (key_sc == KEY_BACKSPACE) {
        if (name_sel > 0) {
            for (int i = name_sel; i <= name_len; i++) name_buf[i - name_sel] = name_buf[i];
            name_len -= name_sel;
            name_sel = 0;
        } else if (name_len > 0)
            name_buf[--name_len] = 0;
        dirty = 1;
    } else if (key_char && key_char != '/' && name_len < FS_NAME_LEN - 1) {
        if (name_sel > 0) {
            for (int i = name_sel; i <= name_len; i++) name_buf[i - name_sel + 1] = name_buf[i];
            name_buf[0] = key_char;
            name_len -= name_sel - 1;
            name_sel = 0;
        } else {
            name_buf[name_len++] = key_char;
            name_buf[name_len] = 0;
        }
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
            if (mines_click(w->x + 1, w->y + TITLE_H, w->w - 2, w->h - TITLE_H - 1, mouse_x, mouse_y, 1)) dirty = 1;
            return;
        }
        if (w->kind == WK_FILES && mouse_y >= w->y + TITLE_H) {
            int row = files_item_at(w->x, w->y, w->w, w->h);
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
    else if (w->kind == WK_PROPERTIES)
        handle_properties_click(w->x, w->y, w->w, w->h);
    else if (w->kind == WK_PAINT)
        handle_paint_click(w->x, w->y, w->w, w->h);
    else if (w->kind == WK_SNAKE)
        handle_snake_click(w->x, w->y, w->w, w->h);
    else if (w->kind == WK_WORDLE) {
        if (wordle_click(w->x + 1, w->y + TITLE_H, w->w - 2, w->h - TITLE_H - 1, mouse_x, mouse_y, frame_count)) dirty = 1;
    } else if (w->kind == WK_VIEW) {
        int cx, cy, cw, ch;
        app_client(w->x, w->y, w->w, w->h, 1, &cx, &cy, &cw, &ch);
        if (image_viewer_click(cx, cy, cw, ch, mouse_x, mouse_y)) dirty = 1;
    } else if (w->kind == WK_BROWSER) {
        int cx, cy, cw, ch;
        app_client(w->x, w->y, w->w, w->h, 1, &cx, &cy, &cw, &ch);
        if (browser_click(cx, cy, cw, ch, mouse_x, mouse_y)) dirty = 1;
    } else if (w->kind == WK_SPREADSHEET) {
        int cx, cy, cw, ch;
        doc_client(w, &cx, &cy, &cw, &ch);
        spreadsheet_result(spreadsheet_click(cx, cy, cw, ch,
            mouse_x, mouse_y, (ctrl_down ? SPREADSHEET_MOD_CTRL : 0) | (shift_down ? SPREADSHEET_MOD_SHIFT : 0)));
    } else if (w->kind == WK_WRITER) {
        int cx, cy, cw, ch;
        doc_client(w, &cx, &cy, &cw, &ch);
        writer_result(writer_click(cx, cy, cw, ch,
            mouse_x, mouse_y, (ctrl_down ? WRITER_MOD_CTRL : 0) | (shift_down ? WRITER_MOD_SHIFT : 0)));
    } else if (w->kind == WK_PLAYER) {
        int cx, cy, cw, ch;
        app_client(w->x, w->y, w->w, w->h, 0, &cx, &cy, &cw, &ch);
        if (player_click(cx, cy, cw, ch, mouse_x, mouse_y)) dirty = 1;
    } else if (w->kind == WK_TODO)
        handle_todo_click(w->x, w->y, w->w, w->h);
    else if (w->kind == WK_CAL) {
        if (cal_click(w->x + 1, w->y + TITLE_H, w->w - 2, w->h - TITLE_H - 1, mouse_x, mouse_y)) dirty = 1;
    } else if (w->kind == WK_MINES) {
        if (mines_click(w->x + 1, w->y + TITLE_H, w->w - 2, w->h - TITLE_H - 1, mouse_x, mouse_y, shift_down)) dirty = 1;
    } else if (w->kind == WK_2048) {
        if (g2048_click(w->x + 1, w->y + TITLE_H, w->w - 2, w->h - TITLE_H - 1, mouse_x, mouse_y)) dirty = 1;
    } else if (w->kind == WK_BREAKOUT) {
        if (bo_click(w->x + 1, w->y + TITLE_H, w->w - 2, w->h - TITLE_H - 1, mouse_x, mouse_y)) dirty = 1;
    } else if (w->kind == WK_SYSMON) {
        SysInfo si;
        int cx, cy, cw, ch;
        sysinfo_fill(&si);
        app_client(w->x, w->y, w->w, w->h, 1, &cx, &cy, &cw, &ch);
        SysmonAction action = sysmon_click(cx, cy, cw, ch, mouse_x, mouse_y, &si);
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
    if(amount&&(open_dlg||name_dlg)&&!edit_close_dlg&&!display_pending){pick_scroll(amount*3,1);return;}
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
    else if(w->kind==WK_SETTINGS)settings_scroll_by(amount*48);
    else if(w->kind==WK_EDIT){
        int ax,ay,aw,ah,row,col,old=edit_caret;edit_area(w->x,w->y,w->w,w->h,&ax,&ay,&aw,&ah);
        int cols=aw/EDIT_CHAR_W;if(cols<1)cols=1;edit_caret=edit_len;edit_caret_cell(cols,&row,&col);edit_caret=old;
        int max=row+1-ah/EDIT_LINE_H;if(max<0)max=0;
        edit_scroll+=amount*3;if(edit_scroll<0)edit_scroll=0;if(edit_scroll>max)edit_scroll=max;edit_manual_scroll=1;
    }else if(w->kind==WK_FILES){int cols=files_cols(w->w);fm_first+=amount*(fm_details?3:cols);fm_manual_scroll=1;files_scroll(files_rows(w->y,w->h),cols);}
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
        else if (front_kind() == WK_EDIT && act_enabled(ACT_DOC_PROPERTIES)) act_run(ACT_DOC_PROPERTIES);
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
        if(key_sc==KEY_N && shift_down && front_kind()==WK_EDIT){act_run(ACT_NEW_WINDOW);return;}
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
        open_dlg_key();
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
        /* Arrows move through rows, or across and down the icon grid. */
        int step = key_sc == KEY_UP || key_sc == KEY_DOWN ? (fm_details ? 1 : files_cols(wins[context_slot].w)) :
                   !fm_details && (key_sc == KEY_LEFT || key_sc == KEY_RIGHT) ? 1 : 0;
        if (step) {
            int dir = key_sc == KEY_UP || key_sc == KEY_LEFT ? -1 : 1;
            if (vis) {
                if (fm_selected < 0) fm_selected = dir < 0 ? vis - 1 : 0;
                else if (fm_selected + dir * step >= 0 && fm_selected + dir * step < vis) fm_selected += dir * step;
                else if (step == 1) fm_selected = (fm_selected + dir + vis) % vis;
            }
            fm_manual_scroll = 0;
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
    if(fk==WK_PROPERTIES){ properties_key(); return; }
    if (fk == WK_CALC) {
        /* Keys mirror the buttons; Esc still dismisses the window. */
        if (key_sc == KEY_ESC) { close_front(); return; }
        char ch = 0;
        if (key_sc == KEY_ENTER) ch = '=';
        else if (key_sc == KEY_BACKSPACE) ch = 'B';
        else if (key_sc == 0x53) ch = 'E';
        else if (!ctrl_down && !alt_down && key_char) {
            char k = key_char;
            if ((k >= '0' && k <= '9') || k == '.' || k == '+' || k == '-' || k == '*' || k == '/' || k == '=') ch = k;
            else if (k == ',') ch = '.';
            else if (k == 'c' || k == 'C') ch = 'C';
        }
        if (ch) { calc_press(ch); dirty = 1; }
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
        if (key_sc == KEY_ENTER && wordle_over()) {
            wordle_new_game(frame_count);
            dirty = 1;
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
    SZ_SAVER, SZ_SAVER_DELAY, SZ_SAVER_PREVIEW, SZ_DISPLAY, SZ_CLOCK, SZ_MOUSE, SZ_RETRY, SZ_SCROLL
};
typedef struct { int x, y, w, h, kind, arg; } SetZone;
static SetZone set_zones[64];
static int set_zone_n, set_slider_drag;   /* 1 the snap slider, 2 the scroll thumb */
static int set_snap_x, set_snap_w;

/* The page scrolls inside the view [set_view_t, set_view_b). Zones drawn
 * while the page is clipped are cut to the view, so a control scrolled out
 * of sight takes no clicks. A key that changes a control asks for it to be
 * revealed: the next draw scrolls until its zone is inside the view. */
static int settings_scroll, settings_scroll_max, settings_view_h;
static int set_view_on, set_view_t, set_view_b;
static int set_reveal_kind, set_reveal_arg, set_reveal_y, set_reveal_h;
static int set_sb_x, set_sb_y, set_sb_h, set_sb_total, set_scroll_grab;
#define SET_NO_REVEAL (-0x40000000)
/* Hover reads the zones of the previous draw, since a control asks for its
 * hover state before it records its zone. */
static SetZone set_prev_zones[64];
static int set_prev_n;

static void set_zone(int x, int y, int w, int h, int kind, int arg) {
    if (set_view_on) {
        if (kind == set_reveal_kind && arg == set_reveal_arg) { set_reveal_y = y; set_reveal_h = h; }
        if (y < set_view_t) { h -= set_view_t - y; y = set_view_t; }
        if (y + h > set_view_b) h = set_view_b - y;
        if (h <= 0) return;
    }
    if (set_zone_n < (int)(sizeof set_zones / sizeof set_zones[0]))
        set_zones[set_zone_n++] = (SetZone){x, y, w, h, kind, arg};
}
static const SetZone *set_zone_at(int px, int py) {
    for (int i = set_zone_n - 1; i >= 0; i--)
        if (hit(px, py, set_zones[i].x, set_zones[i].y, set_zones[i].w, set_zones[i].h)) return &set_zones[i];
    return 0;
}
static int set_hover(int kind, int arg) {
    for (int i = set_prev_n - 1; i >= 0; i--) {
        const SetZone *z = &set_prev_zones[i];
        if (hit(mouse_x, mouse_y, z->x, z->y, z->w, z->h)) return z->kind == kind && z->arg == arg;
    }
    return 0;
}

static int settings_side_w(void) { return MODERN ? 200 : 192; }
static int settings_window_h(void) { return MODERN ? 588 : 580; }

/* Form layout: a label column and a control column. Classic rows sit on
 * the content with a rule between sections; Modern groups rows in cards. */
typedef struct { int x, y, w, card_y, rows; } SetForm;
static int set_label_w(void) { return MODERN ? 150 : 152; }

/* Word-wrapped help text. Draws when draw is set and returns the number of
 * lines; a word wider than w is cut by kit_text. */
#define SET_LINE_H 16
static int set_wrap(const char *text, int x, int y, int w, uint8_t color, int draw) {
    int lines = 0;
    while (*text) {
        char line[160];
        int n = 0, fit = 0;
        while (text[n] && n < (int)sizeof line - 1) {
            line[n] = text[n];
            line[n + 1] = 0;
            if (n && ui_string_w(line) > w) break;
            n++;
            if (!text[n] || text[n] == ' ') fit = n;
        }
        if (!text[n]) fit = n;
        if (!fit) fit = n;
        kmemcpy(line, text, fit);
        line[fit] = 0;
        if (draw) kit_text(line, x, y + lines * SET_LINE_H, color, w, 0);
        lines++;
        text += fit;
        while (*text == ' ') text++;
    }
    return lines;
}

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
    int hint_x = MODERN ? f->x + 16 : f->x + set_label_w() + 16 + ui_theme.check + 8;
    int hint_w = MODERN ? f->w - 80 : f->x + f->w - hint_x;
    int lines = hint ? set_wrap(hint, hint_x, 0, hint_w, TK_TEXT2, 0) : 0;
    int cx, h = hint ? 22 + lines * SET_LINE_H + 2 : (MODERN ? 20 : 24);
    int y = set_row(f, 0, h, &cx);
    if (MODERN) {
        int lx = f->x + 16;
        kit_text(label, lx, hint ? y + 2 : kit_ty(y, h), TK_TEXT, f->w - 80, 0);
        if (hint) set_wrap(hint, lx, y + 22, hint_w, TK_TEXT2, 1);
        int tx = f->x + f->w - 16 - KIT_TOGGLE_W, ty = y + (h - KIT_TOGGLE_H) / 2;
        kit_toggle(tx, ty, (on ? KIT_ON : 0) | (set_hover(kind, arg) ? KIT_HOVER : 0));
        set_zone(f->x, y - 8, f->w, h + 16, kind, arg);
    } else {
        int w = kit_check_row(cx, y, label, (on ? KIT_ON : 0) | (set_hover(kind, arg) ? KIT_HOVER : 0), 0);
        if (hint) set_wrap(hint, hint_x, y + 24, hint_w, TK_TEXT2, 1);
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
    kit_slider(sx, y, sw, value, max, set_slider_drag == 1 ? KIT_PRESSED : 0);
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
/* Each row: the keys (a second key, when set, is an alternative) and what
 * they do. */
static const char *const shortcut_keys[][3] = {
    {"Super", "Apps", "Ctrl+Space"}, {"Alt+Tab", "Switch windows"},
    {"Alt+F4", "Close window"}, {"Super+Arrow", "Snap or maximize"}, {"Super+D", "Show desktop"},
    {"Alt or F10", "Menu row"}, {"Alt+Space", "Window menu"},
};

static void settings_input(SetForm *f) {
    int cx, y;
    set_heading(f, "Keyboard and Mouse", "These shortcuts work in every app.");
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
        if (shortcut_keys[i][2]) {
            int ox = cx + kit_kbd_w(shortcut_keys[i][0]) + 8;
            kit_text("or", ox, kit_ty(y, MODERN ? 20 : 19), TK_TEXT2, 40, 0);
            kit_kbd(ox + kit_text_w("or", 0) + 8, y, shortcut_keys[i][2]);
        }
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
    {
        RtcTime now;
        rtc_read(&now);
        char longdate[48], n[8];
        kstrcpy(longdate, rtc_day_name[now.wday]);
        kstrcpy(longdate + kstrlen(longdate), ", ");
        fmt_uint(n, (unsigned)now.day);
        kstrcpy(longdate + kstrlen(longdate), n);
        kstrcpy(longdate + kstrlen(longdate), " ");
        kstrcpy(longdate + kstrlen(longdate), rtc_month_name[now.month - 1]);
        kstrcpy(longdate + kstrlen(longdate), " ");
        fmt_uint(n, (unsigned)now.year);
        kstrcpy(longdate + kstrlen(longdate), n);
        draw_string(longdate, f->x, f->y, TK_TEXT2);
    }
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
    kmemcpy(set_prev_zones, set_zones, sizeof set_zones);
    set_prev_n = set_zone_n;
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
    /* Page: drawn clipped to the view at the scroll offset. The page's
     * height is known only after drawing it, so a scroll that lands out of
     * range, or a reveal request, draws the page a second time. */
    int cx = wx + 1 + side + 1, pad = MODERN ? 20 : 24, top_pad = MODERN ? 20 : 28;
    int page_r = wx + ww - 1, failed = settings_failed_pref();
    int view_b = failed >= 0 ? bottom - 16 - BTN_H - 12 : bottom;
    int view_h = view_b - top, zones = set_zone_n, total = 0;
    SetForm f = {0};
    if (view_h < 1) view_h = 1;
    settings_view_h = view_h;
    for (int pass = 0; pass < 2; pass++) {
        set_zone_n = zones;
        draw_rect(cx, top, page_r - cx, view_h, ui_theme.content);
        f = (SetForm){cx + pad, top + top_pad - settings_scroll, page_r - (MODERN ? 0 : SB) - cx - 2 * pad, 0, 0};
        set_view_on = 1;
        set_view_t = top;
        set_view_b = view_b;
        set_reveal_y = SET_NO_REVEAL;
        gfx_clip(cx, top, page_r - (MODERN ? 0 : SB) - cx, view_h);
        switch (settings_page) {
        case SET_APPEARANCE: settings_appearance(&f); break;
        case SET_DESKTOP: settings_desktop(&f); break;
        case SET_DISPLAY: settings_display(&f); break;
        case SET_INPUT: settings_input(&f); break;
        case SET_TIME: settings_time(&f); break;
        default: settings_about(&f); break;
        }
        gfx_unclip();
        set_view_on = 0;
        total = f.y + settings_scroll - top + (MODERN ? 20 : 24);
        settings_scroll_max = total > view_h ? total - view_h : 0;
        int want = settings_scroll;
        if (set_reveal_kind && set_reveal_y != SET_NO_REVEAL) {
            int ry = set_reveal_y + settings_scroll - top, margin = 12;
            if (ry - margin < want) want = ry - margin;
            if (ry + set_reveal_h + margin > want + view_h) want = ry + set_reveal_h + margin - view_h;
        }
        set_reveal_kind = 0;
        if (want > settings_scroll_max) want = settings_scroll_max;
        if (want < 0) want = 0;
        if (want == settings_scroll) break;
        settings_scroll = want;
    }
    /* The scrollbar: Classic keeps its track in the reserved column; Modern
     * overlays a thin thumb at the page's right edge while the page overflows. */
    set_sb_x = page_r - SB;
    set_sb_y = top;
    set_sb_h = view_h;
    set_sb_total = total;
    {
        int sb_state = set_slider_drag == 2 ? KIT_PRESSED : set_hover(SZ_SCROLL, 0) ? KIT_HOVER : 0;
        if (!MODERN) draw_rect(set_sb_x, top, SB, bottom - top, TK_BASE);
        kit_vscroll(set_sb_x, top, view_h, settings_scroll, view_h, total, sb_state);
        if (!MODERN) draw_vline(set_sb_x, top, bottom - top, TK_LINE_SUBTLE);
        if (total > view_h) set_zone(set_sb_x, top, SB, view_h, SZ_SCROLL, 0);
    }
    if (failed >= 0) {
        int by = bottom - 16 - BTN_H, bw = kit_button_w("Retry saving", -1);
        draw_rect(cx, view_b, page_r - (MODERN ? 0 : SB) - cx, bottom - view_b, ui_theme.content);
        draw_hline(cx, view_b, page_r - (MODERN ? 0 : SB) - cx, TK_LINE_SUBTLE);
        kit_text(preference_message(failed), f.x, kit_ty(by, BTN_H), TK_RED_700, f.w - bw - 12, 0);
        kit_button(f.x + f.w - bw, by, bw, BTN_H, "Retry saving", -1, KB_DEFAULT, set_hover(SZ_RETRY, 0) ? KIT_HOVER : 0);
        set_zone(f.x + f.w - bw, by, bw, BTN_H, SZ_RETRY, 0);
    }
}

/* Scrolls the page by dy pixels, kept inside the range of the last draw. */
static void settings_scroll_by(int dy) {
    int v = settings_scroll + dy;
    if (v > settings_scroll_max) v = settings_scroll_max;
    if (v < 0) v = 0;
    if (v != settings_scroll) { settings_scroll = v; dirty = 1; }
}
static void settings_page_set(int page) {
    if (page != settings_page) settings_scroll = 0;
    settings_page = page;
}
/* Scroll thumb geometry, as kit_vscroll draws it. */
static void settings_thumb(int *at, int *size) {
    kit_scroll_thumb(set_sb_h - 4, settings_scroll, set_sb_h, set_sb_total, at, size);
    *at += set_sb_y + 2;
}
static void settings_scroll_drag(void) {
    int at, size, range = set_sb_h - 4;
    settings_thumb(&at, &size);
    if (range <= size) return;
    int y = mouse_y - set_scroll_grab - (set_sb_y + 2);
    settings_scroll_by(y * settings_scroll_max / (range - size) - settings_scroll);
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
    if (set_slider_drag == 2) {
        if (mouse_left) settings_scroll_drag();
        else set_slider_drag = 0;
        dirty = 1;
        return;
    }
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
    case SZ_PAGE: settings_page_set(z->arg); break;
    case SZ_SCROLL: {
        int at, size;
        settings_thumb(&at, &size);
        if (mouse_y < at) settings_scroll_by(-settings_view_h * 7 / 8);
        else if (mouse_y >= at + size) settings_scroll_by(settings_view_h * 7 / 8);
        else { set_slider_drag = 2; set_scroll_grab = mouse_y - at; }
        break;
    }
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

/* Settings keys: Tab and the arrows move between pages, Page Up, Page Down,
 * Home and End scroll the page, 1-4 pick a display mode on the Display
 * page, R retries saving. A control changed from the keyboard is scrolled
 * into view. */
static void settings_key(void) {
    if (key_sc == KEY_ESC) { close_front(); return; }
    if (key_sc == KEY_TAB || key_sc == KEY_DOWN) settings_page_set((settings_page + (shift_down ? SET_PAGES - 1 : 1)) % SET_PAGES);
    else if (key_sc == KEY_UP) settings_page_set((settings_page + SET_PAGES - 1) % SET_PAGES);
    else if (key_sc == 0x49 || key_sc == 0x51) settings_scroll_by((key_sc == 0x49 ? -1 : 1) * settings_view_h * 7 / 8);
    else if (key_sc == 0x47 || key_sc == 0x4f) settings_scroll_by(key_sc == 0x47 ? -settings_scroll : settings_scroll_max);
    else if (key_char >= '1' && key_char <= '0' + DISPLAY_MODE_COUNT) {
        if (settings_page == SET_DISPLAY) { set_reveal_kind = SZ_DISPLAY; set_reveal_arg = key_char - '1'; }
        display_request(key_char - '1');
        dirty = 1;
        return;
    }
    else if (key_char == 'r' || key_char == 'R') preferences_retry();
    else if (key_sc == KEY_SPACE && settings_page == SET_DESKTOP) {
        saver_enabled = !saver_enabled;
        saver_save();
        set_reveal_kind = SZ_SAVER;
        set_reveal_arg = 0;
    }
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
                if(draft>=0&&!fs_is_dir(draft)&&!fs_is_app(draft)&&!fs_streamed(draft)){
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
                if(draft>=0&&!fs_is_dir(draft)&&!fs_is_app(draft)&&!fs_streamed(draft)){
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
                hot_n = 0;
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
/* Keep or revert a new resolution, as an alert with a countdown. */
static void display_confirm_buttons(int x, int y, int *kx, int *rx, int *by, int *kw, int *rw) {
    *kw = kit_button_w("Keep changes", -1);
    *rw = kit_button_w("Revert", -1);
    *kx = x + alert_w() - 16 - *kw;
    *rx = *kx - 8 - *rw;
    alert_buttons_y(y, by);
}
static void draw_display_confirmation(void) {
    if (!display_pending) return;
    int x = (fb_w - alert_w()) / 2, y = (fb_h - alert_h()) / 2;
    char seconds[16], message[80];
    unsigned ticks_left = display_deadline - timer_ticks();
    fmt_uint(seconds, (ticks_left + TIMER_HZ - 1) / TIMER_HZ);
    kstrcpy(message, "The previous resolution returns in ");
    kstrcpy(message + kstrlen(message), seconds);
    kstrcpy(message + kstrlen(message), " seconds.");
    draw_alert(x, y, alert_w(), alert_h(), "Display", app_icon_for(ICON_APP_SETTINGS), UI_ICON_A_WARN,
               "Keep this display resolution?", message);
    int kx, rx, by, kw, rw;
    display_confirm_buttons(x, y, &kx, &rx, &by, &kw, &rw);
    kit_button(rx, by, rw, BTN_H, "Revert", -1, KB_DEFAULT, hot(rx, by, rw, BTN_H));
    kit_button(kx, by, kw, BTN_H, "Keep changes", -1, KB_PRIMARY, hot(kx, by, kw, BTN_H) | KIT_FOCUS);
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
    int x = (fb_w - alert_w()) / 2, y = (fb_h - alert_h()) / 2, kx, rx, by, kw, rw;
    display_confirm_buttons(x, y, &kx, &rx, &by, &kw, &rw);
    if (hit(mouse_x, mouse_y, kx, by, kw, BTN_H)) display_keep();
    else if (hit(mouse_x, mouse_y, rx, by, rw, BTN_H) ||
             hit(mouse_x, mouse_y, close_box_x(x, alert_w()), cap_y(y), CAP_CLOSE_W, CAP_H)) display_revert();
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
            int cx, cy, cw, ch;
            doc_client(w, &cx, &cy, &cw, &ch);
            if (writer_drag(cx, cy, cw, ch, mouse_x, mouse_y)) dirty = 1;
        }
        if (front_kind() == WK_SPREADSHEET && mouse_left && mouse_moved && !open_dlg &&
            !name_dlg && !edit_close_dlg && !launcher_on && open_menu < 0 && dragging_win < 0) {
            Win *w = &wins[win_front()];
            int cx, cy, cw, ch;
            doc_client(w, &cx, &cy, &cw, &ch);
            if (spreadsheet_drag(cx, cy, cw, ch, mouse_x, mouse_y)) dirty = 1;
        }
        if (paint_dragging || paint_shape_drag) paint_drag_tick();
        if (mouse_moved) paint_hover_tick();
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
    if(mouse_moved && dragging_win<0 && resizing_win<0){int h=hot_index();if(h!=hot_last){hot_last=h;dirty=1;}}
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
                int cx,cy,cw,ch;
                context_set(player_slot);
                app_client(w->x,w->y,w->w,w->h,0,&cx,&cy,&cw,&ch);
                if(player_update==PLAYER_AUDIO_PROGRESS)
                    player_draw_audio_progress(cx,cy,cw,ch);
                else player_draw_playback(cx,cy,cw,ch);
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
