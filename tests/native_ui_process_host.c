/* Real endpoint core + production dispatcher/scheduler boundaries. Existing
 * fixture supplies ordinary process backing and unrelated service adapters. */
#define NATIVE_UI_REAL_SERVICE
#define main old_platform_fixture_main
#include "native_platform_dispatch_host.c"
#undef main
static NativeUiAcquired ui_acquired;
static NativeUiHost ui_hosts[8];
static int ui_focus;
static int ui_snapshot(const ProcessBinding *binding,NativeUiHost *out){
    if(!process_binding_live(binding))return 0;
    *out=ui_hosts[binding->slot];
    if((int)binding->slot==ui_focus)out->state|=BOS_UI_STATE_FOCUSED;
    return 1;
}
static void ui_acquire(NativeUiAcquired *out){*out=ui_acquired;out->ticks=now;}
static void ui_focus_owner(const ProcessBinding *binding){ui_focus=(int)binding->slot;}
static int ui(unsigned op,unsigned a,unsigned b,unsigned c,unsigned d){
    return invoke(BOS_CALL_UI,op,a,b,c,d);
}
static BosHandle open_ui(unsigned slot){
    start(slot);assert(ui(BOS_UI_HOST_OPEN,1,256,96,7)==BOS_OK);
    BosUiTargetInfoV1 info;memcpy(&info,user_memory+256,sizeof info);return info.target;
}
static void pointer(BosHandle target,unsigned buttons){
    ui_acquired.serial++;ui_acquired.buttons=buttons;
    InputSample s={.serial=ui_acquired.serial,.ticks=now,.kind=INPUT_POINTER,
        .x=110,.y=110,.buttons=buttons};
    native_ui_route(&s,target);
}
static void drain(BosHandle target){
    while(ui(BOS_UI_READ,target,512,96,0)==BOS_OK){}
}
static void unchanged(unsigned from,unsigned bytes){for(unsigned i=from;i<from+bytes;i++)assert(user_memory[i]==0xa5);}
static void negotiation(void){
    start(0);assert(ui(BOS_UI_QUERY,1,64,64,0)==BOS_E_UNSUPPORTED);
    const NativeUiHooks hooks={ui_snapshot,ui_acquire,ui_focus_owner};native_ui_init(&hooks);
    for(unsigned i=0;i<8;i++)ui_hosts[i]=(NativeUiHost){.view={100,100,160,100,213,133},.state=BOS_UI_STATE_AVAILABLE};
    memset(user_memory,0xa5,sizeof user_memory);
    assert(ui(BOS_UI_QUERY,1,64,16,0)==BOS_OK);
    BosUiInfoV1 info;memcpy(&info,user_memory+64,16);assert(info.size==64&&info.major==1);unchanged(80,64);
    memset(user_memory,0xa5,sizeof user_memory);
    assert(ui(BOS_UI_QUERY,1,64,80,0)==BOS_OK);memcpy(&info,user_memory+64,64);
    assert(info.queue_capacity==64&&info.event_bytes==96&&info.wait_max_ms==60000&&info.ticks_per_second==70);
    assert(info.targets_per_process==1&&info.targets_total==8&&info.context==BOS_CONTEXT_DESKTOP_TASK);unchanged(128,16);
    memset(user_memory,0xa5,sizeof user_memory);
    assert(ui(BOS_UI_QUERY,2,64,64,0)==BOS_E_UNSUPPORTED);
    assert(ui(BOS_UI_QUERY,1,64,15,0)==BOS_E_INVALID);
    assert(ui(BOS_UI_QUERY,1,64,64,1)==BOS_E_INVALID);
    assert(ui(BOS_UI_QUERY,1,USER_CAPACITY-16,64,0)==BOS_E_INVALID);unchanged(64,80);
    assert(ui(99,0,0,0,0)==BOS_E_UNSUPPORTED);
    assert(ui(BOS_UI_HOST_OPEN,2,256,96,7)==BOS_E_UNSUPPORTED);
    assert(ui(BOS_UI_HOST_OPEN,1,256,95,7)==BOS_E_INVALID);
    assert(ui(BOS_UI_HOST_OPEN,1,USER_CAPACITY-16,96,7)==BOS_E_INVALID);
    assert(ui(BOS_UI_HOST_OPEN,1,256,96,0)==BOS_E_INVALID);
    assert(!native_ui_target_at(0));unchanged(256,96);
    assert(ui(BOS_UI_HOST_OPEN,1,256,112,7)==BOS_OK);
    BosUiTargetInfoV1 target;memcpy(&target,user_memory+256,96);unchanged(352,16);
    assert(ui(BOS_UI_HOST_OPEN,1,256,96,7)==BOS_E_BUSY);
    assert(invoke(BOS_CALL_ABI_QUERY,64,96,1,0,0)==BOS_OK);
    BosAbiInfo abi;memcpy(&abi,user_memory+64,96);assert(abi.features&BOS_FEATURE_HOSTED_UI);
    /* Invalid whole declared output does not consume the initial reset. */
    assert(ui(BOS_UI_READ,target.target,USER_CAPACITY-96,112,0)==BOS_E_INVALID);
    assert(ui(BOS_UI_READ,target.target,512,96,1)==BOS_E_INVALID);
    assert(ui(BOS_UI_READ,target.target,512,95,0)==BOS_E_INVALID);
    assert(ui(BOS_UI_WAIT,target.target,1,0,0)==BOS_OK);
    memset(user_memory+512,0xa5,112);
    assert(ui(BOS_UI_READ,target.target,512,112,0)==BOS_OK);
    BosUiEventV1 event;memcpy(&event,user_memory+512,96);assert(event.type==BOS_UI_STATE_RESET);unchanged(608,16);
    memset(user_memory+512,0xa5,112);
    assert(ui(BOS_UI_READ,target.target,512,112,0)==BOS_PENDING);unchanged(512,112);
    assert(ui(BOS_UI_INFO,target.target,512,112,0)==BOS_OK);unchanged(608,16);
    assert(ui(BOS_UI_RELEASE,target.target,1,0,0)==BOS_E_INVALID);
    assert(ui(BOS_UI_RELEASE,target.target,0,0,0)==BOS_OK);
    assert(ui(BOS_UI_READ,target.target,512,96,0)==BOS_E_STALE);
    current_task=0;assert(ui(BOS_UI_QUERY,1,64,64,0)==BOS_E_UNSUPPORTED);
}
static void ui_waits(void){
    BosHandle target=open_ui(0);unsigned before=publications;
    /* Initial reset is immediately ready, and must not publish. */
    assert(ui(BOS_UI_WAIT,target,1,60000,0)==BOS_OK&&publications==before);drain(target);
    assert(ui(BOS_UI_WAIT,target,1,0,0)==BOS_PENDING&&publications==before);
    assert(ui(BOS_UI_WAIT,target,0,1,0)==BOS_E_INVALID);
    assert(ui(BOS_UI_WAIT,target,5,1,0)==BOS_E_INVALID);
    assert(ui(BOS_UI_WAIT,target,1,60001,0)==BOS_E_INVALID);
    assert(ui(BOS_UI_WAIT,target,1,1,1)==BOS_E_INVALID);
    now=100;assert(ui(BOS_UI_WAIT,target,1,1,0)==12345);
    assert(current_task->state==PROCESS_TASK_SLEEPING&&current_task->wait_kind==TASK_WAIT_UI);
    assert(current_task->wait_ui==target&&current_task->wake==101&&publications==before);
    assert(!task_wake(current_task));now=101;
    assert(task_wake(current_task)&&(int)current_task->frame[7]==BOS_E_TIMEOUT&&current_task->wait_kind==TASK_WAIT_NONE);
    assert(ui(BOS_UI_INFO,target,512,96,0)==BOS_OK);
    now=0xfffffff0u;assert(ui(BOS_UI_WAIT,target,1,1000,0)==12345);
    assert(current_task->wake==54&&!task_wake(current_task));now=54;
    pointer(target,1);assert(task_wake(current_task)&&current_task->frame[7]==BOS_OK);
    assert(publications==before);drain(target);pointer(target,0);drain(target);
    /* Byte-key readiness consumes neither source and does not change old keys. */
    assert(ui(BOS_UI_WAIT,target,3,60000,0)==12345);
    assert(process_key(current_task->owner_id,'x'));assert(task_wake(current_task)&&current_task->frame[7]==BOS_OK);
    assert(ui(BOS_UI_WAIT,target,3,0,0)==BOS_OK);assert(invoke(BOS_CALL_KEY,0,0,0,0,0)=='x');
    assert(ui(BOS_UI_WAIT,target,3,0,0)==BOS_PENDING);
    assert(ui(BOS_UI_WAIT,target,1,1000,0)==12345);process_key(current_task->owner_id,'y');assert(!task_wake(current_task));
    now=current_task->wake;assert(task_wake(current_task)&&(int)current_task->frame[7]==BOS_E_TIMEOUT);
    assert(invoke(BOS_CALL_KEY,0,0,0,0,0)=='y');
    /* An event cannot prematurely satisfy an independent storage wait. */
    stub_poll=BOS_PENDING;assert(invoke(BOS_CALL_SYNC_WAIT,BOS_HANDLE_TYPE_OPERATION|1,1000,0,0,0)==12345);
    assert(current_task->wait_kind==TASK_WAIT_SYNC&&publications==before+1);
    pointer(target,1);assert(!task_wake(current_task));stub_poll=BOS_OK;
    assert(task_wake(current_task)&&current_task->frame[7]==BOS_OK);drain(target);pointer(target,0);drain(target);
    before=publications;
    assert(ui(BOS_UI_WAIT,target,1,60000,0)==12345);
    assert(native_ui_release(&current_task->io.binding,target)==BOS_OK);
    assert(task_wake(current_task)&&(int)current_task->frame[7]==BOS_E_STALE&&publications==before);
}
static void ui_lifetimes(void){
    BosHandle target=open_ui(0);ProcessBinding binding=current_task->io.binding;
    drain(target);pointer(target,1);drain(target);
    assert(process_binding_live(&binding));
    assert(!process_request_stop(binding.process));
    assert(!process_binding_live(&binding)&&native_ui_ready(&binding,target)==BOS_E_STALE);
    assert(current_task->state==PROCESS_TASK_READY); /* active Stop defers backing cleanup */
    active=0;assert(process_request_stop(binding.process));assert(current_task->state==PROCESS_TASK_DONE);
    start(0);assert(!process_binding_live(&binding));
    target=open_ui(0);drain(target);
    ProcessBinding owner=current_task->io.binding;
    start(1);memset(user_memory+512,0xa5,96);
    assert(ui(BOS_UI_READ,target,512,96,0)==BOS_E_STALE);unchanged(512,96);
    assert(ui(BOS_UI_RELEASE,target,0,0,0)==BOS_E_STALE);
    select_task(0);assert(ui(BOS_UI_INFO,target,512,96,0)==BOS_OK);
    assert(invoke(BOS_CALL_EXIT,0,0,0,0,0)==12345);
    assert(native_ui_ready(&owner,target)==BOS_E_STALE);
    active=0;task_finalize(current_task);
}
int main(void){
    host_physmem_init(64);negotiation();ui_waits();ui_lifetimes();
    active=0;task_reset_all();assert(!host_physmem_stats().allocated);host_physmem_destroy();
    puts("Native UI process boundary: exact spans, negotiation, wait isolation, wrap/tie/key readiness, no implicit publication and immediate revocation passed.");
    return 0;
}
