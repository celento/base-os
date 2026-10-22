/* The control kit (ui/kit.h): Classic and Modern controls from the same
 * calls. No alpha and no blur: depth is a solid offset, and roundness is a
 * per-row inset table whose first pixel may be mixed with the surface. */
#include "ui/kit.h"
#include "ui/icon_draw.h"
#include "ui/ui_icons.h"
#include "kernel/font.h"

#define MODERN (ui_theme.style == UI_STYLE_MODERN)

/* ---------- Shapes ---------- */

static const uint8_t mask4[] = {2, 1};
static const uint8_t mask6[] = {4, 2, 1, 1};
static const uint8_t mask8[] = {5, 3, 2, 1, 1};
static const uint8_t mask12[] = {9, 6, 5, 4, 3, 2, 1, 1, 1};

static int isqrt(int v) {
    int r = 0;
    while ((r + 1) * (r + 1) <= v) r++;
    return r;
}

int kit_inset(int r, int row) {
    if (row < 0) return 0;
    switch (r) {
    case KIT_R_SQUARE: return 0;
    case KIT_R_NOTCH: return row == 0;
    case 4: return row < 2 ? mask4[row] : 0;
    case 6: return row < 4 ? mask6[row] : 0;
    case 8: return row < 5 ? mask8[row] : 0;
    case 12: return row < 9 ? mask12[row] : 0;
    default:
        if (row >= r) return 0;
        /* A circle of radius r through pixel centers, in eighths of a
         * pixel, rounded to the nearest pixel. */
        {
            int d = r * 8, dy = d - row * 8 - 4;
            int dx = isqrt(d * d - dy * dy);
            int in = (d - dx + 4) / 8;
            return in < 0 ? 0 : in;
        }
    }
}

static int row_inset(int r, int j, int h) {
    int a = kit_inset(r, j), b = kit_inset(r, h - 1 - j);
    return a > b ? a : b;
}

static void mix_px(int x, int y, uint8_t c) {
    if (x < 0 || y < 0 || x >= fb_w || y >= fb_h) return;
    put_pixel(x, y, gfx_mix(get_pixel(x, y), c, 128));
}

static void kit_cov_px(int x, int y, uint8_t c, int cov) {
    if (cov <= 0 || x < 0 || y < 0 || x >= fb_w || y >= fb_h) return;
    put_pixel(x, y, cov >= 16 ? c : gfx_mix(get_pixel(x, y), c, cov * 16));
}

/* Coverage (0-16) of pixel col,row of a corner square of radius r, whose
 * circle center sits at its inner corner (r, r): 4x4 samples per pixel, in
 * eighths of a pixel. */
static int kit_arc_cov(int col, int row, int r) {
    int d = r * 8, n = 0;
    for (int j = 0; j < 4; j++) {
        int dy = d - (row * 8 + 1 + j * 2);
        for (int i = 0; i < 4; i++) {
            int dx = d - (col * 8 + 1 + i * 2);
            if (dx * dx + dy * dy <= d * d) n++;
        }
    }
    return n;
}

/* Circles, pills and knobs: the corner arcs are blended by coverage, so
 * steep parts of the curve get smooth edges too (the inset tables only soften
 * one pixel per row). */
static int kit_round_shape(int w, int h, int r) {
    return r >= 8 && (2 * r >= h || 2 * r >= w);
}

static void kit_fill_round(int x, int y, int w, int h, int r, uint8_t color) {
    if (w <= 0 || h <= 0) return;
    if (2 * r > w) r = w / 2;
    if (2 * r > h) r = h / 2;
    for (int j = 0; j < h; j++) {
        int cr = j < r ? j : (j >= h - r ? h - 1 - j : -1);
        if (cr < 0) { draw_hline(x, y + j, w, color); continue; }
        int solid = r;
        for (int i = 0; i < r; i++) {
            int cov = kit_arc_cov(i, cr, r);
            if (cov >= 16) { solid = i; break; }
            kit_cov_px(x + i, y + j, color, cov);
            kit_cov_px(x + w - 1 - i, y + j, color, cov);
        }
        draw_hline(x + solid, y + j, w - 2 * solid, color);
    }
}

static void fill_rows(int x, int y, int w, int h, int r, uint8_t color, int aa, int top, int bottom) {
    if (w <= 0 || h <= 0) return;
    if (r == KIT_R_NOTCH) r = KIT_R_SQUARE;
    if (aa && top && bottom && kit_round_shape(w, h, r)) { kit_fill_round(x, y, w, h, r, color); return; }
    for (int j = 0; j < h; j++) {
        int a = top ? kit_inset(r, j) : 0, b = bottom ? kit_inset(r, h - 1 - j) : 0;
        int in = a > b ? a : b;
        if (in * 2 >= w) continue;
        if (aa && in > 0) {
            draw_hline(x + in + 1, y + j, w - 2 * in - 2, color);
            mix_px(x + in, y + j, color);
            mix_px(x + w - 1 - in, y + j, color);
        } else {
            draw_hline(x + in, y + j, w - 2 * in, color);
        }
    }
}

void kit_fill(int x, int y, int w, int h, int r, uint8_t color, int aa) {
    fill_rows(x, y, w, h, r, color, aa, 1, 1);
}
void kit_fill_top(int x, int y, int w, int h, int r, uint8_t color) {
    fill_rows(x, y, w, h, r, color, 0, 1, 0);
}
void kit_fill_bottom(int x, int y, int w, int h, int r, uint8_t color) {
    fill_rows(x, y, w, h, r, color, 0, 0, 1);
}

