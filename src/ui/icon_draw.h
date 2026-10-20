#ifndef BASEOS_ICON_DRAW_H
#define BASEOS_ICON_DRAW_H
#include <stdint.h>
/* Icon drawing for shells and apps (ui/icons.c). The art itself is generated
 * into ui/icons.h and ui/app_icons.h. */
/* Load the app icon colors into the framebuffer palette. Call once before
 * drawing color icons; the colors survive display mode changes. */
void icon_palette_install(void);
/* Draw an ICON_* glyph from icons.h (ICON_SIZE square) in ink, antialiased. */
void icon_draw(int id, int x, int y, uint8_t ink);
/* Color application icons (ui/app_icons.h): size is AI_BIG or AI_SMALL. */
void app_icon_draw(int id, int x, int y, int size);
#endif
