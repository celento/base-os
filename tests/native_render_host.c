/* Real graphics, terminal renderer, corner masks and cursor/presenter path. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "layout.h"
#include "term.h"
#include "../assets/cursor_art.h"
static uint8_t test_mirror[FB_CAPACITY];
#undef PRESENT_BASE
#define PRESENT_BASE ((uintptr_t)test_mirror)
#define GFX_HOST_TEST
#include "../src/gfx.c"
#define TITLE_H 32
#define TERM_PAD 12
#define EDIT_LINE_H (EDIT_FONT_H+1)
#define TERM_FG gfx_gray(0xE8)
#define CHAR_H UI_FONT_H
#define CLOSE_S 18
#define INFO_H 28
#define SB 16
#define WIN_INFO 1
#define WIN_SCROLL 2
#define WIN_NOCLOSE 4
#define WIN_INACTIVE 8
#define CURSOR_W CURSOR_ART_W
#define CURSOR_H CURSOR_ART_H
#define MAX_WIN 8
#define WK_TERM 11
static uint8_t ui_border=2,ui_body=15,ui_chrome=3,ui_chrome_dk=2;
static uint8_t ui_text=0,ui_text_dim=1,ui_accent=9,ui_track=3;
typedef struct {int x,y,w,h,open,min,z,kind;} Win;
static Win wins[MAX_WIN];
static int context_slot,dirty,name_dlg,edit_close_dlg,open_dlg,launcher_on;
static int open_menu=-1,display_pending,saver_on,dragging_win=-1,resizing_win=-1;
static int drag_cached=-1,fm_dragging,fm_drag_active,fm_renaming,edit_dragging,paint_dragging,paint_shape_drag,mouse_left;
static int mouse_x,mouse_y,cursor_sx,cursor_sy,cursor_on;
static uint8_t cursor_saved[CURSOR_W*CURSOR_H];
static uint8_t canvas_pixels[64000];
static int canvas_w=160,canvas_h=100,canvas_on=1,view_rows,view_total,polls;
void kmemset(void *d,int v,int n){memset(d,v,(size_t)n);}
void kmemcpy(void *d,const void *s,int n){memmove(d,s,(size_t)n);}
int kstrlen(const char *s){return (int)strlen(s);}
void kstrcpy(char *d,const char *s){strcpy(d,s);}
void platform_poll(void){polls++;}
const unsigned char *term_canvas(void){return canvas_on?canvas_pixels:0;}
int term_canvas_width(void){return canvas_w;}
int term_canvas_height(void){return canvas_h;}
int term_task_running(int slot){(void)slot;return 1;}
int term_count(void){return 3;}
const char *term_get(int row){return row==0?"Canvas updates preserve this text.":row==1?"This longer ordinary line wraps in a narrow Terminal window.":"Canvas task started.";}
void term_prompt(char *out,int max){(void)max;strcpy(out,"/>");}
const char *term_input(void){return "";}
void term_set_view(int rows,int total){view_rows=rows;view_total=total;}
int term_scroll_offset(void){return 0;}
static const char *win_display_title(int slot,char *title){(void)slot;strcpy(title,"canvas.bex");return title;}
#include "native_render_kernel.inc"
static uint8_t back[1280*800],linear[1280*800*4];
static uint8_t expected[sizeof back],expected_linear[sizeof linear],expected_cursor[sizeof cursor_saved];
static void full_scene(Win *w) {
    draw_ramp(0,0,fb_w,fb_h,PAL_DESK,PAL_DESK_N,0,RAMP_END(PAL_DESK_N));
    gui_draw_window(12,38,400,210,"Background",0,WIN_INACTIVE);
    uint8_t corners[256];
    gfx_window_corners(w->x-1,w->y-1,w->w+2,w->h+2,corners,0,ui_border);
    draw_term(w->x,w->y,w->w,w->h,0);
    gfx_window_corners(w->x-1,w->y-1,w->w+2,w->h+2,corners,1,ui_border);
}
static void geometry(void) {
    for(int sw=160;sw<=320;sw+=160)for(int ww=360;ww<=1276;ww+=7)
        for(int wh=200;wh<=760;wh++){
            int h=sw>160&&wh>550?400:wh>360?200:100;
            int ah=wh-TITLE_H-2-TERM_PAD*2-EDIT_LINE_H*2-8,aw=ww-2-TERM_PAD*2;
            if(h>ah)h=ah;
            int w=sw*h/(sw*5/8);
            if(w>aw){w=aw;h=(sw*5/8)*w/sw;}
            int actual_w,actual_h;
            assert(term_canvas_geometry(ww,wh,sw,sw*5/8,&actual_w,&actual_h)==h+8);
            assert(actual_w==w&&actual_h==h);
        }
    int w,h;assert(!term_canvas_geometry(10,30,160,100,&w,&h)&&!w&&!h);
    assert(!term_canvas_geometry(360,200,0,100,&w,&h)&&!w&&!h);
}
static void eligibility(void) {
    memset(wins,0,sizeof wins);
    wins[2]=(Win){.x=100,.y=100,.w=360,.h=200,.open=1,.z=1,.kind=WK_TERM};
    TermTaskUpdate canvas={2,TERM_TASK_CANVAS};
    assert(term_task_render_action(canvas)==TERM_RENDER_CANVAS);
    int *blockers[]={&dirty,&name_dlg,&edit_close_dlg,&open_dlg,&launcher_on,
        &display_pending,&saver_on,&fm_dragging,&fm_drag_active,&fm_renaming,&edit_dragging,&paint_dragging,&paint_shape_drag,&mouse_left};
    for(unsigned i=0;i<sizeof blockers/sizeof *blockers;i++){
        *blockers[i]=1;assert(term_task_render_action(canvas)==TERM_RENDER_FULL);*blockers[i]=0;
    }
    int *indices[]={&open_menu,&dragging_win,&resizing_win,&drag_cached};
    for(unsigned i=0;i<sizeof indices/sizeof *indices;i++){
        *indices[i]=0;assert(term_task_render_action(canvas)==TERM_RENDER_FULL);*indices[i]=-1;
    }
    for(unsigned flags=1;flags<16;flags++)if(flags!=TERM_TASK_CANVAS)
        assert(term_task_render_action((TermTaskUpdate){2,flags})==TERM_RENDER_FULL);
    wins[2].min=1;
    assert(term_task_render_action(canvas)==TERM_RENDER_NONE);
    assert(term_task_render_action((TermTaskUpdate){2,TERM_TASK_TEXT|TERM_TASK_LAYOUT})==TERM_RENDER_NONE);
    assert(term_task_render_action((TermTaskUpdate){2,TERM_TASK_LIFECYCLE})==TERM_RENDER_FULL);
    wins[2].min=0;
    wins[3]=(Win){.x=200,.y=200,.w=400,.h=250,.open=1,.z=2};
    assert(term_task_render_action(canvas)==TERM_RENDER_FULL); /* exposed background */
    wins[3]=(Win){.x=90,.y=60,.w=380,.h=250,.open=1,.z=2};
    assert(term_task_render_action(canvas)==TERM_RENDER_NONE); /* wholly opaque cover */
    assert(term_task_render_action((TermTaskUpdate){2,TERM_TASK_LIFECYCLE})==TERM_RENDER_FULL);
    wins[3].x=100;assert(term_task_render_action(canvas)==TERM_RENDER_FULL); /* corner/edge */
    wins[3].x=90;wins[3].y=70;assert(term_task_render_action(canvas)==TERM_RENDER_FULL); /* titlebar */
    wins[3].y=60;wins[3].h=240;assert(term_task_render_action(canvas)==TERM_RENDER_FULL); /* bottom corner */
    wins[3].h=250;wins[3].min=1;assert(term_task_render_action(canvas)==TERM_RENDER_CANVAS);
    wins[2].kind=0;assert(term_task_render_action(canvas)==TERM_RENDER_NONE);
    wins[2].kind=WK_TERM;
    assert(term_task_render_action((TermTaskUpdate){2,0})==TERM_RENDER_NONE);
    wins[2].open=0;assert(term_task_render_action(canvas)==TERM_RENDER_NONE);
    assert(term_task_render_action((TermTaskUpdate){-1,0})==TERM_RENDER_NONE);
    assert(!partial_client_ready(-1)&&!partial_client_ready(MAX_WIN));
}
static void pixels(int screen_w,int screen_h,int ww,int wh,int sw,int bpp) {
    gfx_init(back,linear,screen_w,screen_h,bpp,screen_w*(bpp/8));
    Win w={.x=2,.y=34,.w=ww,.h=wh};
    canvas_w=sw;canvas_h=sw*5/8;canvas_on=1;
    for(int i=0;i<canvas_w*canvas_h;i++)canvas_pixels[i]=(uint8_t)(i*17+i/canvas_w);
    cursor_on=0;mouse_x=29;mouse_y=90;
    full_scene(&w);cursor_save_draw();gfx_present();
    for(int frame=0;frame<5;frame++){
        for(int i=0;i<canvas_w*canvas_h;i++)canvas_pixels[i]=(uint8_t)(i*7+i/canvas_w+frame*31);
        mouse_x=frame==0?29:frame==1?16:frame==2?screen_w-3:150;
        mouse_y=frame==0?90:frame==1?50:frame==2?screen_h-3:180;
        cursor_restore();polls=0;
        int rows=view_rows,total=view_total;
        draw_term_canvas(w.x,w.y,w.w,w.h);
        assert(polls>0&&rows==view_rows&&total==view_total);
        cursor_save_draw();gfx_present();
        memcpy(expected,back,(size_t)screen_w*screen_h);
        memcpy(expected_linear,linear,(size_t)fb_pitch*screen_h);
        memcpy(expected_cursor,cursor_saved,sizeof cursor_saved);
        cursor_restore();full_scene(&w);cursor_save_draw();gfx_present();
        assert(!memcmp(expected,back,(size_t)screen_w*screen_h));
        assert(!memcmp(expected_linear,linear,(size_t)fb_pitch*screen_h));
        assert(!memcmp(expected_cursor,cursor_saved,sizeof cursor_saved));
    }
}
int main(void) {
    geometry();eligibility();
    for(int bpp=16;bpp<=32;bpp+=8){
        pixels(800,600,360,200,160,bpp);
        pixels(800,600,540,360,160,bpp);
        pixels(800,600,540,361,320,bpp);
        pixels(1024,768,800,550,320,bpp);
        pixels(1280,800,800,551,320,bpp);
        pixels(1280,800,1276,718,320,bpp);
    }
    puts("Native render: exact geometry/guards; partial vs full pixels, rounded corners, cursor moves and 16/24/32-bit presentation passed.");
}
