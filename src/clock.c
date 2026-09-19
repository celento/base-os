#include "app.h"
#include "clock.h"

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

static void hand(int cx, int cy, int step, int len, int thick, uint8_t col) {
    int x1 = cx + csin(step) * len / 1000;
    int y1 = cy - ccos(step) * len / 1000;
    if (thick)
        thick_line(cx, cy, x1, y1, col);
    else
        draw_line(cx, cy, x1, y1, col);
}

void clock_draw(int bx, int by, int bw, int bh) {
    RtcTime t;
    rtc_read(&t);
    draw_rect(bx, by, bw, bh, app_chrome);

    int r = 120;
    int cx = bx + bw / 2;
    int cy = by + 24 + r;
    draw_round_rect(cx - r - 2, cy - r - 2, 2 * r + 4, 2 * r + 4, r + 2, gfx_gray(0x70));
    draw_round_rect(cx - r, cy - r, 2 * r, 2 * r, r, COLOR_WHITE);
    for (int i = 0; i < 60; i++) {
        int len = (i % 5 == 0) ? 12 : 5;
        int x0 = cx + csin(i) * (r - 8) / 1000;
        int y0 = cy - ccos(i) * (r - 8) / 1000;
        int x1 = cx + csin(i) * (r - 8 - len) / 1000;
        int y1 = cy - ccos(i) * (r - 8 - len) / 1000;
        if (i % 5 == 0)
            thick_line(x1, y1, x0, y0, gfx_gray(0x30));
        else
            draw_line(x1, y1, x0, y0, gfx_gray(0xA0));
    }
    int hstep = (t.hour % 12) * 5 + t.min / 12;
    hand(cx, cy, hstep, r - 52, 1, gfx_gray(0x20));
    hand(cx, cy, t.min, r - 30, 1, gfx_gray(0x20));
    hand(cx, cy, t.sec, r - 22, 0, gfx_rgb(0xE0, 0x40, 0x40));
    draw_round_rect(cx - 5, cy - 5, 10, 10, 5, gfx_rgb(0xE0, 0x40, 0x40));
    draw_round_rect(cx - 2, cy - 2, 4, 4, 2, COLOR_WHITE);

    char buf[16];
    fmt_pad2(buf, t.hour);
    buf[2] = ':';
    fmt_pad2(buf + 3, t.min);
    buf[5] = ':';
    fmt_pad2(buf + 6, t.sec);
    int ty = cy + r + 22;
    int tw = logo_string_w(buf);
    draw_logo_string(buf, cx - tw / 2, ty, app_text);

    char date[48];
    int n = 0;
    const char *d = rtc_day_name[t.wday];
    while (*d) date[n++] = *d++;
    date[n++] = ',';
    date[n++] = ' ';
    const char *m = rtc_month_name[t.month - 1];
    while (*m) date[n++] = *m++;
    date[n++] = ' ';
    char num[12];
    fmt_uint(num, (unsigned)t.day);
    for (int i = 0; num[i]; i++) date[n++] = num[i];
    date[n++] = ',';
    date[n++] = ' ';
    fmt_uint(num, (unsigned)t.year);
    for (int i = 0; num[i]; i++) date[n++] = num[i];
    date[n] = 0;
    draw_string(date, cx - ui_string_w(date) / 2, ty + LOGO_FONT_H + 6, app_text_dim);
}
