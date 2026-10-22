#ifndef BASEOS_ICON_DRAW_H
#define BASEOS_ICON_DRAW_H
#include <stdint.h>
/* Icon drawing for shells and apps (ui/icons.c). The art itself is generated
 * into ui/icons.h (toolbar glyphs) and ui/ui_icons.h (the UI icon set). */
/* Draw an ICON_* glyph from icons.h (ICON_SIZE square) in ink, antialiased. */
void icon_draw(int id, int x, int y, uint8_t ink);
/* Draw a UI_ICON_* image in the active style at 16 or 32 px (the logo also
 * at 20, 24, 48 and 128). Glyph parts take ink; paper fills the parts that
 * hide what is behind them, usually the surface under the icon. A size with
 * no image of its own draws the next smaller image centered. */
void ui_icon_draw(int id, int x, int y, int size, uint8_t ink, uint8_t paper);
int ui_icon_has(int id, int size);
/* Application icons by AI_* id (ui/app_icons.h): size is AI_BIG or AI_SMALL. */
void app_icon_draw(int id, int x, int y, int size);
int app_ui_icon_id(int ai);
#endif
