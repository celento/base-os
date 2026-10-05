/* Deterministic launch/adopt contracts through real parser, process dispatcher
 * and endpoint core. No guest instructions, deliberate faults or fuzzing. */
#define NATIVE_UI_REAL_SERVICE
#define main old_platform_fixture_main
#include "native_platform_dispatch_host.c"
#undef main
static NativeUiAcquired ui_acquired;
static NativeUiHost ui_hosts[8];
static int ui_snapshot(const ProcessBinding *binding,NativeUiHost *out){
    if(!process_binding_live(binding))return 0;
    *out=ui_hosts[binding->slot];out->state|=BOS_UI_STATE_FOCUSED;return 1;
}
static void ui_acquire(NativeUiAcquired *out){*out=ui_acquired;out->ticks=now;}
static void ui_focus_owner(const ProcessBinding *binding){(void)binding;}
static int ui(unsigned op,unsigned a,unsigned b,unsigned c,unsigned d){
    return invoke(BOS_CALL_UI,op,a,b,c,d);
}
static void unchanged(unsigned from,unsigned bytes){
    for(unsigned i=from;i<from+bytes;i++)assert(user_memory[i]==0xa5);
}
static unsigned char gui_file[8196];
static void header_word(unsigned index,unsigned value){
    for(unsigned byte=0;byte<4;byte++)gui_file[index*4+byte]=(unsigned char)(value>>(byte*8));
}
static void gui_image(unsigned flags,unsigned minor){
    memset(gui_file,0,sizeof gui_file);
    unsigned words[]={BOS_BEX2_MAGIC,64,1,flags,sizeof gui_file,4096,1,8192,4,4096,0,16384,1,minor,0,0};
    for(unsigned i=0;i<16;i++)header_word(i,words[i]);
}
static void parser_policy(void){
    ExecutablePolicy policy={1,2,BOS_BEX2_FILE_MAX,PROCESS_PRIVATE_PAGE_LIMIT};
    ExecutablePlan plan,unchanged;memset(&plan,0xa5,sizeof plan);unchanged=plan;
    gui_image(BOS_BEX2_FLAG_NATIVE_WINDOW_V1,2);
    assert(executable_plan_bex2(gui_file,sizeof gui_file,&policy,&plan)==EXECUTABLE_UNSUPPORTED);
    assert(!memcmp(&plan,&unchanged,sizeof plan));
    assert(executable_plan_bex2_flags(gui_file,sizeof gui_file,&policy,0,&plan)==EXECUTABLE_UNSUPPORTED);
    assert(!memcmp(&plan,&unchanged,sizeof plan));
    assert(executable_plan_bex2_flags(gui_file,sizeof gui_file,&policy,BOS_BEX2_FLAGS_KNOWN,&plan)==EXECUTABLE_OK);
    assert(plan.flags==BOS_BEX2_FLAG_NATIVE_WINDOW_V1&&plan.kind==BOS_EXECUTABLE_BEX2);
    unchanged=plan;header_word(3,2);
    assert(executable_plan_bex2_flags(gui_file,sizeof gui_file,&policy,~0u,&plan)==EXECUTABLE_UNSUPPORTED);
    assert(!memcmp(&plan,&unchanged,sizeof plan));
    gui_image(BOS_BEX2_FLAG_NATIVE_WINDOW_V1,1);
    assert(executable_plan_bex2_flags(gui_file,sizeof gui_file,&policy,BOS_BEX2_FLAGS_KNOWN,&plan)==EXECUTABLE_UNSUPPORTED);
    gui_image(BOS_BEX2_FLAG_NATIVE_WINDOW_V1,2);policy.abi_minor=1;
    assert(executable_plan_bex2_flags(gui_file,sizeof gui_file,&policy,BOS_BEX2_FLAGS_KNOWN,&plan)==EXECUTABLE_UNSUPPORTED);
    policy.abi_minor=2;gui_image(0,1);
    assert(executable_plan_bex2(gui_file,sizeof gui_file,&policy,&plan)==EXECUTABLE_OK&&!plan.flags);
}
static void creation_modes(void){
    active=0;task_reset_all();
    unsigned serial=owner_serial,ready=tasks_ready,mode=99;
    unsigned pages=host_physmem_stats().allocated;
    ProcessHandle result=0xfeed1234;
    gui_image(BOS_BEX2_FLAG_NATIVE_WINDOW_V1,2);
    assert(process_probe_launch(gui_file,sizeof gui_file,&mode)==0&&mode==PROCESS_LAUNCH_OWNED_WINDOW);
    assert(owner_serial==serial&&tasks_ready==(int)ready&&host_physmem_stats().allocated==pages);
    assert(process_create(gui_file,sizeof gui_file,0,0,&result)==PROCESS_CREATE_UNSUPPORTED);
    assert(process_create_mode(gui_file,sizeof gui_file,0,0,PROCESS_LAUNCH_HOSTED,&result)==PROCESS_CREATE_UNSUPPORTED);
    assert(result==0xfeed1234&&owner_serial==serial&&tasks_ready==(int)ready&&host_physmem_stats().allocated==pages);
    assert(process_create_mode(program,sizeof program,0,0,PROCESS_LAUNCH_OWNED_WINDOW,&result)==PROCESS_CREATE_UNSUPPORTED);
    assert(result==0xfeed1234&&owner_serial==serial&&host_physmem_stats().allocated==pages);
    assert(process_probe_launch(program,sizeof program,&mode)==0&&mode==PROCESS_LAUNCH_HOSTED);
    gui_image(0,1);assert(process_probe_launch(gui_file,sizeof gui_file,&mode)==0&&mode==PROCESS_LAUNCH_HOSTED);
    assert(process_create_mode(gui_file,sizeof gui_file,0,0,PROCESS_LAUNCH_OWNED_WINDOW,&result)==PROCESS_CREATE_UNSUPPORTED);
    gui_image(2,2);mode=99;
    assert(process_probe_launch(gui_file,sizeof gui_file,&mode)==PROCESS_CREATE_UNSUPPORTED&&mode==99);
    assert(process_create_mode(gui_file,sizeof gui_file,0,0,PROCESS_LAUNCH_OWNED_WINDOW,&result)==PROCESS_CREATE_UNSUPPORTED);
    assert(result==0xfeed1234&&owner_serial==serial&&host_physmem_stats().allocated==pages);
    gui_image(BOS_BEX2_FLAG_NATIVE_WINDOW_V1,2);
    assert(process_create_mode(gui_file,sizeof gui_file,0,0,PROCESS_LAUNCH_OWNED_WINDOW,&result)==0);
    assert(process_launch_mode(result)==PROCESS_LAUNCH_OWNED_WINDOW&&process_status(result)==PROCESS_TASK_CREATED);
    handles[0]=result;io.binding=(ProcessBinding){result,0,23};
    assert(process_bind(result,&io)&&process_start(result));select_task(0);
    const NativeUiHooks hooks={ui_snapshot,ui_acquire,ui_focus_owner};native_ui_init(&hooks);
    ui_hosts[0]=(NativeUiHost){.view={100,100,160,100,213,133},.state=BOS_UI_STATE_AVAILABLE,
        .kind=BOS_UI_KIND_OWNED_WINDOW,.capabilities=NATIVE_UI_CAPABILITIES_OWNED};
}
#define OUT 8192u
#define EVENT 8304u
static void owned_dispatch(void){
    unsigned before=publications;
    memset(user_memory+OUT,0xa5,256);
    assert(invoke(BOS_CALL_ABI_QUERY,OUT,112,1,0,0)==BOS_OK);
    BosAbiInfo abi;memcpy(&abi,user_memory+OUT,sizeof abi);
    assert(abi.abi_minor==BOS_ABI_MINOR&&(abi.features&BOS_FEATURE_BEX2)&&(abi.features&BOS_FEATURE_OWNED_NATIVE_WINDOW));
    assert(!(abi.features&BOS_FEATURE_HOSTED_UI));unchanged(OUT+96,16);
    memset(user_memory+OUT,0xa5,256);
    assert(ui(BOS_UI_QUERY,1,OUT,80,0)==BOS_OK);
    BosUiInfoV1 query;memcpy(&query,user_memory+OUT,64);
    assert(query.minor==1&&query.capabilities==NATIVE_UI_CAPABILITIES_OWNED);unchanged(OUT+64,16);
    memset(user_memory+OUT,0xa5,256);
    assert(ui(BOS_UI_HOST_OPEN,1,OUT,112,7)==BOS_E_UNSUPPORTED);unchanged(OUT,112);
    assert(ui(BOS_UI_WINDOW_ADOPT,2,OUT,112,7)==BOS_E_UNSUPPORTED);unchanged(OUT,112);
    assert(ui(BOS_UI_WINDOW_ADOPT,1,12288-96,112,7)==BOS_E_INVALID);
    assert(!native_ui_target_at(0));
    assert(ui(BOS_UI_WINDOW_ADOPT,1,OUT,95,7)==BOS_E_INVALID);
    assert(ui(BOS_UI_WINDOW_ADOPT,1,OUT,112,8)==BOS_E_INVALID);unchanged(OUT,112);
    ui_hosts[0].kind=BOS_UI_KIND_HOSTED_CANVAS;ui_hosts[0].capabilities=NATIVE_UI_CAPABILITIES_HOSTED;
    assert(ui(BOS_UI_WINDOW_ADOPT,1,OUT,112,7)==BOS_E_UNSUPPORTED);unchanged(OUT,112);
    assert(!native_ui_target_at(0));
    ui_hosts[0].kind=BOS_UI_KIND_OWNED_WINDOW;ui_hosts[0].capabilities=NATIVE_UI_CAPABILITIES_OWNED;
    assert(ui(BOS_UI_WINDOW_ADOPT,1,OUT,112,7)==BOS_OK);
    BosUiTargetInfoV1 target;memcpy(&target,user_memory+OUT,96);unchanged(OUT+96,16);
    assert(target.kind==BOS_UI_KIND_OWNED_WINDOW&&target.capabilities==NATIVE_UI_CAPABILITIES_OWNED);
    assert(!target.reserved[0]&&!target.reserved[1]&&publications==before);
    memset(user_memory+OUT,0xa5,112);
    assert(ui(BOS_UI_WINDOW_ADOPT,1,OUT,112,7)==BOS_E_BUSY);unchanged(OUT,112);
    assert(ui(BOS_UI_WAIT,target.target,1,60000,0)==BOS_OK&&publications==before);
    assert(ui(BOS_UI_READ,target.target,12288-96,112,0)==BOS_E_INVALID);
    assert(ui(BOS_UI_READ,target.target,EVENT,112,0)==BOS_OK);
    BosUiEventV1 event;memcpy(&event,user_memory+EVENT,96);
    assert(event.type==BOS_UI_STATE_RESET&&event.reason==BOS_UI_REASON_OPEN);unchanged(EVENT+96,16);
    memset(user_memory+EVENT,0xa5,112);
    assert(ui(BOS_UI_READ,target.target,EVENT,112,0)==BOS_PENDING);unchanged(EVENT,112);
    assert(ui(BOS_UI_WAIT,target.target,1,1,0)==12345&&publications==before);
    now=current_task->wake;assert(task_wake(current_task)&&(int)current_task->frame[7]==BOS_E_TIMEOUT);
    /* RELEASE drops only the endpoint; process identity and byte input remain. */
    assert(ui(BOS_UI_RELEASE,target.target,0,0,0)==BOS_OK);
    assert(process_status(current_task->owner_id)==PROCESS_TASK_READY);
    assert(process_key(current_task->owner_id,'x')&&invoke(BOS_CALL_KEY,0,0,0,0,0)=='x');
    assert(ui(BOS_UI_INFO,target.target,OUT,112,0)==BOS_E_STALE);unchanged(OUT,112);
    assert(ui(BOS_UI_WINDOW_ADOPT,1,OUT,112,7)==BOS_OK);
    BosUiTargetInfoV1 reopened;memcpy(&reopened,user_memory+OUT,96);
    assert(reopened.target!=target.target);
    assert(ui(BOS_UI_READ,reopened.target,EVENT,96,0)==BOS_OK);
    memcpy(&event,user_memory+EVENT,96);assert(event.type==BOS_UI_STATE_RESET);
    /* Existing positive pending sync wait still publishes once in GUI mode. */
    stub_poll=BOS_PENDING;
    assert(invoke(BOS_CALL_SYNC_WAIT,BOS_HANDLE_TYPE_OPERATION|1,1000,0,0,0)==12345&&publications==before+1);
    stub_poll=BOS_OK;assert(task_wake(current_task));
    assert(ui(BOS_UI_WAIT,reopened.target,1,60000,0)==12345&&publications==before+1);
    ProcessBinding binding=current_task->io.binding;
    assert(!process_request_stop(binding.process)&&!process_binding_live(&binding));
    memset(user_memory+OUT,0xa5,112);
    assert(ui(BOS_UI_WINDOW_ADOPT,1,OUT,112,7)==BOS_E_STALE);unchanged(OUT,112);
    assert(task_wake(current_task)&&(int)current_task->frame[7]==BOS_E_STALE);
    /* A revoked waiter cannot resume ready; Stop finalizes before another slice. */
    active=0;assert(process_request_stop(binding.process));assert(process_reap(binding.process));
    assert(!process_launch_mode(binding.process)&&publications==before+1);handles[0]=0;
    current_task=0;active=1;
    assert(ui(BOS_UI_WINDOW_ADOPT,1,OUT,112,7)==BOS_E_UNSUPPORTED);
}
int main(void){
    host_physmem_init(64);parser_policy();creation_modes();owned_dispatch();
    active=0;task_reset_all();assert(!host_physmem_stats().allocated);host_physmem_destroy();
    puts("Owned windows: flags/policy, no-allocation mismatch, contextual ABI, exact ADOPT spans, release/reopen, wait isolation and Stop passed.");
    return 0;
}
