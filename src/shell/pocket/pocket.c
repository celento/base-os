/* Pocket: a small phone-style shell.
 *
 * Pocket is the second implementation of the kernel/shell contract in
 * kernel/shell.h. It runs on the same kernel as the desktop and hosts the same
 * app components (Clock, 2048, Mines, Wordle, Breakout and the Todo model)
 * full screen inside a portrait viewport, plus its own Files, Editor and
 * Settings screens. A home screen with search replaces windows; a bottom bar
 * has Back, Home and Apps (the open-apps list), and each app's actions live
 * in a menu sheet. It draws with the shared control kit in the Classic or
 * Modern style, read from the same preferences as the desktop. It uses only
 * public kernel headers and never touches desktop code. Build it with
 * `make pocket`; see docs/ARCHITECTURE.md. */
#include "kernel/kernel.h"
#include "kernel/shell.h"
#include "kernel/gfx.h"
#include "kernel/font.h"
#include "kernel/fs.h"
#include "kernel/platform.h"
#include "kernel/rtc.h"
#include "kernel/layout.h"
#include "ui/app.h"
#include "ui/app_icons.h"
#include "ui/icon_draw.h"
#include "ui/ui_icons.h"
#include "ui/theme.h"
#include "ui/kit.h"
#include "lib/provision.h"
#include "lib/text_search.h"
#include "apps/clock.h"
#include "apps/game2048.h"
#include "apps/mines.h"
#include "apps/wordle.h"
#include "apps/breakout.h"
#include "apps/todo.h"
#include "assets/cursor_art.h"
#include "build_info.h"

#define MODERN (ui_theme.style == UI_STYLE_MODERN)
#define CHAR_H UI_FONT_H
#define STATUS_H 30
#define HEADER_H 48
#define NAV_H 52
#define ROW_H 44
#define PAD 16
#define EDIT_LINE_H 17
#define HOME -1
#define SWITCHER -2

enum { APP_FILES, APP_EDITOR, APP_SETTINGS, APP_CLOCK, APP_TODO, APP_2048, APP_MINES, APP_WORDLE,
       APP_BREAKOUT, APP_ABOUT, APP_COUNT };
typedef struct { const char *name; int icon; int width, height; } PocketApp;
/* Fixed-size components keep their own geometry; zero means "fill the screen". */
static const PocketApp apps[APP_COUNT] = {
    {"Files", AI_FILES, 0, 0},
    {"Editor", AI_EDITOR, 0, 0},
    {"Settings", AI_SETTINGS, 0, 0},
    {"Clock", AI_CLOCK, CLOCK_W, CLOCK_H},
    {"Todo", AI_TODO, 0, 0},
    {"2048", AI_G2048, G2048_W, G2048_H},
    {"Mines", AI_MINES, MINES_W, MINES_H},
    {"Wordle", AI_WORDLE, WORDLE_W, WORDLE_WIN_H - 54},
    {"Breakout", AI_BREAKOUT, BO_W, BO_H},
    {"About", AI_ABOUT, 0, 0},
};

static int phone_x, phone_y, phone_w, phone_h;
static int current = HOME, selected;
static int dirty = 1, ready;
static int pocket_mouse_x, pocket_mouse_y, pointer_down;
static unsigned modifiers;
static unsigned last_minute = ~0u, last_second = ~0u, last_tick;
static uint8_t cursor_saved[CURSOR_ART_W * CURSOR_ART_H];
static int cursor_x = -1, cursor_y = -1, cursor_shown;
static const char *notice = "";
static unsigned notice_until;
/* Seed buffer for first-boot sample files; the desktop's editor arena is free
 * because the desktop is not linked into this image. After provisioning the
 * same arena holds the Editor's text. */
static char *const seed_scratch = (char *)EDITOR_BASE;

/* Open apps, most recent first. */
static int open_order[APP_COUNT], open_n;
/* The screen Back returns to from each app. */
static int back_to[APP_COUNT];

/* Home search. */
static char search[48];
static int search_len;

/* Exported grid geometry for tools/pocket_test.py. */
static int pocket_grid_x, pocket_grid_y, pocket_cell_w, pocket_cell_h, pocket_columns;

static void notify(const char *text) {
    notice = text;
    notice_until = timer_ticks() + TIMER_HZ * 3;
    dirty = 1;
}

/* ---------- Hover ----------
 * Drawing records rectangles; pointer motion compares the rectangle under
 * the pointer with the last one and redraws on a change. */
#define HOT_MAX 64
typedef struct { int x, y, w, h; } Rect;
static Rect hots[HOT_MAX];
static int hot_n, hot_last = -1;
static int inside(int x, int y, int w, int h) {
    return pocket_mouse_x >= x && pocket_mouse_x < x + w && pocket_mouse_y >= y && pocket_mouse_y < y + h;
}
static int hot(int x, int y, int w, int h) {
    if (hot_n < HOT_MAX) hots[hot_n++] = (Rect){x, y, w, h};
    if (!inside(x, y, w, h)) return 0;
    return pointer_down ? KIT_HOVER | KIT_PRESSED : KIT_HOVER;
}
static int hot_index(void) {
    for (int i = hot_n - 1; i >= 0; i--)
        if (inside(hots[i].x, hots[i].y, hots[i].w, hots[i].h)) return i;
    return -1;
}

/* ---------- Style preferences ----------
 * The desktop keeps one-byte files in /prefs; Pocket reads and writes the
 * same style and accent files, so both shells look alike on one disk. */
static int ui_style = UI_STYLE_CLASSIC, ui_accent;
static int pref_read(const char *name, int count) {
    int dir = fs_find_child(fs_root(), "prefs");
    if (dir < 0 || !fs_is_dir(dir)) return -1;
    int id = fs_find_child(dir, name);
    if (id < 0 || fs_is_dir(id) || fs_is_app(id) || fs_size(id) != 1) return -1;
    char v = fs_data(id)[0];
    return v >= '0' && v < '0' + count ? v - '0' : -1;
}
static int pref_write(const char *name, int value) {
    int dir = fs_find_child(fs_root(), "prefs");
    if (dir >= 0 && !fs_is_dir(dir)) return -1;
    if (dir < 0 && (dir = fs_mkdir(fs_root(), "prefs")) < 0) return dir;
    int id = fs_find_child(dir, name);
    if (id >= 0 && (fs_is_dir(id) || fs_is_app(id))) return -1;
    if (id < 0 && (id = fs_create(dir, name)) < 0) return id;
    char v = (char)('0' + value);
    return fs_write(id, &v, 1);
}
static void pocket_theme(void) {
    ui_theme_install(ui_style, ui_accent, 0);
}
static void set_style(int style, int accent) {
    if (style == ui_style && accent == ui_accent) return;
    int failed = 0;
    if (style != ui_style && pref_write("style", style) < 0) failed = 1;
    if (accent != ui_accent && pref_write("accent", accent) < 0) failed = 1;
    ui_style = style;
    ui_accent = accent;
    pocket_theme();
    if (failed) notify("Not saved: storage is busy or full.");
    dirty = 1;
}

/* ---------- Layout ---------- */

static void pocket_layout(void) {
    phone_h = fb_h - 40;
    if (phone_h > 900) phone_h = 900;
    phone_w = phone_h * 9 / 16;
    if (phone_w < BO_W + 24) phone_w = BO_W + 24;
    if (phone_w > fb_w - 16) phone_w = fb_w - 16;
    phone_x = (fb_w - phone_w) / 2;
    phone_y = (fb_h - phone_h) / 2;
}

/* Screen area below the status bar and above the bottom bar. */
static void screen_rect(int *x, int *y, int *w, int *h) {
    *x = phone_x;
    *y = phone_y + STATUS_H;
    *w = phone_w;
    *h = phone_h - STATUS_H - NAV_H;
}
static int body_top(void) { return phone_y + STATUS_H + HEADER_H; }
static int body_bottom(void) { return phone_y + phone_h - NAV_H; }

/* Origin of a fixed-size component centered in the app area. */
static void app_origin(int app, int *x, int *y, int *w, int *h) {
    int sx, sy, sw, sh;
    screen_rect(&sx, &sy, &sw, &sh);
    sy += HEADER_H;
    sh -= HEADER_H;
    *w = apps[app].width ? apps[app].width : sw;
    *h = apps[app].height ? apps[app].height : sh;
    *x = sx + (sw - *w) / 2;
    *y = sy + (sh - *h) / 2;
    if (*y < sy) *y = sy;
}

static void two_digits(char *out, int value) {
    out[0] = (char)('0' + value / 10 % 10);
    out[1] = (char)('0' + value % 10);
}
static void append(char *out, const char *s) { kstrcpy(out + kstrlen(out), s); }
static void append_uint(char *out, unsigned v) { fmt_uint(out + kstrlen(out), v); }
static int has_text(const char *s, const char *q) {
    for (int i = 0; s[i]; i++) {
        int k = 0;
        while (q[k] && s[i + k]) {
            char a = s[i + k], b = q[k];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) break;
            k++;
        }
        if (!q[k]) return 1;
    }
    return !q[0];
}

/* "4 KB", "512 bytes". */
static void size_text(char *out, int size) {
    out[0] = 0;
    if (size < 1024) { append_uint(out, (unsigned)size); append(out, size == 1 ? " byte" : " bytes"); }
    else if (size < 1024 * 1024) { append_uint(out, (unsigned)((size + 1023) / 1024)); append(out, " KB"); }
    else { append_uint(out, (unsigned)(size / (1024 * 1024))); append(out, " MB"); }
}
/* Days since 2000-01-01 for a civil date. */
static long days_2000(int y, int m, int d) {
    long n = 0;
    for (int k = 2000; k < y; k++) n += (k % 4 == 0 && (k % 100 || k % 400 == 0)) ? 366 : 365;
    for (int k = 1; k < m; k++) n += rtc_days_in_month(y, k);
    return n + d - 1;
}
/* fs_modified seconds since 2000 as "Today", "Yesterday" or "18 Oct". */
static void stamp_text(char *out, unsigned stamp) {
    static const char *const mon[12] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    out[0] = 0;
    if (!stamp) return;
    RtcTime now;
    rtc_read(&now);
    long today = days_2000(now.year, now.month, now.day), day = (long)(stamp / 86400u);
    if (day == today) { kstrcpy(out, "Today"); return; }
    if (day == today - 1) { kstrcpy(out, "Yesterday"); return; }
    int y = 2000, m = 1;
    long left = day;
    for (;;) { int n = (y % 4 == 0 && (y % 100 || y % 400 == 0)) ? 366 : 365; if (left < n) break; left -= n; y++; }
    while (m < 12 && left >= rtc_days_in_month(y, m)) { left -= rtc_days_in_month(y, m); m++; }
    append_uint(out, (unsigned)(left + 1));
    append(out, " ");
    append(out, mon[m - 1]);
    if (y != now.year) { append(out, " "); append_uint(out, (unsigned)y); }
}