void kit_frame(int x, int y, int w, int h, int r, uint8_t border) {
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        int in = row_inset(r, j, h);
        if (j == 0 || j == h - 1) {
            draw_hline(x + in, y + j, w - 2 * in, border);
            continue;
        }
        int up = row_inset(r, j - 1, h), down = row_inset(r, j + 1, h);
        int end = in + 1;
        if (up > end) end = up;
        if (down > end) end = down;
        if (end * 2 > w) end = w / 2;
        draw_hline(x + in, y + j, end - in, border);
        draw_hline(x + w - end, y + j, end - in, border);
    }
}

void kit_box(int x, int y, int w, int h, int r, uint8_t fill, uint8_t border) {
    if (w <= 0 || h <= 0) return;
    if (border && kit_round_shape(w, h, r) && w > 2 && h > 2) {
        /* A smooth ring: the border shape, then the fill one pixel in. */
        kit_fill_round(x, y, w, h, r, border);
        kit_fill_round(x + 1, y + 1, w - 2, h - 2, r - 1, fill);
        return;
    }
    if (r > KIT_R_NOTCH && border) {
        /* Soften the outer stair against the surface before covering it. */
        for (int j = 0; j < h; j++) {
            int in = row_inset(r, j, h);
            if (in > 0) {
                mix_px(x + in - 1, y + j, border);
                mix_px(x + w - in, y + j, border);
            }
        }
    }
    kit_fill(x, y, w, h, r, fill, border ? 0 : 1);
    if (border) kit_frame(x, y, w, h, r, border);
}

static int inside(int r, int w, int h, int col, int row) {
    if (row < 0 || row >= h || col < 0 || col >= w) return 0;
    int in = row_inset(r, row, h);
    return col >= in && col < w - in;
}

/* Shadow pixels take the darker of the band color and what is already
 * there, channel by channel. Over the light desktop and content that is the
 * band color itself; over darker pixels (the Terminal, a dark desktop) the
 * shadow can no longer show up as a light glow. Taking the minimum also
 * means a redraw over pixels that already hold a shadow changes nothing. */
static void kit_shadow_px(int x, int y, uint8_t band) {
    if (x < 0 || y < 0 || x >= fb_w || y >= fb_h) return;
    uint8_t under = get_pixel(x, y);
    if (under == band) return;
    uint32_t u = pal32[under], b = pal32[band];
    int ur = (u >> 16) & 255, ug = (u >> 8) & 255, ub = u & 255;
    int br = (b >> 16) & 255, bg = (b >> 8) & 255, bb = b & 255;
    int r = ur < br ? ur : br, g = ug < bg ? ug : bg, bl = ub < bb ? ub : bb;
    if (r == br && g == bg && bl == bb) put_pixel(x, y, band);
    else if (r != ur || g != ug || bl != ub) put_pixel(x, y, gfx_rgb(r, g, bl));
}

void kit_shadow(int x, int y, int w, int h, int r) {
    if (!MODERN) {
        for (int j = 0; j < h; j++)
            for (int i = 0; i < 2; i++) kit_shadow_px(x + w + i, y + 2 + j, TK_SHADOW);
        for (int j = 0; j < 2; j++)
            for (int i = 0; i < w - 2; i++) kit_shadow_px(x + 2 + i, y + h + j, TK_SHADOW);
        return;
    }
    int rows = 12;
    for (int row = h - rows; row < h + 4; row++) {
        if (row < 0) continue;
        for (int col = 0; col < w; col++) {
            if (inside(r, w, h, col, row)) continue;
            uint8_t c = 0;
            if (inside(r, w, h, col, row - 2)) c = TK_SHADOW;
            else if (inside(r, w, h, col, row - 4)) c = ui_theme.shadow2;
            if (c) kit_shadow_px(x + col, y + row, c);
        }
    }
}

void kit_focus(int x, int y, int w, int h, int r) {
    if (MODERN) {
        int rr = r > KIT_R_NOTCH ? r + 2 : KIT_R_SQUARE;
        kit_frame(x - 2, y - 2, w + 4, h + 4, rr, TK_ACCENT_300);
        kit_frame(x - 1, y - 1, w + 2, h + 2, r > KIT_R_NOTCH ? r + 1 : KIT_R_SQUARE, TK_ACCENT_300);
    } else {
        kit_frame(x - 2, y - 2, w + 4, h + 4, KIT_R_SQUARE, TK_ACCENT);
    }
}

/* ---------- Text ---------- */

int kit_ty(int y, int h) { return y + (h - UI_FONT_H) / 2; }

int kit_text_w(const char *s, int bold) {
    return bold ? uib_string_w(s) : ui_string_w(s);
}

static int char_w(char c, int bold) {
    char t[2] = {c, 0};
    return kit_text_w(t, bold);
}

