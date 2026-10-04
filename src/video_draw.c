#include "video.h"
#include "gfx.h"

void video_draw(int x,int y,int w,int h) {
    if (w<=0 || h<=0) return;
    draw_rect(x,y,w,h,COLOR_BLACK);
    const VideoFrame *frame=video_frame();
    const VideoStatus *status=video_status();
    if (!frame || !frame->pixels || !frame->width || !frame->height) return;
    /* MPEG-1 stores pixel aspect ratio; normalize it before fit-to-window.
     * Intermediate products stay bounded by the accepted source geometry. */
    unsigned display_w=frame->width*status->aspect_num/1000;
    unsigned display_h=frame->height*status->aspect_den/1000;
    int out_w=w,out_h=(int)((unsigned)w*display_h/display_w);
    if (out_h>h) { out_h=h;out_w=(int)((unsigned)h*display_w/display_h); }
    if (out_w<1 || out_h<1) return;
    x+=(w-out_w)/2;y+=(h-out_h)/2;
    /* Backbuffer has a width-byte stride independently of the hardware LFB. */
    int x0=x<0?0:x,y0=y<0?0:y,x1=x+out_w,y1=y+out_h;
    if (x1>fb_w) x1=fb_w;
    if (y1>fb_h) y1=fb_h;
    /* Exact nearest-neighbor indices, calculated once per column instead of
     * dividing for every screen pixel. Small blocks keep stack use bounded. */
    uint16_t source_x[256];
    for (int block=x0;block<x1;block+=256) {
        int columns=x1-block;
        if (columns>256) columns=256;
        for (int col=0;col<columns;++col)
            source_x[col]=(uint16_t)((unsigned)(block+col-x)*frame->width/(unsigned)out_w);
        for (int dy=y0;dy<y1;++dy) {
            unsigned sy=(unsigned)(dy-y)*frame->height/(unsigned)out_h;
            uint8_t *row=fb+dy*fb_w+block;
            const uint8_t *source=frame->pixels+sy*frame->width;
            for (int col=0;col<columns;++col) row[col]=source[source_x[col]];
        }
    }
}