/* An ordinary user file or folder: no apps, no system folders at the root. */
static int user_node(int id) {
    if (!fs_valid(id) || fs_is_app(id)) return 0;
    if (fs_parent(id) == fs_root()) {
        const char *n = fs_name(id);
        if (!kstrcmp(n, "prefs") || !kstrcmp(n, "trash") || !kstrcmp(n, "Trash")) return 0;
    }
    return 1;
}
static int node_icon(int id) {
    if (fs_is_dir(id)) return UI_ICON_FOLDER;
    const char *n = fs_name(id);
    int dot = -1;
    for (int i = 0; n[i]; i++) if (n[i] == '.') dot = i;
    const char *e = dot >= 0 ? n + dot + 1 : "";
    if (!kstrcmp(e, "bmp") || !kstrcmp(e, "pbm") || !kstrcmp(e, "png") || !kstrcmp(e, "jpg")) return UI_ICON_IMG;
    if (!kstrcmp(e, "c") || !kstrcmp(e, "h") || !kstrcmp(e, "bas") || !kstrcmp(e, "bsh") || !kstrcmp(e, "html")) return UI_ICON_CODE;
    return UI_ICON_DOC;
}
static uint8_t node_ink(int icon) {
    return icon == UI_ICON_FOLDER ? TK_AMBER : icon == UI_ICON_IMG ? TK_GREEN : icon == UI_ICON_CODE ? TK_ACCENT : TK_ICON;
}
static void folder_name(int id, char *out) {
    int p = fs_parent(id);
    kstrcpy(out, p == fs_root() || p < 0 ? "Home" : fs_name(p));
}

/* ---------- Files model ---------- */
#define FILES_MAX 256
static int files_cwd = -1, files_ids[FILES_MAX], files_n, files_first, files_by_date, files_sel = -1;
static void files_refresh(void) {
    if (files_cwd < 0 || !fs_is_dir(files_cwd)) files_cwd = fs_root();
    int ids[FILES_MAX], n = fs_list(files_cwd, ids, FILES_MAX);
    files_n = 0;
    for (int i = 0; i < n; i++) if (user_node(ids[i])) files_ids[files_n++] = ids[i];
    /* Folders first, then by name or newest first. */
    for (int i = 1; i < files_n; i++) {
        int v = files_ids[i], j = i - 1;
        while (j >= 0) {
            int a = files_ids[j], before;
            if (fs_is_dir(a) != fs_is_dir(v)) before = fs_is_dir(v);
            else if (files_by_date) before = fs_modified(v) > fs_modified(a);
            else {
                const char *x = fs_name(v), *y = fs_name(a);
                int k = 0;
                while (x[k] && (x[k] | 32) == (y[k] | 32)) k++;
                before = (x[k] | 32) < (y[k] | 32);
            }
            if (!before) break;
            files_ids[j + 1] = a;
            j--;
        }
        files_ids[j + 1] = v;
    }
    if (files_sel >= files_n) files_sel = files_n - 1;
}
static int files_rows(void) { return (body_bottom() - body_top() - (MODERN ? 24 : 0) - 32) / ROW_H; }

/* ---------- Editor model ---------- */
#define ED_CAP 65535
static char *const ed_buf = (char *)EDITOR_BASE;
static int ed_len, ed_caret, ed_top, ed_file = -1, ed_changed, ed_open;
static unsigned ed_identity;
static int find_open;
static char find_text[48];
static int find_len;

static int ed_text_like(const char *data, int n) {
    for (int i = 0; i < n; i++) if (!data[i]) return 0;
    return 1;
}
static int ed_valid(void) { return ed_file >= 0 && fs_valid(ed_file) && fs_identity(ed_file) == ed_identity; }
static const char *ed_title(void) { return ed_valid() ? fs_name(ed_file) : "untitled"; }
static int ed_load(int id) {
    int n = fs_size(id);
    if (n > ED_CAP) { notify("Too large for Editor."); return 0; }
    if (!ed_text_like(fs_data(id), n)) { notify("Pocket opens text files only."); return 0; }
    kmemcpy(ed_buf, fs_data(id), n);
    ed_buf[n] = 0;
    ed_len = n;
    ed_caret = ed_top = 0;
    ed_file = id;
    ed_identity = fs_identity(id);
    ed_changed = 0;
    ed_open = 1;
    return 1;
}
static void ed_new(void) {
    ed_len = ed_caret = ed_top = 0;
    ed_buf[0] = 0;
    ed_file = -1;
    ed_changed = 0;
    ed_open = 1;
}
static int ed_save(void) {
    if (!ed_valid()) {
        /* A new document goes to Documents, or the root when that is missing. */
        int dir = fs_find_child(fs_root(), "Documents");
        if (dir < 0 || !fs_is_dir(dir)) dir = fs_root();
        char name[FS_NAME_LEN];
        if (fs_unique_file(dir, name) < 0) { notify("Not saved: no free name."); return 0; }
        int id = fs_create(dir, name);
        if (id < 0) { notify(id == FS_ERR_BUSY ? "Storage is busy; try again." : "Not saved: storage is full."); return 0; }
        ed_file = id;
        ed_identity = fs_identity(id);
    }
    int r = fs_write(ed_file, ed_buf, ed_len);
    if (r < 0) { notify(r == FS_ERR_BUSY ? "Storage is busy; try again." : "Not saved: storage is full."); return 0; }
    ed_changed = 0;
    notify("Saved.");
    return 1;
}
static int ed_line_start(int at) { while (at > 0 && ed_buf[at - 1] != '\n') at--; return at; }
static int ed_line_of(int at) { int n = 0; for (int i = 0; i < at; i++) if (ed_buf[i] == '\n') n++; return n; }
static int ed_index_of_line(int line) {
    int i = 0;
    while (line > 0 && i < ed_len) { if (ed_buf[i] == '\n') line--; i++; }
    return i;
}
static void ed_insert(const char *s, int n) {
    if (ed_len + n > ED_CAP) { notify("The document is full."); return; }
    for (int i = ed_len; i >= ed_caret; i--) ed_buf[i + n] = ed_buf[i];
    kmemcpy(ed_buf + ed_caret, s, n);
    ed_len += n;
    ed_caret += n;
    ed_changed = 1;
}
static void ed_delete(int at) {
    if (at < 0 || at >= ed_len) return;
    for (int i = at; i < ed_len; i++) ed_buf[i] = ed_buf[i + 1];
    ed_len--;
    if (ed_caret > at) ed_caret--;
    ed_changed = 1;
}
static void ed_vertical(int delta) {
    int start = ed_line_start(ed_caret), col = ed_caret - start;
    int line = ed_line_of(ed_caret) + delta;
    if (line < 0) line = 0;
    int s = ed_index_of_line(line), e = s;
    while (e < ed_len && ed_buf[e] != '\n') e++;
    ed_caret = s + col < e ? s + col : e;
}
static int ed_rows(void) {
    return (body_bottom() - body_top() - 12 - (find_open ? 52 : 0)) / EDIT_LINE_H;
}
static void ed_follow(void) {
    int line = ed_line_of(ed_caret), rows = ed_rows();
    if (line < ed_top) ed_top = line;
    if (line >= ed_top + rows) ed_top = line - rows + 1;
    if (ed_top < 0) ed_top = 0;
}
static void ed_find_next(void) {
    if (!find_len) return;
    int at = text_find(ed_buf, ed_len, find_text, ed_caret < ed_len ? ed_caret + 1 : 0, 1, 0);
    if (at < 0) { notify("No match."); return; }
    ed_caret = at;
    ed_follow();
}

/* ---------- Open apps ---------- */
static void open_push(int app) {
    int at = open_n;
    for (int i = 0; i < open_n; i++) if (open_order[i] == app) at = i;
    if (at == open_n) open_n++;
    for (int i = at; i > 0; i--) open_order[i] = open_order[i - 1];
    open_order[0] = app;
}
static void open_remove(int app) {
    int k = 0;
    for (int i = 0; i < open_n; i++) if (open_order[i] != app) open_order[k++] = open_order[i];
    open_n = k;
}

/* ---------- Menu sheet ----------
 * Each app's actions, from the header's more button. A sheet can also ask
 * whether to save before closing the Editor. */
enum { ACT_NONE, ACT_SAVE, ACT_FIND, ACT_PROPS, ACT_NEW_DOC, ACT_CLOSE, ACT_NEW_GAME, ACT_SORT_NAME,
       ACT_SORT_DATE, ACT_SHUTDOWN, ACT_CLOSE_SAVE, ACT_CLOSE_DISCARD, ACT_CANCEL, ACT_SEP };
