#ifndef PANE_CHROME_H
#define PANE_CHROME_H
/* Window pane pieces shared by Calendar, Clock, Todo and the games: the
 * toolbar strip under the title, the status bar along the bottom, stat
 * boxes, result cards and hover plumbing. Header-only, drawn with the kit.
 *
 * Every helper works on the client rectangle the shell passes to an app:
 * x, y is the first pixel below the title bar (and the Classic menu row),
 * inside the frame, and y + h is the window's bottom border row. */
#include "kernel/gfx.h"
#include "kernel/font.h"
#include "ui/theme.h"
#include "ui/kit.h"
#include "ui/icons.h"
#include "ui/icon_draw.h"

#define PANE_MODERN (ui_theme.style == UI_STYLE_MODERN)

/* Hover: the shell passes its hover registrar (or 0, for an inactive window
 * or a host test). It records the rectangle as a repaint target and returns
 * the KIT_HOVER / KIT_PRESSED state of the pointer over it. */
typedef int (*PaneHot)(int x, int y, int w, int h);
static inline int pane_hot(PaneHot hot, int x, int y, int w, int h) {
    return hot ? hot(x, y, w, h) : 0;
}

typedef struct { int x, y, w, h; } PaneRect;
static inline int pane_hit(int mx, int my, const PaneRect *r) {
    return r->w > 0 && hit(mx, my, r->x, r->y, r->w, r->h);
}

/* Metrics shared with Files and Editor. */
static inline int pane_tool_h(void) { return PANE_MODERN ? 44 : 34; }
static inline int pane_status_h(void) { return PANE_MODERN ? 28 : 24; }
static inline int pane_pad(void) { return PANE_MODERN ? 12 : 8; }
/* A Modern toolbar continues the header and covers its rule. */
static inline int pane_tool_y(int y) { return PANE_MODERN ? y - 1 : y; }
static inline int pane_body_y(int y) { return pane_tool_y(y) + pane_tool_h(); }
static inline int pane_ctl_y(int y) {
    return pane_tool_y(y) + (pane_tool_h() - 1 - ui_theme.control_h) / 2;
}
static inline int pane_status_y(int y, int h) { return y + h - pane_status_h(); }

static inline void pane_toolbar(int x, int y, int w) {
    kit_toolbar(x, pane_tool_y(y), w, pane_tool_h());
}

/* A toolbar button: an ICON_* glyph (or -1) and an optional label on a kit
 * button of the given kind, with the kit state (hover, pressed, on,
 * disabled). pane_tool_w is its natural width. */
static inline int pane_tool_w(const char *label) {
    if (!label || !label[0]) return ui_theme.control_h;
    return ICON_SIZE + 6 + kit_text_w(label, 0) + (PANE_MODERN ? 24 : 18);
}
static inline void pane_tool(const PaneRect *r, int icon, const char *label, int kind, int state) {
    int dis = state & KIT_DISABLED, on = state & KIT_ON;
    uint8_t ink = dis ? TK_LINE_STRONG : TK_TEXT, glyph = dis ? TK_LINE_STRONG : TK_ICON;
    if (on && !dis) {
        kit_button(r->x, r->y, r->w, r->h, "", -1, KB_FLAT, state);
        kit_fill(r->x, r->y, r->w, r->h, ui_theme.r_control, PANE_MODERN ? TK_ACCENT_100 : TK_ACCENT_50, 1);
        if (!PANE_MODERN) kit_frame(r->x, r->y, r->w, r->h, KIT_R_SQUARE, TK_ACCENT);
        ink = glyph = TK_ACCENT_700;
    } else {
        kit_button(r->x, r->y, r->w, r->h, "", -1, kind, state);
        if ((kind == KB_PRIMARY || kind == KB_DANGER) && !dis) ink = glyph = TK_ON_ACCENT;
    }
    int down = (state & KIT_PRESSED) && !dis && !PANE_MODERN;
    int iy = r->y + (r->h - ICON_SIZE) / 2 + down;
    if (label && label[0]) {
        int content = (icon >= 0 ? ICON_SIZE + 6 : 0) + kit_text_w(label, 0);
        int ix = r->x + (r->w - content) / 2;
        if (ix < r->x + 6) ix = r->x + 6;
        if (icon >= 0) { icon_draw(icon, ix, iy, glyph); ix += ICON_SIZE + 6; }
        kit_text(label, ix, kit_ty(r->y, r->h) + down, ink, r->x + r->w - 4 - ix, 0);
    } else if (icon >= 0) icon_draw(icon, r->x + (r->w - ICON_SIZE) / 2, iy, glyph);
}