int kit_text(const char *s, int x, int y, uint8_t color, int maxw, int bold) {
    if (!s) return 0;
    int w = kit_text_w(s, bold);
    if (maxw <= 0) return 0;
    if (w <= maxw) {
        if (bold) draw_string_bold(s, x, y, color);
        else draw_string(s, x, y, color);
        return w;
    }
    int dots = kit_text_w("...", bold), used = 0, n = 0;
    while (s[n] && used + char_w(s[n], bold) + dots <= maxw) used += char_w(s[n++], bold);
    char buf[160];
    if (n > (int)sizeof buf - 4) n = (int)sizeof buf - 4;
    for (int i = 0; i < n; i++) buf[i] = s[i];
    /* Trim a trailing space so the ellipsis sits on the last word. */
    while (n > 0 && buf[n - 1] == ' ') { n--; used -= char_w(' ', bold); }
    buf[n] = '.'; buf[n + 1] = '.'; buf[n + 2] = '.'; buf[n + 3] = 0;
    if (used + dots > maxw) {
        if (dots > maxw) return 0;
        buf[0] = '.'; buf[1] = '.'; buf[2] = '.'; buf[3] = 0;
        used = 0;
    }
    if (bold) draw_string_bold(buf, x, y, color);
    else draw_string(buf, x, y, color);
    return used + dots;
}

void kit_text_center(const char *s, int x, int y, int w, uint8_t color, int bold) {
    int tw = kit_text_w(s, bold);
    if (tw > w) { kit_text(s, x, y, color, w, bold); return; }
    kit_text(s, x + (w - tw) / 2, y, color, tw, bold);
}

void kit_text_right(const char *s, int right, int y, uint8_t color, int bold) {
    int tw = kit_text_w(s, bold);
    kit_text(s, right - tw, y, color, tw, bold);
}

/* ---------- Buttons and fields ---------- */

static int ctl_r(void) { return MODERN ? ui_theme.r_control : KIT_R_NOTCH; }

int kit_button_w(const char *label, int icon) {
    int pad = MODERN ? 14 : 12, min = MODERN ? 84 : 80;
    int w = pad * 2 + (label && label[0] ? uib_string_w(label) : 0);
    if (icon >= 0) w += 16 + (label && label[0] ? 6 : 0);
    if (label && label[0] && w < min) w = min;
    return w;
}

void kit_button(int x, int y, int w, int h, const char *label, int icon, int kind, int state) {
    int r = ctl_r(), dis = state & KIT_DISABLED, prs = (state & KIT_PRESSED) && !dis;
    /* The default ring is the focus ring: Enter acts on that button. */
    int hov = (state & KIT_HOVER) && !dis, foc = (state & (KIT_FOCUS | KIT_DEFAULT)) && !dis;
    uint8_t fill = TK_RAISED, border = TK_LINE, ink = TK_TEXT;
    int bold = 0, ghost = 0;
    if (kind == KB_PRIMARY || kind == KB_DANGER) {
        int pri = kind == KB_PRIMARY;
        fill = pri ? TK_ACCENT : TK_RED;
        border = MODERN ? fill : pri ? TK_ACCENT_700 : TK_RED_700;
        if (hov) fill = pri ? TK_ACCENT_600 : gfx_mix(TK_RED, TK_RED_700, 128);
        if (prs) fill = pri ? TK_ACCENT_700 : TK_RED_700;
        if (MODERN && (hov || prs)) border = fill;
        ink = TK_ON_ACCENT;
        bold = 1;
    } else if (kind == KB_FLAT) {
        ghost = !(hov || prs || (state & KIT_ON));
        fill = (state & KIT_ON) ? TK_RAISED : TK_HOVER;
        border = (state & KIT_ON) && !MODERN ? TK_LINE : 0;
    } else {
        if (hov) { fill = MODERN ? TK_46 : TK_BASE; border = TK_LINE_STRONG; }
        if (prs) { fill = TK_HOVER; border = TK_LINE_STRONG; }
    }
    if (dis) {
        fill = MODERN ? TK_SUNKEN : TK_BASE;
        border = TK_LINE_SUBTLE;
        ink = TK_LINE_STRONG;
        bold = 0;
        ghost = kind == KB_FLAT;
    }
    if (!ghost) {
        kit_box(x, y, w, h, r, fill, border);
        if (!MODERN && border && kind == KB_DEFAULT && !hov && !prs && !dis && !foc)
            kit_fill(x + 1, y + h - 1, w - 2, 1, KIT_R_SQUARE, TK_LINE_STRONG, 0);
    }
    if (foc) {
        if (MODERN) {
            if (kind == KB_DEFAULT) kit_frame(x, y, w, h, r, TK_ACCENT);
            kit_focus(x, y, w, h, r);
        } else if (kind == KB_PRIMARY || kind == KB_DANGER) {
            kit_frame(x, y, w, h, r, TK_TEXT);
            kit_frame(x + 1, y + 1, w - 2, h - 2, KIT_R_SQUARE, TK_RAISED);
        } else {
            kit_frame(x, y, w, h, r, TK_ACCENT);
            kit_frame(x + 1, y + 1, w - 2, h - 2, KIT_R_SQUARE, TK_ACCENT);
        }
    }
    int dy = prs && !MODERN ? 1 : 0;
    int has_label = label && label[0];
    int content = (icon >= 0 ? 16 : 0) + (has_label ? kit_text_w(label, bold) : 0) + (icon >= 0 && has_label ? 6 : 0);
    int cx = x + (w - content) / 2;
    if (cx < x + 4) cx = x + 4;
    if (icon >= 0) {
        ui_icon_draw(icon, cx, y + (h - 16) / 2 + dy, 16, dis ? TK_LINE_STRONG : (ink == TK_TEXT ? TK_ICON : ink), fill);
        cx += 22;
    }
    if (has_label) kit_text(label, cx, kit_ty(y, h) + dy, ink, x + w - 4 - cx, bold);
}