typedef struct { int action, icon; char label[64]; } SheetItem;
static SheetItem sheet[8];
static int sheet_n, sheet_open, sheet_sel, sheet_closing = -1;
static char sheet_title[96];
static void sheet_add(int action, int icon, const char *label) {
    if (sheet_n >= 8) return;
    sheet[sheet_n].action = action;
    sheet[sheet_n].icon = icon;
    kstrcpy(sheet[sheet_n].label, label);
    sheet_n++;
}
static int sheet_first_action(void) {
    for (int i = 0; i < sheet_n; i++) if (sheet[i].action != ACT_SEP) return i;
    return 0;
}
static void sheet_for(int app) {
    sheet_n = 0;
    sheet_closing = -1;
    kstrcpy(sheet_title, app == APP_EDITOR ? ed_title() : apps[app].name);
    if (app == APP_EDITOR) { append(sheet_title, " - Editor"); }
    char close[64];
    kstrcpy(close, "Close ");
    append(close, app == APP_EDITOR ? ed_title() : apps[app].name);
    switch (app) {
    case APP_EDITOR:
        sheet_add(ACT_SAVE, UI_ICON_SAVE, "Save");
        sheet_add(ACT_NEW_DOC, UI_ICON_NEWF, "New document");
        sheet_add(ACT_FIND, UI_ICON_SEARCH, "Find");
        sheet_add(ACT_PROPS, UI_ICON_INFO, "Properties");
        break;
    case APP_FILES:
        sheet_add(ACT_SORT_NAME, files_by_date ? -1 : UI_ICON_CHECK, "Sort by name");
        sheet_add(ACT_SORT_DATE, files_by_date ? UI_ICON_CHECK : -1, "Sort by date modified");
        break;
    case APP_2048: case APP_MINES: case APP_WORDLE: case APP_BREAKOUT:
        sheet_add(ACT_NEW_GAME, UI_ICON_RESTART, "New game");
        break;
    case APP_ABOUT:
        sheet_add(ACT_SHUTDOWN, UI_ICON_POWER, "Shut down");
        break;
    default: break;
    }
    if (sheet_n) sheet_add(ACT_SEP, -1, "");
    sheet_add(ACT_CLOSE, UI_ICON_X, close);
    sheet_open = 1;
    sheet_sel = sheet_first_action();
    dirty = 1;
}
static void sheet_ask_save(int app) {
    sheet_n = 0;
    sheet_closing = app;
    kstrcpy(sheet_title, "Save changes to ");
    append(sheet_title, ed_title());
    append(sheet_title, "?");
    sheet_add(ACT_CLOSE_SAVE, UI_ICON_SAVE, "Save");
    sheet_add(ACT_CLOSE_DISCARD, UI_ICON_TRASH, "Don't save");
    sheet_add(ACT_SEP, -1, "");
    sheet_add(ACT_CANCEL, UI_ICON_X, "Cancel");
    sheet_open = 1;
    sheet_sel = 0;
    dirty = 1;
}
static int sheet_item_h(int i) { return sheet[i].action == ACT_SEP ? 13 : ROW_H; }
static int sheet_height(void) {
    int h = (MODERN ? 22 : 0) + 34 + 8;
    for (int i = 0; i < sheet_n; i++) h += sheet_item_h(i);
    return h;
}
static int sheet_top(void) { return body_bottom() - sheet_height(); }

/* ---------- Drawing: frame, status bar, bottom bar ---------- */

static void draw_status_bar(void) {
    RtcTime now;
    rtc_read(&now);
    char clock[6];
    two_digits(clock, now.hour);
    clock[2] = ':';
    two_digits(clock + 3, now.min);
    clock[5] = 0;
    int y = phone_y, ty = kit_ty(y, STATUS_H);
    uint8_t bar = MODERN ? TK_SUNKEN : TK_BASE;
    if (MODERN) kit_fill_top(phone_x + 1, y + 1, phone_w - 2, STATUS_H - 1, 13, bar);
    else draw_rect(phone_x + 1, y + 1, phone_w - 2, STATUS_H - 1, bar);
    if (!MODERN) draw_hline(phone_x + 1, y + STATUS_H - 1, phone_w - 2, TK_LINE_SUBTLE);
    kit_text(clock, phone_x + PAD, ty, TK_TEXT, 60, 1);
    int x = phone_x + phone_w - PAD - 16;
    ui_icon_draw(UI_ICON_BAT, x, y + (STATUS_H - 16) / 2, 16, TK_ICON, bar);
    x -= 22;
    ui_icon_draw(UI_ICON_VOL, x, y + (STATUS_H - 16) / 2, 16, TK_ICON, bar);
    if (fs_needs_sync()) kit_text_right("Saving", x - 8, ty, TK_TEXT2, 0);
}

static void draw_nav(void) {
    int y = body_bottom(), w = phone_w / 3;
    uint8_t bar = MODERN ? TK_SUNKEN : TK_BASE;
    if (MODERN) kit_fill_bottom(phone_x + 1, y, phone_w - 2, NAV_H - 1, 13, bar);
    else draw_rect(phone_x + 1, y, phone_w - 2, NAV_H - 1, bar);
    draw_hline(phone_x + 1, y, phone_w - 2, TK_LINE_SUBTLE);
    static const char *const labels[3] = {"Back", "Home", "Apps"};
    static const int icons[3] = {UI_ICON_BACK, UI_ICON_HOME, UI_ICON_STACK};
    int active = current == HOME ? 1 : current == SWITCHER ? 2 : -1;
    for (int i = 0; i < 3; i++) {
        int x = phone_x + i * w, on = i == active, st = hot(x, y, w, NAV_H);
        uint8_t ink = on ? TK_ACCENT_700 : TK_TEXT;
        if (MODERN) {
            int px = x + (w - 48) / 2, py = y + 7;
            if (on) kit_fill(px, py, 48, 26, 13, TK_ACCENT_100, 1);
            else if (st & KIT_HOVER) kit_fill(px, py, 48, 26, 13, TK_HOVER, 1);
            ui_icon_draw(icons[i], px + 16, py + 5, 16, on ? TK_ACCENT_700 : TK_ICON, on ? TK_ACCENT_100 : bar);
            kit_text_center(labels[i], x, y + 34, w, ink, on);
        } else {
            if (st & KIT_HOVER) draw_rect(x + 1, y + 1, w - 2, NAV_H - 2, TK_HOVER);
            int cw = 16 + 8 + kit_text_w(labels[i], on), cx = x + (w - cw) / 2;
            ui_icon_draw(icons[i], cx, y + (NAV_H - 16) / 2, 16, on ? TK_ACCENT_700 : TK_ICON, (st & KIT_HOVER) ? TK_HOVER : bar);
            kit_text(labels[i], cx + 24, kit_ty(y, NAV_H), ink, w, on);
            if (on) draw_rect(x + w / 8, y + NAV_H - 4, w - w / 4, 3, TK_ACCENT);
        }
    }
}

/* Header: back chevron, title and subtitle, and up to two actions. */
static int header_more_x(void) { return phone_x + phone_w - 8 - 36; }
static int header_search_x(void) { return header_more_x() - 36; }
static void header_button(int x, int y, int icon, int on) {
    int st = hot(x, y, 36, 36);
    if (on) kit_box(x, y, 36, 36, ui_theme.r_control, MODERN ? TK_HOVER : TK_BASE, MODERN ? 0 : TK_LINE);
    else if (st & KIT_HOVER) kit_fill(x, y, 36, 36, ui_theme.r_control, TK_HOVER, 1);
    ui_icon_draw(icon, x + 10, y + 10, 16, TK_ICON, on || (st & KIT_HOVER) ? (MODERN ? TK_HOVER : TK_BASE) : ui_theme.content);
}
static void draw_header(const char *title, const char *sub, int dirty_dot, int more, int search_button, int search_on) {
    int x = phone_x + 1, y = phone_y + STATUS_H, w = phone_w - 2;
    uint8_t bg = MODERN ? TK_SUNKEN : ui_theme.content;
    draw_rect(x, y, w, HEADER_H, bg);
    draw_hline(x, y + HEADER_H - 1, w, TK_LINE_SUBTLE);
    int st = hot(phone_x + 4, y + 6, 36, 36);
    if (st & KIT_HOVER) kit_fill(phone_x + 4, y + 6, 36, 36, ui_theme.r_control, TK_HOVER, 1);
    ui_icon_draw(UI_ICON_BACK, phone_x + 14, y + 16, 16, TK_ICON, (st & KIT_HOVER) ? TK_HOVER : bg);
    int tx = phone_x + 48, right = (search_button ? header_search_x() : more ? header_more_x() : phone_x + phone_w - 8) - 8;
    if (sub && sub[0]) {
        int tw = kit_text(title, tx, y + 6, TK_TEXT, right - tx - (dirty_dot ? 12 : 0), 1);
        if (dirty_dot) kit_fill(tx + tw + 6, y + 6 + CHAR_H / 2 - 3, 6, 6, 3, TK_TEXT2, 0);
        kit_text(sub, tx, y + 24, TK_TEXT2, right - tx, 0);
    } else kit_text(title, tx, kit_ty(y, HEADER_H), TK_TEXT, right - tx, 1);
    if (search_button) header_button(header_search_x(), y + 6, UI_ICON_SEARCH, search_on);
    if (more) header_button(header_more_x(), y + 6, UI_ICON_DOTS, sheet_open);
}

/* A list row; Modern rows sit in a white card. Returns the text ink. */
static uint8_t list_row(int x, int y, int w, int last, int on) {
    int st = hot(x, y, w, ROW_H) | (on ? KIT_ON : 0);
    uint8_t ink = TK_TEXT;
    if (on) { draw_rect(x, y, w, ROW_H - 1, MODERN ? TK_HOVER : TK_ACCENT_50); }
    else if (st & KIT_HOVER) draw_rect(x, y, w, ROW_H - 1, TK_HOVER);
    if (!last || !MODERN) draw_hline(x + (MODERN ? 12 : 0), y + ROW_H - 1, w - (MODERN ? 24 : 0), TK_LINE_SUBTLE);
    return ink;
}
static uint8_t row_bg(int x, int y, int w, int on) {
    return on ? (MODERN ? TK_HOVER : TK_ACCENT_50) : inside(x, y, w, ROW_H) ? TK_HOVER : MODERN ? TK_BASE : ui_theme.content;
}
static void card(int x, int y, int w, int h) {
    if (MODERN) kit_box(x, y, w, h, 8, TK_BASE, TK_LINE_SUBTLE);
}
static void section_label(const char *s, int x, int y) { kit_text(s, x, y, TK_TEXT2, phone_w - 2 * PAD, 1); }

/* ---------- Home ---------- */