/* Status bar with left and right text; either may be 0. */
static inline void pane_statusbar(int x, int y, int w, int h, const char *left, uint8_t left_ink,
                                  const char *right, uint8_t right_ink) {
    int sy = pane_status_y(y, h), sh = pane_status_h(), pad = pane_pad();
    if (PANE_MODERN) {
        draw_hline(x, sy, w, TK_LINE_SUBTLE);
        kit_fill_bottom(x, sy + 1, w, sh, ui_theme.r_window - 1, ui_theme.statusbar);
    } else kit_statusbar(x, sy, w, sh);
    int ty = kit_ty(sy + 1, sh - 1), rw = right && right[0] ? kit_text_w(right, 0) : 0;
    if (rw > w - pad * 2) rw = w - pad * 2;
    if (left && left[0]) kit_text(left, x + pad, ty, left_ink, w - pad * 2 - (rw ? rw + 16 : 0), 0);
    if (rw) kit_text(right, x + w - pad - rw, ty, right_ink, rw, 0);
}

/* A labelled number for score panels: label in secondary text above a bold
 * value, inside a sunken box. Returns the box width. */
static inline int pane_stat_w(const char *label, const char *value) {
    int a = kit_text_w(label, 0), b = kit_text_w(value, 1);
    return (a > b ? a : b) + (PANE_MODERN ? 24 : 16);
}
static inline void pane_stat(int x, int y, int w, int h, const char *label, const char *value) {
    if (PANE_MODERN) kit_box(x, y, w, h, 8, TK_SUNKEN, 0);
    else kit_box(x, y, w, h, KIT_R_SQUARE, TK_RAISED, TK_LINE);
    int gap = (h - 2 * UI_FONT_H) / 3;
    if (gap < 0) gap = 0;
    kit_text_center(label, x, y + gap, w, TK_TEXT2, 0);
    kit_text_center(value, x, y + h - gap - UI_FONT_H, w, TK_TEXT, 1);
}

/* An inline stat for a toolbar row: "Label value". Returns the width. */
static inline int pane_inline_stat(int x, int y, int h, const char *label, const char *value) {
    int ty = kit_ty(y, h), lw = kit_text_w(label, 0);
    kit_text(label, x, ty, TK_TEXT2, lw, 0);
    int vw = kit_text_w(value, 1);
    kit_text(value, x + lw + 6, ty, TK_TEXT, vw, 1);
    return lw + 6 + vw;
}

/* Result card: an overlay surface with a title, one line of text and one
 * button. pane_card_layout gives the card and button rectangles centered on
 * the area, so drawing and clicks agree. */
#define PANE_CARD_W 248
static inline int pane_card_h(void) {
    int pad = PANE_MODERN ? 20 : 16;
    return pad + UI_FONT_H + 6 + UI_FONT_H + 16 + ui_theme.control_h + pad;
}
static inline void pane_card_layout(int ax, int ay, int aw, int ah, PaneRect *card, PaneRect *button) {
    int w = aw - 16 < PANE_CARD_W ? aw - 16 : PANE_CARD_W, h = pane_card_h();
    card->x = ax + (aw - w) / 2; card->y = ay + (ah - h) / 2; card->w = w; card->h = h;
    int pad = PANE_MODERN ? 20 : 16, bw = 112;
    button->x = card->x + (w - bw) / 2; button->y = card->y + h - pad - ui_theme.control_h;
    button->w = bw; button->h = ui_theme.control_h;
}
static inline void pane_card(const PaneRect *card, const PaneRect *button, const char *title,
                             const char *text, const char *label, int button_state) {
    int r = PANE_MODERN ? 10 : KIT_R_SQUARE, pad = PANE_MODERN ? 20 : 16;
    kit_shadow(card->x, card->y, card->w, card->h, r);
    kit_box(card->x, card->y, card->w, card->h, r, ui_theme.overlay, PANE_MODERN ? TK_LINE : TK_LINE_STRONG);
    kit_text_center(title, card->x, card->y + pad, card->w, TK_TEXT, 1);
    kit_text_center(text, card->x, card->y + pad + UI_FONT_H + 6, card->w, TK_TEXT2, 0);
    kit_button(button->x, button->y, button->w, button->h, label, -1, KB_PRIMARY, button_state | KIT_FOCUS);
}

/* Unsigned to decimal text. */
static inline void pane_num(char *s, unsigned v) {
    char t[12]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *s++ = t[--n];
    *s = 0;
}
/* Concatenate up to three parts into out (cap bytes). */
static inline void pane_join(char *out, int cap, const char *a, const char *b, const char *c) {
    const char *parts[3] = {a, b, c};
    int n = 0;
    for (int i = 0; i < 3; i++)
        for (const char *p = parts[i]; p && *p && n < cap - 1; p++) out[n++] = *p;
    out[n] = 0;
}

#endif
