#ifndef UI_WALLPAPER_H
#define UI_WALLPAPER_H
/* Desktop backgrounds beyond a flat color: procedural gradients drawn with
 * ordered dithering over a small palette band, and pictures decoded from a
 * file on disk. Nothing here is stored in the kernel image except the preset
 * stop colors. */
#include <stdint.h>

enum { UI_WALL_COLOR, UI_WALL_GRADIENT, UI_WALL_PICTURE, UI_WALL_KIND_N };
enum { UI_SHAPE_LINEAR, UI_SHAPE_DIAGONAL, UI_SHAPE_RADIAL, UI_SHAPE_N };
#define UI_GRADIENT_N 8

const char *ui_gradient_name(int gradient);
/* Three stops, top to bottom (or center to edge), in the installed style.
 * Gradient 0 follows the accent color. */
void ui_gradient_stops(int gradient, uint32_t stops[3]);
/* Loads a gradient into the wallpaper band and sets TK_DESKTOP to its middle
 * stop, then commits the palette. A drawn gradient recolors in place, so
 * switching presets needs no redraw. */
void ui_gradient_install(int gradient);
/* Fills a rectangle with the installed gradient. */
void ui_gradient_fill(int x, int y, int w, int h, int shape);
/* A small preview of any preset, dithered against the whole palette; round
 * clips it to the circle that fits the width. */
void ui_gradient_swatch(int x, int y, int w, int h, int gradient, int shape, int round);

/* Decodes an image file and draws it over the whole back buffer, scaled to
 * cover the screen (centered, overflow cropped) and dithered to the cube and
 * gray ramp, which no theme rewrites. The back buffer is untouched on
 * failure. Returns an IMAGE_* code from lib/image_decode.h. */
int ui_picture_render(const void *file, unsigned bytes);

#endif
