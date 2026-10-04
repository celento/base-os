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
static int render_skip=-1;
static int mouse_x,mouse_y,cursor_sx,cursor_sy,cursor_on;
static uint8_t cursor_saved[CURSOR_W*CURSOR_H];
typedef struct {uint8_t pixels[64000];int w,h,on;} TestCanvas;
static TestCanvas canvases[MAX_WIN];
#define canvas_pixels (canvases[context_slot].pixels)
#define canvas_w (canvases[context_slot].w)
#define canvas_h (canvases[context_slot].h)
#define canvas_on (canvases[context_slot].on)
static int view_rows,view_total,polls;
void kmemset(void *d,int v,int n){memset(d,v,(size_t)n);}
void kmemcpy(void *d,const void *s,int n){memmove(d,s,(size_t)n);}
int kstrlen(const char *s){return (int)strlen(s);}
void kstrcpy(char *d,const char *s){strcpy(d,s);}
void platform_poll(void){polls++;}
const unsigned char *term_canvas(void){return canvas_on?canvas_pixels:0;}
int term_canvas_width(void){return canvas_w;}
int term_canvas_height(void){return canvas_h;}
int term_canvas_size(int slot,int *width,int *height){
    assert(slot>=0&&slot<MAX_WIN);
    *width=canvases[slot].w;*height=canvases[slot].h;return canvases[slot].on;
}
int term_task_running(int slot){(void)slot;return 1;}
int term_count(void){return 3;}
const char *term_get(int row){return row==0?"Canvas updates preserve this text.":row==1?"This longer ordinary line wraps in a narrow Terminal window.":"Canvas task started.";}
void term_prompt(char *out,int max){(void)max;strcpy(out,"/>");}
const char *term_input(void){return "";}
void term_set_view(int rows,int total){view_rows=rows;view_total=total;}
int term_scroll_offset(void){return 0;}
static const char *win_display_title(int slot,char *title){(void)slot;strcpy(title,"canvas.bex");return title;}
static void context_set(int slot){if(slot>=0&&slot<MAX_WIN)context_slot=slot;}
static void draw_window_contents(Win *w,int inactive);
#include "native_render_kernel.inc"
/* Only the application dispatch is a fixture. Both kinds use the production
 * chrome, Terminal renderer, z-order traversal and per-window corner masks. */
