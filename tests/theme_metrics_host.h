/* The production style metrics (src/ui/theme.c) for harnesses that extract
 * desktop code without the graphics layer. Palette writes are dropped; the
 * UiTheme struct, the app tokens and the preset tables are the real ones.
 * Call ui_theme_install(style, accent, desktop) before using desktop chrome
 * geometry such as TITLE_H or MENUBAR_H. */
#ifndef THEME_METRICS_HOST_H
#define THEME_METRICS_HOST_H
#include "kernel/gfx.h"
#ifndef THEME_METRICS_HOST_HAVE_PAL32
uint32_t pal32[256];
#endif
#define gfx_set_color(index, rgb) ((void)sizeof(index), (void)sizeof(rgb))
#define gfx_pal_commit() ((void)0)
#include "../src/ui/theme.c"
#undef gfx_set_color
#undef gfx_pal_commit
#endif
