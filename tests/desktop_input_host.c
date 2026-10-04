/* Real kernel compatibility router with observable app callbacks. No hardware,
 * corruption probes, or invented alternative routing implementation. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "input_ingress.h"
#include "native_ui.h"
#define MAX_WIN 8
#define MENUBAR_H 36
#define TITLE_H 32
#define TIMER_HZ 70
#define KEY_ESC 1
#define MENU_NONE -1
enum { WK_NONE=-1, WK_FILES, WK_EDIT, WK_PAINT, WK_WRITER, WK_SPREADSHEET, WK_TERM };
typedef struct { int kind,x,y,w,h,z,open,seq,min,maximized,old_x,old_y,old_w,old_h; } Win;
static Win wins[MAX_WIN];
static struct { struct {int dragging;} doc; int last_click_item; } window_state[MAX_WIN];
static uint32_t frame_count, last_input_frame;
static int input_routing,input_cursor_moved,input_tail_pending;
static uint32_t input_sample_ticks;
static uint64_t input_wake_serial;
static unsigned input_suppressed,input_saver_buttons,input_unknown_buttons,input_sync_loss;
static InputIngress device_input;
static int mouse_x,mouse_y,mouse_left,mouse_right,mouse_moved,shift_down,ctrl_down,alt_down;
static uint8_t key_sc;static char key_char;
static int open_menu=-1,open_dlg,name_dlg,edit_close_dlg,launcher_on,saver_on,display_pending;
static int dragging_win=-1,resizing_win=-1,drag_active,fm_dragging,fm_drag_active;
static int paint_dragging,paint_shape_drag,dirty,title_click_window,icon_last,pick_last_click_item;
static int drag_from_x,drag_from_y,drag_orig_x,drag_orig_y,fm_drag_sx,fm_drag_sy;
static int context_slot,edit_sel_b,edit_caret,fb_w=800,menu_sel,taskbar_hover;
static int resize_start_w,resize_start_h,resize_edges;
static uint32_t pick_last_click_frame;
#define fm_last_click_item (window_state[context_slot].last_click_item)
#define edit_dragging (window_state[context_slot].doc.dragging)
static int fm_cwd;
static void desktop_input_fence(void);
static void saver_start(void);
static void pointer(int dx,int dy,unsigned buttons,int wheel,unsigned ticks);
static void key(unsigned byte,unsigned ticks){input_keyboard_byte(&device_input,(uint8_t)byte,ticks);}
static void kmemcpy(void *to,const void *from,int n){memcpy(to,from,(unsigned)n);}
static int win_front(void){return wins[1].open && wins[1].z>wins[0].z?1:0;}
static int front_kind(void){return wins[win_front()].kind;}
static void win_clamp(Win *w){(void)w;}
static void win_minimum(Win *w,int *x,int *y){(void)w;*x=100;*y=100;}
static int win_geom_kind(int kind,int *x,int *y,int *w,int *h){(void)kind;*x=*y=0;*w=*h=100;return 1;}
static int edit_index_at(int x,int y,int w,int h,int mx,int my){(void)x;(void)y;(void)w;(void)h;(void)my;return mx;}
static int fs_root(void){return 0;}
static int arranged,drops,releases,paint_commits,paint_last_x,writer_active,writer_moves,writer_releases;
static void win_arrange(int i,int mode){(void)i;(void)mode;++arranged;}
static void fm_go_up(void){}
static void files_drop(void){++drops;fm_dragging=fm_drag_active=0;}
static void writer_release(void){writer_active=0;++writer_releases;}
static void spreadsheet_release(void){}
static void paint_mouse_up(void){++releases;if(paint_shape_drag)++paint_commits;paint_shape_drag=paint_dragging=0;}
static void paint_drag_tick(void){paint_last_x=mouse_x;}
static int writer_drag(int x,int y,int w,int h,int mx,int my){(void)x;(void)y;(void)w;(void)h;(void)mx;(void)my;if(writer_active)++writer_moves;return writer_active;}
static int spreadsheet_drag(int x,int y,int w,int h,int mx,int my){return writer_drag(x,y,w,h,mx,my);}
static int menu_item_at(int menu,int x,int y){(void)menu;(void)x;return y/30;}
static int taskbar_hover_at(void){return mouse_x/100;}
static void saver_stop(void){saver_on=0;last_input_frame=frame_count;dirty=1;}
static unsigned terminal_losses;
static void term_input_lost(int slot){(void)slot;++terminal_losses;}
static void drain_8042(void){}
static unsigned acquire_tail;
static unsigned input_acquire(unsigned budget){unsigned n=acquire_tail<budget?acquire_tail:budget;acquire_tail-=n;return n;}
#ifndef NATIVE_UI_DESKTOP_REAL
static uint32_t timer_ticks(void){return frame_count;}
static unsigned desktop_native_pointer(const InputSample *sample){(void)sample;return 0;}
void native_ui_cancel_all(unsigned reason,unsigned ticks,unsigned buttons){(void)reason;(void)ticks;(void)buttons;}
void native_ui_refresh(unsigned ticks,unsigned buttons){(void)ticks;(void)buttons;}
void native_ui_input_loss(unsigned ticks,unsigned buttons,unsigned dropped){(void)ticks;(void)buttons;(void)dropped;}
#endif
static uint32_t input_gesture_ticks(void);
static int double_click(int item,int *last_item,uint32_t *last_frame);
struct Record{char kind;int x,y,buttons,mod,value;unsigned ticks;};
static struct Record record[512];static unsigned records;
static void note(char kind,int value){assert(records<512);record[records++]=(struct Record){kind,mouse_x,mouse_y,mouse_left|(mouse_right<<1),shift_down|(ctrl_down<<1)|(alt_down<<2),value,input_gesture_ticks()};}
enum { CLICK_ONLY, TITLE_DRAG, PAINT_SHAPE, PICKER, START_SAVER, POLL_INSIDE, FOCUS_CHANGE, RESIZE };
static int mode,double_clicks;
static void handle_click(void){
    note('D',0);
    if(mode==TITLE_DRAG){++wins[0].z;dragging_win=0;drag_active=0;drag_from_x=mouse_x;drag_from_y=mouse_y;drag_orig_x=wins[0].x;drag_orig_y=wins[0].y;}
    if(mode==PAINT_SHAPE){paint_shape_drag=1;paint_last_x=mouse_x;}
    if(mode==PICKER)double_clicks+=double_click(4,&pick_last_click_item,&pick_last_click_frame);
    if(mode==START_SAVER)saver_start();
    if(mode==POLL_INSIDE){pointer(10,0,0,0,99);assert(mouse_x==400 && mouse_left==1);}
    if(mode==FOCUS_CHANGE)wins[0].z+=2;
    if(mode==RESIZE){resizing_win=0;resize_edges=2|8;drag_from_x=mouse_x;drag_from_y=mouse_y;drag_orig_x=wins[0].x;drag_orig_y=wins[0].y;resize_start_w=wins[0].w;resize_start_h=wins[0].h;}
}
static void handle_rclick(void){note('R',0);}
static void handle_wheel(int amount){note('W',amount);}
static void handle_key(void){note('K',key_char);if(key_char=='f'){wins[1].open=1;wins[1].z=wins[0].z+1;}if(key_char=='s')saver_start();}
#include "desktop_input_kernel.inc"
static void pointer(int dx,int dy,unsigned buttons,int wheel,unsigned ticks){
    input_mouse_byte(&device_input,(uint8_t)(8|buttons|(dx<0?16:0)|(dy<0?32:0)),ticks);
    input_mouse_byte(&device_input,(uint8_t)dx,ticks);input_mouse_byte(&device_input,(uint8_t)dy,ticks);
    input_mouse_byte(&device_input,(uint8_t)wheel,ticks);
}
static void fresh(void){
    memset(wins,0,sizeof wins);memset(window_state,0,sizeof window_state);memset(&input_scene,0,sizeof input_scene);
    wins[0]=(Win){.open=1,.seq=1,.kind=WK_PAINT,.x=100,.y=100,.w=300,.h=250,.z=1};
    mouse_x=400;mouse_y=300;mouse_left=mouse_right=mouse_moved=0;
    input_init(&device_input,800,600,400,300);input_mouse_type(&device_input,3);
    open_menu=-1;open_dlg=name_dlg=edit_close_dlg=launcher_on=saver_on=display_pending=0;
    input_suppressed=input_saver_buttons=input_unknown_buttons=input_sync_loss=0;input_wake_serial=0;input_cursor_moved=input_routing=input_tail_pending=0;acquire_tail=0;
    dragging_win=resizing_win=-1;drag_active=fm_dragging=fm_drag_active=0;paint_shape_drag=paint_dragging=0;
    records=0;terminal_losses=0;releases=drops=arranged=paint_commits=writer_active=writer_moves=double_clicks=0;mode=CLICK_ONLY;
    pick_last_click_item=title_click_window=icon_last=-1;pick_last_click_frame=0;
    frame_count=1000;desktop_input_remember_scene();
}
int main(void){
    fresh();key(0x2a,1);pointer(0,0,1,0,2);key(0xaa,3);pointer(5,-3,0,-1,4);key(0x1e,5);
    assert(desktop_input_turn()==5 && records==3 && releases==1);
    assert(record[0].kind=='D' && record[0].mod==1 && record[0].ticks==2);
    assert(record[1].kind=='W' && record[1].x==405 && record[1].y==303 && record[1].value==-1);
    assert(record[2].kind=='K' && record[2].value=='a');
    puts("Desktop ordering: exact sampled modifiers, wheel and legacy keys passed.");

    fresh();mode=TITLE_DRAG;
    pointer(0,0,1,0,10);pointer(30,-20,1,0,11);pointer(20,-10,0,0,12);
    uint64_t epoch=device_input.epoch;
    assert(desktop_input_turn()==3 && records==1 && releases==1);
    assert(wins[0].x==150 && wins[0].y==130 && dragging_win==-1 && !arranged && device_input.epoch==epoch);
    fresh();mode=RESIZE;pointer(0,0,1,0,10);pointer(30,-20,0,0,11);desktop_input_turn();
    assert(wins[0].w==330 && wins[0].h==270 && resizing_win==-1 && releases==1);
    fresh();mode=PAINT_SHAPE;pointer(0,0,1,0,10);pointer(30,0,0,0,11);desktop_input_turn();
    assert(records==1 && paint_commits==1 && paint_last_x==430 && !paint_shape_drag);
    fresh();mode=POLL_INSIDE;pointer(0,0,1,0,10);assert(desktop_input_turn()==2);
    assert(records==1 && releases==1 && mouse_x==410);
    puts("Desktop gestures: route-time focus, move, resize, final-UP coordinates and nested acquisition passed.");

    fresh();mode=PICKER;open_dlg=1;desktop_input_remember_scene();
    pointer(0,0,1,0,10);pointer(0,0,0,0,11);pointer(0,0,1,0,50);pointer(0,0,0,0,51);
    desktop_input_turn();assert(records==2 && !double_clicks);
    pointer(0,0,1,0,55);pointer(0,0,0,0,56);desktop_input_turn();assert(records==3 && double_clicks==1);
    fresh();mode=PAINT_SHAPE;pointer(0,0,1,0,10);key(0x21,11);pointer(20,0,1,0,12);pointer(0,0,0,0,13);
    desktop_input_turn();assert(records==2 && !paint_commits && !paint_shape_drag && !mouse_left);
    pointer(0,0,1,0,14);pointer(0,0,0,0,15);desktop_input_turn();assert(paint_commits==1);
    puts("Desktop cancellation: sampled double-click timing, picker history and focus-loss suppression passed.");

    fresh();pointer(0,0,1,0,1);pointer(0,0,0,0,2);key(0x1e,3);
    ++wins[0].seq;desktop_input_turn();assert(records==1 && record[0].kind=='K' && !mouse_left);
    pointer(0,0,1,0,4);pointer(0,0,0,0,5);desktop_input_turn();assert(records==2 && releases==1);
    fresh();input_mouse_byte(&device_input,9,1);++wins[0].seq;desktop_input_turn();
    input_mouse_byte(&device_input,0,2);input_mouse_byte(&device_input,0,2);input_mouse_byte(&device_input,0,2);
    desktop_input_turn();assert(!records && !mouse_left);
    pointer(0,0,0,0,3);pointer(0,0,1,0,4);pointer(0,0,0,0,5);desktop_input_turn();assert(records==1);
    puts("Desktop lifetimes: autonomous same-slot reuse and in-progress packet fencing passed.");

    fresh();for(unsigned i=0;i<65;++i)key(0x1e,i);
    assert(desktop_input_turn()==64 && records==64 && input_pending(&device_input));
    assert(desktop_input_turn()==1 && records==65 && !input_pending(&device_input));
    fresh();wins[1].open=1;wins[1].kind=WK_TERM;desktop_input_remember_scene();
    mode=PAINT_SHAPE;pointer(0,0,1,0,1);desktop_input_turn();assert(paint_shape_drag);
    for(unsigned i=0;i<INPUT_CAPACITY;++i)key(0x1e,2+i);
    pointer(0,0,1,0,300);desktop_input_turn();assert(!paint_shape_drag && !paint_commits && records==1 && !mouse_left && terminal_losses==1);
    pointer(2,0,1,0,301);desktop_input_turn();assert(records==1);
    pointer(0,0,0,0,302);pointer(0,0,1,0,303);pointer(0,0,0,0,304);desktop_input_turn();assert(records==2 && paint_commits==1);
    /* Begin at the conservative unknown-button recovery state, then use only
     * valid samples. A key-driven scene change must not clear this guard. */
    fresh();input_unknown_buttons=INPUT_LEFT|INPUT_RIGHT;
    key(0x21,1);pointer(0,0,1,0,2);desktop_input_turn();assert(records==1 && !mouse_left);
    pointer(0,0,0,0,3);pointer(0,0,1,0,4);pointer(0,0,0,0,5);desktop_input_turn();assert(records==2);
    puts("Desktop bounded loss: 64/65 batch boundary, overflow cancellation and release/repress passed.");

    fresh();mode=START_SAVER;pointer(0,0,1,0,1);pointer(0,0,0,0,2);desktop_input_turn();
    assert(saver_on && records==1);
    pointer(3,0,0,0,3);key(0x1e,4);desktop_input_turn();assert(!saver_on && records==1 && mouse_x==403);
    key(0x30,5);desktop_input_turn();assert(records==2 && record[1].value=='b');
    fresh();key(0x1f,1);key(0x1e,2);desktop_input_turn();assert(!saver_on && records==1 && record[0].value=='s');
    fresh();saver_start();desktop_input_remember_scene();
    for(unsigned i=0;i<100;++i)key(0x1e,i+1);
    assert(desktop_input_turn()==64 && !saver_on && !records);assert(desktop_input_turn()==36 && !records);
    key(0x30,200);desktop_input_turn();assert(records==1 && record[0].value=='b');
    puts("Desktop saver: release-only, same-batch activation/wake and multi-batch wake backlog passed.");

    fresh();key(0x1e,1);pointer(0,0,1,0,2);key(0x30,3);desktop_program_input(1);
    int before_releases=writer_releases;
    assert(desktop_program_key()=='b' && !records && !mouse_left && writer_releases==before_releases);
    key(0x2e,4);pointer(3,0,1,0,5);desktop_program_input(0);
    assert(input_pending(&device_input) && desktop_input_turn()==2 && records==1 && record[0].value=='c' && mouse_x==403);
    pointer(0,0,1,0,6);pointer(0,0,0,0,7);desktop_input_turn();assert(records==1);
    key(0x20,8);desktop_input_turn();assert(records==2 && record[1].value=='d');
    fresh();for(unsigned i=0;i<100;++i)key(i==99?0x30:0x1e,i);
    assert(desktop_program_key()=='b' && !input_pending(&device_input));
    fresh();acquire_tail=1100;desktop_program_input(0);assert(input_tail_pending && acquire_tail==76);
    key(0x1e,1);assert(desktop_input_turn()==INPUT_BATCH && input_tail_pending && !records);
    assert(desktop_input_turn()==INPUT_BATCH && input_tail_pending);
    assert(desktop_input_turn()==INPUT_BATCH && !input_tail_pending && !acquire_tail && !records);
    key(0x30,2);desktop_input_turn();assert(records==1 && record[0].value=='b');
    puts("Desktop synchronous owner: INKEY last-character snapshot, keyboard typeahead and pointer fencing passed.");
    puts("All production desktop input checks passed.");
    return 0;
}
