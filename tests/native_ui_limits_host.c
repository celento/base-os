/* Direct finite-state boundary checks. No guest code, machine fault, random
 * input, private memory probing or alternate dispatcher is executed. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/native_ui.c"
static ProcessBinding owner={BOS_HANDLE_TYPE_PROCESS|1,0,1};
static NativeUiHost host={{100,100,160,100,160,100},BOS_UI_STATE_AVAILABLE|BOS_UI_STATE_FOCUSED};
static NativeUiAcquired acquired;
static int snap(const ProcessBinding *b,NativeUiHost *out){if(!binding_equal(b,&owner))return 0;*out=host;return 1;}
static void acquire(NativeUiAcquired *out){*out=acquired;}
static void focus_owner(const ProcessBinding *b){assert(binding_equal(b,&owner));}
static const NativeUiHooks config={snap,acquire,focus_owner};
static BosHandle opened(void){
    BosUiTargetInfoV1 out;assert(native_ui_open(&owner,7,&out)==BOS_OK);
    BosUiEventV1 event;assert(native_ui_read(&owner,out.target,&event)==BOS_OK);
    return out.target;
}
static void unchanged(BosHandle handle){
    BosUiTargetInfoV1 info,old;memset(&info,0xa5,sizeof info);old=info;
    assert(native_ui_info(&owner,handle,&info)==BOS_E_STALE&&!memcmp(&info,&old,sizeof info));
    assert(native_ui_ready(&owner,handle)==BOS_E_STALE);
}
int main(void){
    native_ui_init(&config);BosHandle handle=opened();Target *t=by_handle(handle);
    /* The last sequence is delivered once. The next normal event revokes. */
    t->sequence=~(uint64_t)0-1;
    InputSample sample={.kind=INPUT_POINTER,.serial=1,.x=110,.y=110};acquired.serial=1;
    native_ui_route(&sample,handle);BosUiEventV1 event;
    assert(native_ui_read(&owner,handle,&event)==BOS_OK&&event.sequence_lo==~0u&&event.sequence_hi==~0u);
    sample.serial=++acquired.serial;sample.x++;native_ui_route(&sample,handle);unchanged(handle);
    BosHandle next=opened();assert(next!=handle);t=by_handle(next);
    t->geometry_epoch=~0u;host.view.x++;native_ui_refresh(3,0);unchanged(next);
    handle=opened();assert(handle!=next);t=by_handle(handle);
    t->stream_epoch=~0u;native_ui_input_loss(4,0,1);unchanged(handle);
    next=opened();assert(next!=handle);
    t=by_handle(next);sample.serial=++acquired.serial;sample.buttons=acquired.buttons=1;
    native_ui_route(&sample,next);assert(native_ui_read(&owner,next,&event)==BOS_OK);
    assert(native_ui_read(&owner,next,&event)==BOS_OK);
    t->sequence=~(uint64_t)0;sample.serial=++acquired.serial;sample.x++;sample.buttons=acquired.buttons=0;
    native_ui_route(&sample,next);unchanged(next);
    handle=opened();sample.serial=++acquired.serial;sample.buttons=acquired.buttons=1;
    native_ui_route(&sample,handle);assert(native_ui_read(&owner,handle,&event)==BOS_OK&&event.type==BOS_UI_POINTER_MOVE);
    assert(native_ui_read(&owner,handle,&event)==BOS_OK&&event.type==BOS_UI_POINTER_BUTTON&&event.buttons==1);
    native_ui_release_owner(owner.process);acquired.buttons=0;
    /* A handle's final serial is legal and must never reappear after init. */
    next_serial=BOS_HANDLE_SERIAL_MAX-1;handle=opened();assert(handle==(BOS_HANDLE_TYPE_UI_TARGET|BOS_HANDLE_SERIAL_MAX));
    assert(native_ui_release(&owner,handle)==BOS_OK);native_ui_init(&config);
    BosUiTargetInfoV1 out,old;memset(&out,0xa5,sizeof out);old=out;
    assert(native_ui_open(&owner,1,&out)==BOS_E_CAPACITY&&!memcmp(&out,&old,sizeof out));
    assert(!native_ui_target_at(0));
    puts("Native UI finite boundaries: final/nonreused target serial, 64-bit sequence, geometry and stream exhaustion revoke and permit fresh identities.");
    return 0;
}