void kit_icon_button(int x, int y, int size, int icon, int state) {
    int r = ctl_r(), dis = state & KIT_DISABLED;
    uint8_t ink = TK_ICON, bg = 0;
    if (state & KIT_ON) {
        if (MODERN) { kit_fill(x, y, size, size, r, TK_ACCENT_100, 1); ink = TK_ACCENT_700; bg = TK_ACCENT_100; }
        else { kit_box(x, y, size, size, r, TK_RAISED, TK_LINE); bg = TK_RAISED; }
    } else if ((state & (KIT_HOVER | KIT_PRESSED)) && !dis) {
        kit_fill(x, y, size, size, r, TK_HOVER, 1);
        bg = TK_HOVER;
    }
    if (dis) ink = MODERN ? gfx_rgb(0xB7, 0xC0, 0xCC) : TK_LINE;
    if ((state & KIT_FOCUS) && !dis) {
        if (MODERN) kit_focus(x, y, size, size, r);
        else { kit_frame(x, y, size, size, r, TK_ACCENT); kit_frame(x + 1, y + 1, size - 2, size - 2, 0, TK_ACCENT); }
    }
    if (!bg) bg = get_pixel(x + size / 2, y + 1);
    ui_icon_draw(icon, x + (size - 16) / 2, y + (size - 16) / 2, 16, ink, bg);
}

int kit_field(int x, int y, int w, int h, int state) {
    int r = ctl_r();
    uint8_t fill = TK_RAISED, border = MODERN ? TK_LINE : TK_LINE_STRONG;
    if (state & KIT_DISABLED) { fill = MODERN ? TK_SUNKEN : TK_BASE; border = TK_LINE_SUBTLE; }
    if (state & KIT_ERROR) border = TK_RED;
    else if (state & KIT_FOCUS) border = TK_ACCENT;
    kit_box(x, y, w, h, r, fill, border);
    if ((state & KIT_ERROR) && !MODERN) kit_frame(x + 1, y + 1, w - 2, h - 2, 0, TK_RED);
    else if (state & KIT_ERROR) kit_frame(x + 1, y + 1, w - 2, h - 2, r > 1 ? r - 2 : 0, TK_RED);
    else if (state & KIT_FOCUS) {
        if (MODERN) kit_focus(x, y, w, h, r);
        else kit_frame(x + 1, y + 1, w - 2, h - 2, 0, TK_ACCENT);
    }
    return x + (MODERN ? 10 : 6) + 1;
}

int kit_search_clear_x(int x, int w) { return x + w - (MODERN ? 28 : 24); }

int kit_search(int x, int y, int w, int h, const char *text, const char *placeholder, int state) {
    uint8_t paper = TK_RAISED;
    int tx;
    if (MODERN && !(state & (KIT_FOCUS | KIT_ERROR | KIT_DISABLED))) {
        /* Borderless well; the text sits where the field would put it. */
        paper = TK_HOVER;
        kit_fill(x, y, w, h, ctl_r(), paper, 1);
        tx = x + 11;
    } else tx = kit_field(x, y, w, h, state);
    ui_icon_draw(UI_ICON_SEARCH, tx - 1, y + (h - 16) / 2, 16, (state & KIT_FOCUS) ? TK_ICON : TK_TEXT2, paper);
    tx += 21;
    int filled = text && text[0];
    int right = filled ? kit_search_clear_x(x, w) - 4 : x + w - 6;
    if (filled) {
        kit_text(text, tx, kit_ty(y, h), TK_TEXT, right - tx, 0);
        ui_icon_draw(UI_ICON_X, kit_search_clear_x(x, w) + 2, y + (h - 16) / 2, 16, TK_TEXT, paper);
    } else if (placeholder) {
        kit_text(placeholder, tx, kit_ty(y, h), TK_TEXT2, right - tx, 0);
    }
    return tx;
}

void kit_dropdown(int x, int y, int w, int h, const char *text, int state) {
    int tx = kit_field(x, y, w, h, state);
    uint8_t ink = (state & KIT_DISABLED) ? TK_LINE_STRONG : TK_TEXT;
    kit_text(text, tx, kit_ty(y, h), ink, w - (tx - x) - 26, 0);
    ui_icon_draw(UI_ICON_DOWN, x + w - 22, y + (h - 16) / 2, 16, (state & KIT_DISABLED) ? TK_LINE_STRONG : TK_ICON, TK_RAISED);
}

void kit_caret(int x, int y) { draw_vline(x, y + 1, 15, TK_TEXT); }

/* A 2px check mark in a box of size s at x,y. */
static void check_mark(int x, int y, int s, uint8_t c) {
    int ox = x + (s - 10) / 2, oy = y + (s - 8) / 2;
    for (int t = 0; t < 2; t++) {
        draw_line(ox, oy + 3 + t, ox + 3, oy + 6 + t, c);
        draw_line(ox + 3, oy + 6 + t, ox + 9, oy + t, c);
    }
}

void kit_check(int x, int y, int state) {
    int s = ui_theme.check, r = MODERN ? 4 : KIT_R_NOTCH;
    int dis = state & KIT_DISABLED, on = state & (KIT_ON | KIT_MIXED);
    uint8_t fill = TK_RAISED, border = MODERN ? TK_LINE_STRONG : TK_TEXT2;
    if (on) { fill = TK_ACCENT; border = MODERN ? TK_ACCENT : TK_ACCENT_700; }
    if (dis) { border = TK_LINE; fill = on ? TK_LINE : (MODERN ? TK_SUNKEN : TK_BASE); }
    kit_box(x, y, s, s, r, fill, border);
    if (state & KIT_MIXED) draw_rect(x + 3, y + s / 2 - 1, s - 6, 2, TK_ON_ACCENT);
    else if (state & KIT_ON) check_mark(x, y, s, TK_ON_ACCENT);
    if ((state & KIT_FOCUS) && !dis) kit_focus(x, y, s, s, r);
}

