/* App theme tokens and the Classic and Modern styles. Shells install a style
 * with ui_theme_install; app components only read the tokens.
 * tools/gen_ui_icons.py reads the color tables below to quantize icon art, so
 * keep each table a plain list of 0xRRGGBB values. */
#include "ui/app.h"
#include "ui/theme.h"
#include "ui/ui_icons.h"

uint8_t app_accent;
uint8_t app_accent_dk;
uint8_t app_text;
uint8_t app_text_dim;
uint8_t app_chrome;
uint8_t app_chrome_dk;
uint8_t app_body;
uint8_t app_field;
uint8_t app_paper;
uint8_t app_border;
uint8_t app_on_accent;

UiTheme ui_theme;

/* Tokens 16-47. */
static const uint32_t classic_tokens[32] = {
    0xFFFFFF, 0xE9EDF2, 0xDDE3EA, 0xD3DAE2, 0xC9D1DB, 0xB3BDC9, 0x8E99A8, 0x4A5361,
    0x3C434C, 0x1B1F24, 0x8DA2B8, 0x6F8399, 0xE3ECFC, 0xD3E2FD, 0x8FB2F7, 0x2563EB,
    0x1E54CC, 0x1A45A6, 0xFFFFFF, 0xFDEDEC, 0xD0342C, 0xA3261F, 0xFFF5DC, 0xF2B21B,
    0x8A5A00, 0xE6F5EC, 0x2E9E5B, 0x1D6B3D, 0xFCE3A0, 0xC5CED9, 0xF8F9FB, 0xFFFFFF
};
static const uint32_t modern_tokens[32] = {
    0xFFFFFF, 0xFBFBFC, 0xECEFF3, 0xD5DAE2, 0xD8DDE4, 0xC3CAD4, 0x8E99AA, 0x545C68,
    0x444B57, 0x16191F, 0xAEB9C9, 0x94A0B2, 0xF0F0FE, 0xD6D8F8, 0xA5A9F4, 0x4F46E5,
    0x4338CA, 0x3730A3, 0xFFFFFF, 0xFDECEC, 0xDC3B3B, 0xB42323, 0xFFF1C7, 0xF5B30B,
    0x8C5B00, 0xE3F4EA, 0x22A25A, 0x17713E, 0xFCE3A0, 0xE4E8EE, 0xF2F4F7, 0xA2ADBE
};
/* Icon colors and swatches, 48-63. */
static const uint32_t classic_icon[16] = {
    0xF2C14E, 0xE0A82A, 0xD9A21E, 0x9A6E08, 0xF8DA8F, 0x7FE0A0, 0xE5A100, 0x0E7C86,
    0x1F7A3F, 0x6D4AD6, 0xC25A12, 0xD9D4C7, 0xCFDCCF, 0xD8D0E0, 0xE3E5E8, 0x9FAEBF
};
static const uint32_t modern_icon[16] = {
    0xF2C14E, 0xE0A82A, 0xD9A21E, 0x9A6E08, 0xF8DA8F, 0x7FE0A0, 0xE5A100, 0x0E7C86,
    0x1F7A3F, 0xB4369A, 0xC25A12, 0xC2B8A3, 0xA9BFAA, 0xBDB1CE, 0xD3D7DE, 0x4A5568
};
/* Terminal colors, 64-79: Classic is tuned for white, Modern is dark slate. */
static const uint32_t classic_ansi[16] = {
    0x1B1F24, 0xC4322B, 0x1F7A3F, 0x8A6100, 0x1F5BD6, 0xA23BB0, 0x0E7C86, 0xB3BDC9,
    0x4A5361, 0xD43F37, 0x25884A, 0x946A00, 0x2F6AE0, 0xB44AC4, 0x11818C, 0xFFFFFF
};
static const uint32_t modern_ansi[16] = {
    0x1E2330, 0xFF8A80, 0x7FD49B, 0xF2C66D, 0x8AB4FF, 0xD7A6F2, 0x7FD7DD, 0xD8DDE6,
    0x8E99AA, 0xFFA59E, 0x9BE3B2, 0xF7D68E, 0xA9C7FF, 0xE2BDF6, 0xA1E4E8, 0xF4F6F9
};

