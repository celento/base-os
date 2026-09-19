#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "layout.h"
static uint8_t test_mirror[FB_CAPACITY];
#undef PRESENT_BASE
#define PRESENT_BASE ((uintptr_t)test_mirror)
#define GFX_HOST_TEST
#include "../src/gfx.c"
void kmemset(void *d,int v,int n){memset(d,v,(size_t)n);}
void kmemcpy(void *d,const void *s,int n){memmove(d,s,(size_t)n);}
static uint8_t back[73*53],linear[73*53*4+53*12],reference[sizeof linear];
static void output_equivalence(int bpp){
    int pitch=73*(bpp/8)+12;
    memset(linear,0xa5,sizeof linear);gfx_init(back,linear,73,53,bpp,pitch);
    draw_ramp(0,0,73,53,PAL_DESK,PAL_DESK_N,0,RAMP_END(PAL_DESK_N));gfx_present();
    draw_rect(0,0,1,1,COLOR_RED);draw_rect(72,52,1,1,COLOR_WHITE);
    draw_round_rect(10,9,40,30,8,COLOR_BLUE);draw_string("Test",11,15,COLOR_WHITE);gfx_present();
    memcpy(reference,linear,sizeof linear);flip_vga();assert(!memcmp(linear,reference,sizeof linear));
    /* Explicit cursor updates must not leave a stale presentation cache. */
    uint8_t old=back[12*73+12];put_pixel(12,12,COLOR_GREEN);flip_rect(12,12,1,1);put_pixel(12,12,old);gfx_present();
    memcpy(reference,linear,sizeof linear);flip_vga();assert(!memcmp(linear,reference,sizeof linear));
    gfx_set_ramp(PAL_DESK,PAL_DESK_N,0xff0000,0x200000);gfx_pal_commit();gfx_present();
    memcpy(reference,linear,sizeof linear);flip_vga();assert(!memcmp(linear,reference,sizeof linear));
    for(int y=0;y<53;y++)for(int i=73*(bpp/8);i<pitch;i++)assert(linear[y*pitch+i]==0xa5);
}
int main(void){
    output_equivalence(16);output_equivalence(24);output_equivalence(32);
    gfx_init(back,linear,73,53,32,73*4);
    draw_rect(0,0,73,53,COLOR_GREEN);uint8_t saved[256];gfx_window_corners(10,10,40,30,saved,0,COLOR_GRAY);
    draw_rect(10,10,40,30,COLOR_WHITE);gfx_window_corners(10,10,40,30,saved,1,COLOR_GRAY);
    assert(get_pixel(10,10)==COLOR_GREEN&&get_pixel(49,39)==COLOR_GREEN&&get_pixel(30,25)==COLOR_WHITE);
    uint8_t before[sizeof back];memcpy(before,back,sizeof back);draw_shadow(10,10,40,30);assert(!memcmp(before,back,sizeof back));
    gfx_window_corners(-4,-4,40,30,saved,0,COLOR_GRAY);gfx_window_corners(-4,-4,40,30,saved,1,COLOR_GRAY);
    draw_rect(0,0,73,53,COLOR_GREEN);
    draw_round_frame(10,10,28,28,14,COLOR_WHITE);
    assert(get_pixel(24,24)==COLOR_GREEN&&get_pixel(18,18)==COLOR_GREEN);
    assert(get_pixel(10,10)==COLOR_GREEN&&get_pixel(24,10)!=COLOR_GREEN);
    for(int y=0;y<28;y++)for(int x=0;x<28;x++)assert(get_pixel(10+x,10+y)==get_pixel(37-x,37-y));
    draw_round_frame(-4,-4,12,12,6,COLOR_WHITE);
    draw_round_frame(0,0,1,1,8,COLOR_WHITE);assert(get_pixel(0,0)==COLOR_WHITE);
    /* Palette changes must invalidate cached nearest-color results. */
    gfx_set_ramp(PAL_DESK,1,0x123456,0x123456);gfx_pal_commit();assert(gfx_rgb(0x12,0x34,0x56)==PAL_DESK);
    gfx_set_ramp(PAL_DESK,1,0xffeeee,0xffeeee);gfx_pal_commit();assert(gfx_rgb(0x12,0x34,0x56)!=PAL_DESK);
    for(int ch=32;ch<127;ch++){draw_char(ch,65,45,COLOR_BLACK);draw_edit_char(ch,-4,-3,COLOR_WHITE);}
    assert(glyph_level((uint8_t[]){0x1f},1,0,0)==1&&glyph_level((uint8_t[]){0x1f},1,0,1)==15);
    puts("graphics: clipped rounded corners, palette invalidation, 4-bit fonts, dirty spans, cursor restoration, 16/24/32-bit pitch passed");
}
