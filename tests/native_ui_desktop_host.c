/* The production WM hook + ordered adapter + endpoint core, with ordinary
 * window/Terminal fixtures instead of device hardware or guest execution. */
#define NATIVE_UI_DESKTOP_REAL
#include "native_ui.h"
static int fb_h=600;
#define TERM_PAD 12
#define EDIT_LINE_H 10
#define TASKBAR_Y (fb_h-44)
static uint32_t timer_ticks(void);
static void win_focus(int slot);
static int term_binding_matches(const ProcessBinding *binding);
static int term_canvas_size(int slot,int *width,int *height);
static int hit(int x,int y,int ax,int ay,int w,int h);
static int desktop_input_blocked(void);
#define main legacy_adapter_fixture_main
#include "desktop_input_host.c"
#undef main
static ProcessBinding native_bindings[8];
static int native_live[8],published_w[8],published_h[8];
static uint32_t timer_ticks(void){return frame_count;}
static void win_focus(int slot){wins[slot].z=wins[win_front()].z+1;context_slot=slot;}
static int hit(int x,int y,int ax,int ay,int w,int h){return x>=ax&&y>=ay&&x-ax<w&&y-ay<h;}
int process_binding_live(const ProcessBinding *binding){
    return binding&&binding->slot<8&&native_live[binding->slot]&&
        !memcmp(binding,native_bindings+binding->slot,sizeof *binding);
}
static int term_binding_matches(const ProcessBinding *binding){return process_binding_live(binding);}
static int term_canvas_size(int slot,int *width,int *height){
    *width=published_w[slot];*height=published_h[slot];return *width&&*height;
}
static BosHandle ui_open(int slot){
    BosUiTargetInfoV1 info;assert(native_ui_open(native_bindings+slot,7,&info)==BOS_OK);
    return info.target;
}
static BosUiEventV1 event(int slot,BosHandle target,unsigned type){
    BosUiEventV1 e;assert(native_ui_read(native_bindings+slot,target,&e)==BOS_OK);
    if(e.type!=type)fprintf(stderr,"expected %u got %u seq %u\n",type,e.type,e.sequence_lo);
    assert(e.type==type);return e;
}
static void no_event(int slot,BosHandle target){BosUiEventV1 e;assert(native_ui_read(native_bindings+slot,target,&e)==BOS_PENDING);}
static void fixture(void){
    fresh();memset(native_live,0,sizeof native_live);memset(published_w,0,sizeof published_w);memset(published_h,0,sizeof published_h);
    for(int i=0;i<2;i++){
        wins[i]=(Win){.open=1,.seq=i+1,.kind=WK_TERM,.x=50+i*360,.y=80,.w=320,.h=300,.z=2-i};
        native_bindings[i]=(ProcessBinding){BOS_HANDLE_TYPE_PROCESS|(unsigned)(i+1),(unsigned)i,1};
        native_live[i]=1;published_w[i]=160;published_h[i]=100;
    }
    mouse_x=80;mouse_y=140;input_init(&device_input,800,600,mouse_x,mouse_y);input_mouse_type(&device_input,3);
    const NativeUiHooks hooks={native_host_snapshot,native_host_acquired,native_host_focus};native_ui_init(&hooks);
    desktop_input_remember_scene();
}
static void move(int x,int y,unsigned buttons,int wheel,unsigned ticks){
    int dx=x-device_input.x,dy=device_input.y-y;
    /* Ordinary valid PS/2 packets, bounded signed device deltas. */
    while(dx>100||dx<-100||dy>100||dy<-100){
        int sx=dx>100?100:dx<-100?-100:dx,sy=dy>100?100:dy<-100?-100:dy;
        pointer(sx,sy,device_input.buttons,0,ticks);desktop_input_turn();
        dx=x-device_input.x;dy=device_input.y-y;
    }
    pointer(dx,dy,buttons,wheel,ticks);desktop_input_turn();
}
int main(void){
    fixture();BosHandle a=ui_open(0),b=ui_open(1);event(0,a,BOS_UI_STATE_RESET);event(1,b,BOS_UI_STATE_RESET);
    /* Real front-to-back canvas hit, then outside capture over the peer. */
    move(90,150,1,0,1);event(0,a,BOS_UI_POINTER_MOVE);event(0,a,BOS_UI_POINTER_BUTTON);
    assert(!records&&!mouse_left&&!mouse_right);
    move(440,150,1,1,2);BosUiEventV1 e=event(0,a,BOS_UI_POINTER_MOVE);
    if(e.flags&BOS_UI_EVENT_INSIDE)e=event(0,a,BOS_UI_POINTER_MOVE);
    assert(e.x>160&&!(e.flags&BOS_UI_EVENT_INSIDE));
    event(0,a,BOS_UI_POINTER_WHEEL);no_event(1,b);
    move(440,150,0,0,3);e=event(0,a,BOS_UI_POINTER_BUTTON);assert(!e.buttons&&!records);
    move(440,150,1,0,4);event(0,a,BOS_UI_FOCUS);event(1,b,BOS_UI_FOCUS);
    event(1,b,BOS_UI_POINTER_MOVE);event(1,b,BOS_UI_POINTER_BUTTON);assert(win_front()==1&&!records);
    move(440,150,0,0,5);event(1,b,BOS_UI_POINTER_BUTTON);
    /* An overlay loses focus, cancels before the snapshot and suppresses held
     * input without replaying it into the old or underlying application. */
    move(440,150,1,0,6);event(1,b,BOS_UI_POINTER_BUTTON);
    open_menu=0;desktop_input_turn();event(1,b,BOS_UI_CANCEL);event(1,b,BOS_UI_FOCUS);event(1,b,BOS_UI_AVAILABILITY);
    move(450,150,0,0,7);no_event(1,b);open_menu=-1;desktop_input_turn();
    event(0,a,BOS_UI_AVAILABILITY); /* blocked then unblocked */
    event(0,a,BOS_UI_AVAILABILITY);
    event(1,b,BOS_UI_FOCUS);event(1,b,BOS_UI_AVAILABILITY);
    /* Published dimension change retains the old cancellation transform. */
    move(450,150,1,0,8);event(1,b,BOS_UI_POINTER_MOVE);event(1,b,BOS_UI_POINTER_BUTTON);
    published_w[1]=320;published_h[1]=200;native_ui_refresh(9,1);
    e=event(1,b,BOS_UI_CANCEL);assert(e.logical_w==160);
    e=event(1,b,BOS_UI_GEOMETRY);assert(e.logical_w==320);
    move(450,150,0,0,10);no_event(1,b);
    /* A higher ordinary window blocks the underlying endpoint at a point. */
    wins[0]=(Win){.open=1,.seq=5,.kind=WK_PAINT,.x=420,.y=120,.w=200,.h=200,.z=20};
    native_live[0]=0;desktop_input_turn();event(1,b,BOS_UI_FOCUS);
    records=0;move(450,150,1,0,11);assert(records==1&&record[0].kind=='D');no_event(1,b);
    move(450,150,0,0,12);no_event(1,b);
    /* Lost decoded input emits a mandatory native reset through the adapter. */
    input_device_loss(&device_input,13);desktop_input_turn();
    e=event(1,b,BOS_UI_STATE_RESET);assert(e.reason==BOS_UI_REASON_INPUT_LOSS&&!e.buttons);
    /* An unarmed endpoint owns the remainder of a rejected gesture too. It
     * must never become a synthetic shell click when the pointer leaves. */
    fixture();a=ui_open(0);move(90,150,1,0,20);assert(!records);
    event(0,a,BOS_UI_STATE_RESET);
    move(95,155,1,0,21);move(760,540,1,0,22);assert(!records);
    move(760,540,0,0,23);assert(!records);
    move(760,540,1,0,24);assert(records==1);move(760,540,0,0,25);
    /* Acquired DOWN before subscription is fenced, including outside motion
     * after the initial reset has been read. */
    fixture();pointer(10,-10,1,0,30);a=ui_open(0);event(0,a,BOS_UI_STATE_RESET);
    desktop_input_turn();assert(!records);move(760,540,1,0,31);assert(!records);
    move(760,540,0,0,32);assert(!records);
    /* A focus notification filling the queue rejects DOWN before capture;
     * its held tail must still be globally consumed. */
    fixture();b=ui_open(1);event(1,b,BOS_UI_STATE_RESET);
    for(unsigned i=0;i<64;i++){wins[1].x+=(i&1)?-1:1;native_ui_refresh(40+i,0);}
    move(440,150,1,0,110);assert(!records);
    e=event(1,b,BOS_UI_STATE_RESET);assert(e.reason==BOS_UI_REASON_QUEUE_LOSS&&e.dropped==65);
    move(760,540,1,0,111);assert(!records);move(760,540,0,0,112);assert(!records);
    /* Acquisition can be ahead of routing. Release/Stop/publication must
     * consume already-buffered held MOVE through UP, even if latest is up. */
    for(unsigned cause=0;cause<3;cause++){
        fixture();a=ui_open(0);event(0,a,BOS_UI_STATE_RESET);
        move(90,150,1,0,120);event(0,a,BOS_UI_POINTER_MOVE);event(0,a,BOS_UI_POINTER_BUTTON);
        pointer(5,0,1,0,121);pointer(0,0,0,0,122);assert(!device_input.buttons&&device_input.count==2);
        if(cause==0)assert(native_ui_release(native_bindings,a)==BOS_OK);
        else if(cause==1){native_live[0]=0;native_ui_revoke_owner(native_bindings[0].process);}
        else {published_w[0]=320;published_h[0]=200;native_ui_refresh(123,device_input.buttons);}
        desktop_input_turn();assert(!records&&!mouse_left);
        if(cause==2){event(0,a,BOS_UI_CANCEL);event(0,a,BOS_UI_GEOMETRY);no_event(0,a);}
    }
    /* Opening between ordinary PS/2 packet bytes suppresses the already-held
     * DOWN even when its completed sample has a newer serial than OPEN. */
    fixture();input_mouse_byte(&device_input,9,130);assert(device_input.packet_n==1);
    a=ui_open(0);event(0,a,BOS_UI_STATE_RESET);
    input_mouse_byte(&device_input,0,130);input_mouse_byte(&device_input,0,130);input_mouse_byte(&device_input,0,130);
    desktop_input_turn();assert(!records);move(760,540,1,0,131);assert(!records);
    move(760,540,0,0,132);assert(!records);
    puts("Native UI desktop adapter: real point ownership, focus/capture, overlays, publication geometry, legacy exclusion and input-loss recovery passed.");
    return 0;
}
