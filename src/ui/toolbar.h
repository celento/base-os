#ifndef TOOLBAR_H
#define TOOLBAR_H
/* Shared toolbar controls: icon buttons with optional labels, drawn with the
 * control kit so they follow the Classic or Modern style. Header-only; app
 * modules and their host tests link ui/icons.c and ui/kit.c. */
#include "kernel/gfx.h"
#include "ui/app.h"
#include "ui/icons.h"
#include "ui/icon_draw.h"
#include "ui/kit.h"

enum { TB_NORMAL, TB_ACTIVE, TB_DISABLED, TB_PRIMARY, TB_FLAT };

#define TB_H 26
#define TB_ICON_W 28
#define TB_TEXT_H 17 /* UI font line height (UI_FONT_H) */

/* Width of a button: square for icon-only, icon + label otherwise. */
static inline int tool_button_w(const char *label) {
    return label && label[0] ? ICON_SIZE + ui_string_w(label) + 22 : TB_ICON_W;
}

/* TB_NORMAL is a bordered button, TB_FLAT a borderless tool, TB_ACTIVE a
 * toggled tool, TB_PRIMARY the accent button. */
static inline void tool_button(int x, int y, int w, int h, int icon, const char *label, int state) {
    uint8_t ink = TK_TEXT;
    if (state == TB_ACTIVE) {
        kit_button(x, y, w, h, "", -1, KB_FLAT, KIT_ON);
        if (ui_theme.style == UI_STYLE_MODERN) { kit_fill(x, y, w, h, ui_theme.r_control, TK_ACCENT_100, 1); ink = TK_ACCENT_700; }
    } else if (state == TB_PRIMARY) {
        kit_button(x, y, w, h, "", -1, KB_PRIMARY, 0);
        ink = TK_ON_ACCENT;
    } else if (state == TB_NORMAL) {
        kit_button(x, y, w, h, "", -1, KB_DEFAULT, 0);
    } else if (state == TB_DISABLED) {
        ink = TK_LINE_STRONG;
    }
    int iy = y + (h - ICON_SIZE) / 2;
    if (label && label[0]) {
        int content = ICON_SIZE + 6 + ui_string_w(label);
        int ix = x + (w - content) / 2;
        if (ix < x + 6) ix = x + 6;
        if (icon >= 0) icon_draw(icon, ix, iy, state == TB_DISABLED || state == TB_PRIMARY || state == TB_ACTIVE ? ink : TK_ICON);
        draw_string_clip(label, ix + ICON_SIZE + 6, y + (h - TB_TEXT_H) / 2, ink, x + w - 4);
    } else if (icon >= 0) {
        icon_draw(icon, x + (w - ICON_SIZE) / 2, iy, state == TB_NORMAL || state == TB_FLAT ? TK_ICON : ink);
    }
}

/* Thin separator between button groups. */
static inline void tool_divider(int x, int y, int h) {
    kit_vseparator(x, y + 5, h - 10);
}

#endif