/* Accent ramps for entries 28-34: 50, 100, 300, 500, 600, 700, on. */
typedef struct { const char *name; uint32_t v[7]; } AccentPreset;
static const AccentPreset classic_accents[UI_ACCENT_N] = {
    {"Blue",     {0xEAF1FE, 0xD3E2FD, 0x8FB2F7, 0x2563EB, 0x1E54CC, 0x1A45A6, 0xFFFFFF}},
    {"Teal",     {0xE6F4F5, 0xC8E8EA, 0x7CC3C8, 0x0E7C86, 0x0B6A73, 0x09555C, 0xFFFFFF}},
    {"Green",    {0xE7F4EC, 0xCBE7D4, 0x86C49B, 0x1F7A3F, 0x196834, 0x145329, 0xFFFFFF}},
    {"Purple",   {0xF0ECFC, 0xE0D8FA, 0xB6A4EE, 0x6D4AD6, 0x5B3CBB, 0x4A3099, 0xFFFFFF}},
    {"Orange",   {0xFCEFE6, 0xF8DCC8, 0xE9A57A, 0xC25A12, 0xA84D0F, 0x8A3F0C, 0xFFFFFF}},
    {"Graphite", {0xEFF1F4, 0xDDE1E6, 0x6F8399, 0x3C434C, 0x2F353D, 0x1B1F24, 0xFFFFFF}},
};
static const AccentPreset modern_accents[UI_ACCENT_N] = {
    {"Iris",     {0xF0F0FE, 0xD6D8F8, 0xA5A9F4, 0x4F46E5, 0x4338CA, 0x3730A3, 0xFFFFFF}},
    {"Teal",     {0xE6F4F5, 0xC3E3E6, 0x7CC3C8, 0x0E7C86, 0x0B6A73, 0x09555C, 0xFFFFFF}},
    {"Green",    {0xE7F4EC, 0xC6E3CF, 0x86C49B, 0x1F7A3F, 0x196834, 0x145329, 0xFFFFFF}},
    {"Magenta",  {0xFBEEF8, 0xF2D3EA, 0xDE95CC, 0xB4369A, 0x9A2C83, 0x7E236B, 0xFFFFFF}},
    {"Orange",   {0xFCEFE6, 0xF5D8C3, 0xE9A57A, 0xC25A12, 0xA84D0F, 0x8A3F0C, 0xFFFFFF}},
    {"Graphite", {0xEEF1F4, 0xD5DAE2, 0xA2ADBE, 0x444B57, 0x363C46, 0x24282F, 0xFFFFFF}},
};
typedef struct { const char *name; uint32_t rgb; } DesktopPreset;
static const DesktopPreset classic_desktops[UI_DESKTOP_N] = {
    {"Mist", 0x8DA2B8}, {"Sand", 0xD9D4C7}, {"Sage", 0xCFDCCF},
    {"Lilac", 0xD8D0E0}, {"Paper", 0xE3E5E8}, {"Slate", 0x9FAEBF},
};
static const DesktopPreset modern_desktops[UI_DESKTOP_N] = {
    {"Slate", 0xAEB9C9}, {"Sand", 0xC2B8A3}, {"Sage", 0xA9BFAA},
    {"Lilac", 0xBDB1CE}, {"Mist", 0xD3D7DE}, {"Night", 0x4A5568},
};

static const UiTheme classic_metrics = {
    .style = UI_STYLE_CLASSIC, .border = 1, .title_h = 28, .menu_h = 24, .topbar_h = 0,
    .taskbar_h = 34, .dock_margin = 0, .caption_w = 24, .caption_h = 20, .caption_gap = 2,
    .control_h = 26, .row_h = 24, .menu_item_h = 24, .menu_min_w = 180, .menu_max_w = 360,
    .pad_panel = 12, .pad_dialog = 16, .scrollbar = 12, .check = 14,
    .r_control = 0, .r_window = 0, .r_dock = 0,
    .content = TK_46, .overlay = TK_RAISED, .chrome = TK_45, .sidebar = TK_BASE,
    .statusbar = TK_BASE, .taskbar = TK_SUNKEN, .shadow2 = 0,
};
static const UiTheme modern_metrics = {
    .style = UI_STYLE_MODERN, .border = 1, .title_h = 38, .menu_h = 0, .topbar_h = 28,
    .taskbar_h = 48, .dock_margin = 8, .caption_w = 28, .caption_h = 28, .caption_gap = 8,
    .control_h = 30, .row_h = 30, .menu_item_h = 28, .menu_min_w = 200, .menu_max_w = 380,
    .pad_panel = 14, .pad_dialog = 20, .scrollbar = 10, .check = 16,
    .r_control = 6, .r_window = 8, .r_dock = 12,
    .content = TK_BASE, .overlay = TK_46, .chrome = TK_45, .sidebar = TK_SUNKEN,
    .statusbar = TK_SUNKEN, .taskbar = TK_45, .shadow2 = TK_47,
};