static void draw_window_contents(Win *w,int inactive){
    context_set((int)(w-wins));
    if(w->kind==WK_TERM)draw_term(w->x,w->y,w->w,w->h,inactive);
    else gui_draw_window(w->x,w->y,w->w,w->h,"Opaque cover",0,inactive?WIN_INACTIVE:0);
}
static uint8_t back[1280*800],linear[1280*800*4];
static uint8_t expected[sizeof back],expected_linear[sizeof linear],expected_cursor[sizeof cursor_saved];
static void fixture_reset(void){
    memset(wins,0,sizeof wins);memset(canvases,0,sizeof canvases);context_slot=0;
    for(int i=0;i<MAX_WIN;i++){canvases[i].w=160;canvases[i].h=100;canvases[i].on=1;}
}
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
    fixture_reset();
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
    TermTaskUpdate text={2,TERM_TASK_TEXT};
    wins[3].x=100;assert(term_task_render_action(text)==TERM_RENDER_FULL); /* corner/edge */
    wins[3].x=90;wins[3].y=70;assert(term_task_render_action(text)==TERM_RENDER_FULL); /* titlebar */
    wins[3].y=60;wins[3].h=240;assert(term_task_render_action(text)==TERM_RENDER_FULL); /* bottom corner */
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
static void composite_scene(void){
    draw_ramp(0,0,fb_w,fb_h,PAL_DESK,PAL_DESK_N,0,RAMP_END(PAL_DESK_N));
    draw_ui();
}
static void composite_compare(void){
    memcpy(expected,back,(size_t)fb_w*fb_h);
    memcpy(expected_linear,linear,(size_t)fb_pitch*fb_h);
    memcpy(expected_cursor,cursor_saved,sizeof cursor_saved);
    cursor_restore();composite_scene();cursor_save_draw();gfx_present();
    assert(!memcmp(expected,back,(size_t)fb_w*fb_h));
    assert(!memcmp(expected_linear,linear,(size_t)fb_pitch*fb_h));
    assert(!memcmp(expected_cursor,cursor_saved,sizeof cursor_saved));
}
static void composite_begin(int screen_w,int screen_h,int bpp){
    gfx_init(back,linear,screen_w,screen_h,bpp,screen_w*(bpp/8));
    for(int owner=0;owner<MAX_WIN;owner++){
        TestCanvas *c=&canvases[owner];
        for(int i=0;i<c->w*c->h;i++)c->pixels[i]=(uint8_t)(i*17+i/c->w+owner*37);
    }
    mouse_x=120;mouse_y=180;cursor_on=0;
    composite_scene();cursor_save_draw();gfx_present();
}
static void composite_update(int action,unsigned flags,int phase){
    TestCanvas *c=&canvases[2];
    for(int i=0;i<c->w*c->h;i++)c->pixels[i]=(uint8_t)(i*13+i/c->w+phase*29);
    /* An unrelated selected terminal has a different size and no frame. */
    context_slot=7;canvases[7].on=0;
    assert(term_task_render_action((TermTaskUpdate){2,flags})==action&&context_slot==7);
    mouse_x=phase%2?fb_w-3:120;mouse_y=phase%2?fb_h-3:180;
    cursor_restore();polls=0;
    if(dirty||action==TERM_RENDER_FULL)composite_scene();
    else if(action==TERM_RENDER_CANVAS){
        context_set(2);Win *w=&wins[2];draw_term_canvas(w->x,w->y,w->w,w->h);
    }
    else assert(polls==0);
    cursor_save_draw();gfx_present();composite_compare();
}
static void canvas_guards(void){
    fixture_reset();
    wins[2]=(Win){.x=100,.y=100,.w=500,.h=380,.open=1,.z=1,.kind=WK_TERM};
    wins[3]=wins[2];wins[3].z=2;
    TermTaskUpdate update={2,TERM_TASK_CANVAS};
    assert(!window_content_hidden(2)&&term_canvas_hidden(2));
    assert(term_task_render_action(update)==TERM_RENDER_NONE);
    int *blockers[]={&dirty,&name_dlg,&edit_close_dlg,&open_dlg,&launcher_on,
        &display_pending,&saver_on,&fm_dragging,&fm_drag_active,&fm_renaming,&edit_dragging,&paint_dragging,&paint_shape_drag,&mouse_left};
    for(unsigned i=0;i<sizeof blockers/sizeof *blockers;i++){
        *blockers[i]=1;assert(term_task_render_action(update)==TERM_RENDER_FULL);*blockers[i]=0;
    }
    int *indices[]={&open_menu,&dragging_win,&resizing_win,&drag_cached};
    for(unsigned i=0;i<sizeof indices/sizeof *indices;i++){
        *indices[i]=0;assert(term_task_render_action(update)==TERM_RENDER_FULL);*indices[i]=-1;
    }
    for(unsigned flags=1;flags<16;flags++)if(flags!=TERM_TASK_CANVAS)
        assert(term_task_render_action((TermTaskUpdate){2,flags})==TERM_RENDER_FULL);
    canvases[2].on=0;assert(term_task_render_action(update)==TERM_RENDER_FULL);
    canvases[2].on=1;wins[2].h=30;
    assert(term_task_render_action(update)==TERM_RENDER_FULL); /* No drawable rectangle. */
}
static void occlusion_pixels(int bpp){
    /* Same-size/maximized native terminals: each has independent pixels. The
     * background border is not contained, but all its canvas damage is. */
    const int screens[][2]={{800,600},{1024,768},{1280,720},{1280,800}};
    for(unsigned mode=0;mode<sizeof screens/sizeof *screens;mode++)for(int size=0;size<2;size++){
        fixture_reset();int sw=screens[mode][0],sh=screens[mode][1];
        wins[2]=(Win){.x=2,.y=34,.w=sw-4,.h=sh-82,.open=1,.z=1,.kind=WK_TERM};
        wins[3]=wins[2];wins[3].z=2;wins[5]=wins[3];wins[5].z=3;
        canvases[2].w=size?320:160;canvases[2].h=size?200:100;
        assert(!window_content_hidden(2)&&term_canvas_hidden(2));
        composite_begin(sw,sh,bpp);composite_update(TERM_RENDER_NONE,TERM_TASK_CANVAS,1);
        composite_update(TERM_RENDER_NONE,TERM_TASK_CANVAS,2);
        /* Reveal the latest suppressed frame by moving both covering windows.
         * Normal UI changes set dirty; no task update is needed for repaint. */
        wins[3].x=sw-100;wins[5].x=sw-80;dirty=1;
        cursor_restore();composite_scene();cursor_save_draw();gfx_present();
        assert(get_pixel(2+1+TERM_PAD,34+TITLE_H+1+TERM_PAD)==canvases[2].pixels[0]);
        composite_compare();
        composite_update(TERM_RENDER_FULL,TERM_TASK_CANVAS,3);dirty=0;
    }
    fixture_reset();
    wins[2]=(Win){.x=100,.y=100,.w=500,.h=380,.open=1,.z=1,.kind=WK_TERM};
    int dw,dh;term_canvas_geometry(500,380,160,100,&dw,&dh);
    int x=113,y=145;
    Win exact={.x=x-8,.y=y-TITLE_H-1,.w=dw+16,.h=dh+TITLE_H+9,.open=1,.z=2};
    /* Inclusive near and exclusive far edges of the conservative opaque area. */
    for(int edge=0;edge<10;edge++){
        wins[3]=exact;
        if(edge==1)wins[3].x++;
        if(edge==2)wins[3].y++;
        if(edge==3)wins[3].w--;
        if(edge==4)wins[3].h--;
        if(edge==5){wins[3].x+=8;wins[3].y+=dh-6;} /* Rounded top corner exposed. */
        if(edge==6){wins[3].x+=8;wins[3].h=TITLE_H+7;} /* Rounded bottom corner. */
        if(edge==7)wins[3].z=0;
        if(edge==8)wins[3].min=1;
        if(edge==9)wins[3].open=0;
        composite_begin(800,600,bpp);
        int action=edge==0?TERM_RENDER_NONE:edge>=7?TERM_RENDER_CANVAS:TERM_RENDER_FULL;
        composite_update(action,TERM_TASK_CANVAS,edge+1);
    }
    /* A truly exposed one-pixel canvas strip must not disappear. */
    wins[3]=exact;wins[3].x=x+1;wins[3].y=80;
    composite_begin(800,600,bpp);composite_update(TERM_RENDER_FULL,TERM_TASK_CANVAS,10);
    /* Existing whole-window containment remains valid for canvas updates. */
    wins[3]=(Win){.x=80,.y=60,.w=540,.h=450,.open=1,.z=2};
    assert(window_content_hidden(2));
    composite_begin(800,600,bpp);composite_update(TERM_RENDER_NONE,TERM_TASK_CANVAS,11);
    /* Multiple partial covers are intentionally not combined into a region. */
    wins[3]=exact;wins[3].w=dw/2+16;
    wins[4]=exact;wins[4].x+=dw/2;wins[4].w=dw/2+16;wins[4].z=3;
    composite_begin(800,600,bpp);composite_update(TERM_RENDER_FULL,TERM_TASK_CANVAS,12);
    wins[4].open=0;
    /* A cover fitting a selected 160x100 frame must not hide the explicit
     * owner's larger 320x200 frame at its different scale. */
    wins[3]=exact;wins[2].h=600;canvases[2].w=320;canvases[2].h=200;
    composite_begin(1280,800,bpp);composite_update(TERM_RENDER_FULL,TERM_TASK_CANVAS,13);
    wins[2].h=380;canvases[2].w=160;canvases[2].h=100;
    wins[3]=exact;wins[2].min=1;
    composite_begin(800,600,bpp);composite_update(TERM_RENDER_NONE,TERM_TASK_CANVAS,11);
    composite_update(TERM_RENDER_FULL,TERM_TASK_LIFECYCLE,12);
    wins[2].min=0;
    /* Text/layout/lifecycle bits stay full, even when canvas pixels fit. */
    for(unsigned flags=1;flags<16;flags++)if(flags!=TERM_TASK_CANVAS){
        composite_begin(800,600,bpp);composite_update(TERM_RENDER_FULL,flags,13+(int)flags);
    }
    /* Theme/structural invalidation and drag-cache paths cannot be suppressed. */
    dirty=1;ui_chrome=9;ui_border=10;ui_body=12;
    composite_update(TERM_RENDER_FULL,TERM_TASK_CANVAS,30);dirty=0;
    dragging_win=3;drag_cached=3;
    composite_update(TERM_RENDER_FULL,TERM_TASK_CANVAS,31);dragging_win=drag_cached=-1;
}
int main(void) {
    geometry();eligibility();canvas_guards();
    for(int bpp=16;bpp<=32;bpp+=8){
        fixture_reset();
        pixels(800,600,360,200,160,bpp);
        pixels(800,600,540,360,160,bpp);
        pixels(800,600,540,361,320,bpp);
        pixels(1024,768,800,550,320,bpp);
        pixels(1280,800,800,551,320,bpp);
        pixels(1280,800,1276,718,320,bpp);
        occlusion_pixels(bpp);
    }
    puts("Native render: exact geometry/guards; partial and occluded vs full pixels, independent owners, maximized/minimized/partial covers, edge/corner boundaries, moved covers, lifecycle and 16/24/32-bit presentation passed.");
}
