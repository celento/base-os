#include "app.h"
#include "sysmon.h"

static char cpu_vendor[13];
static char cpu_brand[49];
static int cpu_done;

static void cpuid(unsigned leaf, unsigned *a, unsigned *b, unsigned *c, unsigned *d) {
    asm volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

static void put4(char *dst, unsigned v) {
    dst[0] = (char)(v & 255);
    dst[1] = (char)((v >> 8) & 255);
    dst[2] = (char)((v >> 16) & 255);
    dst[3] = (char)((v >> 24) & 255);
}

static void cpu_probe(void) {
    unsigned a, b, c, d;
    cpuid(0, &a, &b, &c, &d);
    put4(cpu_vendor, b);
    put4(cpu_vendor + 4, d);
    put4(cpu_vendor + 8, c);
    cpu_vendor[12] = 0;
    cpuid(0x80000000u, &a, &b, &c, &d);
    if (a >= 0x80000004u) {
        for (unsigned i = 0; i < 3; i++) {
            cpuid(0x80000002u + i, &a, &b, &c, &d);
            put4(cpu_brand + i * 16, a);
            put4(cpu_brand + i * 16 + 4, b);
            put4(cpu_brand + i * 16 + 8, c);
            put4(cpu_brand + i * 16 + 12, d);
        }
        cpu_brand[48] = 0;
    } else {
        cpu_brand[0] = 0;
    }
    cpu_done = 1;
}

static const char *brand_trim(void) {
    const char *p = cpu_brand;
    while (*p == ' ')
        p++;
    return p;
}

static void row(int x, int y, int w, const char *label, const char *value) {
    draw_string(label, x, y, app_text_dim);
    draw_string(value, x + w - ui_string_w(value), y, app_text);
}

static void bar(int x, int y, int w, int used, int cap) {
    draw_round_rect(x, y, w, 8, 4, gfx_gray(0xDC));
    int fw = cap > 0 ? (int)((long)w * used / cap) : 0;
    if (fw > w) fw = w;
    if (fw > 0)
        draw_round_rect(x, y, fw < 8 ? 8 : fw, 8, 4, app_accent);
}

static void fmt_time(char *out, unsigned sec) {
    unsigned h = sec / 3600, m = (sec / 60) % 60, s = sec % 60;
    char t[12];
    int n = 0;
    fmt_uint(t, h);
    for (int i = 0; t[i]; i++) out[n++] = t[i];
    out[n++] = 'h';
    out[n++] = ' ';
    fmt_pad2(t, (int)m);
    out[n++] = t[0]; out[n++] = t[1];
    out[n++] = 'm';
    out[n++] = ' ';
    fmt_pad2(t, (int)s);
    out[n++] = t[0]; out[n++] = t[1];
    out[n++] = 's';
    out[n] = 0;
}

static void win_rows(int bx, int by, int *x, int *y0, int *w) {
    *x = bx + 20;
    *y0 = by + 318;
    *w = SYSMON_W - 40;
}

void sysmon_draw(int bx, int by, int bw, int bh, const SysInfo *si) {
    if (!cpu_done)
        cpu_probe();
    draw_rect(bx, by, bw, bh, COLOR_WHITE);
    int x = bx + 20;
    int y = by + 16;
    int w = bw - 40;
    char buf[40];

    draw_string_bold("System", x, y, app_text);
    y += UI_FONT_H + 8;
    row(x, y, w, "Processor", cpu_brand[0] ? brand_trim() : cpu_vendor);
    y += UI_FONT_H + 6;
    fmt_time(buf, si->uptime_sec);
    row(x, y, w, "Uptime", buf);
    y += UI_FONT_H + 6;
    {
        char r[24];
        int n = 0;
        char t[12];
        fmt_uint(t, (unsigned)si->fb_w);
        for (int i = 0; t[i]; i++) r[n++] = t[i];
        r[n++] = 'x';
        fmt_uint(t, (unsigned)si->fb_h);
        for (int i = 0; t[i]; i++) r[n++] = t[i];
        r[n++] = ' '; r[n++] = '@'; r[n++] = ' ';
        fmt_uint(t, (unsigned)si->fb_bpp);
        for (int i = 0; t[i]; i++) r[n++] = t[i];
        r[n++] = 'b'; r[n++] = 'p'; r[n++] = 'p';
        r[n] = 0;
        row(x, y, w, "Display", r);
    }
    y += UI_FONT_H + 6;
    fmt_uint(buf, (unsigned)si->mem_mb);
    {
        int n = 0;
        while (buf[n]) n++;
        buf[n++] = ' '; buf[n++] = 'M'; buf[n++] = 'B'; buf[n] = 0;
    }
    row(x, y, w, "Memory", buf);
    y += UI_FONT_H + 6;
    fmt_uint(buf, si->frames);
    row(x, y, w, "Frames rendered", buf);
    y += UI_FONT_H + 14;

    draw_string_bold("Storage", x, y, app_text);
    y += UI_FONT_H + 8;
    {
        char t[12], r[32];
        int n = 0;
        fmt_uint(t, (unsigned)si->fs_nodes);
        for (int i = 0; t[i]; i++) r[n++] = t[i];
        r[n++] = ' '; r[n++] = 'o'; r[n++] = 'f'; r[n++] = ' ';
        fmt_uint(t, (unsigned)si->fs_max);
        for (int i = 0; t[i]; i++) r[n++] = t[i];
        r[n] = 0;
        row(x, y, w, "Files and folders", r);
        y += UI_FONT_H + 4;
        bar(x, y, w, si->fs_nodes, si->fs_max);
        y += 16;
        fmt_uint(t, (unsigned)(si->fs_bytes / 1024));
        n = 0;
        for (int i = 0; t[i]; i++) r[n++] = t[i];
        r[n++] = ' '; r[n++] = 'K'; r[n++] = 'B'; r[n++] = ' '; r[n++] = 'o'; r[n++] = 'f'; r[n++] = ' ';
        fmt_uint(t, (unsigned)(si->fs_cap / 1024));
        for (int i = 0; t[i]; i++) r[n++] = t[i];
        r[n++] = ' '; r[n++] = 'K'; r[n++] = 'B';
        r[n] = 0;
        row(x, y, w, "Data used", r);
        y += UI_FONT_H + 4;
        bar(x, y, w, si->fs_bytes, si->fs_cap);
    }

    int rx, ry, rw;
    win_rows(bx, by, &rx, &ry, &rw);
    draw_string_bold("Windows", rx, ry - UI_FONT_H - 8, app_text);
    if (si->win_n == 0)
        draw_string("Nothing open", rx, ry, app_text_dim);
    for (int i = 0; i < si->win_n; i++) {
        int yy = ry + i * 24;
        draw_string(si->win_name[i], rx, yy + 3, app_text);
        if (si->win_min[i])
            draw_string("minimized", rx + 140, yy + 3, app_text_dim);
        int cw = 60;
        draw_round_rect(rx + rw - cw, yy, cw, 22, 5, gfx_gray(0xB0));
        draw_round_rect(rx + rw - cw + 1, yy + 1, cw - 2, 20, 4, COLOR_WHITE);
        draw_string("Close", rx + rw - cw + (cw - ui_string_w("Close")) / 2, yy + 3, app_text);
    }
}

int sysmon_click(int bx, int by, int bw, int mx, int my, const SysInfo *si) {
    (void)bw;
    int rx, ry, rw;
    win_rows(bx, by, &rx, &ry, &rw);
    for (int i = 0; i < si->win_n; i++) {
        int yy = ry + i * 24;
        if (hit(mx, my, rx + rw - 60, yy, 60, 22))
            return si->win_id[i];
    }
    return -1;
}