static int home_matches(int *list, int max) {
    int n = 0;
    for (int i = 0; i < APP_COUNT && n < max; i++) if (has_text(apps[i].name, search)) list[n++] = i;
    return n;
}
static void grid_geometry(void) {
    pocket_columns = 4;
    pocket_grid_x = phone_x + PAD;
    pocket_cell_w = (phone_w - 2 * PAD) / 4;
    pocket_cell_h = 92;
    pocket_grid_y = phone_y + STATUS_H + 12 + 36 + 34;
}
static void home_cell(int i, int *x, int *y) {
    grid_geometry();
    *x = pocket_grid_x + (i % pocket_columns) * pocket_cell_w;
    *y = pocket_grid_y + (i / pocket_columns) * pocket_cell_h;
}
/* Recent files, newest first, or files whose names match the search. */
#define RECENT_MAX 8
static int recent_ids[RECENT_MAX], recent_n;
static void recent_refresh(int max) {
    int ids[512], n = fs_list_files(ids, 512);
    recent_n = 0;
    if (max > RECENT_MAX) max = RECENT_MAX;
    for (int i = 0; i < n; i++) {
        int id = ids[i];
        if (fs_is_dir(id) || !user_node(id) || !has_text(fs_name(id), search)) continue;
        int p = fs_parent(id), system = 0;
        while (p >= 0 && p != fs_root()) { if (!user_node(p)) system = 1; p = fs_parent(p); }
        if (system) continue;
        int at = recent_n;
        while (at > 0 && fs_modified(recent_ids[at - 1]) < fs_modified(id)) at--;
        if (at >= max) continue;
        if (recent_n < max) recent_n++;
        for (int k = recent_n - 1; k > at; k--) recent_ids[k] = recent_ids[k - 1];
        recent_ids[at] = id;
    }
}
static int home_list_y(int rows_of_apps) {
    return pocket_grid_y + rows_of_apps * pocket_cell_h + 12;
}
static int home_list_rows(int list_y) { return (body_bottom() - list_y - 30 - 8) / ROW_H; }

static void draw_home(void) {
    int sx, sy, sw, sh;
    screen_rect(&sx, &sy, &sw, &sh);
    uint8_t bg = MODERN ? TK_SUNKEN : ui_theme.content;
    draw_rect(sx + 1, sy, sw - 2, sh, bg);
    int fx = sx + PAD, fy = sy + 12, fw = sw - 2 * PAD;
    int tx = kit_search(fx, fy, fw, 36, search, search_len ? "" : "Search apps and files", KIT_FOCUS);
    if (search_len) {
        kit_text(search, tx, kit_ty(fy, 36), TK_TEXT, fx + fw - 32 - tx, 0);
        if ((timer_ticks() / (TIMER_HZ / 2)) % 2 == 0) kit_caret(tx + kit_text_w(search, 0), kit_ty(fy, 36));
    }
    grid_geometry();
    int list[APP_COUNT], n = home_matches(list, APP_COUNT);
    section_label("Apps", sx + PAD, pocket_grid_y - 24);
    for (int k = 0; k < n; k++) {
        int i = list[k], x, y;
        home_cell(k, &x, &y);
        int st = hot(x, y, pocket_cell_w, pocket_cell_h - 4), on = k == selected;
        int bx = x + (pocket_cell_w - 56) / 2, by = y + 4;
        if (MODERN) {
            if (on || (st & KIT_HOVER)) kit_fill(x + 4, y, pocket_cell_w - 8, pocket_cell_h - 6, 10, on ? TK_ACCENT_100 : TK_HOVER, 1);
            kit_shadow(bx, by, 56, 56, 12);
            kit_box(bx, by, 56, 56, 12, TK_BASE, TK_LINE_SUBTLE);
        } else {
            if (on || (st & KIT_HOVER)) draw_rect(x + 2, y, pocket_cell_w - 4, pocket_cell_h - 6, on ? TK_ACCENT_100 : TK_HOVER);
            kit_box(bx, by, 56, 56, KIT_R_NOTCH, TK_RAISED, TK_LINE);
        }
        app_icon_draw(apps[i].icon, bx + 12, by + 12, AI_BIG);
        kit_text_center(apps[i].name, x, y + 66, pocket_cell_w, TK_TEXT, 0);
    }
    if (!n) kit_text("No apps match", sx + PAD, pocket_grid_y + 8, TK_TEXT2, sw - 2 * PAD, 0);
    int ly = home_list_y(n ? (n + pocket_columns - 1) / pocket_columns : 1);
    int rows = home_list_rows(ly);
    recent_refresh(rows);
    section_label(search_len ? "Files" : "Recent files", sx + PAD, ly);
    ly += 24;
    int cx = MODERN ? sx + PAD : sx + 1, cw = MODERN ? sw - 2 * PAD : sw - 2;
    if (recent_n) card(cx, ly, cw, recent_n * ROW_H);
    else kit_text(search_len ? "No files match" : "No files yet", sx + PAD, ly + 8, TK_TEXT2, sw - 2 * PAD, 0);
    if (!MODERN && recent_n) draw_hline(cx, ly - 1, cw, TK_LINE_SUBTLE);
    for (int i = 0; i < recent_n; i++) {
        int id = recent_ids[i], y = ly + i * ROW_H;
        uint8_t ink = list_row(cx + 1, y, cw - 2, i == recent_n - 1, 0);
        int icon = node_icon(id);
        ui_icon_draw(icon, cx + 14, y + (ROW_H - 16) / 2, 16, node_ink(icon), row_bg(cx + 1, y, cw - 2, 0));
        char folder[FS_NAME_LEN];
        folder_name(id, folder);
        int fw2 = kit_text_w(folder, 0);
        kit_text(fs_name(id), cx + 42, kit_ty(y, ROW_H), ink, cw - 42 - fw2 - 28, 0);
        kit_text_right(folder, cx + cw - 14, kit_ty(y, ROW_H), TK_TEXT2, 0);
    }
}

/* ---------- Files ---------- */

static void files_title(char *title, char *sub) {
    if (files_cwd == fs_root()) { kstrcpy(title, "Home"); sub[0] = 0; return; }
    kstrcpy(title, fs_name(files_cwd));
    int p = fs_parent(files_cwd);
    kstrcpy(sub, p == fs_root() ? "Home" : fs_name(p));
}
static int files_list_x(void) { return MODERN ? phone_x + 10 : phone_x + 1; }
static int files_list_w(void) { return MODERN ? phone_w - 20 : phone_w - 2; }
static int files_list_y(void) { return body_top() + (MODERN ? 10 : 0); }
static void draw_files(void) {
    files_refresh();
    int x = phone_x + 1, y = body_top(), w = phone_w - 2;
    draw_rect(x, y, w, body_bottom() - y, MODERN ? TK_SUNKEN : ui_theme.content);
    int rows = files_rows(), lx = files_list_x(), lw = files_list_w(), ly = files_list_y();
    if (files_first > files_n - rows) files_first = files_n - rows;
    if (files_first < 0) files_first = 0;
    int shown = files_n - files_first < rows ? files_n - files_first : rows;
    if (shown > 0) card(lx, ly, lw, shown * ROW_H);
    for (int k = 0; k < shown; k++) {
        int i = files_first + k, id = files_ids[i], ry = ly + k * ROW_H;
        uint8_t ink = list_row(lx + 1, ry, lw - 2, k == shown - 1, i == files_sel);
        int icon = node_icon(id);
        uint8_t bg = row_bg(lx + 1, ry, lw - 2, i == files_sel);
        ui_icon_draw(icon, lx + 14, ry + (ROW_H - 16) / 2, 16, node_ink(icon), bg);
        char right[48];
        right[0] = 0;
        if (fs_is_dir(id)) { append_uint(right, (unsigned)fs_child_count(id)); append(right, fs_child_count(id) == 1 ? " item" : " items"); }
        else {
            char when[24];
            size_text(right, fs_size(id));
            stamp_text(when, fs_modified(id));
            if (when[0]) { append(right, " - "); append(right, when); }
        }
        int rx = lx + lw - 14 - (fs_is_dir(id) ? 22 : 0), rw = kit_text_w(right, 0);
        kit_text(fs_name(id), lx + 42, kit_ty(ry, ROW_H), ink, rx - rw - 12 - lx - 42, 0);
        kit_text_right(right, rx, kit_ty(ry, ROW_H), TK_TEXT2, 0);
        if (fs_is_dir(id)) ui_icon_draw(UI_ICON_RIGHT, lx + lw - 30, ry + (ROW_H - 16) / 2, 16, TK_TEXT2, bg);
    }
    if (!files_n) kit_text("This folder is empty.", lx + 14, ly + 12, TK_TEXT2, lw - 28, 0);
    char foot[96], number[24];
    foot[0] = 0;
    append_uint(foot, (unsigned)files_n);
    append(foot, files_n == 1 ? " item - " : " items - ");
    unsigned cap = fs_capacity(), used = fs_used_bytes(), left = cap > used ? cap - used : 0;
    size_text(number, (int)left);
    append(foot, number);
    append(foot, " free");
    if (files_n > rows) {
        append(foot, " - ");
        append_uint(foot, (unsigned)(files_first + 1));
        append(foot, "-");
        append_uint(foot, (unsigned)(files_first + shown));
    }
    kit_text(foot, lx + (MODERN ? 4 : 12), ly + shown * ROW_H + 10, TK_TEXT2, lw - 16, 0);
}
static void files_open(int index) {
    if (index < 0 || index >= files_n) return;
    int id = files_ids[index];
    if (fs_is_dir(id)) { files_cwd = id; files_first = 0; files_sel = -1; dirty = 1; return; }
    if (ed_open && ed_changed && ed_file != id) { notify("Save or close the open document first."); return; }
    if (!ed_load(id)) return;
    back_to[APP_EDITOR] = APP_FILES;
    open_push(APP_EDITOR);
    current = APP_EDITOR;
    kprint_debug("POCKET-OPEN Editor\n");
    dirty = 1;
}

/* ---------- Editor ---------- */

