/* Clock: an analog face with the digital time and date under it, and a
 * status bar with the week and day of the year. */
#include "ui/app.h"
#include "apps/clock.h"
#include "apps/pane_chrome.h"

static const int sin60[60] = {
    0, 105, 208, 309, 407, 500, 588, 669, 743, 809, 866, 914, 951, 978, 995,
    1000, 995, 978, 951, 914, 866, 809, 743, 669, 588, 500, 407, 309, 208, 105,
    0, -105, -208, -309, -407, -500, -588, -669, -743, -809, -866, -914, -951, -978, -995,
    -1000, -995, -978, -951, -914, -866, -809, -743, -669, -588, -500, -407, -309, -208, -105
};

static int csin(int step) { return sin60[((step % 60) + 60) % 60]; }
static int ccos(int step) { return sin60[((step + 15) % 60 + 60) % 60]; }

static void thick_line(int x0, int y0, int x1, int y1, uint8_t col) {
    draw_line(x0, y0, x1, y1, col);
    draw_line(x0 + 1, y0, x1 + 1, y1, col);
    draw_line(x0, y0 + 1, x1, y1 + 1, col);
}

static void hand(int cx, int cy, int step, int len, int tail, int thick, uint8_t col) {
    int x0 = cx - csin(step) * tail / 1000;
    int y0 = cy + ccos(step) * tail / 1000;
    int x1 = cx + csin(step) * len / 1000;
    int y1 = cy - ccos(step) * len / 1000;
    if (thick)
        thick_line(x0, y0, x1, y1, col);
    else
        draw_line(x0, y0, x1, y1, col);
}

static int leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static int day_of_year(int y, int m, int d) {
    static const int before[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    return before[(m - 1) % 12] + d + (m > 2 && leap(y));
}
/* ISO 8601 week number: weeks start on Monday, week 1 holds the year's
 * first Thursday. */
static int iso_week(int y, int m, int d) {
    int wd = rtc_weekday(y, m, d);
    if (wd == 0) wd = 7;
    int week = (day_of_year(y, m, d) - wd + 10) / 7;
    if (week < 1) {
        int jan1 = rtc_weekday(y - 1, 1, 1);
        return jan1 == 4 || (jan1 == 3 && leap(y - 1)) ? 53 : 52;
    }
    if (week == 53) {
        int jan1 = rtc_weekday(y, 1, 1);
        if (!(jan1 == 4 || (jan1 == 3 && leap(y)))) return 1;
    }
    return week;
}

void clock_draw(int bx, int by, int bw, int bh) {
    RtcTime t;
    rtc_read(&t);
    int modern = PANE_MODERN;
    draw_rect(bx, by, bw, bh, ui_theme.content);

    int body = pane_status_y(by, bh) - by;
    int r = (bw < body - 110 ? bw : body - 110) / 2 - 24;
    if (r > 120) r = 120;
    if (r < 40) r = 40;
    int cx = bx + bw / 2;
    int cy = by + 24 + r;
    /* Face: a raised disc with a soft ring (Modern) or a 1px rule
     * (Classic). */
    if (modern) kit_fill(cx - r - 4, cy - r - 4, 2 * r + 8, 2 * r + 8, r + 4, TK_SUNKEN, 1);
    kit_box(cx - r, cy - r, 2 * r, 2 * r, r, TK_RAISED, modern ? TK_LINE : TK_LINE_STRONG);
    for (int i = 0; i < 60; i++) {
        int hour = i % 5 == 0, len = hour ? 10 : 4;
        int x0 = cx + csin(i) * (r - 8) / 1000;
        int y0 = cy - ccos(i) * (r - 8) / 1000;
        int x1 = cx + csin(i) * (r - 8 - len) / 1000;
        int y1 = cy - ccos(i) * (r - 8 - len) / 1000;
        if (hour)
            thick_line(x1, y1, x0, y0, TK_TEXT);
        else
            draw_line(x1, y1, x0, y0, TK_LINE_STRONG);
    }
    static const char *const num[4] = {"12", "3", "6", "9"};
    for (int q = 0; q < 4; q++) {
        int step = q * 15, rr = r - 34;
        int nx = cx + csin(step) * rr / 1000, ny = cy - ccos(step) * rr / 1000;
        kit_text_center(num[q], nx - 15, ny - UI_FONT_H / 2, 30, TK_TEXT2, 1);
    }
    int hstep = (t.hour % 12) * 5 + t.min / 12;
    hand(cx, cy, hstep, r * 50 / 100, 8, 1, TK_TEXT);
    hand(cx, cy, t.min, r * 75 / 100, 10, 1, TK_TEXT);
    hand(cx, cy, t.sec, r * 82 / 100, 16, 0, TK_ACCENT);
    kit_fill(cx - 5, cy - 5, 10, 10, 5, TK_ACCENT, 1);
    kit_fill(cx - 2, cy - 2, 4, 4, 2, TK_ON_ACCENT, 0);

    char buf[16];
    fmt_pad2(buf, t.hour);
    buf[2] = ':';
    fmt_pad2(buf + 3, t.min);
    buf[5] = ':';
    fmt_pad2(buf + 6, t.sec);
    int ty = cy + r + (modern ? 22 : 18);
    int tw = logo_string_w(buf);
    draw_logo_string(buf, cx - tw / 2, ty, TK_TEXT);

    char date[48], n1[12], n2[12];
    fmt_uint(n1, (unsigned)t.day);
    fmt_uint(n2, (unsigned)t.year);
    pane_join(date, sizeof date, rtc_day_name[t.wday], ", ", rtc_month_name[t.month - 1]);
    pane_append(date, sizeof date, " "); pane_append(date, sizeof date, n1);
    pane_append(date, sizeof date, ", "); pane_append(date, sizeof date, n2);
    kit_text_center(date, bx, ty + LOGO_FONT_H + 6, bw, TK_TEXT2, 0);

    char right[40];
    fmt_uint(n1, (unsigned)iso_week(t.year, t.month, t.day));
    fmt_uint(n2, (unsigned)day_of_year(t.year, t.month, t.day));
    pane_join(right, sizeof right, "Week ", n1, ", day ");
    pane_append(right, sizeof right, n2);
    pane_statusbar(bx, by, bw, bh, "Local time, 24-hour", TK_TEXT2, right, TK_TEXT2);
}