static int cur_accent, cur_desktop;

static const AccentPreset *accents(int style) {
    return style == UI_STYLE_MODERN ? modern_accents : classic_accents;
}
static const DesktopPreset *desktops(int style) {
    return style == UI_STYLE_MODERN ? modern_desktops : classic_desktops;
}

static void set_app_tokens(void) {
    app_accent = TK_ACCENT;
    app_accent_dk = TK_ACCENT_700;
    app_text = TK_TEXT;
    app_text_dim = TK_TEXT2;
    app_chrome = ui_theme.style == UI_STYLE_MODERN ? TK_SUNKEN : TK_BASE;
    app_chrome_dk = TK_HOVER;
    app_body = ui_theme.content;
    app_field = TK_RAISED;
    app_paper = TK_RAISED;
    app_border = TK_LINE;
    app_on_accent = TK_ON_ACCENT;
}

static void write_accent(void) {
    const AccentPreset *a = &accents(ui_theme.style)[cur_accent];
    for (int i = 0; i < 7; i++)
        gfx_set_color(TK_ACCENT_50 + i, a->v[i]);
}

void ui_theme_install(int style, int accent, int desktop) {
    if (style < 0 || style >= UI_STYLE_COUNT) style = UI_STYLE_CLASSIC;
    if (accent < 0 || accent >= UI_ACCENT_N) accent = 0;
    if (desktop < 0 || desktop >= UI_DESKTOP_N) desktop = 0;
    ui_theme = style == UI_STYLE_MODERN ? modern_metrics : classic_metrics;
    cur_accent = accent;
    cur_desktop = desktop;
    const uint32_t *tokens = style == UI_STYLE_MODERN ? modern_tokens : classic_tokens;
    const uint32_t *icon = style == UI_STYLE_MODERN ? modern_icon : classic_icon;
    const uint32_t *ansi = style == UI_STYLE_MODERN ? modern_ansi : classic_ansi;
    for (int i = 0; i < 32; i++) gfx_set_color(PAL_TOKEN + i, tokens[i]);
    for (int i = 0; i < 16; i++) gfx_set_color(PAL_ICON + i, icon[i]);
    for (int i = 0; i < 16; i++) gfx_set_color(PAL_ANSI + i, ansi[i]);
    for (int i = 0; i < PAL_ICON_X_N; i++) gfx_set_color(PAL_ICON_X + i, ui_icon_extra[style][i]);
    write_accent();
    gfx_set_color(TK_DESKTOP, desktops(style)[desktop].rgb);
    gfx_pal_commit();
    set_app_tokens();
}

void ui_theme_set_accent(int accent) {
    if (accent < 0 || accent >= UI_ACCENT_N) accent = 0;
    cur_accent = accent;
    write_accent();
    gfx_pal_commit();
}

void ui_theme_set_desktop(int desktop) {
    if (desktop < 0 || desktop >= UI_DESKTOP_N) desktop = 0;
    cur_desktop = desktop;
    gfx_set_color(TK_DESKTOP, desktops(ui_theme.style)[desktop].rgb);
    gfx_pal_commit();
}

int ui_theme_accent(void) { return cur_accent; }
int ui_theme_desktop(void) { return cur_desktop; }

const char *ui_style_name(int style) {
    return style == UI_STYLE_MODERN ? "Modern" : "Classic";
}
const char *ui_accent_name(int style, int accent) {
    return accent >= 0 && accent < UI_ACCENT_N ? accents(style)[accent].name : "";
}
const char *ui_desktop_name(int style, int desktop) {
    return desktop >= 0 && desktop < UI_DESKTOP_N ? desktops(style)[desktop].name : "";
}
uint32_t ui_accent_rgb(int style, int accent) {
    return accent >= 0 && accent < UI_ACCENT_N ? accents(style)[accent].v[3] : 0;
}
uint32_t ui_desktop_rgb(int style, int desktop) {
    return desktop >= 0 && desktop < UI_DESKTOP_N ? desktops(style)[desktop].rgb : 0;
}
uint32_t ui_token_rgb(int token) {
    return token >= 0 && token < 256 ? pal32[token] : 0;
}