void kit_radio(int x, int y, int state) {
    int s = ui_theme.check, r = s / 2, dis = state & KIT_DISABLED, on = state & KIT_ON;
    uint8_t border = MODERN ? TK_LINE_STRONG : TK_TEXT2, fill = TK_RAISED, dot = TK_ACCENT;
    if (on) border = TK_ACCENT;
    if (dis) { border = TK_LINE; fill = MODERN ? TK_SUNKEN : TK_BASE; dot = TK_LINE; }
    if (MODERN && on) {
        kit_box(x, y, s, s, r, dot, dot);
        kit_fill(x + 5, y + 5, s - 10, s - 10, (s - 10) / 2, dis ? TK_RAISED : TK_RAISED, 1);
    } else {
        kit_box(x, y, s, s, r, fill, border);
        if (on) kit_fill(x + 4, y + 4, s - 8, s - 8, (s - 8) / 2, dot, 1);
    }
    if ((state & KIT_FOCUS) && !dis) kit_focus(x, y, s, s, r);
}

void kit_toggle(int x, int y, int state) {
    int on = state & KIT_ON, dis = state & KIT_DISABLED;
    int r = MODERN ? 10 : KIT_R_NOTCH;
    uint8_t track = on ? TK_ACCENT : (MODERN ? gfx_rgb(0xB7, 0xC0, 0xCC) : TK_LINE_STRONG);
    if (dis) track = TK_LINE_SUBTLE;
    kit_fill(x, y, KIT_TOGGLE_W, KIT_TOGGLE_H, r, track, 1);
    int kx = on ? x + KIT_TOGGLE_W - 18 : x + 2;
    kit_fill(kx, y + 2, 16, 16, MODERN ? 8 : KIT_R_SQUARE, TK_RAISED, 1);
    if ((state & KIT_FOCUS) && !dis) kit_focus(x, y, KIT_TOGGLE_W, KIT_TOGGLE_H, r);
}

int kit_check_row(int x, int y, const char *label, int state, int radio) {
    int h = ui_theme.row_h, s = ui_theme.check;
    if (radio) kit_radio(x, y + (h - s) / 2, state);
    else kit_check(x, y + (h - s) / 2, state);
    int gap = MODERN ? 10 : 8;
    kit_text(label, x + s + gap, kit_ty(y, h), (state & KIT_DISABLED) ? TK_LINE_STRONG : TK_TEXT, 1000, 0);
    return s + gap + ui_string_w(label);
}

/* ---------- Segmented and tabs ---------- */

int kit_segmented_h(void) { return MODERN ? 30 : 26; }

int kit_segmented(int x, int y, const char *const *labels, int n, int sel, int hover, int *xs, int *ws) {
    int total = 0, pad = 12;
    for (int i = 0; i < n; i++) total += uib_string_w(labels[i]) + pad * 2;
    if (MODERN) {
        total += 4 + (n - 1) * 2;
        kit_fill(x, y, total, 30, 8, TK_HOVER, 1);
        int cx = x + 2;
        for (int i = 0; i < n; i++) {
            int w = uib_string_w(labels[i]) + pad * 2;
            if (i == sel) kit_box(cx, y + 2, w, 26, 6, TK_RAISED, TK_LINE);
            else if (i == hover) kit_fill(cx, y + 2, w, 26, 6, TK_LINE_SUBTLE, 1);
            kit_text_center(labels[i], cx, kit_ty(y + 2, 26), w, i == sel ? TK_TEXT : TK_ICON, i == sel);
            if (xs) xs[i] = cx;
            if (ws) ws[i] = w;
            cx += w + 2;
        }
        return total;
    }
    total += 2 + (n - 1);
    kit_box(x, y, total, 26, KIT_R_NOTCH, TK_RAISED, TK_LINE);
    int cx = x + 1;
    for (int i = 0; i < n; i++) {
        int w = uib_string_w(labels[i]) + pad * 2;
        if (i) draw_vline(cx - 1, y + 1, 24, TK_LINE);
        if (i == sel) draw_rect(cx, y + 1, w, 24, TK_ACCENT_100);
        else if (i == hover) draw_rect(cx, y + 1, w, 24, TK_BASE);
        kit_text_center(labels[i], cx, kit_ty(y + 1, 24), w, i == sel ? TK_ACCENT_700 : TK_TEXT, i == sel);
        if (xs) xs[i] = cx;
        if (ws) ws[i] = w;
        cx += w + 1;
    }
    return total;
}

int kit_tabs_h(void) { return MODERN ? 30 : 29; }

