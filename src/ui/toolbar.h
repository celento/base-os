#ifndef TOOLBAR_H
#define TOOLBAR_H
/* Shared toolbar controls: icon buttons with optional labels, in the app
 * theme tokens. Header-only so app modules and their host tests need no new
 * sources beyond ui/icons.c, which draws the icons. */
#include "kernel/gfx.h"
#include "ui/app.h"
#include "ui/icons.h"
#include "ui/icon_draw.h"

enum { TB_NORMAL, TB_ACTIVE, TB_DISABLED, TB_PRIMARY, TB_FLAT };

#define TB_H 26
#define TB_ICON_W 28
#define TB_TEXT_H 17 /* UI font line height (UI_FONT_H) */

/* Width of a button: square for icon-only, icon + label otherwise. */
static inline int tool_button_w(const char *label) {
    return label && label[0] ? ICON_SIZE + ui_string_w(label) + 22 : TB_ICON_W;
}

static inline void tool_button(int x, int y, int w, int h, int icon, const char *label, int state) {
    uint8_t ink = app_text;
    if (state == TB_ACTIVE || state == TB_PRIMARY) {
        draw_round_rect(x, y, w, h, 6, app_accent);
        ink = app_on_accent;
    } else if (state == TB_NORMAL) {
        draw_round_rect(x, y, w, h, 6, app_chrome_dk);
    } else if (state == TB_DISABLED) {
        ink = gfx_mix(app_text_dim, app_chrome, 110);
    }
    int iy = y + (h - ICON_SIZE) / 2;
    if (label && label[0]) {
        int content = ICON_SIZE + 6 + ui_string_w(label);
        int ix = x + (w - content) / 2;
        if (ix < x + 6) ix = x + 6;
        if (icon >= 0) icon_draw(icon, ix, iy, ink);
        draw_string_clip(label, ix + ICON_SIZE + 6, y + (h - TB_TEXT_H) / 2, ink, x + w - 4);
    } else if (icon >= 0) {
        icon_draw(icon, x + (w - ICON_SIZE) / 2, iy, ink);
    }
}

/* Thin separator between button groups. */
static inline void tool_divider(int x, int y, int h) {
    draw_vline(x, y + 5, h - 10, app_chrome_dk);
}

#endif
