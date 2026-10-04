/* Production example and its normal SDK results, without guest/debug hooks. */
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#define BASEOS_SDK_H
#include "../sdk/baseos_abi.h"
static int bos_ui_query(BosUiInfoV1 *,unsigned);
#ifdef BOS_APP_NATIVE_WINDOW_V1
static int bos_abi_query(BosAbiInfo *,unsigned);
static int bos_ui_window_adopt(unsigned,BosUiTargetInfoV1 *);
#else
static int bos_ui_host_open(unsigned,BosUiTargetInfoV1 *);
#endif
static int bos_ui_read(BosHandle,BosUiEventV1 *);
static int bos_ui_wait(BosHandle,unsigned,unsigned);
static int bos_ui_release(BosHandle);
static int bos_canvas_size(unsigned,unsigned);
static int bos_print(const char *);
static int bos_key(void);
static void bos_plot(int,int,unsigned);
static void bos_rect(int,int,unsigned,unsigned,unsigned);
static void bos_present(void);
#define main pointer_main
#include "../examples/c/pointer.c"
#undef main

static BosUiEventV1 queue[8];
static unsigned queue_count,queue_at,opens,releases,presents,waits,stage,key_at;
static unsigned working_w,working_h,published_w,published_h,geometry;
static unsigned live,query_mode,wait_error,open_error;
#ifdef BOS_APP_NATIVE_WINDOW_V1
static unsigned abi_mode,target_mode,reopen_error,abi_queries;
#endif
static char output[8192];
static const char *keys[]={"p","rp","p","o","c","p","q"};
static const unsigned ready=READY;
static BosUiEventV1 event(unsigned type,int x,int y,unsigned buttons,unsigned changed) {
    unsigned state=ready;
    if(type==BOS_UI_STATE_RESET)state=BOS_UI_STATE_FOCUSED|BOS_UI_STATE_AVAILABLE;
    if(type==BOS_UI_CANCEL)state&=~BOS_UI_STATE_CAPTURED;
    return (BosUiEventV1){.size=sizeof(BosUiEventV1),.major=BOS_UI_MAJOR,
        .target=target,.type=type,.sequence_lo=model.sequence_lo+1,
        .geometry_epoch=model.geometry,.stream_epoch=model.stream,
        .logical_w=model.logical_w,.logical_h=model.logical_h,
        .x=x,.y=y,.buttons=buttons,.changed_buttons=changed,
        .state=state,.flags=(state&BOS_UI_STATE_POSITION_VALID)?BOS_UI_EVENT_INSIDE:0};
}
static void feed(unsigned type,int x,int y,unsigned buttons,unsigned changed) {
    BosUiEventV1 e=event(type,x,y,buttons,changed);accept_event(&model,&e);
}
static unsigned bits(unsigned char pixels[2][INK_BYTES]) {
    unsigned n=0;
    for(unsigned c=0;c<2;c++)for(unsigned i=0;i<INK_BYTES;i++)for(unsigned j=0;j<8;j++)n+=(pixels[c][i]>>j)&1;
    return n;
}
static void initial(void) {
    model=(PointerModel){.width=160,.height=100,.logical_w=160,.logical_h=100,.geometry=1,.stream=1};
    target=1;clear_ink(&model);
}
static void model_checks(void) {
    initial();feed(BOS_UI_POINTER_BUTTON,8,55,1,1);
    assert(!model.drag&&!bits(preview)); /* OPEN reset has not been read. */
    feed(BOS_UI_STATE_RESET,0,0,0,0);assert(model.armed&&model.resets==1);
    feed(BOS_UI_POINTER_BUTTON,8,55,1,1);assert(model.drag&&bits(preview)==1&&!bits(ink));
    feed(BOS_UI_POINTER_MOVE,12,55,1,0);assert(bits(preview)==5);
    feed(BOS_UI_POINTER_BUTTON,12,55,3,2);assert(model.drag&&!model.strokes);
    feed(BOS_UI_POINTER_MOVE,12,59,3,0);
    feed(BOS_UI_POINTER_BUTTON,12,59,2,1);assert(model.drag&&!model.strokes);
    feed(BOS_UI_POINTER_BUTTON,12,59,0,2);
    assert(!model.drag&&model.strokes==1&&bits(ink)>5&&!bits(preview));
    unsigned saved=bits(ink);
    feed(BOS_UI_POINTER_BUTTON,30,55,2,2);feed(BOS_UI_POINTER_MOVE,40,60,2,0);
    assert(bits(preview)>0);feed(BOS_UI_CANCEL,40,60,0,0);
    assert(model.cancels==1&&!model.drag&&!bits(preview)&&bits(ink)==saved&&model.strokes==1);
    feed(BOS_UI_POINTER_BUTTON,40,60,0,2);assert(model.strokes==1);
    feed(BOS_UI_POINTER_BUTTON,30,55,1,1);feed(BOS_UI_STATE_RESET,0,0,0,0);
    assert(model.resets==2&&!model.drag&&!bits(preview)&&bits(ink)==saved);
    feed(BOS_UI_POINTER_BUTTON,30,55,0,1);assert(model.strokes==1);
    feed(BOS_UI_POINTER_BUTTON,30,55,1,1);
    BosUiEventV1 e=event(BOS_UI_GEOMETRY,30,55,0,0);e.geometry_epoch++;
    accept_event(&model,&e);assert(!model.drag&&!bits(preview)&&bits(ink)==saved);
    feed(BOS_UI_POINTER_BUTTON,30,55,1,1);
    e=event(BOS_UI_FOCUS,30,55,0,0);e.state&=~BOS_UI_STATE_FOCUSED;
    accept_event(&model,&e);assert(!model.drag&&!bits(preview));
    feed(BOS_UI_POINTER_BUTTON,30,55,1,1);
    feed(BOS_UI_POINTER_MOVE,INT_MIN,INT_MAX,1,0);
    assert(model.drag&&model.x==INT_MIN&&model.y==INT_MAX&&!model.pen);
    unsigned preview_before=bits(preview);feed(BOS_UI_POINTER_MOVE,80,70,1,0);
    assert(bits(preview)==preview_before+1); /* Outside movement cannot draw a bridge. */
    e=event(500,99,99,0,0);accept_event(&model,&e);
    assert(model.unknown==1&&model.drag&&model.x==80);
    e=event(BOS_UI_POINTER_MOVE,81,70,1u|0x80000000u,0);e.state|=0x80000000u;e.flags|=0x80000000u;
    accept_event(&model,&e);assert(model.buttons==1&&model.drag);
    e.target++;e.x=90;accept_event(&model,&e);assert(model.x==81);
    clear_ink(&model);assert(!bits(ink)&&!bits(preview)&&!model.drag);
    feed(BOS_UI_POINTER_BUTTON,81,70,3,2);assert(!model.drag); /* Clear while held doesn't restart. */
    feed(BOS_UI_POINTER_BUTTON,81,70,0,3);assert(model.strokes==1);
    model.x=INT_MIN;model.y=INT_MAX;status("STATUS");
    assert(strstr(output,"x=-2147483648 y=2147483647"));
}
static int bos_print(const char *s) {
    assert(strlen(output)+strlen(s)<sizeof output);strcat(output,s);return (int)strlen(s);
}
static int bos_ui_query(BosUiInfoV1 *out,unsigned capacity) {
    assert(capacity==sizeof *out);
    if(query_mode==1)return -1;
    *out=(BosUiInfoV1){.size=sizeof *out,.major=BOS_UI_MAJOR,.minor=BOS_UI_MINOR,
        .capabilities=POINTER_BACKEND_CAPABILITIES|BOS_UI_CAP_POINTER|BOS_UI_CAP_IMPLICIT_CAPTURE|
            BOS_UI_CAP_BOUNDED_WAIT|BOS_UI_CAP_LEGACY_KEY_READINESS|0x80000000u,
        .subscriptions_supported=BOS_UI_SUB_POINTER|BOS_UI_SUB_HOVER|BOS_UI_SUB_WHEEL|0x80000000u,
        .event_bytes=sizeof(BosUiEventV1),.wait_max_ms=60000};
    if(query_mode==2)out->capabilities&=~BOS_UI_CAP_POINTER;
#ifdef BOS_APP_NATIVE_WINDOW_V1
    if(query_mode==3)out->capabilities&=~BOS_UI_CAP_OWNED_WINDOW;
    if(query_mode==4)out->capabilities&=~BOS_UI_CAP_FORCED_CLOSE;
    if(query_mode==5)out->minor=0;
#endif
    return BOS_OK;
}
#ifdef BOS_APP_NATIVE_WINDOW_V1
static int bos_abi_query(BosAbiInfo *out,unsigned capacity) {
    assert(capacity==sizeof *out);++abi_queries;
    if(abi_mode==1)return BOS_E_UNSUPPORTED;
    *out=(BosAbiInfo){.struct_size=sizeof *out,.abi_major=BOS_ABI_MAJOR,.abi_minor=2,
        .features=BOS_FEATURE_BEX2|BOS_FEATURE_OWNED_NATIVE_WINDOW|0x80000000u,
        .context=BOS_CONTEXT_DESKTOP_TASK};
    if(abi_mode==2)out->features&=~BOS_FEATURE_OWNED_NATIVE_WINDOW;
    if(abi_mode==3)out->features&=~BOS_FEATURE_BEX2;
    if(abi_mode==4)out->abi_minor=1;
    if(abi_mode==5)out->context=BOS_CONTEXT_LEGACY_EXEC;
    if(abi_mode==6)out->struct_size=16;
    if(abi_mode==7)out->abi_major=0;
    return BOS_OK;
}
#endif
static void enqueue(unsigned type,unsigned reason) {
    assert(queue_count<8);
    unsigned index=queue_count++;
    queue[index]=(BosUiEventV1){.size=sizeof(BosUiEventV1),.major=BOS_UI_MAJOR,
        .target=live,.type=type,.sequence_lo=queue_count,
        .geometry_epoch=geometry,.stream_epoch=1,.logical_w=published_w,
        .logical_h=published_h,.state=BOS_UI_STATE_FOCUSED|BOS_UI_STATE_AVAILABLE,.reason=reason};
}
static int pointer_backend_open(unsigned sub,BosUiTargetInfoV1 *out) {
    assert(!live&&sub==(BOS_UI_SUB_POINTER|BOS_UI_SUB_HOVER|BOS_UI_SUB_WHEEL));
    if(open_error)return BOS_E_UNSUPPORTED;
#ifdef BOS_APP_NATIVE_WINDOW_V1
    if(reopen_error&&opens)return BOS_E_UNSUPPORTED;
#endif
    live=++opens;
    *out=(BosUiTargetInfoV1){.size=sizeof *out,.major=BOS_UI_MAJOR,.minor=BOS_UI_MINOR,.target=live,
        .kind=BOS_UI_KIND_HOSTED_CANVAS,.logical_w=published_w,.logical_h=published_h,
        .geometry_epoch=geometry,.stream_epoch=1};
#ifdef BOS_APP_NATIVE_WINDOW_V1
    out->kind=BOS_UI_KIND_OWNED_WINDOW;
    out->capabilities=POINTER_REQUIRED_CAPABILITIES|0x80000000u;
    unsigned mode=target_mode>=10?(opens>1?target_mode-10:0):target_mode;
    if(mode==1)out->kind=BOS_UI_KIND_HOSTED_CANVAS;
    if(mode==2)out->capabilities&=~BOS_UI_CAP_OWNED_WINDOW;
    if(mode==3)out->capabilities&=~BOS_UI_CAP_FORCED_CLOSE;
    if(mode==4)out->minor=0;
    if(mode==5)out->capabilities&=~BOS_UI_CAP_POINTER;
#endif
    queue_count=queue_at=0;enqueue(BOS_UI_STATE_RESET,BOS_UI_REASON_OPEN);return BOS_OK;
}
static int bos_ui_read(BosHandle handle,BosUiEventV1 *out) {
    assert(handle==live&&live);
    if(queue_at==queue_count)return BOS_PENDING;
    *out=queue[queue_at++];return BOS_OK;
}
static int bos_ui_wait(BosHandle handle,unsigned ready_bits,unsigned ms) {
    assert(handle==live&&ready_bits==(BOS_UI_WAIT_QUEUE|BOS_UI_WAIT_LEGACY_KEY)&&ms==1000);
    assert(published_w==working_w&&published_h==working_h&&!model.dirty);
    ++waits;++stage;key_at=0;
    if(wait_error)return BOS_E_STALE;
    return queue_at<queue_count?BOS_OK:BOS_E_TIMEOUT;
}
static int bos_ui_release(BosHandle handle) {
    assert(handle==live&&live);live=0;++releases;return BOS_OK;
}
static int bos_canvas_size(unsigned w,unsigned h) {
    assert((w==160&&h==100)||(w==320&&h==200));working_w=w;working_h=h;return 0;
}
static void bos_plot(int x,int y,unsigned color) {
    assert(x>=0&&y>=0&&x<(int)working_w&&y<(int)working_h&&color<16);
}
static void bos_rect(int x,int y,unsigned w,unsigned h,unsigned color) {
    assert(x>=0&&y>=0&&(unsigned)x+w<=working_w&&(unsigned)y+h<=working_h&&color<16);
}
static void bos_present(void) {
    ++presents;
    if(published_w!=working_w||published_h!=working_h){
        published_w=working_w;published_h=working_h;++geometry;
        if(live){queue_count=queue_at=0;enqueue(BOS_UI_GEOMETRY,BOS_UI_REASON_GEOMETRY);}
    }
}
static int bos_key(void) {
    assert(stage<sizeof keys/sizeof keys[0]);
    int key=keys[stage][key_at];if(key)++key_at;return key;
}
static void reset_host(void) {
    opens=releases=presents=waits=stage=key_at=live=query_mode=wait_error=open_error=0;
    queue_count=queue_at=working_w=working_h=published_w=published_h=geometry=0;
    output[0]=0;
#ifdef BOS_APP_NATIVE_WINDOW_V1
    abi_mode=target_mode=reopen_error=abi_queries=0;
#endif
}
int main(void) {
    model_checks();reset_host();assert(!pointer_main());
    assert(opens==2&&releases==2&&!live&&waits==6&&model.width==320&&model.height==200);
    assert(strstr(output,"POINTER STATUS")&&strstr(output,"POINTER RESIZE_WORKING"));
    assert(strstr(output,"logical=160x100 working=320x200"));
    assert(strstr(output,"logical=320x200 working=320x200"));
    assert(strstr(output,"POINTER REOPEN")&&strstr(output,"POINTER CLEAR")&&strstr(output,"POINTER EXIT"));
    assert(model.resets==2&&model.geometry==2);
    reset_host();query_mode=1;assert(!pointer_main()&&!opens&&!presents&&strstr(output,"POINTER UNSUPPORTED"));
    reset_host();query_mode=2;assert(!pointer_main()&&!opens&&!presents&&strstr(output,"POINTER UNSUPPORTED"));
    reset_host();wait_error=1;assert(pointer_main()==1&&releases==1&&!live&&strstr(output,"POINTER ERROR wait"));
    reset_host();open_error=1;assert(pointer_main()==1&&!live&&strstr(output,"POINTER ERROR open"));
#ifdef BOS_APP_NATIVE_WINDOW_V1
    for(unsigned mode=1;mode<=7;mode++){
        reset_host();abi_mode=mode;
        assert(!pointer_main()&&abi_queries==1&&!opens&&!presents&&strstr(output,"POINTER UNSUPPORTED"));
    }
    for(unsigned mode=3;mode<=5;mode++){
        reset_host();query_mode=mode;
        assert(!pointer_main()&&!opens&&!presents&&strstr(output,"POINTER UNSUPPORTED"));
    }
    for(unsigned mode=1;mode<=5;mode++)for(unsigned reopening=0;reopening<2;reopening++){
        reset_host();target_mode=mode+10*reopening;
        assert(pointer_main()==1&&!live&&opens==1+reopening&&releases==opens);
        assert(strstr(output,reopening?"POINTER ERROR reopen":"POINTER ERROR open"));
    }
    reset_host();reopen_error=1;
    assert(pointer_main()==1&&opens==1&&releases==1&&!live&&strstr(output,"POINTER ERROR reopen"));
#endif
    puts("Pointer example: transactional chords, reset/cancel, signed coordinates, unknown events, explicit publication, reopen, waits and compatibility passed");
    return 0;
}
