#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "video.h"
#include "gfx.h"
static uint8_t screen[640*480],pixels[320*240];
uint8_t *fb=screen;
int fb_w=640,fb_h=480;
static VideoFrame frame={.pixels=pixels,.width=320,.height=240};
static VideoStatus status={.aspect_num=10000,.aspect_den=10000};
static int available=1;
const VideoFrame *video_frame(void){return available?&frame:0;}
const VideoStatus *video_status(void){return &status;}
void draw_rect(int x,int y,int w,int h,uint8_t color){
    for(int dy=y;dy<y+h;++dy)for(int dx=x;dx<x+w;++dx)
        if(dx>=0&&dy>=0&&dx<fb_w&&dy<fb_h)fb[dy*fb_w+dx]=color;
}
#include "../src/video_draw.c"
static void check(int x,int y,int w,int h){
    memset(screen,253,sizeof screen);video_draw(x,y,w,h);
    unsigned sw=frame.width*status.aspect_num/1000,sh=frame.height*status.aspect_den/1000;
    int dw=w,dh=(int)((unsigned)w*sh/sw);
    if(dh>h){dh=h;dw=(int)((unsigned)h*sw/sh);}
    int left=x+(w-dw)/2,top=y+(h-dh)/2;
    for(int sy=0;sy<fb_h;++sy)for(int sx=0;sx<fb_w;++sx){
        uint8_t expected=253;
        if(sx>=x&&sx<x+w&&sy>=y&&sy<y+h){
            expected=COLOR_BLACK;
            if(available&&sx>=left&&sx<left+dw&&sy>=top&&sy<top+dh)
                expected=pixels[(unsigned)(sy-top)*frame.height/(unsigned)dh*frame.width+(unsigned)(sx-left)*frame.width/(unsigned)dw];
        }
        assert(screen[sy*fb_w+sx]==expected);
    }
}
int main(void){
    for(unsigned i=0;i<sizeof pixels;++i)pixels[i]=(uint8_t)(i%249+1);
    check(0,0,640,480);check(10,20,420,220);check(200,120,150,310);
    check(-30,-25,500,320);check(520,350,300,280);
    status.aspect_num=12051;check(4,4,620,440);
    available=0;check(10,20,400,300);
    puts("Video fit, crop, letterbox, pixel aspect, frame samples and framebuffer bounds passed.");
}
