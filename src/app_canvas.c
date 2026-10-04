#include "app_canvas.h"
#include "fs.h"
unsigned app_canvas_reset(AppCanvas *c){
    kmemset(c->pixels,0,sizeof c->pixels);c->on=0;
    c->width=PROGRAM_CANVAS_DEFAULT_WIDTH;c->height=PROGRAM_CANVAS_DEFAULT_HEIGHT;
    /* Keep buffered mode when a script clears a live native host. */
    c->pending=0;c->published_on=0;
    c->published_width=PROGRAM_CANVAS_DEFAULT_WIDTH;
    c->published_height=PROGRAM_CANVAS_DEFAULT_HEIGHT;
    return APP_CANVAS_LAYOUT_CHANGED;
}
static unsigned canvas_drawn(AppCanvas *c){
    unsigned dirty=0;
    if(c->buffered)c->pending=1;
    else {
        dirty=APP_CANVAS_PIXELS_CHANGED;
        if(!c->on)dirty|=APP_CANVAS_LAYOUT_CHANGED;
    }
    c->on=1;return dirty;
}
unsigned app_canvas_plot(AppCanvas *c,int x,int y,int color){
    if(x<0||x>=c->width||y<0||y>=c->height)return 0;
    unsigned dirty=canvas_drawn(c);
    c->pixels[y*c->width+x]=(unsigned char)color;return dirty;
}
/* Clip before adding coordinates, including signed origins. Empty/offscreen
 * rectangles do not activate or dirty the canvas. */
unsigned app_canvas_rect(AppCanvas *c,int x,int y,int width,int height,int color){
    if(width<=0||height<=0||x>=c->width||y>=c->height)return 0;
    if(x<0){if(x<=-width)return 0;width+=x;x=0;}
    if(y<0){if(y<=-height)return 0;height+=y;y=0;}
    if(width>c->width-x)width=c->width-x;
    if(height>c->height-y)height=c->height-y;
    unsigned dirty=canvas_drawn(c);
    unsigned char *row=c->pixels+y*c->width+x;
    for(int i=0;i<height;i++)kmemset(row+i*c->width,color,width);
    return dirty;
}
int app_canvas_resize(AppCanvas *c,int width,int height){
    if(!((width==(int)PROGRAM_CANVAS_DEFAULT_WIDTH&&height==(int)PROGRAM_CANVAS_DEFAULT_HEIGHT)||
         (width==(int)PROGRAM_CANVAS_MAX_WIDTH&&height==(int)PROGRAM_CANVAS_MAX_HEIGHT)))return -1;
    kmemset(c->pixels,0,sizeof c->pixels);c->width=width;c->height=height;c->on=1;
    if(c->buffered){c->pending=1;return 0;}
    return APP_CANVAS_LAYOUT_CHANGED;
}
/* Serialized kernel context copies the entire active frame before publishing
 * its dimensions. This function never polls or dispatches another process. */
unsigned app_canvas_publish(AppCanvas *c,unsigned char *published){
    if(!c->buffered||!c->pending)return 0;
    int layout=c->published_on!=c->on||c->published_width!=c->width||c->published_height!=c->height;
    if(c->on)kmemcpy(published,c->pixels,c->width*c->height);
    c->published_on=c->on;c->published_width=c->width;c->published_height=c->height;c->pending=0;
    return layout?APP_CANVAS_LAYOUT_CHANGED:APP_CANVAS_PIXELS_CHANGED;
}
AppCanvasFrame app_canvas_frame(const AppCanvas *c,const unsigned char *published){
    return (AppCanvasFrame){c->buffered?(c->published_on?published:0):(c->on?c->pixels:0),
                           c->buffered?c->published_width:c->width,
                           c->buffered?c->published_height:c->height};
}
