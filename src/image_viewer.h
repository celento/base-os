#ifndef BASEOS_IMAGE_VIEWER_H
#define BASEOS_IMAGE_VIEWER_H

#include "image_decode.h"

#define IMAGE_VIEWER_W 720
#define IMAGE_VIEWER_H 520
#define IMAGE_VIEWER_MIN_W 360
#define IMAGE_VIEWER_MIN_H 200

/* One image window. Open is synchronous and returns IMAGE_OK (0) on success,
 * otherwise a negative IMAGE_* error. Both success and failure replace the
 * earlier image; draw also presents errors clearly. No file pointer is kept.
 * x/y/w/h describe the client area, excluding the OS window title/border. */
int image_viewer_open(const void *file, unsigned bytes, const char *name);
void image_viewer_draw(int x, int y, int w, int h);
int image_viewer_key(int sc, char ch);
int image_viewer_click(int x, int y, int w, int h, int mx, int my);
int image_viewer_scroll(int lines);
void image_viewer_close(void);
int image_viewer_loaded(void);
const char *image_viewer_title(void);
const char *image_viewer_status(void);

#endif
