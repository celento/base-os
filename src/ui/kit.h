#ifndef UI_KIT_H
#define UI_KIT_H
/* The shared control kit. Every control reads its colors from the TK_*
 * tokens and its sizes and corner shapes from ui_theme, so the same calls
 * draw Classic or Modern. Controls draw only; hit testing stays with the
 * caller, which knows the rectangles it passed in. */
#include <stdint.h>
#include "ui/theme.h"

/* Control state bits. */
#define KIT_HOVER    1
#define KIT_PRESSED  2
#define KIT_FOCUS    4
#define KIT_DISABLED 8
#define KIT_ON       16   /* checked, toggled, selected */
#define KIT_MIXED    32
#define KIT_ERROR    64
#define KIT_UNFOCUSED 128 /* selection in a list that does not have focus */

enum { KB_DEFAULT, KB_PRIMARY, KB_DANGER, KB_FLAT };

/* Corner shapes. KIT_R_NOTCH skips the corner pixel of a border and keeps
 * the fill square (Classic controls); the other radii use per-row inset
 * tables. */
#define KIT_R_SQUARE 0
#define KIT_R_NOTCH  1

/* Rounded shapes. aa paints the first pixel of each inset row as a mix with
 * whatever is already drawn; pass 0 for windows and menus over unknown
 * pixels. */
void kit_fill(int x, int y, int w, int h, int r, uint8_t color, int aa);
/* Fill plus a 1px border (border 0: none). */
void kit_box(int x, int y, int w, int h, int r, uint8_t fill, uint8_t border);
void kit_frame(int x, int y, int w, int h, int r, uint8_t border);
/* Top-only or bottom-only rounding, for window headers and footers. */
void kit_fill_top(int x, int y, int w, int h, int r, uint8_t color);
void kit_fill_bottom(int x, int y, int w, int h, int r, uint8_t color);
int kit_inset(int r, int row);  /* pixels skipped at corner row `row` */
/* Window and menu shadow: Classic draws 2px of TK_SHADOW right and below;
 * Modern draws two 2px bands below the shape. */
void kit_shadow(int x, int y, int w, int h, int r);
/* Focus ring around a control box (Modern draws outside it). */
void kit_focus(int x, int y, int w, int h, int r);

/* Text. The UI font has one 17px cell; ty centers it in a row of height h. */
int kit_ty(int y, int h);
int kit_text_w(const char *s, int bold);
/* Draw text cut with an ellipsis to fit maxw; returns the width drawn. */
int kit_text(const char *s, int x, int y, uint8_t color, int maxw, int bold);
void kit_text_center(const char *s, int x, int y, int w, uint8_t color, int bold);
void kit_text_right(const char *s, int right, int y, uint8_t color, int bold);

/* Buttons. kit_button_w is the natural width for a label (min width
 * applied); icon is a UI_ICON_* id or -1. */
int kit_button_w(const char *label, int icon);
void kit_button(int x, int y, int w, int h, const char *label, int icon, int kind, int state);
void kit_icon_button(int x, int y, int size, int icon, int state);
/* Text field box; the caller draws the text. Returns the text x. */
int kit_field(int x, int y, int w, int h, int state);
/* Search field with a magnifier and, when filled, a clear button at the
 * right (kit_search_clear_x gives its x). */
int kit_search(int x, int y, int w, int h, const char *text, const char *placeholder, int state);
int kit_search_clear_x(int x, int w);
/* Dropdown box with its label and chevron. */
void kit_dropdown(int x, int y, int w, int h, const char *text, int state);
void kit_caret(int x, int y);
void kit_check(int x, int y, int state);
void kit_radio(int x, int y, int state);
void kit_toggle(int x, int y, int state);   /* 34x20 */
#define KIT_TOGGLE_W 34
#define KIT_TOGGLE_H 20
/* A labelled checkbox or radio row; returns the row width. */
int kit_check_row(int x, int y, const char *label, int state, int radio);
/* Segmented control: n labels, sel selected. Writes each segment's x and
 * width into xs/ws when they are not null. Returns the total width. */
int kit_segmented(int x, int y, const char *const *labels, int n, int sel, int hover, int *xs, int *ws);
int kit_segmented_h(void);
/* Tabs along a line; returns the total width. */
int kit_tabs(int x, int y, int w, const char *const *labels, int n, int sel, int hover, int *xs, int *ws);
int kit_tabs_h(void);
void kit_slider(int x, int y, int w, int value, int max, int state);
void kit_progress(int x, int y, int w, int value, int max);
void kit_progress_busy(int x, int y, int w, int phase);
/* Scrollbars: track at x,y of length len; pos/visible/total in any unit. */
void kit_vscroll(int x, int y, int len, int pos, int visible, int total, int state);
void kit_hscroll(int x, int y, int len, int pos, int visible, int total, int state);
/* Thumb geometry for hit testing: offset and length along the track. */
void kit_scroll_thumb(int len, int pos, int visible, int total, int *at, int *size);
void kit_tooltip(int x, int y, const char *text);
int kit_tooltip_w(const char *text);
void kit_kbd(int x, int y, const char *key);
int kit_kbd_w(const char *key);
void kit_badge(int x, int y, const char *text);
/* Inline banners (info, warning, error, success). Returns the height. */
enum { KIT_INFO, KIT_WARN, KIT_ERROR_MSG, KIT_OK };
int kit_banner(int x, int y, int w, int kind, const char *text);
/* List rows. kit_row paints the row background for a state and returns the
 * ink for its text; kit_row_dim is the ink for secondary text. */
uint8_t kit_row(int x, int y, int w, int h, int state);
uint8_t kit_row_dim(int state);
void kit_list_header(int x, int y, int w, int h);
void kit_separator(int x, int y, int w);
void kit_vseparator(int x, int y, int h);
/* Menus. */
#define KIT_MENU_CHECK  1
#define KIT_MENU_RADIO  2
#define KIT_MENU_SUB    4
#define KIT_MENU_SEP    8
void kit_menu_frame(int x, int y, int w, int h);
int kit_menu_pad(void);    /* padding above the first item */
int kit_menu_sep_h(void);
void kit_menu_item(int x, int y, int w, const char *label, const char *shortcut, int flags, int state);
void kit_menu_sep(int x, int y, int w);
int kit_menu_width(const char *label, const char *shortcut);
/* Status bar and toolbar strips (fill plus their rule). */
void kit_statusbar(int x, int y, int w, int h);
void kit_toolbar(int x, int y, int w, int h);
/* Disclosure chevron (right or down) with a label. */
void kit_disclosure(int x, int y, const char *label, int open);

#endif