static int ed_gutter(void) {
    int lines = 1, digits = 1;
    for (int i = 0; i < ed_len; i++) if (ed_buf[i] == '\n') lines++;
    for (int n = lines; n >= 10; n /= 10) digits++;
    return (digits < 2 ? 2 : digits) * EDIT_CHAR_W + 14;
}
static int ed_text_x(void) { return phone_x + 1 + ed_gutter() + 8; }
static int ed_text_y(void) { return body_top() + 6; }
static void draw_editor(void) {
    int x = phone_x + 1, y = body_top(), w = phone_w - 2, bottom = body_bottom() - (find_open ? 52 : 0);
    draw_rect(x, y, w, body_bottom() - y, TK_BASE == ui_theme.content ? TK_BASE : ui_theme.content);
    int g = ed_gutter(), rows = ed_rows(), cols = (x + w - 8 - ed_text_x()) / EDIT_CHAR_W;
    if (!MODERN) draw_vline(x + g, y, bottom - y, TK_LINE_SUBTLE);
    ed_follow();
    int line = 0, i = 0, caret_line = ed_line_of(ed_caret);
    while (line < ed_top && i < ed_len) { if (ed_buf[i] == '\n') line++; i++; }
    int needle = find_open ? find_len : 0, match_end = -1;
    for (int r = 0; r < rows && line <= ed_line_of(ed_len); r++, line++) {
        int ry = ed_text_y() + r * EDIT_LINE_H;
        if (line == caret_line) draw_rect(x, ry, w, EDIT_LINE_H, TK_ACCENT_50);
        char num[12];
        fmt_uint(num, (unsigned)(line + 1));
        int nw = kstrlen(num) * EDIT_CHAR_W;
        for (int k = 0; num[k]; k++)
            draw_edit_char(num[k], x + g - 6 - nw + k * EDIT_CHAR_W, ry, line == caret_line ? TK_TEXT : TK_LINE_STRONG);
        int c = 0;
        for (; i <= ed_len; i++) {
            if (needle && i >= match_end && text_match(ed_buf, ed_len, i, find_text, 0)) match_end = i + needle;
            if (i == ed_caret && (timer_ticks() / (TIMER_HZ / 2)) % 2 == 0 && c <= cols)
                draw_rect(ed_text_x() + c * EDIT_CHAR_W, ry, 2, EDIT_FONT_H, TK_TEXT);
            if (i == ed_len) break;
            if (ed_buf[i] == '\n') { i++; break; }
            if (c < cols) {
                if (i < match_end) draw_rect(ed_text_x() + c * EDIT_CHAR_W, ry, EDIT_CHAR_W, EDIT_LINE_H, TK_FIND);
                draw_edit_char(ed_buf[i], ed_text_x() + c * EDIT_CHAR_W, ry, TK_TEXT);
            }
            c++;
        }
        if (i >= ed_len && line >= ed_line_of(ed_len)) break;
    }
    if (find_open) {
        int fy = body_bottom() - 52;
        draw_rect(x, fy, w, 52, MODERN ? TK_SUNKEN : TK_BASE);
        draw_hline(x, fy, w, TK_LINE_SUBTLE);
        int bw = kit_button_w("Next", -1), fx = phone_x + 12, fw = phone_w - 24 - bw - 8 - 36;
        int tx = kit_search(fx, fy + 9, fw, 34, find_text, find_len ? "" : "Find", KIT_FOCUS);
        kit_text(find_text, tx, kit_ty(fy + 9, 34), TK_TEXT, fx + fw - 30 - tx, 0);
        kit_caret(tx + kit_text_w(find_text, 0), kit_ty(fy + 9, 34));
        kit_button(fx + fw + 8, fy + 9, bw, 34, "Next", -1, KB_DEFAULT, hot(fx + fw + 8, fy + 9, bw, 34));
        kit_icon_button(phone_x + phone_w - 12 - 32, fy + 10, 32, UI_ICON_X, hot(phone_x + phone_w - 44, fy + 10, 32, 32));
    }
}
static void editor_tap(int mx, int my) {
    if (find_open && my >= body_bottom() - 52) {
        int bw = kit_button_w("Next", -1), fx = phone_x + 12, fw = phone_w - 24 - bw - 8 - 36;
        if (mx >= fx + fw + 8 && mx < fx + fw + 8 + bw) ed_find_next();
        else if (mx >= phone_x + phone_w - 44) find_open = 0;
        dirty = 1;
        return;
    }
    int row = (my - ed_text_y()) / EDIT_LINE_H;
    if (row < 0) row = 0;
    int s = ed_index_of_line(ed_top + row), e = s;
    while (e < ed_len && ed_buf[e] != '\n') e++;
    int col = (mx - ed_text_x() + EDIT_CHAR_W / 2) / EDIT_CHAR_W;
    if (col < 0) col = 0;
    ed_caret = s + col < e ? s + col : e;
    dirty = 1;
}

/* ---------- Settings ---------- */

static int seg_xs[2], seg_ws[2];
static int set_style_y(void) { return body_top() + 46; }
static int set_accent_y(void) { return body_top() + 46 + 60 + 34; }
static void draw_settings(void) {
    int x = phone_x + 1, y = body_top(), w = phone_w - 2;
    draw_rect(x, y, w, body_bottom() - y, MODERN ? TK_SUNKEN : ui_theme.content);
    int lx = phone_x + PAD;
    section_label("Style", lx, y + 18);
    static const char *const styles[2] = {"Classic", "Modern"};
    int *xs = seg_xs, *ws = seg_ws;
    int hover = -1;
    kit_segmented(lx, set_style_y(), styles, 2, ui_style, -1, xs, ws);
    for (int i = 0; i < 2; i++) if (inside(xs[i], set_style_y(), ws[i], kit_segmented_h())) hover = i;
    if (hover >= 0 && hover != ui_style) kit_segmented(lx, set_style_y(), styles, 2, ui_style, hover, xs, ws);
    (void)hot(lx, set_style_y(), xs[1] + ws[1] - lx, kit_segmented_h());
    kit_text("Shared with the desktop on this disk.", lx, set_style_y() + kit_segmented_h() + 10, TK_TEXT2, w - 2 * PAD, 0);
    section_label("Accent color", lx, set_accent_y() - 28);
    for (int i = 0; i < UI_ACCENT_N; i++) {
        int sx = lx + i * 44, on = i == ui_accent;
        uint32_t rgb = ui_accent_rgb(ui_style, i);
        uint8_t c = gfx_rgb((int)(rgb >> 16) & 255, (int)(rgb >> 8) & 255, (int)rgb & 255);
        if (on) kit_frame(sx - 3, set_accent_y() - 3, 38, 38, MODERN ? 19 : 0, TK_TEXT);
        kit_fill(sx, set_accent_y(), 32, 32, MODERN ? 16 : 0, c, 1);
        (void)hot(sx, set_accent_y(), 32, 32);
    }
    kit_text(ui_accent_name(ui_style, ui_accent), lx, set_accent_y() + 44, TK_TEXT2, w - 2 * PAD, 0);
}
static void settings_tap(int mx, int my) {
    int lx = phone_x + PAD;
    for (int i = 0; i < 2; i++)
        if (mx >= seg_xs[i] && mx < seg_xs[i] + seg_ws[i] && my >= set_style_y() && my < set_style_y() + kit_segmented_h()) { set_style(i, ui_accent); return; }
    for (int i = 0; i < UI_ACCENT_N; i++)
        if (mx >= lx + i * 44 && mx < lx + i * 44 + 32 && my >= set_accent_y() && my < set_accent_y() + 32) { set_style(ui_style, i); return; }
}

/* ---------- Todo and About ---------- */

static void draw_todo_screen(int x, int y, int w, int h) {
    draw_rect(x + 1, y, w - 2, h, MODERN ? TK_SUNKEN : ui_theme.content);
    int row = y + 12, count = todo_count();
    int cx = MODERN ? x + 10 : x + 1, cw = MODERN ? w - 20 : w - 2;
    int shown = 0;
    for (int i = 0; i < count && row + shown * ROW_H + ROW_H < y + h - 80; ++i) shown++;
    if (shown) card(cx, row, cw, shown * ROW_H);
    for (int i = 0; i < shown; ++i) {
        int ry = row + i * ROW_H;
        uint8_t ink = list_row(cx + 1, ry, cw - 2, i == shown - 1, 0);
        kit_check(cx + 14, ry + (ROW_H - ui_theme.check) / 2, todo_done(i) ? KIT_ON : 0);
        kit_text(todo_text(i), cx + 14 + ui_theme.check + 12, kit_ty(ry, ROW_H), todo_done(i) ? TK_TEXT2 : ink,
                 cw - 14 - ui_theme.check - 24, 0);
        if (todo_done(i)) draw_hline(cx + 14 + ui_theme.check + 12, ry + ROW_H / 2,
                                     kit_text_w(todo_text(i), 0) < cw - 60 ? kit_text_w(todo_text(i), 0) : cw - 60, TK_TEXT2);
    }
    if (!count) kit_text("Nothing to do yet. Type below and press Enter.", x + PAD, row + 6, TK_TEXT2, w - 2 * PAD, 0);
    int fy = y + h - 54;
    int tx = kit_field(x + 12, fy, w - 24, 38, todo_field_active() ? KIT_FOCUS : 0);
    const char *field = todo_field();
    if (field[0]) kit_text(field, tx, kit_ty(fy, 38), TK_TEXT, w - 24 - (tx - x - 12) - 10, 0);
    else kit_text("New item", tx, kit_ty(fy, 38), TK_TEXT2, w - 48, 0);
    if (todo_field_active()) kit_caret(tx + kit_text_w(field, 0), kit_ty(fy, 38));
    const char *status = todo_status();
    if (status && status[0]) kit_text(status, x + PAD, fy - CHAR_H - 8, TK_TEXT2, w - 2 * PAD, 0);
}

static int about_shutdown_y(int y) { return y + 24 + 6 * 36 + 16; }
static void draw_about_screen(int x, int y, int w, int h) {
    draw_rect(x + 1, y, w - 2, h, MODERN ? TK_SUNKEN : ui_theme.content);
    char number[16], text[48];
    const char *labels[6] = {"Shell", "Build", "Memory", "Display", "Files", "Storage"};
    char values[6][48];
    kstrcpy(values[0], baseos_shell.name);
    kstrcpy(values[1], BASEOS_BUILD_LABEL);
    fmt_uint(number, platform_memory_mb()); kstrcpy(text, number); append(text, " MiB"); kstrcpy(values[2], text);
    fmt_uint(number, (unsigned)fb_w); kstrcpy(text, number); append(text, " x ");
    fmt_uint(number, (unsigned)fb_h); append(text, number); kstrcpy(values[3], text);
    fmt_uint(number, (unsigned)fs_node_count()); kstrcpy(text, number); append(text, " of ");
    fmt_uint(number, (unsigned)fs_node_limit()); append(text, number); kstrcpy(values[4], text);
    const char *storage = fs_storage_status();
    kstrcpy(values[5], storage ? storage : "Ready");
    int cx = MODERN ? x + 10 : x + 1, cw = MODERN ? w - 20 : w - 2, row = y + 12;
    card(cx, row, cw, 6 * 36);
    for (int i = 0; i < 6; i++) {
        int ry = row + i * 36;
        if (i < 5) draw_hline(cx + (MODERN ? 12 : 0), ry + 35, cw - (MODERN ? 24 : 0), TK_LINE_SUBTLE);
        kit_text(labels[i], cx + 14, kit_ty(ry, 36), TK_TEXT2, 110, 0);
        kit_text_right(values[i], cx + cw - 14, kit_ty(ry, 36), TK_TEXT, 0);
    }
    kit_text("Pocket hosts the same app components as the desktop, on the same kernel.",
             x + PAD, row + 6 * 36 + 12, TK_TEXT2, w - 2 * PAD, 0);
    int by = about_shutdown_y(y) + 20;
    kit_button(x + PAD, by, w - 2 * PAD, 38, "Shut down", UI_ICON_POWER, KB_PRIMARY, hot(x + PAD, by, w - 2 * PAD, 38));
}
static int about_button(int x, int y, int w, int mx, int my) {
    int by = about_shutdown_y(y) + 20;
    return hit(mx, my, x + PAD, by, w - 2 * PAD, 38);
}