int kit_tabs(int x, int y, int w, const char *const *labels, int n, int sel, int hover, int *xs, int *ws) {
    int cx = x, pad = MODERN ? 14 : 12;
    if (!MODERN) draw_hline(x, y + 28, w, TK_LINE);
    for (int i = 0; i < n; i++) {
        int tw = uib_string_w(labels[i]) + pad * 2;
        if (MODERN) {
            if (i == sel) kit_fill(cx, y, tw, 30, 12, TK_ACCENT_100, 1);
            else if (i == hover) kit_fill(cx, y, tw, 30, 12, TK_HOVER, 1);
            kit_text_center(labels[i], cx, kit_ty(y, 30), tw, i == sel ? TK_ACCENT_700 : (i == hover ? TK_TEXT : TK_TEXT2), i == sel);
        } else {
            if (i == hover && i != sel) draw_rect(cx, y, tw, 28, TK_BASE);
            if (i == sel) draw_rect(cx, y + 26, tw, 2, TK_ACCENT);
            kit_text_center(labels[i], cx, kit_ty(y, 28), tw, i == sel || i == hover ? TK_TEXT : TK_TEXT2, i == sel);
        }
        if (xs) xs[i] = cx;
        if (ws) ws[i] = tw;
        cx += tw + (MODERN ? 4 : 0);
    }
    return cx - x;
}

/* ---------- Slider, progress, scrollbars ---------- */

void kit_slider(int x, int y, int w, int value, int max, int state) {
    if (max <= 0) max = 1;
    if (value < 0) value = 0;
    if (value > max) value = max;
    int tw = MODERN ? 16 : 12, travel = w - tw, kx = x + travel * value / max;
    uint8_t fill = (state & KIT_DISABLED) ? TK_LINE : TK_ACCENT;
    if (MODERN) {
        kit_fill(x, y + 6, w, 4, 4, TK_LINE, 1);
        kit_fill(x, y + 6, kx - x + tw / 2, 4, 4, fill, 1);
        kit_box(kx, y, 16, 16, 8, TK_RAISED, TK_LINE_STRONG);
        if (state & KIT_FOCUS) kit_focus(kx, y, 16, 16, 8);
    } else {
        draw_rect(x, y + 7, w, 4, TK_LINE);
        draw_rect(x, y + 7, kx - x + tw / 2, 4, fill);
        kit_box(kx, y, tw, 18, KIT_R_NOTCH, (state & KIT_PRESSED) ? TK_BASE : TK_RAISED, TK_TEXT2);
        if (state & KIT_FOCUS) kit_focus(kx, y, tw, 18, 0);
    }
}

void kit_progress(int x, int y, int w, int value, int max) {
    if (max <= 0) max = 1;
    if (value < 0) value = 0;
    if (value > max) value = max;
    if (MODERN) {
        kit_fill(x, y, w, 8, 4, TK_HOVER, 1);
        int fw = w * value / max;
        if (fw > 0) kit_fill(x, y, fw < 8 ? 8 : fw, 8, 4, TK_ACCENT, 1);
    } else {
        kit_box(x, y, w, 12, KIT_R_SQUARE, TK_RAISED, TK_LINE_STRONG);
        int fw = (w - 4) * value / max;
        if (fw > 0) draw_rect(x + 2, y + 2, fw, 8, TK_ACCENT);
    }
}

void kit_progress_busy(int x, int y, int w, int phase) {
    int iw = MODERN ? w : w - 4, span = iw + 32, at = (phase * 4) % span - 32;
    if (MODERN) kit_fill(x, y, w, 8, 4, TK_HOVER, 1);
    else kit_box(x, y, w, 12, KIT_R_SQUARE, TK_RAISED, TK_LINE_STRONG);
    int a = at < 0 ? 0 : at, b = at + 32 > iw ? iw : at + 32;
    if (b > a) {
        if (MODERN) kit_fill(x + a, y, b - a, 8, b - a >= 8 ? 4 : 0, TK_ACCENT, 1);
        else draw_rect(x + 2 + a, y + 2, b - a, 8, TK_ACCENT);
    }
}

/* a * b / c for non-negative a, b <= c, without 64-bit division: the
 * operands are scaled down until the product fits in 32 bits. */
static int mul_div(int a, int b, int c) {
    unsigned ua = (unsigned)a, ub = (unsigned)b, uc = (unsigned)c;
    while (ub > 0xFFFFu || uc > 0xFFFFu) { ub >>= 1; uc >>= 1; }
    if (!uc) return 0;
    return (int)(ua * ub / uc);
}

void kit_scroll_thumb(int len, int pos, int visible, int total, int *at, int *size) {
    if (total <= visible || total <= 0) { *at = 0; *size = len; return; }
    int s = mul_div(len, visible, total);
    if (s < 20) s = 20;
    if (s > len) s = len;
    int range = total - visible;
    if (pos < 0) pos = 0;
    if (pos > range) pos = range;
    *at = mul_div(len - s, pos, range);
    *size = s;
}

static void scroll(int x, int y, int len, int pos, int visible, int total, int state, int vertical) {
    int sb = ui_theme.scrollbar, at, size;
    kit_scroll_thumb(len - 4, pos, visible, total, &at, &size);
    uint8_t thumb = (state & (KIT_HOVER | KIT_PRESSED)) ? TK_LINE_STRONG : TK_LINE;
    if (MODERN) {
        if (total <= visible) return;
        if (vertical) kit_fill(x + (sb - 4) / 2, y + 2 + at, 4, size, 4, thumb, 1);
        else kit_fill(x + 2 + at, y + (sb - 4) / 2, size, 4, 4, thumb, 1);
        return;
    }
    if (vertical) {
        draw_rect(x, y, sb, len, TK_BASE);
        draw_vline(x, y, len, TK_LINE_SUBTLE);
        if (total > visible) kit_box(x + 3, y + 2 + at, 6, size, KIT_R_SQUARE, thumb, 0);
    } else {
        draw_rect(x, y, len, sb, TK_BASE);
        draw_hline(x, y, len, TK_LINE_SUBTLE);
        if (total > visible) kit_box(x + 2 + at, y + 3, size, 6, KIT_R_SQUARE, thumb, 0);
    }
}

