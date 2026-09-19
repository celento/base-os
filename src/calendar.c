#include "app.h"
#include "calendar.h"

static int view_year;
static int view_month;
static int inited;

void cal_reset(void) {
    RtcTime t;
    rtc_read(&t);
    view_year = t.year;
    view_month = t.month;
    inited = 1;
}

static void step(int dir) {
    view_month += dir;
    if (view_month < 1) {
        view_month = 12;
        view_year--;
    } else if (view_month > 12) {
        view_month = 1;
        view_year++;
    }
}

static void nav_rects(int bx, int by, int bw, int *px, int *nx, int *y) {
    *y = by + 14;
    *px = bx + 16;
    *nx = bx + bw - 16 - 28;
}

static void draw_nav(int x, int y, int dir) {
    draw_round_rect(x, y, 28, 28, 6, app_chrome_dk);
    draw_round_rect(x + 1, y + 1, 26, 26, 5, COLOR_WHITE);
    int cx = x + 14, cy = y + 14;
    for (int i = 0; i < 5; i++) {
        int xx = dir < 0 ? cx + 2 - i : cx - 2 + i;
        draw_line(xx, cy - i, xx, cy + i, app_text);
    }
}

void cal_draw(int bx, int by, int bw, int bh) {
    if (!inited)
        cal_reset();
    RtcTime t;
    rtc_read(&t);
    draw_rect(bx, by, bw, bh, COLOR_WHITE);
    int px, nx, ny;
    nav_rects(bx, by, bw, &px, &nx, &ny);
    draw_nav(px, ny, -1);
    draw_nav(nx, ny, 1);

    char title[32];
    int n = 0;
    const char *m = rtc_month_name[view_month - 1];
    while (*m) title[n++] = *m++;
    title[n++] = ' ';
    char num[12];
    fmt_uint(num, (unsigned)view_year);
    for (int i = 0; num[i]; i++) title[n++] = num[i];
    title[n] = 0;
    draw_string_bold(title, bx + (bw - uib_string_w(title)) / 2, ny + (28 - UI_FONT_H) / 2, app_text);

    int gx = bx + 16;
    int gy = ny + 44;
    int cw = (bw - 32) / 7;
    static const char *dn[7] = { "Su", "Mo", "Tu", "We", "Th", "Fr", "Sa" };
    for (int i = 0; i < 7; i++)
        draw_string(dn[i], gx + i * cw + (cw - ui_string_w(dn[i])) / 2, gy, app_text_dim);
    draw_hline(gx, gy + UI_FONT_H + 6, cw * 7, app_chrome_dk);
    gy += UI_FONT_H + 12;

    int first = rtc_weekday(view_year, view_month, 1);
    int days = rtc_days_in_month(view_year, view_month);
    int ch = (bh - (gy - by) - 12) / 6;
    if (ch < 20) ch = 20;
    for (int d = 1; d <= days; d++) {
        int idx = first + d - 1;
        int col = idx % 7;
        int row = idx / 7;
        int x = gx + col * cw;
        int y = gy + row * ch;
        int today = (view_year == t.year && view_month == t.month && d == t.day);
        char s[4];
        fmt_uint(s, (unsigned)d);
        int tw = ui_string_w(s);
        if (today) {
            int sz = ch - 4 < cw - 4 ? ch - 4 : cw - 4;
            draw_round_rect(x + (cw - sz) / 2, y + (ch - sz) / 2, sz, sz, sz / 2, app_accent);
            draw_string_bold(s, x + (cw - uib_string_w(s)) / 2, y + (ch - UI_FONT_H) / 2, COLOR_WHITE);
        } else {
            uint8_t col_ink = (col == 0 || col == 6) ? app_text_dim : app_text;
            draw_string(s, x + (cw - tw) / 2, y + (ch - UI_FONT_H) / 2, col_ink);
        }
    }
}

int cal_click(int bx, int by, int bw, int mx, int my) {
    int px, nx, ny;
    nav_rects(bx, by, bw, &px, &nx, &ny);
    if (hit(mx, my, px, ny, 28, 28)) {
        step(-1);
        return 1;
    }
    if (hit(mx, my, nx, ny, 28, 28)) {
        step(1);
        return 1;
    }
    return 0;
}

int cal_key(int sc) {
    if (sc == KEY_LEFT) {
        step(-1);
        return 1;
    }
    if (sc == KEY_RIGHT) {
        step(1);
        return 1;
    }
    return 0;
}
