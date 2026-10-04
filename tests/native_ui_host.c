/* Deterministic normal device gestures through the production endpoint core. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "native_ui.h"
#ifdef UI_TEST_OWNED
#define native_ui_open native_ui_adopt
#endif
static ProcessBinding bindings[8];
static NativeUiHost hosts[8];
static int live[8],focused;
static NativeUiAcquired acquired;
static int host_snapshot(const ProcessBinding *b,NativeUiHost *out){
    if(!b||b->slot>=8||!live[b->slot]||memcmp(b,bindings+b->slot,sizeof *b))return 0;
    *out=hosts[b->slot];
    if((int)b->slot==focused)out->state|=BOS_UI_STATE_FOCUSED;
    return 1;
}
static void acquire(NativeUiAcquired *out){*out=acquired;}
static void focus(const ProcessBinding *b){focused=(int)b->slot;}
static NativeUiHooks test_hooks={host_snapshot,acquire,focus};
static void reset(void){
    memset(hosts,0,sizeof hosts);memset(live,0,sizeof live);acquired=(NativeUiAcquired){0};focused=0;
    for(unsigned i=0;i<8;i++){
        bindings[i]=(ProcessBinding){BOS_HANDLE_TYPE_PROCESS|(i+1),i,i+1};live[i]=1;
        hosts[i].view=(CanvasView){100+(int)i*400,100,160,100,213,133};
        hosts[i].state=BOS_UI_STATE_AVAILABLE;
#ifdef UI_TEST_OWNED
        hosts[i].kind=BOS_UI_KIND_OWNED_WINDOW;hosts[i].capabilities=NATIVE_UI_CAPABILITIES_OWNED;
#else
        hosts[i].kind=BOS_UI_KIND_HOSTED_CANVAS;hosts[i].capabilities=NATIVE_UI_CAPABILITIES_HOSTED;
#endif
    }
    native_ui_init(&test_hooks);
}
static BosHandle open_target(unsigned slot,unsigned subs){
    BosUiTargetInfoV1 out;
    assert(native_ui_open(bindings+slot,subs,&out)==BOS_OK);
    assert(out.size==96&&out.target&&(out.target&0xf0000000u)==BOS_HANDLE_TYPE_UI_TARGET);
    assert(out.logical_w==160&&out.viewport_w==213&&out.geometry_epoch&&out.stream_epoch);
    return out.target;
}
static BosUiEventV1 read_event(unsigned slot,BosHandle target,unsigned type){
    BosUiEventV1 e;
    assert(native_ui_read(bindings+slot,target,&e)==BOS_OK);
    if(e.type!=type)fprintf(stderr,"slot %u: expected %u got %u (seq %u)\n",slot,type,e.type,e.sequence_lo);
    assert(e.type==type&&e.size==96&&e.major==1&&e.target==target&&!e.reserved);
    return e;
}
static void empty(unsigned slot,BosHandle target){
    BosUiEventV1 e,expected;memset(&e,0xa5,sizeof e);expected=e;
    assert(native_ui_read(bindings+slot,target,&e)==BOS_PENDING);
    assert(!memcmp(&e,&expected,sizeof e));
}
static unsigned sample(BosHandle hit,int x,int y,unsigned buttons,int wheel,unsigned mods){
    acquired.serial++;acquired.ticks++;acquired.buttons=buttons;
    InputSample s={.serial=acquired.serial,.ticks=acquired.ticks,.x=x,.y=y,
        .buttons=buttons,.wheel=wheel,.modifiers=mods,.kind=INPUT_POINTER};
    return native_ui_route(&s,hit);
}
static void initial(unsigned slot,BosHandle target){
    BosUiEventV1 e=read_event(slot,target,BOS_UI_STATE_RESET);
    assert(e.reason==BOS_UI_REASON_OPEN&&!e.buttons&&!e.dropped&&!e.x&&!e.y&&!e.modifiers);
    assert(!(e.state&BOS_UI_STATE_POSITION_VALID));empty(slot,target);
}
static void normal_capture(void){
    reset();BosHandle a=open_target(0,7),b=open_target(1,7);initial(0,a);initial(1,b);
    assert(sample(a,110,111,1,0,BOS_UI_MOD_LSHIFT)==3);
    BosUiEventV1 e=read_event(0,a,BOS_UI_POINTER_MOVE);assert(e.x==7&&e.y==8&&!e.buttons);
    unsigned seq=e.sequence_lo;
    e=read_event(0,a,BOS_UI_POINTER_BUTTON);assert(e.buttons==1&&e.changed_buttons==1&&e.sequence_lo>seq);
    assert(e.modifiers==BOS_UI_MOD_LSHIFT&&(e.state&BOS_UI_STATE_CAPTURED));
    assert(sample(b,550,80,3,1,BOS_UI_MOD_RCTRL)==3);
    e=read_event(0,a,BOS_UI_POINTER_MOVE);assert(e.buttons==1&&e.x==338&&e.y==-16&&!(e.flags&BOS_UI_EVENT_INSIDE));
    e=read_event(0,a,BOS_UI_POINTER_BUTTON);assert(e.buttons==3&&e.changed_buttons==2);
    e=read_event(0,a,BOS_UI_POINTER_WHEEL);assert(e.buttons==3&&e.wheel_y==1&&e.modifiers==BOS_UI_MOD_RCTRL);
    assert(sample(0,90,90,0,-1,0)==3);
    e=read_event(0,a,BOS_UI_POINTER_MOVE);assert(e.buttons==3&&e.x==-8&&e.y==-8);
    e=read_event(0,a,BOS_UI_POINTER_BUTTON);assert(!e.buttons&&e.changed_buttons==3);
    e=read_event(0,a,BOS_UI_POINTER_WHEEL);assert(!e.buttons&&e.wheel_y==-1);
    BosUiTargetInfoV1 info;assert(native_ui_info(bindings,a,&info)==BOS_OK);
    assert(!(info.state&BOS_UI_STATE_CAPTURED));empty(0,a);empty(1,b);
    /* A background down gets focus before move/button, with no broadcast. */
    assert(sample(b,510,110,1,0,0)==3);
    e=read_event(0,a,BOS_UI_FOCUS);assert(!(e.state&BOS_UI_STATE_FOCUSED));
    e=read_event(1,b,BOS_UI_FOCUS);assert(e.state&BOS_UI_STATE_FOCUSED);
    read_event(1,b,BOS_UI_POINTER_MOVE);read_event(1,b,BOS_UI_POINTER_BUTTON);
    empty(0,a);sample(b,510,110,0,0,0);read_event(1,b,BOS_UI_POINTER_BUTTON);
}
static void fences_ownership(void){
    reset();acquired.serial=10;acquired.buttons=1;
    BosHandle a=open_target(0,7);
    InputSample old={.serial=9,.ticks=3,.x=110,.y=110,.buttons=1,.kind=INPUT_POINTER};
    assert(native_ui_route(&old,a)==3);initial(0,a);
    assert(sample(a,111,111,1,0,0)==3);empty(0,a);
    sample(a,111,111,0,0,0);empty(0,a);
    sample(a,111,111,1,0,0);read_event(0,a,BOS_UI_POINTER_MOVE);read_event(0,a,BOS_UI_POINTER_BUTTON);
    BosUiEventV1 output,unchanged;memset(&output,0xcc,sizeof output);unchanged=output;
    assert(native_ui_read(bindings+1,a,&output)==BOS_E_STALE&&!memcmp(&output,&unchanged,sizeof output));
    assert(native_ui_release(bindings+1,a)==BOS_E_STALE);
    ProcessBinding stale=bindings[0];stale.generation++;
    assert(native_ui_read(&stale,a,&output)==BOS_E_STALE);
    assert(native_ui_release(bindings,a)==BOS_OK);
    assert(native_ui_read(bindings,a,&output)==BOS_E_STALE);
    BosHandle next=open_target(0,7);assert(next!=a);initial(0,next);
    assert(sample(next,120,120,1,0,0)==3);empty(0,next);
    assert(sample(next,120,120,0,0,0)==3);empty(0,next);
    assert(sample(next,120,120,1,0,0)==3);
    read_event(0,next,BOS_UI_POINTER_MOVE);read_event(0,next,BOS_UI_POINTER_BUTTON);
    native_ui_revoke_owner(bindings[0].process);assert(native_ui_ready(bindings,next)==BOS_E_STALE);
    assert(native_ui_target_at(0)==0);native_ui_release_owner(bindings[0].process);
    BosHandle third=open_target(0,1);assert(third!=next);initial(0,third);
}
static void geometry_cancel(void){
    reset();BosHandle a=open_target(0,7);initial(0,a);
    sample(a,110,110,1,0,0);read_event(0,a,BOS_UI_POINTER_MOVE);read_event(0,a,BOS_UI_POINTER_BUTTON);
    hosts[0].view.x=99;hosts[0].view.logical_w=320;
    native_ui_refresh(22,1);
    BosUiEventV1 e=read_event(0,a,BOS_UI_CANCEL);
    assert(e.reason==BOS_UI_REASON_GEOMETRY&&e.logical_w==160&&e.geometry_epoch==1&&!e.buttons);
    e=read_event(0,a,BOS_UI_GEOMETRY);assert(e.logical_w==320&&e.geometry_epoch==2&&!(e.state&BOS_UI_STATE_POSITION_VALID));
    assert(sample(a,120,120,1,0,0)==3);empty(0,a);
    assert(sample(a,120,120,0,0,0)==3);empty(0,a);
    sample(a,120,120,1,0,0);read_event(0,a,BOS_UI_POINTER_MOVE);read_event(0,a,BOS_UI_POINTER_BUTTON);
    hosts[0].state|=BOS_UI_STATE_BLOCKED;native_ui_refresh(23,1);
    e=read_event(0,a,BOS_UI_CANCEL);assert(e.reason==BOS_UI_REASON_BLOCKED);
    e=read_event(0,a,BOS_UI_FOCUS);assert(!(e.state&BOS_UI_STATE_FOCUSED));
    e=read_event(0,a,BOS_UI_AVAILABILITY);assert(e.state&BOS_UI_STATE_BLOCKED);
    hosts[0].state&=~BOS_UI_STATE_BLOCKED;native_ui_refresh(24,1);
    read_event(0,a,BOS_UI_FOCUS);read_event(0,a,BOS_UI_AVAILABILITY);
    sample(a,120,120,0,0,0);empty(0,a);
    hosts[0].state|=BOS_UI_STATE_MINIMIZED;native_ui_refresh(25,0);
    read_event(0,a,BOS_UI_FOCUS);e=read_event(0,a,BOS_UI_AVAILABILITY);
    assert((e.state&BOS_UI_STATE_MINIMIZED)&&!(e.state&BOS_UI_STATE_AVAILABLE));
    e=read_event(0,a,BOS_UI_GEOMETRY);assert(e.geometry_epoch==3&&e.logical_w==320);
    hosts[0].state&=~BOS_UI_STATE_MINIMIZED;native_ui_refresh(26,0);
    read_event(0,a,BOS_UI_FOCUS);read_event(0,a,BOS_UI_AVAILABILITY);read_event(0,a,BOS_UI_GEOMETRY);
    hosts[0].view=(CanvasView){0};hosts[0].state=0;native_ui_refresh(27,0);
    read_event(0,a,BOS_UI_FOCUS);read_event(0,a,BOS_UI_AVAILABILITY);
    e=read_event(0,a,BOS_UI_GEOMETRY);assert(!e.logical_w&&!e.viewport_w&&e.geometry_epoch==5);
}
static void coalescing_loss(void){
    reset();BosHandle a=open_target(0,7);initial(0,a);
    sample(a,110,110,0,0,0);sample(a,120,120,0,0,0);sample(a,130,130,0,0,0);
    BosUiEventV1 e=read_event(0,a,BOS_UI_POINTER_MOVE);assert(e.x==22&&e.sequence_lo==4);empty(0,a);
    sample(a,140,140,0,0,0);sample(a,150,150,0,0,BOS_UI_MOD_LSHIFT);
    read_event(0,a,BOS_UI_POINTER_MOVE);read_event(0,a,BOS_UI_POINTER_MOVE);empty(0,a);
    /* Exactly 64 wheel barriers fit, the next never silently drops a record. */
    for(unsigned i=0;i<64;i++)sample(a,150,150,0,1,BOS_UI_MOD_LSHIFT);
    assert(native_ui_ready(bindings,a)==BOS_OK);
    sample(a,150,150,0,1,BOS_UI_MOD_LSHIFT);
    e=read_event(0,a,BOS_UI_STATE_RESET);assert(e.reason==BOS_UI_REASON_QUEUE_LOSS&&e.dropped==65&&e.stream_epoch==2);
    empty(0,a);
    sample(a,150,150,0,0,0);read_event(0,a,BOS_UI_POINTER_MOVE);
    for(unsigned i=0;i<64;i++)sample(a,150,150,0,1,0);
    /* New hover motion plus wheel cannot rearm position after MOVE overflow. */
    sample(a,160,160,0,1,0);e=read_event(0,a,BOS_UI_STATE_RESET);
    assert(e.reason==BOS_UI_REASON_QUEUE_LOSS&&!(e.state&BOS_UI_STATE_POSITION_VALID));
    empty(0,a);
    /* Full queue on final UP must terminate the old gesture via RESET. */
    sample(a,150,150,1,0,0);read_event(0,a,BOS_UI_POINTER_MOVE);read_event(0,a,BOS_UI_POINTER_BUTTON);
    for(unsigned i=0;i<64;i++)sample(a,150,150,1,1,0);
    sample(a,150,150,0,0,0);
    e=read_event(0,a,BOS_UI_STATE_RESET);assert(e.reason==BOS_UI_REASON_QUEUE_LOSS&&!e.buttons&&e.dropped==65);
    assert(!(e.state&BOS_UI_STATE_CAPTURED));empty(0,a);
    sample(a,150,150,1,0,0);read_event(0,a,BOS_UI_POINTER_MOVE);read_event(0,a,BOS_UI_POINTER_BUTTON);
    for(unsigned i=0;i<64;i++)sample(a,150,150,1,1,0);
    sample(a,155,150,0,0,0);e=read_event(0,a,BOS_UI_STATE_RESET);
    assert(e.reason==BOS_UI_REASON_QUEUE_LOSS&&!e.buttons);
    sample(a,150,150,1,0,0);read_event(0,a,BOS_UI_POINTER_MOVE);read_event(0,a,BOS_UI_POINTER_BUTTON);
    native_ui_input_loss(100,1,7);
    /* State while reset is pending remains authoritative and cannot rearm. */
    hosts[0].view.viewport_w=200;native_ui_refresh(101,1);
    sample(a,160,160,1,0,0);
    e=read_event(0,a,BOS_UI_STATE_RESET);assert(e.reason==BOS_UI_REASON_INPUT_LOSS&&e.dropped==7&&!e.buttons&&e.viewport_w==200);
    sample(a,160,160,0,0,0);empty(0,a);
}
static void interrupted_peer(void){
    reset();BosHandle a=open_target(0,7),b=open_target(1,7);initial(0,a);
    sample(a,110,110,1,0,0);read_event(0,a,BOS_UI_POINTER_MOVE);read_event(0,a,BOS_UI_POINTER_BUTTON);
    /* An unrelated peer's unread initial reset cannot swallow captured UP. */
    sample(b,510,110,0,1,0);
    read_event(0,a,BOS_UI_POINTER_MOVE);
    BosUiEventV1 e=read_event(0,a,BOS_UI_POINTER_BUTTON);assert(!e.buttons&&e.changed_buttons==1);
    read_event(0,a,BOS_UI_POINTER_WHEEL);initial(1,b);empty(0,a);
    /* Chording after an outside press accepts only the newly pressed button. */
    sample(0,0,0,1,0,0);read_event(0,a,BOS_UI_POINTER_MOVE);
    sample(a,110,110,1,0,0);sample(a,110,110,3,0,0);
    read_event(0,a,BOS_UI_POINTER_MOVE);e=read_event(0,a,BOS_UI_POINTER_BUTTON);
    assert(e.buttons==2&&e.changed_buttons==2);
    sample(a,110,110,1,0,0);e=read_event(0,a,BOS_UI_POINTER_BUTTON);
    assert(!e.buttons&&e.changed_buttons==2);empty(0,a);
}
static void scene_baseline(void){
    reset();BosHandle a=open_target(0,7),b=open_target(1,7);initial(0,a);
    sample(a,110,110,0,0,0);read_event(0,a,BOS_UI_POINTER_MOVE);
    sample(b,510,110,0,0,0);BosUiEventV1 e=read_event(0,a,BOS_UI_POINTER_MOVE);
    assert(!(e.state&BOS_UI_STATE_POSITION_VALID));initial(1,b);empty(0,a);
    sample(a,110,110,1,0,0);read_event(0,a,BOS_UI_POINTER_MOVE);read_event(0,a,BOS_UI_POINTER_BUTTON);
    /* A queued physical release discarded by a scene fence still resets the
     * gesture baseline; the next real down must not disappear. */
    acquired.buttons=0;acquired.serial++;
    native_ui_cancel_all(BOS_UI_REASON_SCENE,20,0);read_event(0,a,BOS_UI_CANCEL);
    sample(a,110,110,1,0,0);read_event(0,a,BOS_UI_POINTER_MOVE);read_event(0,a,BOS_UI_POINTER_BUTTON);
    native_ui_cancel_all(BOS_UI_REASON_SCENE,21,1);read_event(0,a,BOS_UI_CANCEL);
    acquired.buttons=0;native_ui_cancel_all(BOS_UI_REASON_SCENE,22,0);
    sample(a,110,110,1,0,0);read_event(0,a,BOS_UI_POINTER_MOVE);read_event(0,a,BOS_UI_POINTER_BUTTON);
    native_ui_cancel_all(BOS_UI_REASON_SCENE,23,1);read_event(0,a,BOS_UI_CANCEL);
    acquired.buttons=0;native_ui_input_loss(24,0,1);read_event(0,a,BOS_UI_STATE_RESET);
    sample(a,110,110,1,0,0);read_event(0,a,BOS_UI_POINTER_MOVE);read_event(0,a,BOS_UI_POINTER_BUTTON);
}
static void hover_leave_limits(void){
    reset();BosHandle a=open_target(0,7),b=open_target(1,7);initial(0,a);initial(1,b);
    sample(a,110,110,0,0,0);read_event(0,a,BOS_UI_POINTER_MOVE);
    sample(b,510,110,0,0,0);
    BosUiEventV1 e=read_event(0,a,BOS_UI_POINTER_MOVE);
    assert(!(e.state&BOS_UI_STATE_POSITION_VALID)&&!e.x&&!e.y&&!e.modifiers);
    sample(b,520,120,0,0,0);empty(0,a);empty(1,b);
    /* A press that begins elsewhere never becomes a native drag. */
    assert(sample(0,0,0,1,0,0)==0);assert(sample(a,120,120,1,0,0)==0);
    assert(sample(a,120,120,0,0,0)==0);empty(0,a);
    BosUiTargetInfoV1 info;
    assert(native_ui_open(bindings,1,&info)==BOS_E_BUSY);
    for(unsigned i=2;i<8;i++)initial(i,open_target(i,1));
    assert(native_ui_open(bindings,0,&info)==BOS_E_INVALID);
    assert(native_ui_open(bindings,8,&info)==BOS_E_INVALID);
    live[0]=0;assert(native_ui_ready(bindings,a)==BOS_E_STALE);
    native_ui_release_owner(bindings[0].process);live[0]=1;bindings[0].generation++;
    assert(open_target(0,1)!=a);
}
int main(void){
    normal_capture();fences_ownership();geometry_cancel();coalescing_loss();
    interrupted_peer();scene_baseline();hover_leave_limits();
    puts("All native UI core checks passed.");return 0;
}
