/* Pocket: a small phone-style shell.
 *
 * Pocket is the second implementation of the kernel/shell contract in
 * kernel/shell.h. It runs on the same kernel as the desktop and hosts the same
 * app components (Clock, 2048, Mines, Wordle, Breakout and the Todo model)
 * full screen inside a portrait viewport, with a home screen instead of
 * windows. It uses only public kernel headers and never touches desktop code.
 * Build it with `make pocket`; see docs/ARCHITECTURE.md. */
#include "kernel/kernel.h"
#include "kernel/shell.h"
#include "kernel/gfx.h"
#include "kernel/font.h"
#include "kernel/fs.h"
#include "kernel/platform.h"
#include "kernel/rtc.h"
#include "ui/app.h"
#include "ui/app_icons.h"
#include "ui/icon_draw.h"
#include "ui/theme.h"
#include "lib/provision.h"
#include "apps/clock.h"
#include "apps/game2048.h"
#include "apps/mines.h"
#include "apps/wordle.h"
#include "apps/breakout.h"
#include "apps/todo.h"
#include "assets/cursor_art.h"
#include "build_info.h"

#define INK(i) ((uint8_t)(PAL_GRAY + (i)))
#define CHAR_H UI_FONT_H
#define STATUS_H 30
#define HOME_BAR_H 26
#define HEADER_H 46
#define TILE 112
#define HOME -1

