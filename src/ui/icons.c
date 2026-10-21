/* Toolbar glyphs and color app icons, drawn through the kernel framebuffer.
 * The icon art is UI data, so it lives with the shells rather than in gfx.c. */
#include "kernel/gfx.h"
#include "ui/icon_draw.h"
#define ICONS_DEFINE_DATA
#include "ui/icons.h"
#define APP_ICONS_DEFINE_DATA
#include "ui/app_icons.h"

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
/* Color icons: a palette index per pixel followed by packed 4-bit alpha. */
void app_icon_draw(int id, int x, int y, int size) {
    if (id < 0 || id >= AI_COUNT || (size != AI_BIG && size != AI_SMALL)) return;
    const uint8_t *bits = size == AI_BIG ? app_icon_big[id] : app_icon_small[id];
    const uint8_t *alpha = bits + size * size;
    for (int row = 0; row < size; row++) {
        for (int col = 0; col < size; col++) {
            int i = row * size + col;
            int level = (i & 1) ? alpha[i >> 1] & 15 : alpha[i >> 1] >> 4;
            if (!level) continue;
            int px = x + col, py = y + row;
            if (px < 0 || py < 0 || px >= fb_w || py >= fb_h) continue;
            put_pixel(px, py, level == 15 ? bits[i] : gfx_mix(get_pixel(px, py), bits[i], level * 17));
        }
    }
}

void icon_palette_install(void) { gfx_set_icon_palette(app_icon_palette); }