/* ---------- Open apps ---------- */

static int card_y(int i) { return body_top() + 10 + i * 68; }
static void app_subtitle(int app, char *out) {
    if (app == APP_EDITOR) { kstrcpy(out, "Editor"); return; }
    if (app == APP_FILES) { kstrcpy(out, files_cwd == fs_root() || files_cwd < 0 ? "Home" : fs_name(files_cwd)); return; }
    kstrcpy(out, apps[app].name);
}
static void app_card_title(int app, char *out) {
    if (app == APP_EDITOR) kstrcpy(out, ed_title());
    else if (app == APP_FILES) kstrcpy(out, "Files");
    else kstrcpy(out, apps[app].name);
}
static void draw_switcher(void) {
    int x = phone_x + 1, y = body_top(), w = phone_w - 2;
    draw_rect(x, y, w, body_bottom() - y, MODERN ? TK_SUNKEN : ui_theme.content);
    int cx = phone_x + 12, cw = phone_w - 24;
    for (int i = 0; i < open_n; i++) {
        int app = open_order[i], cy = card_y(i), on = i == 0;
        if (cy + 60 > body_bottom() - 40) break;
        int st = hot(cx, cy, cw, 60);
        uint8_t fill = MODERN ? (on ? TK_ACCENT_50 : TK_BASE) : (st & KIT_HOVER) ? TK_ACCENT_50 : TK_RAISED;
        if (MODERN) kit_box(cx, cy, cw, 60, 10, fill, on ? TK_ACCENT : TK_LINE_SUBTLE);
        else kit_box(cx, cy, cw, 60, 0, fill, on ? TK_ACCENT : TK_LINE);
        if (on) kit_frame(cx + 1, cy + 1, cw - 2, 58, MODERN ? 9 : 0, TK_ACCENT);
        app_icon_draw(apps[app].icon, cx + 14, cy + 14, AI_BIG);
        char title[FS_NAME_LEN], sub[FS_NAME_LEN];
        app_card_title(app, title);
        app_subtitle(app, sub);
        if (on) append(sub, " - current");
        int tw = kit_text(title, cx + 60, cy + 12, TK_TEXT, cw - 60 - 50, 1);
        if (app == APP_EDITOR && ed_changed) kit_fill(cx + 60 + tw + 6, cy + 12 + CHAR_H / 2 - 3, 6, 6, 3, TK_TEXT2, 0);
        kit_text(sub, cx + 60, cy + 31, TK_TEXT2, cw - 60 - 50, 0);
        kit_icon_button(cx + cw - 44, cy + 14, 32, UI_ICON_X, hot(cx + cw - 44, cy + 14, 32, 32));
    }
    int fy = card_y(open_n) + 4;
    if (fy > body_bottom() - 40) fy = body_bottom() - 40;
    kit_text(open_n ? "Tap a card to switch. Closing an app with unsaved changes asks first." : "No apps are open.",
             cx, fy, TK_TEXT2, cw, 0);
}
static int close_all_w(void) { return kit_button_w("Close all", -1); }
static void draw_switcher_header(void) {
    int x = phone_x + 1, y = phone_y + STATUS_H, w = phone_w - 2;
    uint8_t bg = MODERN ? TK_SUNKEN : ui_theme.content;
    draw_rect(x, y, w, HEADER_H, bg);
    draw_hline(x, y + HEADER_H - 1, w, TK_LINE_SUBTLE);
    kit_text("Open apps", phone_x + PAD, kit_ty(y, HEADER_H), TK_TEXT, w - 120, 1);
    if (open_n) {
        int bw = close_all_w(), bx = phone_x + phone_w - 12 - bw;
        kit_button(bx, y + 8, bw, 32, "Close all", -1, MODERN ? KB_FLAT : KB_DEFAULT, hot(bx, y + 8, bw, 32));
    }
}

/* ---------- Apps ---------- */

static void draw_app(int app) {
    if (app == APP_EDITOR) draw_header(ed_title(), "Editor", ed_changed, 1, 0, 0);
    else if (app == APP_FILES) {
        char title[FS_NAME_LEN], sub[FS_NAME_LEN];
        files_title(title, sub);
        draw_header(title, sub, 0, 1, 0, 0);
    } else draw_header(apps[app].name, 0, 0, 1, 0, 0);
    int sx, sy, sw, sh;
    screen_rect(&sx, &sy, &sw, &sh);
    if (app == APP_FILES) { draw_files(); return; }
    if (app == APP_EDITOR) { draw_editor(); return; }
    if (app == APP_SETTINGS) { draw_settings(); return; }
    draw_rect(sx + 1, sy + HEADER_H, sw - 2, sh - HEADER_H, MODERN ? TK_SUNKEN : ui_theme.content);
    int x, y, w, h;
    app_origin(app, &x, &y, &w, &h);
    switch (app) {
    case APP_CLOCK: clock_draw(x, y, w, h); break;
    case APP_TODO: draw_todo_screen(x, y, w, h); break;
    case APP_2048: g2048_draw(x, y); break;
    case APP_MINES: mines_draw(x, y); break;
    case APP_WORDLE: wordle_draw(x, y); break;
    case APP_BREAKOUT: bo_draw(x, y); break;
    case APP_ABOUT: draw_about_screen(x, y, w, h); break;
    }
}

static void draw_sheet(void) {
    if (!sheet_open) return;
    int x = phone_x + 1, w = phone_w - 2, top = sheet_top();
    /* Dim the screen behind it. */
    for (int yy = phone_y + STATUS_H; yy < top; yy += 2)
        for (int xx = x + (yy & 2 ? 1 : 0); xx < x + w; xx += 2) put_pixel(xx, yy, TK_LINE);
    int y = top;
    if (MODERN) {
        kit_shadow(x + 1, y, w - 2, body_bottom() - y + 10, 14);
        kit_fill_top(x, y, w, body_bottom() - y, 14, ui_theme.overlay);
        kit_frame(x, y, w, body_bottom() - y + 14, 14, TK_LINE);
        draw_rect(x + 1, body_bottom(), w - 2, 1, ui_theme.overlay);
        kit_fill(x + (w - 36) / 2, y + 8, 36, 4, 2, TK_LINE_STRONG, 0);
        y += 22;
    } else {
        draw_rect(x, y, w, body_bottom() - y, ui_theme.overlay);
        draw_hline(x, y, w, TK_LINE_STRONG);
        draw_hline(x, y + 1, w, TK_LINE_STRONG);
    }
    kit_text(sheet_title, phone_x + PAD, kit_ty(y, 34), TK_TEXT2, w - 2 * PAD, 0);
    y += 34;
    for (int i = 0; i < sheet_n; i++) {
        if (sheet[i].action == ACT_SEP) { kit_separator(phone_x + PAD, y + 6, w - 2 * PAD); y += 13; continue; }
        int st = hot(x, y, w, ROW_H), on = i == sheet_sel;
        uint8_t bg = ui_theme.overlay, ink = TK_TEXT, ic = TK_ICON;
        int rx = MODERN ? x + 8 : x, rw = MODERN ? w - 16 : w;
        if (on) { bg = TK_ACCENT; ink = ic = TK_ON_ACCENT; kit_fill(rx, y, rw, ROW_H, MODERN ? 8 : 0, bg, 1); }
        else if (st & KIT_HOVER) { bg = TK_HOVER; kit_fill(rx, y, rw, ROW_H, MODERN ? 8 : 0, bg, 1); }
        if (sheet[i].icon >= 0) ui_icon_draw(sheet[i].icon, phone_x + PAD, y + (ROW_H - 16) / 2, 16, ic, bg);
        kit_text(sheet[i].label, phone_x + PAD + 26, kit_ty(y, ROW_H), ink, w - 2 * PAD - 26, 0);
        y += ROW_H;
    }
}

static void draw_toast(void) {
    if (!notice[0]) return;
    if (timer_ticks() > notice_until && current != HOME) { notice = ""; return; }
    int w = kit_text_w(notice, 0) + 28;
    if (w > phone_w - 24) w = phone_w - 24;
    int x = phone_x + (phone_w - w) / 2, y = body_bottom() - 48;
    kit_shadow(x, y, w, 34, MODERN ? 8 : 0);
    kit_box(x, y, w, 34, MODERN ? 8 : 0, TK_TEXT, 0);
    kit_text_center(notice, x, kit_ty(y, 34), w, ui_theme.content, 0);
}

static void draw_scene(void) {
    hot_n = 0;
    draw_rect(0, 0, fb_w, fb_h, MODERN ? TK_SUNKEN : TK_HOVER);
    int r = MODERN ? 14 : 0;
    kit_shadow(phone_x, phone_y, phone_w, phone_h, r);
    kit_box(phone_x, phone_y, phone_w, phone_h, r, ui_theme.content, TK_LINE_STRONG);
    draw_status_bar();
    if (current == HOME) draw_home();
    else if (current == SWITCHER) { draw_switcher_header(); draw_switcher(); }
    else draw_app(current);
    draw_sheet();
    draw_toast();
    draw_nav();
}

static void cursor_hide(void) {
    if (!cursor_shown) return;
    for (int j = 0; j < CURSOR_ART_H; ++j)
        for (int i = 0; i < CURSOR_ART_W; ++i)
            put_pixel(cursor_x + i, cursor_y + j, cursor_saved[j * CURSOR_ART_W + i]);
    cursor_shown = 0;
}

static void cursor_show(void) {
    cursor_x = pocket_mouse_x;
    cursor_y = pocket_mouse_y;
    for (int j = 0; j < CURSOR_ART_H; ++j) {
        for (int i = 0; i < CURSOR_ART_W; ++i) {
            uint8_t color = get_pixel(cursor_x + i, cursor_y + j);
            cursor_saved[j * CURSOR_ART_W + i] = color;
            if (cursor_outline_a[j][i]) color = gfx_mix(color, COLOR_BLACK, cursor_outline_a[j][i] * 17);
            if (cursor_fill_a[j][i]) color = gfx_mix(color, COLOR_WHITE, cursor_fill_a[j][i] * 17);
            put_pixel(cursor_x + i, cursor_y + j, color);
        }
    }
    cursor_shown = 1;
}

