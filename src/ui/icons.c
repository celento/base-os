/* Toolbar glyphs, UI icons and app icons, drawn through the kernel
 * framebuffer. The icon art is UI data, so it lives here rather than in gfx.c. */
#include "kernel/gfx.h"
#include "ui/icon_draw.h"
#define ICONS_DEFINE_DATA
#include "ui/icons.h"
#include "ui/app_icons.h"
#define UI_ICONS_DEFINE_DATA
#include "ui/ui_icons.h"
#include "ui/theme.h"

/* Icons: ICON_SIZE (toolbars, rows) or APP_ICON_SIZE (tiles, Start menu)
 * squares with 16 coverage levels blended over the surface. */
static void icon_blit(const uint8_t *bits, int size, int x, int y, uint8_t ink) {
    for (int row = 0; row < size; row++) {
        for (int col = 0; col < size; col++) {
            int i = row * size + col;
            int level = (i & 1) ? bits[i >> 1] & 15 : bits[i >> 1] >> 4;
            if (!level) continue;
            int px = x + col, py = y + row;
            if (px < 0 || py < 0 || px >= fb_w || py >= fb_h) continue;
            put_pixel(px, py, level == 15 ? ink : gfx_mix(get_pixel(px, py), ink, level * 256 / 15));
        }
    }
}
void icon_draw(int id, int x, int y, uint8_t ink) {
    if (id >= 0 && id < ICON_COUNT) icon_blit(icon_bits[id], ICON_SIZE, x, y, ink);
}
/* UI icons (ui/ui_icons.h) in the active style. Each image stores a palette
 * index and a 4-bit alpha per pixel plus a short list of two-color mixes. */
static int ui_icon_image(int id, int size) {
    int best = -1;
    for (int i = 0; i < UI_ICON_IMAGES; i++) {
        if (ui_icon_images[i].icon != id) continue;
        if (ui_icon_images[i].size == size) return i;
        if (ui_icon_images[i].size < size && (best < 0 || ui_icon_images[i].size > ui_icon_images[best].size)) best = i;
    }
    if (best < 0)
        for (int i = 0; i < UI_ICON_IMAGES; i++)
            if (ui_icon_images[i].icon == id && (best < 0 || ui_icon_images[i].size < ui_icon_images[best].size)) best = i;
    return best;
}

int ui_icon_has(int id, int size) {
    int i = ui_icon_image(id, size);
    return i >= 0 && ui_icon_images[i].size == size;
}

static uint8_t ui_icon_color(uint8_t index, uint8_t ink, uint8_t paper) {
    return index == 255 ? ink : index == 254 ? paper : index;
}

void ui_icon_draw(int id, int x, int y, int size, uint8_t ink, uint8_t paper) {
    if (id < 0 || id >= UI_ICON_COUNT) return;
    int image = ui_icon_image(id, size);
    if (image < 0) return;
    int style = ui_theme.style == UI_STYLE_MODERN ? 1 : 0;
    int s = ui_icon_images[image].size;
    /* A smaller image than asked for is centered in the requested box. */
    x += (size - s) / 2;
    y += (size - s) / 2;
    const uint8_t *idx = ui_icon_data[style] + ui_icon_offsets[style][image][0];
    const uint8_t *alpha = idx + s * s;
    const uint8_t *mix = alpha + s * s / 2;
    int mixes = (int)ui_icon_offsets[style][image][1], m = 0;
    for (int i = 0; i < s * s; i++) {
        int level = (i & 1) ? alpha[i >> 1] & 15 : alpha[i >> 1] >> 4;
        int at = m < mixes ? mix[m * 4] | (mix[m * 4 + 1] << 8) : -1;
        uint8_t c = ui_icon_color(idx[i], ink, paper);
        if (at == i) {
            c = gfx_mix(c, ui_icon_color(mix[m * 4 + 2], ink, paper), mix[m * 4 + 3]);
            m++;
        }
        if (!level) continue;
        int px = x + i % s, py = y + i / s;
        if (px < 0 || py < 0 || px >= fb_w || py >= fb_h) continue;
        put_pixel(px, py, level == 15 ? c : gfx_mix(get_pixel(px, py), c, level * 17));
    }
}

/* Application icons by AI_* id, drawn from the UI icon set. */
static const uint8_t app_ui_icon[AI_COUNT] = {
    [AI_FILES] = UI_ICON_A_FILES, [AI_EDITOR] = UI_ICON_A_EDITOR, [AI_CALC] = UI_ICON_A_CALC,
    [AI_PAINT] = UI_ICON_A_PAINT, [AI_VIEWER] = UI_ICON_A_VIEWER, [AI_SNAKE] = UI_ICON_A_SNAKE,
    [AI_WORDLE] = UI_ICON_A_WORDLE, [AI_TERMINAL] = UI_ICON_A_TERM, [AI_TODO] = UI_ICON_A_TODO,
    [AI_CLOCK] = UI_ICON_A_CLOCK, [AI_CALENDAR] = UI_ICON_A_CAL, [AI_MINES] = UI_ICON_A_MINES,
    [AI_G2048] = UI_ICON_A_2048, [AI_BREAKOUT] = UI_ICON_A_BREAKOUT, [AI_MONITOR] = UI_ICON_A_TASKS,
    [AI_SETTINGS] = UI_ICON_A_SETTINGS, [AI_BROWSER] = UI_ICON_A_BROWSER, [AI_PLAYER] = UI_ICON_A_MUSIC,
    [AI_WRITER] = UI_ICON_A_WRITER, [AI_SHEET] = UI_ICON_A_SHEET, [AI_TRASH] = UI_ICON_A_TRASH,
    [AI_NATIVE] = UI_ICON_A_NATIVE, [AI_HELLO] = UI_ICON_A_HELLO, [AI_ABOUT] = UI_ICON_A_INFO,
    [AI_HELP] = UI_ICON_A_HELP, [AI_DOCUMENT] = UI_ICON_A_DOC, [AI_WINDOW] = UI_ICON_A_WINDOW,
};

int app_ui_icon_id(int ai) {
    return ai >= 0 && ai < AI_COUNT ? app_ui_icon[ai] : UI_ICON_A_WINDOW;
}

void app_icon_draw(int ai, int x, int y, int size) {
    ui_icon_draw(app_ui_icon_id(ai), x, y, size, TK_ICON, TK_RAISED);
}