void kit_vscroll(int x, int y, int len, int pos, int visible, int total, int state) {
    scroll(x, y, len, pos, visible, total, state, 1);
}
void kit_hscroll(int x, int y, int len, int pos, int visible, int total, int state) {
    scroll(x, y, len, pos, visible, total, state, 0);
}

/* ---------- Small parts ---------- */

int kit_tooltip_w(const char *text) { return ui_string_w(text) + (MODERN ? 16 : 12); }

void kit_tooltip(int x, int y, const char *text) {
    int h = MODERN ? 25 : 21;
    kit_fill(x, y, kit_tooltip_w(text), h, MODERN ? 6 : 0, TK_TEXT, 0);
    draw_string(text, x + (MODERN ? 8 : 6), kit_ty(y, h), TK_ON_ACCENT);
}

int kit_kbd_w(const char *key) { return ui_string_w(key) + (MODERN ? 14 : 12); }

void kit_kbd(int x, int y, const char *key) {
    int w = kit_kbd_w(key), h = MODERN ? 20 : 19;
    if (MODERN) kit_box(x, y, w, h, 4, TK_RAISED, TK_LINE);
    else {
        kit_box(x, y, w, h, KIT_R_NOTCH, TK_BASE, TK_LINE);
        draw_hline(x + 1, y + h - 1, w - 2, TK_LINE_STRONG);
    }
    draw_string(key, x + (MODERN ? 7 : 6), kit_ty(y, h), TK_TEXT);
}

void kit_badge(int x, int y, const char *text) {
    int h = MODERN ? 20 : 17, w = uib_string_w(text) + (MODERN ? 14 : 10);
    kit_fill(x, y, w, h, MODERN ? 10 : KIT_R_SQUARE, TK_ACCENT, 1);
    draw_string_bold(text, x + (MODERN ? 7 : 5), kit_ty(y, h), TK_ON_ACCENT);
}

/* Word wrap into lines no wider than w; draws when draw is set. */
static int wrap(const char *s, int x, int y, int w, uint8_t color, int draw) {
    int lines = 0;
    while (*s) {
        int n = 0, last = -1, used = 0;
        while (s[n] && s[n] != '\n') {
            int cw = char_w(s[n], 0);
            if (used + cw > w && n > 0) break;
            if (s[n] == ' ') last = n;
            used += cw;
            n++;
        }
        int cut = n;
        if (s[n] && s[n] != '\n' && last > 0) cut = last;
        if (draw) {
            char buf[160];
            int m = cut < (int)sizeof buf - 1 ? cut : (int)sizeof buf - 1;
            for (int i = 0; i < m; i++) buf[i] = s[i];
            buf[m] = 0;
            draw_string(buf, x, y + lines * UI_FONT_H, color);
        }
        lines++;
        s += cut;
        while (*s == ' ') s++;
        if (*s == '\n') s++;
    }
    return lines ? lines : 1;
}

int kit_banner(int x, int y, int w, int kind, const char *text) {
    static const uint8_t fills[] = {TK_ACCENT_50, TK_AMBER_50, TK_RED_50, TK_GREEN_50};
    static const uint8_t borders[] = {TK_ACCENT, TK_AMBER, TK_RED, TK_GREEN};
    static const int icons[] = {UI_ICON_INFO_C, UI_ICON_WARN, UI_ICON_ERR, UI_ICON_OK};
    if (kind < 0 || kind > 3) kind = 0;
    int px = MODERN ? 14 : 12, py = MODERN ? 10 : 7, gap = MODERN ? 10 : 8;
    int tw = w - px * 2 - 16 - gap;
    int lines = wrap(text, 0, 0, tw, 0, 0);
    int h = py * 2 + lines * UI_FONT_H;
    if (MODERN) kit_box(x, y, w, h, 8, fills[kind], 0);
    else kit_box(x, y, w, h, KIT_R_SQUARE, fills[kind], borders[kind]);
    ui_icon_draw(icons[kind], x + px, y + py, 16, TK_ICON, fills[kind]);
    wrap(text, x + px + 16 + gap, y + py, tw, TK_TEXT, 1);
    return h;
}

uint8_t kit_row(int x, int y, int w, int h, int state) {
    uint8_t bg = 0, ink = TK_TEXT;
    int r = KIT_R_SQUARE;
    if (MODERN) { x += 6; w -= 12; r = 6; }
    if (state & KIT_ON) {
        if (state & KIT_UNFOCUSED) bg = TK_ACCENT_100;
        else { bg = TK_ACCENT; ink = TK_ON_ACCENT; }
    } else if (state & KIT_HOVER) bg = MODERN ? TK_SUNKEN : TK_ACCENT_50;
    if (state & KIT_PRESSED) {
        /* Drop target. */
        kit_fill(x, y, w, h, r, TK_ACCENT_50, 1);
        kit_frame(x, y, w, h, r, MODERN ? TK_ACCENT : TK_ACCENT_300);
        kit_frame(x + 1, y + 1, w - 2, h - 2, r > 1 ? r - 2 : 0, MODERN ? TK_ACCENT : TK_ACCENT_300);
    } else if (bg) kit_fill(x, y, w, h, r, bg, 1);
    if (state & KIT_DISABLED) ink = TK_LINE_STRONG;
    return ink;
}