/* ---------- Navigation ---------- */

static int wordle_ready;
static void open_app(int app) {
    int from = current;
    current = app;
    sheet_open = 0;
    if (from != app && from != SWITCHER) back_to[app] = from;
    if (app == APP_TODO) todo_focus_field();
    if (app == APP_WORDLE && !wordle_ready) { wordle_new_game(timer_ticks()); wordle_ready = 1; }
    if (app == APP_EDITOR && !ed_open) ed_new();
    if (app == APP_FILES) files_refresh();
    open_push(app);
    kprint_debug("POCKET-OPEN ");
    kprint_debug(apps[app].name);
    kprint_debug("\n");
    dirty = 1;
}

static void go_home(void) {
    sheet_open = 0;
    if (current == HOME) return;
    if (current >= 0) {
        int list[APP_COUNT], n = home_matches(list, APP_COUNT);
        for (int k = 0; k < n; k++) if (list[k] == current) selected = k;
    }
    current = HOME;
    kprint_debug("POCKET-HOME\n");
    dirty = 1;
}

/* Close an app and forget its state; the Editor asks before dropping edits. */
static void close_app(int app, int confirmed) {
    if (app == APP_EDITOR && ed_changed && !confirmed) {
        if (current != APP_EDITOR) open_app(APP_EDITOR);
        sheet_ask_save(app);
        return;
    }
    if (app == APP_EDITOR) { ed_open = 0; ed_changed = 0; ed_file = -1; find_open = 0; }
    open_remove(app);
    sheet_open = 0;
    if (current == app) go_home();
    dirty = 1;
}

static void go_back(void) {
    if (sheet_open) { sheet_open = 0; dirty = 1; return; }
    if (current == HOME) {
        if (search_len) { search_len = 0; search[0] = 0; selected = 0; dirty = 1; }
        return;
    }
    if (current == SWITCHER) { go_home(); return; }
    if (current == APP_EDITOR && find_open) { find_open = 0; dirty = 1; return; }
    if (current == APP_FILES && files_cwd != fs_root() && fs_valid(files_cwd)) {
        files_cwd = fs_parent(files_cwd);
        files_first = 0;
        files_sel = -1;
        dirty = 1;
        return;
    }
    int to = back_to[current];
    if (to >= 0 && to < APP_COUNT && to != current) { int from = current; current = to; open_push(to); back_to[from] = HOME; dirty = 1; kprint_debug("POCKET-OPEN "); kprint_debug(apps[to].name); kprint_debug("\n"); }
    else go_home();
}

static void shut_down(void) {
    if (ed_changed) { notify("Save or close the open document first."); return; }
    if (todo_prepare_shutdown() < 0 || fs_sync() < 0) {
        notify("Shutdown paused: storage is not ready.");
        go_home();
        return;
    }
    kprint_debug("POCKET-SHUTDOWN\n");
    kernel_power_off();
}

static void props_notice(void) {
    static char text[96];
    char size[24], when[24];
    if (!ed_valid()) { notify("Not saved yet."); return; }
    size_text(size, fs_size(ed_file));
    stamp_text(when, fs_modified(ed_file));
    kstrcpy(text, size);
    if (when[0]) { append(text, ", modified "); append(text, when); }
    notify(text);
}

static void sheet_run(int i) {
    if (i < 0 || i >= sheet_n) return;
    int action = sheet[i].action, app = current;
    if (action == ACT_SEP) return;
    sheet_open = 0;
    switch (action) {
    case ACT_SAVE: ed_save(); break;
    case ACT_NEW_DOC: if (ed_changed) notify("Save the document first."); else ed_new(); break;
    case ACT_FIND: find_open = 1; break;
    case ACT_PROPS: props_notice(); break;
    case ACT_SORT_NAME: files_by_date = 0; files_refresh(); break;
    case ACT_SORT_DATE: files_by_date = 1; files_refresh(); break;
    case ACT_NEW_GAME:
        if (app == APP_2048) g2048_new(timer_ticks());
        else if (app == APP_MINES) mines_new(timer_ticks());
        else if (app == APP_WORDLE) wordle_new_game(timer_ticks());
        else if (app == APP_BREAKOUT) bo_new();
        break;
    case ACT_SHUTDOWN: shut_down(); break;
    case ACT_CLOSE: if (app >= 0) close_app(app, 0); break;
    case ACT_CLOSE_SAVE: if (ed_save()) close_app(sheet_closing, 1); break;
    case ACT_CLOSE_DISCARD: close_app(sheet_closing, 1); break;
    case ACT_CANCEL: default: break;
    }
    dirty = 1;
}

/* ---------- Input ---------- */

static void tap(int mx, int my) {
    int nav_y = body_bottom();
    if (hit(mx, my, phone_x, nav_y, phone_w, NAV_H)) {
        int i = (mx - phone_x) * 3 / phone_w;
        if (i == 0) go_back();
        else if (i == 1) go_home();
        else { sheet_open = 0; current = SWITCHER; kprint_debug("POCKET-APPS\n"); dirty = 1; }
        return;
    }
    if (sheet_open) {
        int y = sheet_top() + (MODERN ? 22 : 0) + 34;
        if (my < sheet_top()) { sheet_open = 0; dirty = 1; return; }
        for (int i = 0; i < sheet_n; i++) {
            if (my >= y && my < y + sheet_item_h(i)) { sheet_run(i); return; }
            y += sheet_item_h(i);
        }
        return;
    }
    int sx, sy, sw, sh;
    screen_rect(&sx, &sy, &sw, &sh);
    if (current == HOME) {
        if (hit(mx, my, sx + PAD, sy + 12, sw - 2 * PAD, 36)) {
            if (search_len && mx >= kit_search_clear_x(sx + PAD, sw - 2 * PAD)) { search_len = 0; search[0] = 0; dirty = 1; }
            return;
        }
        int list[APP_COUNT], n = home_matches(list, APP_COUNT);
        for (int k = 0; k < n; ++k) {
            int x, y;
            home_cell(k, &x, &y);
            if (hit(mx, my, x, y, pocket_cell_w, pocket_cell_h - 4)) { selected = k; open_app(list[k]); return; }
        }
        int ly = home_list_y(n ? (n + pocket_columns - 1) / pocket_columns : 1) + 24;
        recent_refresh(home_list_rows(ly - 24));
        for (int i = 0; i < recent_n; i++)
            if (hit(mx, my, sx, ly + i * ROW_H, sw, ROW_H)) {
                int id = recent_ids[i];
                files_cwd = fs_parent(id);
                files_refresh();
                if (ed_open && ed_changed && ed_file != id) { notify("Save or close the open document first."); return; }
                if (ed_load(id)) { back_to[APP_EDITOR] = HOME; current = HOME; open_app(APP_EDITOR); back_to[APP_EDITOR] = HOME; }
                return;
            }
        return;
    }
    if (current == SWITCHER) {
        if (open_n && hit(mx, my, phone_x + phone_w - 12 - close_all_w(), sy + 8, close_all_w(), 32)) {
            for (int i = open_n - 1; i >= 0; i--) {
                int app = open_order[i];
                if (app == APP_EDITOR && ed_changed) continue;
                close_app(app, 0);
            }
            current = SWITCHER;
            if (open_n) notify("Unsaved documents stay open.");
            dirty = 1;
            return;
        }
        int cx = phone_x + 12, cw = phone_w - 24;
        for (int i = 0; i < open_n; i++) {
            int cy = card_y(i);
            if (!hit(mx, my, cx, cy, cw, 60)) continue;
            int app = open_order[i];
            if (hit(mx, my, cx + cw - 44, cy + 14, 32, 32)) { close_app(app, 0); if (!sheet_open) current = SWITCHER; }
            else { current = HOME; open_app(app); }
            dirty = 1;
            return;
        }
        return;
    }
    if (hit(mx, my, sx, sy, 44, HEADER_H)) { go_back(); return; }
    if (hit(mx, my, header_more_x(), sy + 6, 36, 36)) { sheet_for(current); return; }
    if (my < sy + HEADER_H) return;
    int x, y, w, h;
    app_origin(current, &x, &y, &w, &h);
    switch (current) {
    case APP_FILES: {
        int ly = files_list_y();
        for (int k = 0; k < files_rows() && files_first + k < files_n; k++)
            if (hit(mx, my, files_list_x(), ly + k * ROW_H, files_list_w(), ROW_H)) { files_sel = files_first + k; files_open(files_first + k); return; }
        break;
    }
    case APP_EDITOR: editor_tap(mx, my); break;
    case APP_SETTINGS: settings_tap(mx, my); break;
    case APP_MINES: if (mines_click(x, y, mx, my, 0)) dirty = 1; break;
    case APP_WORDLE: if (wordle_click(x, y, mx, my)) dirty = 1; break;
    case APP_BREAKOUT: bo_click(); dirty = 1; break;
    case APP_ABOUT: if (about_button(x, y, w, mx, my)) shut_down(); break;
    case APP_TODO: {
        int row = y + 12, cx = MODERN ? x + 10 : x + 1, cw = MODERN ? w - 20 : w - 2;
        for (int i = 0; i < todo_count() && row + i * ROW_H + ROW_H < y + h - 80; ++i)
            if (hit(mx, my, cx, row + i * ROW_H, cw, ROW_H)) { if (todo_toggle(i)) dirty = 1; return; }
        todo_focus_field();
        dirty = 1;
        break;
    }
    default: break;
    }
}

static void wheel(int delta) {
    if (current == APP_FILES) { files_first += delta * 3; dirty = 1; }
    else if (current == APP_EDITOR) {
        ed_top += delta * 3;
        if (ed_top < 0) ed_top = 0;
        int last = ed_line_of(ed_len);
        if (ed_top > last) ed_top = last;
        /* Keep the caret on screen so the view does not jump back. */
        int line = ed_line_of(ed_caret), rows = ed_rows();
        if (line < ed_top) ed_caret = ed_index_of_line(ed_top);
        else if (line >= ed_top + rows) ed_caret = ed_index_of_line(ed_top + rows - 1);
        dirty = 1;
    }
}

