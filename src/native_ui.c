#include "native_ui.h"

#define BUTTONS (BOS_UI_BUTTON_LEFT|BOS_UI_BUTTON_RIGHT)
#define SUBSCRIPTIONS (BOS_UI_SUB_POINTER|BOS_UI_SUB_HOVER|BOS_UI_SUB_WHEEL)
#define BASE_STATE (BOS_UI_STATE_FOCUSED|BOS_UI_STATE_AVAILABLE|BOS_UI_STATE_MINIMIZED|BOS_UI_STATE_BLOCKED)
#define CAPABILITIES (BOS_UI_CAP_HOSTED_CANVAS|BOS_UI_CAP_POINTER|BOS_UI_CAP_HOVER|BOS_UI_CAP_WHEEL|BOS_UI_CAP_IMPLICIT_CAPTURE|BOS_UI_CAP_BOUNDED_WAIT|BOS_UI_CAP_LEGACY_KEY_READINESS|BOS_UI_CAP_HOST_FORCED_CLOSE)
#define CONSUMED (NATIVE_UI_CONSUMED_POINTER|NATIVE_UI_CONSUMED_WHEEL)
typedef struct {
    ProcessBinding binding;
    BosHandle target;
    NativeUiHost host;
    BosUiEventV1 queue[BOS_UI_QUEUE_CAPACITY], reset;
    uint64_t fence, sequence;
    unsigned subscriptions,head,count,reset_pending,revoked;
    unsigned geometry_epoch,stream_epoch,accepted,suppressed;
    int x,y;
    unsigned position_valid,inside,modifiers;
} Target;
static Target targets[BOS_UI_TARGETS_TOTAL];
static NativeUiHooks hooks;
static unsigned next_serial;
static BosHandle capture,hover;
static unsigned swallowed,prior_buttons;
_Static_assert(sizeof targets<=54u*1024u,"native UI queues exceed bounded BSS budget");
_Static_assert(BOS_UI_BUTTON_LEFT==INPUT_LEFT && BOS_UI_BUTTON_RIGHT==INPUT_RIGHT,
               "pointer button adapter must be explicit if device bits change");
_Static_assert(BOS_UI_MOD_LSHIFT==INPUT_LSHIFT && BOS_UI_MOD_RALT==INPUT_RALT,
               "pointer modifier adapter must be explicit if device bits change");

