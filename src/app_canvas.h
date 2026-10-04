#ifndef APP_CANVAS_H
#define APP_CANVAS_H
#include "program.h"

#define APP_CANVAS_PIXELS (PROGRAM_CANVAS_MAX_WIDTH*PROGRAM_CANVAS_MAX_HEIGHT)
/* Internal dirty classifications retain the historical Terminal flag values. */
enum { APP_CANVAS_PIXELS_CHANGED=1, APP_CANVAS_LAYOUT_CHANGED=4 };
typedef struct {
    int on,width,height,buffered,pending;
    int published_on,published_width,published_height;
    unsigned char pixels[APP_CANVAS_PIXELS];
} AppCanvas;
typedef struct {
    const unsigned char *pixels;
    int width,height;
} AppCanvasFrame;
/* Drawing touches only this explicit working canvas. No selection, input,
 * process dispatch or publication occurs in these functions. */
unsigned app_canvas_reset(AppCanvas *canvas);
unsigned app_canvas_plot(AppCanvas *canvas,int x,int y,int color);
unsigned app_canvas_rect(AppCanvas *canvas,int x,int y,int width,int height,int color);
/* Returns -1 for unsupported geometry; otherwise the dirty classification. */
int app_canvas_resize(AppCanvas *canvas,int width,int height);
unsigned app_canvas_publish(AppCanvas *canvas,unsigned char *published);
AppCanvasFrame app_canvas_frame(const AppCanvas *canvas,const unsigned char *published);
#endif
