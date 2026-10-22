#ifndef UI_THEME_H
#define UI_THEME_H
/* The two visual styles, Classic and Modern, as palette values plus a
 * metrics struct. Both styles use the same token indices, so code that draws
 * with TK_* tokens and ui_theme metrics works unchanged in either style;
 * switching style or accent only rewrites palette entries and the struct. */
#include <stdint.h>
#include "kernel/gfx.h"

enum { UI_STYLE_CLASSIC, UI_STYLE_MODERN, UI_STYLE_COUNT };

/* Token palette indices. Comments give the Classic role; Modern reuses the
 * index with the role noted where it differs. */
enum {
    TK_RAISED = PAL_TOKEN, /* fields, menus, buttons (Modern: fields, buttons) */
    TK_BASE,            /* menu rows, toolbars, sidebars (Modern: window content, cards) */
    TK_SUNKEN,          /* taskbar (Modern: sidebars, status bars) */
    TK_HOVER,           /* icon button hover, pressed button */
    TK_LINE_SUBTLE,     /* separators, list rules */
    TK_LINE,            /* control borders, scroll thumb */
    TK_LINE_STRONG,     /* field border, inactive frame, disabled text */
    TK_TEXT2,           /* labels, metadata, shortcuts */
    TK_ICON,            /* glyphs on light surfaces */
    TK_TEXT,            /* body text, caret, tooltip fill */
    TK_DESKTOP,         /* desktop fill, set by the desktop preset */
    TK_SHADOW,          /* hard shadow (Modern: near band) */
    TK_ACCENT_50,       /* row hover, current line */
    TK_ACCENT_100,      /* unfocused selection, current place */
    TK_ACCENT_300,      /* drop target (Modern: focus ring, corner pixels) */
    TK_ACCENT,          /* selection, primary button, focus */
    TK_ACCENT_600,      /* active title bar, primary hover */
    TK_ACCENT_700,      /* active frame, pressed, accent text */
    TK_ON_ACCENT,       /* text on TK_ACCENT */
    TK_RED_50, TK_RED, TK_RED_700,
    TK_AMBER_50, TK_AMBER, TK_AMBER_700,
    TK_GREEN_50, TK_GREEN, TK_GREEN_700,
    TK_FIND,            /* search hits */
    TK_45,              /* Classic: inactive title bar. Modern: chrome */
    TK_46,              /* Classic: window content. Modern: overlay surfaces */
    TK_47               /* Classic: free. Modern: far shadow band */
};

/* Icon colors and Settings swatches (48-63). */
#define TK_PRESET(i) ((uint8_t)(PAL_ICON + (i)))

#define UI_ACCENT_N 6
#define UI_DESKTOP_N 6
enum { UI_PATTERN_NONE, UI_PATTERN_DOTS, UI_PATTERN_GRID, UI_PATTERN_N };

typedef struct {
    int style;
    /* Window frame. Classic draws a 1px border around a 28px title bar and a
     * 24px menu row; Modern has a 38px header with no in-window menu row, and
     * its menus live in a global top bar. */
    int border;          /* frame width */
    int title_h;         /* title bar or header, inside the border */
    int menu_h;          /* menu row under the title (0: global top bar) */
    int topbar_h;        /* global top bar height including its rule */
    int taskbar_h;       /* taskbar or dock height */
    int dock_margin;     /* gap under a floating dock */
    int caption_w, caption_h, caption_gap;
    int control_h;       /* buttons and fields */
    int row_h;           /* list rows */
    int menu_item_h;
    int menu_min_w, menu_max_w;
    int pad_panel, pad_dialog;
    int scrollbar;       /* scrollbar track width */
    int check;           /* checkbox and radio size */
    int r_control, r_window, r_dock; /* corner radius; 0 = square or notch */
    /* Role tokens that move between indices. */
    uint8_t content;     /* window content */
    uint8_t overlay;     /* menus, dialogs, launcher */
    uint8_t chrome;      /* title bar fill when inactive / header fill */
    uint8_t sidebar;
    uint8_t statusbar;
    uint8_t taskbar;
    uint8_t shadow2;     /* second shadow band, 0 when there is none */
} UiTheme;

extern UiTheme ui_theme;

/* Install a style with an accent preset, desktop color preset and pattern.
 * Writes palette entries 16-79 and 112-127, the app_* tokens and ui_theme,
 * then commits the palette. Out-of-range values fall back to 0. */
void ui_theme_install(int style, int accent, int desktop);
/* Rewrite only the accent ramp (entries 28-34) or the desktop entry (26). */
void ui_theme_set_accent(int accent);
void ui_theme_set_desktop(int desktop);
int ui_theme_accent(void);
int ui_theme_desktop(void);
const char *ui_style_name(int style);
const char *ui_accent_name(int style, int accent);
const char *ui_desktop_name(int style, int desktop);
uint32_t ui_accent_rgb(int style, int accent);   /* swatch color (500) */
uint32_t ui_desktop_rgb(int style, int desktop);
/* Palette color of a token in the active style, as 0xRRGGBB. */
uint32_t ui_token_rgb(int token);

#endif