static unsigned add_saturated(unsigned a,unsigned b){return b>~a?~0u:a+b;}
static int binding_equal(const ProcessBinding *a,const ProcessBinding *b){
    return a&&b&&a->process==b->process&&a->slot==b->slot&&a->generation==b->generation;
}
static Target *by_handle(BosHandle handle){
    if(!handle)return 0;
    for(unsigned i=0;i<BOS_UI_TARGETS_TOTAL;i++)if(targets[i].target==handle)return targets+i;
    return 0;
}
static void acquisition(NativeUiAcquired *out){
    *out=(NativeUiAcquired){0};if(hooks.acquired)hooks.acquired(out);
    out->buttons&=BUTTONS;
}
static unsigned state(const Target *t){
    return t->host.state|(capture==t->target?BOS_UI_STATE_CAPTURED:0)|
           (t->position_valid?BOS_UI_STATE_POSITION_VALID:0);
}
static void forget_capture(Target *t,unsigned physical){
    if(capture==t->target){
        /* Acquisition may already contain the final UP while older held MOVE
         * samples remain queued. Preserve the routed gesture tail as well. */
        swallowed|=(physical|t->accepted)&BUTTONS;capture=0;
    }
    t->accepted=0;t->suppressed|=physical&BUTTONS;
}
static void revoke(Target *t,unsigned physical){
    forget_capture(t,physical);
    if(hover==t->target)hover=0;
    t->revoked=1;t->head=t->count=t->reset_pending=0;
    t->position_valid=t->inside=t->modifiers=0;t->x=t->y=0;
}
static int next_sequence(Target *t){
    if(t->sequence==~(uint64_t)0){NativeUiAcquired a;acquisition(&a);revoke(t,a.buttons);return 0;}
    ++t->sequence;return 1;
}
static void snapshot_event(const Target *t,BosUiEventV1 *e,unsigned type,unsigned ticks){
    *e=(BosUiEventV1){0};
    e->size=sizeof *e;e->major=BOS_UI_MAJOR;e->type=type;
    e->flags=t->position_valid&&t->inside?BOS_UI_EVENT_INSIDE:0;
    e->target=t->target;e->sequence_lo=(unsigned)t->sequence;e->sequence_hi=(unsigned)(t->sequence>>32);
    e->ticks=ticks;e->geometry_epoch=t->geometry_epoch;e->stream_epoch=t->stream_epoch;
    e->logical_w=t->host.view.logical_w;e->logical_h=t->host.view.logical_h;
    e->viewport_w=t->host.view.viewport_w;e->viewport_h=t->host.view.viewport_h;
    e->x=t->position_valid?t->x:0;e->y=t->position_valid?t->y:0;
    e->buttons=t->accepted;e->modifiers=t->position_valid?t->modifiers:0;e->state=state(t);
}
static void refresh_reset(Target *t,unsigned ticks){
    unsigned reason=t->reset.reason,dropped=t->reset.dropped;
    snapshot_event(t,&t->reset,BOS_UI_STATE_RESET,ticks);
    t->reset.reason=reason;t->reset.dropped=dropped;
}
static void reset_latch(Target *t,unsigned reason,unsigned ticks,unsigned physical,unsigned dropped){
    if(t->revoked)return;
    if(t->reset_pending){
        t->reset.dropped=add_saturated(t->reset.dropped,add_saturated(t->count,dropped));
        /* A loss upgrades the initial OPEN snapshot rather than hiding it. */
        if(reason!=BOS_UI_REASON_OPEN)t->reset.reason=reason;
    }else{
        if(t->stream_epoch==~0u){revoke(t,physical);return;}
        if(reason!=BOS_UI_REASON_OPEN)++t->stream_epoch;
        if(!next_sequence(t))return;
        t->reset.reason=reason;t->reset.dropped=add_saturated(t->count,dropped);
        t->reset_pending=1;
    }
    forget_capture(t,physical);
    t->head=t->count=0;t->position_valid=t->inside=t->modifiers=0;t->x=t->y=0;
    if(hover==t->target)hover=0;
    refresh_reset(t,ticks);
}
static int same_move(const BosUiEventV1 *a,const BosUiEventV1 *b){
    return a->type==BOS_UI_POINTER_MOVE&&b->type==BOS_UI_POINTER_MOVE&&a->target==b->target&&
        a->geometry_epoch==b->geometry_epoch&&a->stream_epoch==b->stream_epoch&&
        a->buttons==b->buttons&&a->modifiers==b->modifiers&&a->flags==b->flags&&a->state==b->state;
}
static int emit(Target *t,unsigned type,unsigned ticks,unsigned changed,int wheel,unsigned reason,unsigned physical){
    if(t->revoked)return 0;
    if(t->reset_pending){refresh_reset(t,ticks);return 0;}
    if(!next_sequence(t))return 0;
    BosUiEventV1 e;snapshot_event(t,&e,type,ticks);
    e.changed_buttons=changed;e.wheel_y=wheel;e.reason=reason;
    if(t->count){
        unsigned tail=(t->head+t->count-1)%BOS_UI_QUEUE_CAPACITY;
        if(same_move(t->queue+tail,&e)){t->queue[tail]=e;return 1;}
    }
    if(t->count==BOS_UI_QUEUE_CAPACITY){
        reset_latch(t,BOS_UI_REASON_QUEUE_LOSS,ticks,physical,1);return 0;
    }
    t->queue[(t->head+t->count)%BOS_UI_QUEUE_CAPACITY]=e;++t->count;return 1;
}
static void position_clear(Target *t){t->position_valid=t->inside=t->modifiers=0;t->x=t->y=0;}
static void cancel(Target *t,unsigned reason,unsigned ticks,unsigned physical){
    int had=t->accepted||capture==t->target;
    forget_capture(t,physical);
    if(had)emit(t,BOS_UI_CANCEL,ticks,0,0,reason,physical);
    position_clear(t);if(hover==t->target)hover=0;
    if(t->reset_pending)refresh_reset(t,ticks);
}
static int geometry_equal(const CanvasView *a,const CanvasView *b){
    return a->x==b->x&&a->y==b->y&&a->logical_w==b->logical_w&&a->logical_h==b->logical_h&&
        a->viewport_w==b->viewport_w&&a->viewport_h==b->viewport_h;
}
static int refresh_target(Target *t,unsigned ticks,unsigned physical){
    if(t->revoked)return 0;
    NativeUiHost host;
    if(!hooks.snapshot||!hooks.snapshot(&t->binding,&host)){revoke(t,physical);return 0;}
    host.state&=BASE_STATE;
    if((host.state&BOS_UI_STATE_MINIMIZED)||host.view.logical_w<=0||host.view.logical_h<=0||host.view.viewport_w<=0||host.view.viewport_h<=0)
        host.state&=~BOS_UI_STATE_AVAILABLE;
    if(!(host.state&BOS_UI_STATE_AVAILABLE)||(host.state&(BOS_UI_STATE_MINIMIZED|BOS_UI_STATE_BLOCKED)))
        host.state&=~BOS_UI_STATE_FOCUSED;
    unsigned changed=host.state^t->host.state;
    int geometry=!geometry_equal(&host.view,&t->host.view)||!!(changed&BOS_UI_STATE_AVAILABLE);
    unsigned reason=geometry?BOS_UI_REASON_GEOMETRY:
        (changed&BOS_UI_STATE_BLOCKED)?BOS_UI_REASON_BLOCKED:
        (changed&BOS_UI_STATE_FOCUSED)?BOS_UI_REASON_FOCUS:BOS_UI_REASON_UNAVAILABLE;
    if(geometry || (changed && (!(host.state&BOS_UI_STATE_FOCUSED)||!(host.state&BOS_UI_STATE_AVAILABLE))))
        cancel(t,reason,ticks,physical);
    if(t->revoked)return 0;
    if(geometry){if(t->geometry_epoch==~0u){revoke(t,physical);return 0;}++t->geometry_epoch;}
    t->host=host;
    if(changed&BOS_UI_STATE_FOCUSED)emit(t,BOS_UI_FOCUS,ticks,0,0,reason,physical);
    if(changed&(BOS_UI_STATE_AVAILABLE|BOS_UI_STATE_MINIMIZED|BOS_UI_STATE_BLOCKED))
        emit(t,BOS_UI_AVAILABILITY,ticks,0,0,reason,physical);
    if(geometry)emit(t,BOS_UI_GEOMETRY,ticks,0,0,BOS_UI_REASON_GEOMETRY,physical);
    if(t->reset_pending)refresh_reset(t,ticks);
    return !t->revoked;
}
static Target *owned(const ProcessBinding *binding,BosHandle target){
    Target *t=by_handle(target);
    if(!t||!binding_equal(binding,&t->binding)||t->revoked)return 0;
    NativeUiAcquired a;acquisition(&a);
    return refresh_target(t,a.ticks,a.buttons)?t:0;
}
static void target_info(const Target *t,BosUiTargetInfoV1 *out){
    *out=(BosUiTargetInfoV1){0};out->size=sizeof *out;out->major=BOS_UI_MAJOR;out->minor=BOS_UI_MINOR;
    out->target=t->target;out->kind=BOS_UI_KIND_HOSTED_CANVAS;out->capabilities=CAPABILITIES;
    out->subscriptions=t->subscriptions;out->state=state(t);
    out->logical_w=t->host.view.logical_w;out->logical_h=t->host.view.logical_h;
    out->viewport_w=t->host.view.viewport_w;out->viewport_h=t->host.view.viewport_h;
    out->geometry_epoch=t->geometry_epoch;out->stream_epoch=t->stream_epoch;out->buttons=t->accepted;
    out->x=t->position_valid?t->x:0;out->y=t->position_valid?t->y:0;out->modifiers=t->position_valid?t->modifiers:0;
    out->queue_capacity=BOS_UI_QUEUE_CAPACITY;out->event_bytes=sizeof(BosUiEventV1);
    out->wait_max_ms=BOS_UI_WAIT_MAX_MS;out->buttons_supported=BUTTONS;
}
void native_ui_init(const NativeUiHooks *configured){
    for(unsigned i=0;i<BOS_UI_TARGETS_TOTAL;i++)targets[i]=(Target){0};
    hooks=configured?*configured:(NativeUiHooks){0};capture=hover=0;swallowed=prior_buttons=0;
    /* next_serial deliberately survives reinitialization. */
}
int native_ui_available(void){return hooks.snapshot&&hooks.acquired&&hooks.focus;}
void native_ui_query(BosUiInfoV1 *out,unsigned hz){
    *out=(BosUiInfoV1){0};out->size=sizeof *out;out->major=BOS_UI_MAJOR;out->minor=BOS_UI_MINOR;
    out->capabilities=CAPABILITIES;out->subscriptions_supported=SUBSCRIPTIONS;out->buttons_supported=BUTTONS;
    out->targets_per_process=1;out->targets_total=BOS_UI_TARGETS_TOTAL;out->queue_capacity=BOS_UI_QUEUE_CAPACITY;
    out->event_bytes=sizeof(BosUiEventV1);out->wait_max_ms=BOS_UI_WAIT_MAX_MS;out->ticks_per_second=hz;
    out->context=BOS_CONTEXT_DESKTOP_TASK;
}
int native_ui_open(const ProcessBinding *binding,unsigned subscriptions,BosUiTargetInfoV1 *out){
    if(!native_ui_available())return BOS_E_UNSUPPORTED;
    if(!binding||!binding->process||!binding->generation||binding->slot>=PROCESS_TASKS||!out||
       !(subscriptions&BOS_UI_SUB_POINTER)||(subscriptions&~SUBSCRIPTIONS))return BOS_E_INVALID;
    NativeUiHost host;
    if(!hooks.snapshot(binding,&host))return BOS_E_STALE;
    Target *empty=0;
    for(unsigned i=0;i<BOS_UI_TARGETS_TOTAL;i++){
        if(targets[i].target&&!targets[i].revoked&&targets[i].binding.process==binding->process)return BOS_E_BUSY;
        if((!targets[i].target||targets[i].revoked)&&!empty)empty=targets+i;
    }
    if(!empty||next_serial==BOS_HANDLE_SERIAL_MAX)return BOS_E_CAPACITY;
    NativeUiAcquired a;acquisition(&a);
    *empty=(Target){0};empty->binding=*binding;empty->target=BOS_HANDLE_TYPE_UI_TARGET|++next_serial;
    empty->subscriptions=subscriptions;empty->geometry_epoch=empty->stream_epoch=1;
    empty->host=host;empty->host.state&=BASE_STATE;empty->fence=a.serial;empty->suppressed=a.buttons;
    /* Normalize the initial snapshot without treating it as a geometry change. */
    if((host.state&BOS_UI_STATE_MINIMIZED)||host.view.logical_w<=0||host.view.logical_h<=0||host.view.viewport_w<=0||host.view.viewport_h<=0)
        empty->host.state&=~BOS_UI_STATE_AVAILABLE;
    if(!(empty->host.state&BOS_UI_STATE_AVAILABLE)||(empty->host.state&(BOS_UI_STATE_MINIMIZED|BOS_UI_STATE_BLOCKED)))
        empty->host.state&=~BOS_UI_STATE_FOCUSED;
    reset_latch(empty,BOS_UI_REASON_OPEN,a.ticks,a.buttons,0);
    target_info(empty,out);return BOS_OK;
}
int native_ui_info(const ProcessBinding *binding,BosHandle target,BosUiTargetInfoV1 *out){
    Target *t=owned(binding,target);if(!t)return BOS_E_STALE;if(!out)return BOS_E_INVALID;
    target_info(t,out);return BOS_OK;
}
int native_ui_read(const ProcessBinding *binding,BosHandle target,BosUiEventV1 *out){
    Target *t=owned(binding,target);if(!t)return BOS_E_STALE;if(!out)return BOS_E_INVALID;
    if(t->reset_pending){*out=t->reset;t->reset_pending=0;return BOS_OK;}
    if(!t->count)return BOS_PENDING;
    *out=t->queue[t->head];t->head=(t->head+1)%BOS_UI_QUEUE_CAPACITY;--t->count;return BOS_OK;
}
int native_ui_ready(const ProcessBinding *binding,BosHandle target){
    Target *t=owned(binding,target);return !t?BOS_E_STALE:(t->reset_pending||t->count)?BOS_OK:BOS_PENDING;
}
int native_ui_release(const ProcessBinding *binding,BosHandle target){
    Target *t=owned(binding,target);if(!t)return BOS_E_STALE;
    NativeUiAcquired a;acquisition(&a);revoke(t,a.buttons);*t=(Target){0};return BOS_OK;
}
void native_ui_revoke_owner(ProcessHandle owner){
    NativeUiAcquired a;acquisition(&a);
    for(unsigned i=0;i<BOS_UI_TARGETS_TOTAL;i++)if(targets[i].target&&targets[i].binding.process==owner)
        revoke(targets+i,a.buttons);
}
void native_ui_release_owner(ProcessHandle owner){
    native_ui_revoke_owner(owner);
    for(unsigned i=0;i<BOS_UI_TARGETS_TOTAL;i++)if(targets[i].target&&targets[i].binding.process==owner)
        targets[i]=(Target){0};
}
BosHandle native_ui_target_at(unsigned slot){
    for(unsigned i=0;i<BOS_UI_TARGETS_TOTAL;i++)if(targets[i].target&&!targets[i].revoked&&targets[i].binding.slot==slot)
        return targets[i].target;
    return 0;
}
void native_ui_refresh(unsigned ticks,unsigned physical){
    for(unsigned i=0;i<BOS_UI_TARGETS_TOTAL;i++)if(targets[i].target)refresh_target(targets+i,ticks,physical);
}
void native_ui_cancel_all(unsigned reason,unsigned ticks,unsigned physical){
    prior_buttons=physical&BUTTONS;swallowed&=physical;
    for(unsigned i=0;i<BOS_UI_TARGETS_TOTAL;i++)if(targets[i].target&&!targets[i].revoked){
        targets[i].suppressed&=physical;cancel(targets+i,reason,ticks,physical);
    }
    /* This explicit barrier discards the prior routed backlog. */
    swallowed&=physical;
}
void native_ui_input_loss(unsigned ticks,unsigned physical,unsigned dropped){
    prior_buttons=physical&BUTTONS;swallowed&=physical;
    for(unsigned i=0;i<BOS_UI_TARGETS_TOTAL;i++)if(targets[i].target&&!targets[i].revoked){
        targets[i].suppressed&=physical;
        reset_latch(targets+i,BOS_UI_REASON_INPUT_LOSS,ticks,physical,dropped);
    }
    swallowed&=physical;
}
static int eligible(const Target *t){
    return t&&!t->revoked&&!t->reset_pending&&(t->host.state&BOS_UI_STATE_AVAILABLE)&&
        !(t->host.state&(BOS_UI_STATE_MINIMIZED|BOS_UI_STATE_BLOCKED));
}
static int update_position(Target *t,const InputSample *sample){
    int x,y;canvas_view_position(&t->host.view,sample->x,sample->y,&x,&y);
    unsigned inside=canvas_view_contains(&t->host.view,sample->x,sample->y);
    int changed=!t->position_valid||t->x!=x||t->y!=y||t->inside!=inside||t->modifiers!=sample->modifiers;
    t->position_valid=1;t->x=x;t->y=y;t->inside=inside;t->modifiers=sample->modifiers;
    return changed;
}
unsigned native_ui_route(const InputSample *sample,BosHandle hit_target){
    if(!sample||sample->kind!=INPUT_POINTER)return 0;
    unsigned physical=sample->buttons&BUTTONS,previous=prior_buttons;
    prior_buttons=physical;
    native_ui_refresh(sample->ticks,physical);
    for(unsigned i=0;i<BOS_UI_TARGETS_TOTAL;i++)targets[i].suppressed&=physical;
    unsigned was_swallowed=swallowed;swallowed&=physical;
    Target *t=by_handle(capture),*hit=by_handle(hit_target);
    if(t&&t->revoked)t=0;
    if(!t&&!was_swallowed&&eligible(hit)&&sample->serial>hit->fence&&
       canvas_view_contains(&hit->host.view,sample->x,sample->y)){
        unsigned down=physical&~previous&~hit->suppressed;
        if(down){
            hooks.focus(&hit->binding);native_ui_refresh(sample->ticks,physical);
            if(!eligible(hit)||!(hit->host.state&BOS_UI_STATE_FOCUSED)){
                hit->suppressed|=physical;swallowed|=physical;return CONSUMED;
            }
            t=hit;t->suppressed|=previous;capture=t->target;
        }
    }
    /* Focus/state changes precede a hover boundary notification. A peer's
     * unread RESET cannot keep the old hover position alive or steal capture. */
    if(!t&&hover&&(!hit||hit->target!=hover)){
        Target *old=by_handle(hover);hover=0;
        if(old&&!old->revoked&&old->position_valid){
            position_clear(old);emit(old,BOS_UI_POINTER_MOVE,sample->ticks,0,0,0,physical);
        }
    }
    if(!t&&was_swallowed){swallowed|=physical;return CONSUMED;}
    if(!t&&hit&&(hit->reset_pending||sample->serial<=hit->fence)){
        hit->suppressed|=physical;swallowed|=physical;return CONSUMED;
    }
    if(!t&&hit&&(!eligible(hit)||!canvas_view_contains(&hit->host.view,sample->x,sample->y)))hit=0;
    if(t){
        /* State/focus is already queued. Motion carries prior accepted buttons;
         * button carries post-transition buttons; wheel follows final UP too. */
        int moved=update_position(t,sample);
        unsigned before=t->accepted,after=physical&~t->suppressed;
        if(moved&&!emit(t,BOS_UI_POINTER_MOVE,sample->ticks,0,0,0,physical))return CONSUMED;
        if(t->revoked||t->reset_pending)return CONSUMED;
        t->accepted=after;
        if(before!=after&&!emit(t,BOS_UI_POINTER_BUTTON,sample->ticks,before^after,0,0,physical))return CONSUMED;
        if(t->revoked||t->reset_pending)return CONSUMED;
        if(sample->wheel&&(t->subscriptions&BOS_UI_SUB_WHEEL))
            emit(t,BOS_UI_POINTER_WHEEL,sample->ticks,0,sample->wheel,0,physical);
        if(!t->accepted&&capture==t->target){capture=0;swallowed|=physical;}
        if(t->position_valid&&t->inside&&(t->subscriptions&BOS_UI_SUB_HOVER))hover=t->target;
        return CONSUMED;
    }
    if(!hit||physical||previous||!(hit->host.state&BOS_UI_STATE_FOCUSED))return 0;
    unsigned result=0;
    if(hit->subscriptions&BOS_UI_SUB_HOVER){
        hover=hit->target;
        if(update_position(hit,sample)&&!emit(hit,BOS_UI_POINTER_MOVE,sample->ticks,0,0,0,physical))return CONSUMED;
        result|=NATIVE_UI_CONSUMED_POINTER;
    }
    if(sample->wheel&&(hit->subscriptions&BOS_UI_SUB_WHEEL)){
        update_position(hit,sample);
        emit(hit,BOS_UI_POINTER_WHEEL,sample->ticks,0,sample->wheel,0,physical);
        result|=NATIVE_UI_CONSUMED_WHEEL;
    }
    return result;
}