enum { APP_CLOCK, APP_TODO, APP_2048, APP_MINES, APP_WORDLE, APP_BREAKOUT, APP_ABOUT, APP_COUNT };
typedef struct { const char *name; int icon; int width, height; } PocketApp;
/* Fixed-size components keep their own geometry; zero means "fill the screen". */
static const PocketApp apps[APP_COUNT] = {
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
static unsigned last_minute = ~0u, last_second = ~0u, last_tick;
static uint8_t accent, accent_dark, on_accent;
static uint8_t cursor_saved[CURSOR_ART_W * CURSOR_ART_H];
static int cursor_x = -1, cursor_y = -1, cursor_shown;
static const char *notice = "";
/* Seed buffer for first-boot sample files; the desktop's editor arena is free
 * because the desktop is not linked into this image. */
static char *const seed_scratch = (char *)EDITOR_BASE;

static void pocket_theme(void) {
    /* The shared theme with a green accent, so app components draw with
     * this shell's colors. */
    ui_theme_install(UI_STYLE_MODERN, 2, 0);
    accent = TK_ACCENT;
    accent_dark = TK_ACCENT_700;
    on_accent = TK_ON_ACCENT;
}

static void pocket_layout(void) {
    phone_h = fb_h - 40;
    if (phone_h > 900) phone_h = 900;
    phone_w = phone_h * 9 / 16;
    if (phone_w < BO_W + 24) phone_w = BO_W + 24;
    if (phone_w > fb_w - 16) phone_w = fb_w - 16;
    phone_x = (fb_w - phone_w) / 2;
    phone_y = (fb_h - phone_h) / 2;
}

/* Screen area below the status bar and above the home indicator. */
static void screen_rect(int *x, int *y, int *w, int *h) {
    *x = phone_x;
    *y = phone_y + STATUS_H;
    *w = phone_w;
    *h = phone_h - STATUS_H - HOME_BAR_H;
}

static void home_cell(int i, int *x, int *y) {
    int sx, sy, sw, sh;
    screen_rect(&sx, &sy, &sw, &sh);
    int columns = sw / TILE;
    if (columns > 4) columns = 4;
    int left = sx + (sw - columns * TILE) / 2;
    *x = left + (i % columns) * TILE;
    *y = sy + 92 + (i / columns) * (TILE + 8);
}

static int home_columns(void) {
    int columns = phone_w / TILE;
    return columns > 4 ? 4 : columns;
}

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

static void draw_status_bar(void) {
    RtcTime now;
    rtc_read(&now);
    char clock[6];
    two_digits(clock, now.hour);
    clock[2] = ':';
    two_digits(clock + 3, now.min);
    clock[5] = 0;
    int y = phone_y + (STATUS_H - CHAR_H) / 2 + 2;
    draw_string_bold(clock, phone_x + 22, y, INK(29));
    const char *storage = fs_needs_sync() ? "Saving" : "Saved";
    draw_string(storage, phone_x + phone_w - 22 - ui_string_w(storage), y, INK(18));
    const char *label = "BaseOS";
    draw_string(label, phone_x + (phone_w - ui_string_w(label)) / 2, y, INK(18));
}

static void draw_home_bar(void) {
    int w = 128, y = phone_y + phone_h - HOME_BAR_H / 2 - 3;
    draw_round_rect(phone_x + (phone_w - w) / 2, y, w, 6, 3, INK(current == HOME ? 12 : 24));
}

static void draw_home(void) {
    int sx, sy, sw, sh;
    screen_rect(&sx, &sy, &sw, &sh);
    draw_logo_string("BaseOS", sx + 24, sy + 18, INK(29));
    draw_string("Pocket shell. Same kernel, same apps.", sx + 26, sy + 18 + LOGO_FONT_H + 4, INK(18));
    for (int i = 0; i < APP_COUNT; ++i) {
        int x, y;
        home_cell(i, &x, &y);
        if (i == selected)
            draw_round_rect(x + 6, y - 4, TILE - 12, TILE, 14, INK(4));
        int ix = x + (TILE - AI_BIG) / 2 - 10;
        draw_round_rect(ix, y + 6, AI_BIG + 20, AI_BIG + 20, 14, INK(0));
        draw_round_frame(ix, y + 6, AI_BIG + 20, AI_BIG + 20, 14, INK(8));
        app_icon_draw(apps[i].icon, ix + 10, y + 16, AI_BIG);
        const char *name = apps[i].name;
        draw_string(name, x + (TILE - ui_string_w(name)) / 2, y + AI_BIG + 34, INK(27));
    }
    if (notice[0])
        draw_string(notice, sx + 24, sy + sh - CHAR_H - 12, INK(18));
}

static void draw_header(int app) {
    int sx, sy, sw, sh;
    screen_rect(&sx, &sy, &sw, &sh);
    (void)sh;
    draw_rect(sx, sy, sw, HEADER_H, INK(2));
    draw_hline(sx, sy + HEADER_H - 1, sw, INK(8));
    draw_string("< Home", sx + 16, sy + (HEADER_H - CHAR_H) / 2, accent);
    const char *name = apps[app].name;
    draw_string_bold(name, sx + (sw - uib_string_w(name)) / 2, sy + (HEADER_H - CHAR_H) / 2, INK(29));
}

static void draw_todo_screen(int x, int y, int w, int h) {
    draw_rect(x, y, w, h, app_body);
    int row = y + 14;
    int count = todo_count();
    for (int i = 0; i < count && row + 40 < y + h - 70; ++i) {
        draw_round_rect(x + 14, row, w - 28, 36, 8, INK(0));
        int box = x + 26, by = row + 9;
        if (todo_done(i)) draw_round_rect(box, by, 18, 18, 5, accent);
        else draw_round_frame(box, by, 18, 18, 5, INK(14));
        draw_string_clip(todo_text(i), box + 30, row + (36 - CHAR_H) / 2,
                         todo_done(i) ? INK(16) : INK(28), x + w - 26);
        row += 42;
    }
    if (!count)
        draw_string("Nothing to do yet. Type below and press Enter.", x + 22, row + 6, INK(18));
    int fy = y + h - 58;
    draw_round_rect(x + 14, fy, w - 28, 40, 10, INK(0));
    draw_round_frame(x + 14, fy, w - 28, 40, 10, todo_field_active() ? accent : INK(10));
    const char *field = todo_field();
    if (field[0]) draw_string_clip(field, x + 28, fy + (40 - CHAR_H) / 2, INK(29), x + w - 30);
    else draw_string("New item", x + 28, fy + (40 - CHAR_H) / 2, INK(16));
    const char *status = todo_status();
    if (status && status[0]) draw_string_clip(status, x + 18, fy - CHAR_H - 6, INK(18), x + w - 18);
}

static void about_line(int x, int *y, const char *label, const char *value) {
    draw_string(label, x, *y, INK(18));
    draw_string_bold(value, x + 150, *y, INK(29));
    *y += CHAR_H + 12;
}

static void draw_about_screen(int x, int y, int w, int h) {
    draw_rect(x, y, w, h, app_body);
    char number[16], text[48];
    int row = y + 24;
    about_line(x + 24, &row, "Shell", baseos_shell.name);
    about_line(x + 24, &row, "Build", BASEOS_BUILD_LABEL);
    fmt_uint(number, platform_memory_mb());
    kstrcpy(text, number); kstrcpy(text + kstrlen(text), " MiB");
    about_line(x + 24, &row, "Memory", text);
    fmt_uint(number, (unsigned)fb_w); kstrcpy(text, number); kstrcpy(text + kstrlen(text), " x ");
    fmt_uint(number, (unsigned)fb_h); kstrcpy(text + kstrlen(text), number);
    about_line(x + 24, &row, "Display", text);
    fmt_uint(number, (unsigned)fs_node_count()); kstrcpy(text, number); kstrcpy(text + kstrlen(text), " of ");
    fmt_uint(number, (unsigned)fs_node_limit()); kstrcpy(text + kstrlen(text), number);
    about_line(x + 24, &row, "Files", text);
    const char *storage = fs_storage_status();
    about_line(x + 24, &row, "Storage", storage ? storage : "Ready");
    row += 8;
    draw_string("Pocket hosts the same app components", x + 24, row, INK(20)); row += CHAR_H + 4;
    draw_string("as the desktop, on the same kernel.", x + 24, row, INK(20)); row += CHAR_H + 24;
    draw_round_rect(x + 24, row, w - 48, 40, 10, accent);
    const char *off = "Shut down";
    draw_string_bold(off, x + (w - uib_string_w(off)) / 2, row + (40 - CHAR_H) / 2, on_accent);
}

static int about_button(int x, int y, int w, int mx, int my) {
    int row = y + 24 + 6 * (CHAR_H + 12) + 8 + 2 * (CHAR_H + 4) + 20;
    return hit(mx, my, x + 24, row, w - 48, 40);
}

static void draw_app(int app) {
    draw_header(app);
    int sx, sy, sw, sh;
    screen_rect(&sx, &sy, &sw, &sh);
    draw_rect(sx, sy + HEADER_H, sw, sh - HEADER_H, app_body);
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

static void draw_scene(void) {
    /* Flat colors: an 8-bit palette bands a dark full-screen gradient. */
    draw_rect(0, 0, fb_w, fb_h, INK(30));
    draw_round_rect(phone_x - 10, phone_y - 10, phone_w + 20, phone_h + 20, 34, COLOR_BLACK);
    draw_round_rect(phone_x, phone_y, phone_w, phone_h, 26, INK(1));
    draw_status_bar();
    if (current == HOME) draw_home();
    else draw_app(current);
    draw_home_bar();
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

static void open_app(int app) {
    current = app;
    notice = "";
    if (app == APP_TODO) todo_focus_field();
    if (app == APP_WORDLE) wordle_new_game(timer_ticks());
    kprint_debug("POCKET-OPEN ");
    kprint_debug(apps[app].name);
    kprint_debug("\n");
    dirty = 1;
}

static void go_home(void) {
    if (current == HOME) return;
    selected = current;
    current = HOME;
    kprint_debug("POCKET-HOME\n");
    dirty = 1;
}

static void shut_down(void) {
    if (todo_prepare_shutdown() < 0 || fs_sync() < 0) {
        notice = "Shutdown paused: storage is not ready.";
        go_home();
        return;
    }
    kprint_debug("POCKET-SHUTDOWN\n");
    kernel_power_off();
}

static void tap(int mx, int my) {
    if (hit(mx, my, phone_x, phone_y + phone_h - HOME_BAR_H, phone_w, HOME_BAR_H)) {
        go_home();
        return;
    }
    int sx, sy, sw, sh;
    screen_rect(&sx, &sy, &sw, &sh);
    if (current == HOME) {
        for (int i = 0; i < APP_COUNT; ++i) {
            int x, y;
            home_cell(i, &x, &y);
            if (hit(mx, my, x, y, TILE, TILE)) {
                selected = i;
                open_app(i);
                return;
            }
        }
        return;
    }
    if (hit(mx, my, sx, sy, 110, HEADER_H)) {
        go_home();
        return;
    }
    int x, y, w, h;
    app_origin(current, &x, &y, &w, &h);
    switch (current) {
    case APP_MINES: if (mines_click(x, y, mx, my, 0)) dirty = 1; break;
    case APP_WORDLE: if (wordle_click(x, y, mx, my)) dirty = 1; break;
    case APP_BREAKOUT: bo_click(); dirty = 1; break;
    case APP_ABOUT: if (about_button(x, y, w, mx, my)) shut_down(); break;
    case APP_TODO: {
        int row = y + 14;
        for (int i = 0; i < todo_count() && row + 40 < y + h - 70; ++i, row += 42)
            if (hit(mx, my, x + 14, row, w - 28, 36)) { if (todo_toggle(i)) dirty = 1; return; }
        todo_focus_field();
        dirty = 1;
        break;
    }
    default: break;
    }
}

static void home_key(int scancode) {
    int columns = home_columns();
    int next = selected;
    if (scancode == KEY_LEFT) next--;
    else if (scancode == KEY_RIGHT) next++;
    else if (scancode == KEY_UP) next -= columns;
    else if (scancode == KEY_DOWN) next += columns;
    else if (scancode == KEY_ENTER) { open_app(selected); return; }
    if (next >= 0 && next < APP_COUNT && next != selected) {
        selected = next;
        dirty = 1;
    }
}

static void key(int scancode, char character) {
    if (scancode == KEY_ESC) {
        if (current != HOME) go_home();
        return;
    }
    if (current == HOME) {
        home_key(scancode);
        return;
    }
    int changed = 0;
    switch (current) {
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
            int down = !!(sample.buttons & INPUT_LEFT);
            if (down && !pointer_down) tap(pocket_mouse_x, pocket_mouse_y);
            pointer_down = down;
        } else if (sample.kind == INPUT_KEY && (sample.flags & INPUT_MAKE)) {
            key((int)sample.scancode, (char)sample.character);
        }
    }
    return moved;
}

static void present_scene(void) {
    cursor_hide();
    draw_scene();
    cursor_show();
    gfx_present();
    dirty = 0;
    if (!ready) {
        ready = 1;
        kprint_debug("POCKET-READY\n");
    }
}

static void pocket_prepare(void) {
    icon_palette_install();
    pocket_theme();
    provision_system_files(seed_scratch, 0x10000);
    todo_load();
    pocket_layout();
    pocket_mouse_x = fb_w / 2;
    pocket_mouse_y = fb_h / 2;
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