static void home_key(int scancode, char character) {
    int list[APP_COUNT], n = home_matches(list, APP_COUNT), columns = 4;
    int next = selected;
    if (scancode == KEY_LEFT) next--;
    else if (scancode == KEY_RIGHT) next++;
    else if (scancode == KEY_UP) next -= columns;
    else if (scancode == KEY_DOWN) next += columns;
    else if (scancode == KEY_ENTER) { if (selected >= 0 && selected < n) open_app(list[selected]); return; }
    else if (scancode == KEY_BACKSPACE) { if (search_len) { search[--search_len] = 0; selected = 0; dirty = 1; } return; }
    else if (character >= 32 && character < 127 && search_len < (int)sizeof search - 1) {
        search[search_len++] = character;
        search[search_len] = 0;
        selected = 0;
        dirty = 1;
        return;
    }
    if (next >= 0 && next < n && next != selected) { selected = next; dirty = 1; }
}

static void editor_key(int scancode, char character) {
    int ctrl = (modifiers & INPUT_CTRL) != 0;
    if (find_open) {
        if (scancode == KEY_ENTER) ed_find_next();
        else if (scancode == KEY_BACKSPACE) { if (find_len) find_text[--find_len] = 0; }
        else if (character >= 32 && character < 127 && !ctrl && find_len < (int)sizeof find_text - 1) {
            find_text[find_len++] = character;
            find_text[find_len] = 0;
        }
        dirty = 1;
        return;
    }
    if (ctrl && scancode == 0x1F) { ed_save(); return; }              /* Ctrl+S */
    if (ctrl && scancode == 0x21) { find_open = 1; dirty = 1; return; } /* Ctrl+F */
    if (scancode == KEY_LEFT) { if (ed_caret > 0) ed_caret--; }
    else if (scancode == KEY_RIGHT) { if (ed_caret < ed_len) ed_caret++; }
    else if (scancode == KEY_UP) ed_vertical(-1);
    else if (scancode == KEY_DOWN) ed_vertical(1);
    else if (scancode == 0x49) ed_vertical(-ed_rows());
    else if (scancode == 0x51) ed_vertical(ed_rows());
    else if (scancode == 0x47) ed_caret = ed_line_start(ed_caret);
    else if (scancode == 0x4F) { while (ed_caret < ed_len && ed_buf[ed_caret] != '\n') ed_caret++; }
    else if (scancode == KEY_BACKSPACE) { if (ed_caret > 0) ed_delete(ed_caret - 1); }
    else if (scancode == 0x53) ed_delete(ed_caret);
    else if (scancode == KEY_ENTER) ed_insert("\n", 1);
    else if (scancode == KEY_TAB) ed_insert("  ", 2);
    else if (character >= 32 && character < 127 && !ctrl) ed_insert(&character, 1);
    else return;
    ed_follow();
    dirty = 1;
}

static void key(int scancode, char character) {
    if (scancode == KEY_ESC) { go_back(); return; }
    if (sheet_open) {
        int n = sheet_n, i = sheet_sel;
        if (scancode == KEY_UP || scancode == KEY_DOWN) {
            do i = (i + (scancode == KEY_UP ? n - 1 : 1)) % n; while (sheet[i].action == ACT_SEP);
            sheet_sel = i;
            dirty = 1;
        } else if (scancode == KEY_ENTER) sheet_run(sheet_sel);
        return;
    }
    if (current == HOME) { home_key(scancode, character); return; }
    if (current == SWITCHER) {
        if (scancode == KEY_ENTER && open_n) { current = HOME; open_app(open_order[0]); }
        return;
    }
    /* F10 or the menu key opens the app's menu sheet. */
    if (scancode == 0x44 || scancode == 0x5D) { sheet_for(current); return; }
    int changed = 0;
    switch (current) {
    case APP_FILES:
        if (scancode == KEY_DOWN || scancode == KEY_UP) {
            int s = files_sel + (scancode == KEY_DOWN ? 1 : -1);
            if (s >= 0 && s < files_n) files_sel = s;
            if (files_sel < files_first) files_first = files_sel;
            if (files_sel >= files_first + files_rows()) files_first = files_sel - files_rows() + 1;
            changed = 1;
        } else if (scancode == KEY_ENTER) files_open(files_sel);
        else if (scancode == KEY_BACKSPACE) go_back();
        break;
    case APP_EDITOR: editor_key(scancode, character); break;
    case APP_SETTINGS:
        if (scancode == KEY_LEFT || scancode == KEY_RIGHT) set_style(scancode == KEY_RIGHT, ui_accent);
        else if (scancode == KEY_UP || scancode == KEY_DOWN)
            set_style(ui_style, (ui_accent + (scancode == KEY_DOWN ? 1 : UI_ACCENT_N - 1)) % UI_ACCENT_N);
        break;
    case APP_2048: changed = g2048_key(scancode); break;
    case APP_BREAKOUT: changed = bo_key(scancode); break;
    case APP_MINES:
        if (scancode == KEY_SPACE || scancode == KEY_ENTER) { mines_new(timer_ticks()); changed = 1; }
        break;
    case APP_WORDLE: {
        int action = scancode == KEY_ENTER ? WORDLE_ACT_ENTER :
                     scancode == KEY_BACKSPACE ? WORDLE_ACT_DELETE : WORDLE_ACT_NONE;
        changed = wordle_key(action, character);
        break;
    }
    case APP_TODO:
        if (scancode == KEY_ENTER) changed = todo_field_enter();
        else if (scancode == KEY_BACKSPACE) changed = todo_field_backspace();
        else if (character) changed = todo_field_char(character);
        break;
    case APP_ABOUT:
        if (scancode == KEY_ENTER) shut_down();
        break;
    default: break;
    }
    if (changed) dirty = 1;
}

/* Pop the kernel's ordered input stream. Pocket keeps no gesture state beyond
 * the left button, so an input loss only clears that. */
static int pocket_input(void) {
    drain_8042();
    InputSample sample;
    int moved = 0, count = 0;
    while (count < (int)INPUT_BATCH && input_pop(&device_input, &sample)) {
        ++count;
        modifiers = sample.modifiers;
        if (sample.kind == INPUT_RESET) {
            pointer_down = 0;
            continue;
        }
        if (sample.kind == INPUT_POINTER) {
            if (sample.x != pocket_mouse_x || sample.y != pocket_mouse_y) moved = 1;
            pocket_mouse_x = sample.x;
            pocket_mouse_y = sample.y;
            if (current == APP_BREAKOUT) {
                int x, y, w, h;
                app_origin(APP_BREAKOUT, &x, &y, &w, &h);
                bo_mouse(x, pocket_mouse_x);
            }
            if (sample.wheel) wheel(sample.wheel > 0 ? 1 : -1);
            int down = !!(sample.buttons & INPUT_LEFT);
            if (down && !pointer_down) tap(pocket_mouse_x, pocket_mouse_y);
            if (down != pointer_down) dirty = 1;
            pointer_down = down;
        } else if (sample.kind == INPUT_KEY && (sample.flags & INPUT_MAKE)) {
            key((int)sample.scancode, (char)sample.character);
        }
    }
    if (moved) {
        int h = hot_index();
        if (h != hot_last) { hot_last = h; dirty = 1; }
    }
    return moved;
}

static void present_scene(void) {
    cursor_hide();
    draw_scene();
    hot_last = hot_index();
    cursor_show();
    gfx_present();
    dirty = 0;
    if (!ready) {
        ready = 1;
        kprint_debug("POCKET-READY\n");
    }
}

static void pocket_prepare(void) {
    provision_system_files(seed_scratch, 0x10000);
    int v;
    if ((v = pref_read("style", UI_STYLE_COUNT)) >= 0) ui_style = v;
    if ((v = pref_read("accent", UI_ACCENT_N)) >= 0) ui_accent = v;
    pocket_theme();
    todo_load();
    pocket_layout();
    files_cwd = fs_root();
    for (int i = 0; i < APP_COUNT; i++) back_to[i] = HOME;
    pocket_mouse_x = fb_w / 2;
    pocket_mouse_y = fb_h / 2;
    (void)pocket_grid_x; (void)pocket_grid_y; (void)pocket_cell_w; (void)pocket_cell_h; (void)pocket_columns;
}

static void pocket_start(void) {
    pocket_mouse_x = device_input.x;
    pocket_mouse_y = device_input.y;
    last_tick = timer_ticks();
    present_scene();
}

static int pocket_turn(void) {
    if (kernel_events_take(KERNEL_EVENT_STORAGE)) dirty = 1;
    if (todo_tick() && current == APP_TODO) dirty = 1;
    int moved = pocket_input();
    unsigned now = timer_ticks();
    if (current == APP_BREAKOUT && now != last_tick) {
        bo_tick();
        dirty = 1;
    }
    last_tick = now;
    /* Carets blink twice a second. */
    if ((current == HOME && search_len) || current == APP_EDITOR || current == APP_TODO) {
        static unsigned blink = ~0u;
        unsigned phase = now / (TIMER_HZ / 2);
        if (phase != blink) { blink = phase; dirty = 1; }
    }
    if (notice[0] && now > notice_until && current != HOME) { notice = ""; dirty = 1; }
    unsigned second = now / TIMER_HZ;
    if (second != last_second) {
        /* The status bar shows wall-clock minutes, so follow the RTC rather
         * than minutes since boot. */
        RtcTime wall;
        rtc_read(&wall);
        unsigned minute = (unsigned)(wall.hour * 60 + wall.min);
        if (current == APP_CLOCK || minute != last_minute) dirty = 1;
        last_minute = minute;
    }
    last_second = second;
    if (dirty) {
        present_scene();
    } else if (moved) {
        int old_x = cursor_x, old_y = cursor_y;
        cursor_hide();
        cursor_show();
        flip_rect(old_x, old_y, CURSOR_ART_W, CURSOR_ART_H);
        flip_rect(cursor_x, cursor_y, CURSOR_ART_W, CURSOR_ART_H);
    }
    /* Start saving only after the frame is on screen; a floppy snapshot can
     * hold the CPU for a moment. */
    if (!pointer_down && fs_needs_sync()) fs_autosync();
    /* Breakout advances once per timer tick; the tick's interrupt wakes the
     * kernel from hlt, so idle here instead of spinning. */
    return dirty;
}

const Shell baseos_shell = {"pocket", pocket_prepare, pocket_start, pocket_turn};