uint8_t kit_row_dim(int state) {
    if ((state & KIT_ON) && !(state & KIT_UNFOCUSED)) return TK_ON_ACCENT;
    if (state & KIT_DISABLED) return TK_LINE_STRONG;
    return TK_TEXT2;
}

void kit_list_header(int x, int y, int w, int h) {
    if (MODERN) {
        draw_hline(x, y + h - 1, w, TK_LINE_SUBTLE);
    } else {
        draw_rect(x, y, w, h - 1, TK_BASE);
        draw_hline(x, y + h - 1, w, TK_LINE_SUBTLE);
    }
}

void kit_separator(int x, int y, int w) { draw_hline(x, y, w, TK_LINE_SUBTLE); }
void kit_vseparator(int x, int y, int h) { draw_vline(x, y, h, TK_LINE_SUBTLE); }

/* ---------- Menus ---------- */

int kit_menu_pad(void) { return MODERN ? 6 : 4; }
int kit_menu_sep_h(void) { return MODERN ? 11 : 9; }

void kit_menu_frame(int x, int y, int w, int h) {
    int r = MODERN ? ui_theme.r_window : KIT_R_SQUARE;
    kit_shadow(x, y, w, h, r);
    kit_box(x, y, w, h, r, ui_theme.overlay, MODERN ? TK_LINE_STRONG : TK_TEXT2);
}

/* Label inset from the menu's left edge; Classic keeps a wider check
 * column. */
static int menu_text_dx(void) { return MODERN ? 30 : 28; }

int kit_menu_width(const char *label, const char *shortcut) {
    int w = menu_text_dx() + ui_string_w(label) + (MODERN ? 22 : 16);
    if (shortcut && shortcut[0]) w += 24 + ui_string_w(shortcut);
    return w;
}

void kit_menu_item(int x, int y, int w, const char *label, const char *shortcut, int flags, int state) {
    int h = ui_theme.menu_item_h, m = MODERN ? 6 : 4;
    int dis = state & KIT_DISABLED, sel = (state & KIT_HOVER) && !dis;
    uint8_t ink = dis ? TK_LINE_STRONG : TK_TEXT, dim = dis ? TK_LINE_STRONG : TK_TEXT2;
    uint8_t bg = ui_theme.overlay;
    if (sel) {
        kit_fill(x + m, y, w - 2 * m, h, MODERN ? 6 : KIT_R_SQUARE, TK_ACCENT, 1);
        ink = TK_ON_ACCENT;
        dim = MODERN ? TK_ACCENT_100 : TK_ON_ACCENT;
        bg = TK_ACCENT;
    } else if (MODERN && !dis && (flags & KIT_MENU_DANGER)) ink = TK_RED_700;
    int mark = x + (MODERN ? 10 : 8);
    if ((flags & KIT_MENU_CHECK) && (state & KIT_ON))
        ui_icon_draw(UI_ICON_CHECK, mark, y + (h - 16) / 2, 16, ink, bg);
    if ((flags & KIT_MENU_RADIO) && (state & KIT_ON))
        ui_icon_draw(UI_ICON_DOT, mark, y + (h - 16) / 2, 16, ink, bg);
    int lx = x + menu_text_dx();
    int right = x + w - m - (MODERN ? 10 : 8);
    if (flags & KIT_MENU_SUB) {
        ui_icon_draw(UI_ICON_RIGHT, right - 14, y + (h - 16) / 2, 16, sel ? ink : dim, bg);
        right -= 18;
    }
    int sw = shortcut && shortcut[0] ? ui_string_w(shortcut) : 0;
    if (sw) draw_string(shortcut, right - sw, kit_ty(y, h), dim);
    /* Strip the '&' marker and remember which letter it names. */
    char t[96];
    int n = 0, mn = -1;
    for (int i = 0; label && label[i] && n < (int)sizeof t - 1; i++) {
        if (label[i] == '&' && label[i + 1]) { if (mn < 0) mn = n; continue; }
        t[n++] = label[i];
    }
    t[n] = 0;
    int maxw = right - lx - (sw ? sw + 16 : 0);
    int drawn = kit_text(t, lx, kit_ty(y, h), ink, maxw, 0);
    if (mn >= 0 && drawn >= kit_text_w(t, 0)) {
        char one[2] = {t[mn], 0};
        t[mn] = 0;
        draw_hline(lx + ui_string_w(t), kit_ty(y, h) + 15, ui_string_w(one), ink);
    }
}

void kit_menu_sep(int x, int y, int w) {
    int m = MODERN ? 12 : 8;
    draw_hline(x + m, y + kit_menu_sep_h() / 2, w - 2 * m, TK_LINE_SUBTLE);
}

/* ---------- Strips ---------- */

void kit_statusbar(int x, int y, int w, int h) {
    draw_hline(x, y, w, TK_LINE_SUBTLE);
    draw_rect(x, y + 1, w, h - 1, ui_theme.statusbar);
}

void kit_toolbar(int x, int y, int w, int h) {
    draw_rect(x, y, w, h - 1, MODERN ? ui_theme.chrome : TK_BASE);
    draw_hline(x, y + h - 1, w, MODERN ? TK_LINE : TK_LINE_SUBTLE);
}

void kit_disclosure(int x, int y, const char *label, int open) {
    ui_icon_draw(open ? UI_ICON_DOWN : UI_ICON_RIGHT, x, y, 16, TK_ACCENT, get_pixel(x, y));
    draw_string(label, x + 20, y, TK_ACCENT);
}
