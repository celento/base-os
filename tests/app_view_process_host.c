/* Real process dispatch/scheduling + real view/canvas publication. Only machine
 * entry, FPU and device services use the existing ordinary host fixture spies. */
static void view_slice(void);
#define PROCESS_LIFETIME_SLICE_HOOK view_slice
#define main process_lifetime_fixture_main
#include "process_lifetime_host.c"
#undef main

static unsigned char view_arena[0xC0000],published_arena[NATIVE_CANVAS_CAPACITY];
#define TERM_MEMORY ((uintptr_t)view_arena)
#define NATIVE_CANVAS_MEMORY ((uintptr_t)published_arena)
#include "../src/app_storage.c"
#include "../src/app_canvas.c"
#include "../src/app_view.c"

static char last_line[PROCESS_TASKS][81];
static unsigned line_count[PROCESS_TASKS];
void term_write_at(int slot,const char *text){
    assert(slot>=0&&slot<PROCESS_TASKS);
    snprintf(last_line[slot],sizeof last_line[slot],"%s",text);
    line_count[slot]++;app_view_mark(slot,APP_VIEW_TEXT);
}
void kstrcpy(char *to,const char *from){strcpy(to,from);}
int fs_valid(int id){return id==1;}
int fs_is_dir(int id){(void)id;return 0;}
int fs_is_app(int id){(void)id;return 0;}
unsigned fs_identity(int id){return id==1?7:0;}
const char *fs_data(int id){assert(id==1);return (const char *)image;}
int fs_size(int id){assert(id==1);return sizeof image;}
const char *fs_name(int id){assert(id==1);return "view.bex";}

enum { VIEW_INITIAL, VIEW_PRESENT, VIEW_YIELD, VIEW_SLEEP_ZERO, VIEW_SLEEP, VIEW_SYNC_WAIT,
       VIEW_EXIT_ZERO, VIEW_EXIT_NONZERO, VIEW_EXIT_STOP_VALUE, VIEW_TIMER, VIEW_ERROR, VIEW_PENDING, VIEW_SCHEDULER };
static int view_mode;
static unsigned owner_slot=4;
enum { SCHEDULE_YIELD, SCHEDULE_SLEEP, SCHEDULE_WAIT };
static unsigned owner_slices[PROCESS_TASKS],owner_action[PROCESS_TASKS];
static int invoke_all(unsigned call,unsigned a,unsigned b,unsigned c,unsigned d,unsigned e){
    uint32_t r[FRAME_WORDS]={0};r[7]=call;r[4]=a;r[6]=b;r[5]=c;r[1]=d;r[0]=e;r[12]=128;r[15]=0x1b;
    assert(process_interrupt(r)==1);return (int)r[7];
}
static void timer_return(void){
    uint32_t r[FRAME_WORDS]={0};r[12]=32;r[15]=0x1b;
    process_interrupt(r);assert(!"Timer must return to the kernel");
}
static void frame_is(int width,int height,int changed){
    AppCanvasFrame frame=app_view_frame((int)owner_slot);
    assert(frame.pixels&&frame.width==width&&frame.height==height);
    for(int y=0;y<height;y++)for(int x=0;x<width;x++)
        assert(frame.pixels[y*width+x]==(changed?(x==7&&y==8?37:22):11));
}
static void view_slice(void){
    assert(current_task);
    if(view_mode==VIEW_SCHEDULER){
        unsigned slot=current_task->io.binding.slot;assert(slot<PROCESS_TASKS);
        ++owner_slices[slot];
        assert(!invoke_all(BOS_CALL_PLOT,1,1,owner_slices[slot],0,0));
        if(owner_action[slot]==SCHEDULE_SLEEP)invoke(BOS_CALL_SLEEP,1000,0);
        if(owner_action[slot]==SCHEDULE_WAIT)invoke(BOS_CALL_SYNC_WAIT,BOS_HANDLE_TYPE_OPERATION|7,1000);
        invoke(BOS_CALL_YIELD,0,0);assert(!"Scheduled slice must suspend");
    }
    assert(current_task->io.binding.slot==owner_slot);
    if(view_mode==VIEW_INITIAL){
        assert(!invoke(BOS_CALL_CANVAS_SIZE,160,100));
        assert(!invoke_all(BOS_CALL_RECT,0,0,160,100,11));
        invoke(BOS_CALL_PRESENT,0,0);assert(!"Present must suspend");
    }
    assert(!invoke(BOS_CALL_CANVAS_SIZE,320,200));
    assert(!invoke_all(BOS_CALL_RECT,0,0,320,200,22));
    frame_is(160,100,0);
    if(view_mode!=VIEW_ERROR&&view_mode!=VIEW_PENDING){
        assert(!process_request_stop(current_task->owner_id));
        assert(current_task->stop_requested&&current_task->resources_live);
        assert(!process_binding_live(&current_task->io.binding));
        frame_is(160,100,0); /* Requesting Stop is never publication. */
    }
    /* Same-slice trusted I/O remains valid after a deferred Stop request. */
    assert(!invoke(BOS_CALL_CANVAS_SIZE,320,200));
    assert(!invoke_all(BOS_CALL_RECT,0,0,320,200,22));
    assert(!invoke_all(BOS_CALL_PLOT,7,8,37,0,0));
    const char text[]="Same-slice output survives deferred Stop";
    memcpy(user_memory+1024,text,sizeof text-1);
    assert(invoke(BOS_CALL_WRITE,1024,sizeof text-1)==(int)sizeof text-1);
    assert(!strcmp(last_line[owner_slot],text));
    assert(views[owner_slot].canvas.pixels[8*320+7]==37);
    frame_is(160,100,0);
    switch(view_mode){
    case VIEW_PRESENT:invoke(BOS_CALL_PRESENT,0,0);break;
    case VIEW_YIELD:invoke(BOS_CALL_YIELD,0,0);break;
    case VIEW_SLEEP_ZERO:invoke(BOS_CALL_SLEEP,0,0);break;
    case VIEW_SLEEP:invoke(BOS_CALL_SLEEP,17,0);break;
    case VIEW_SYNC_WAIT:stub_poll=BOS_PENDING;invoke(BOS_CALL_SYNC_WAIT,BOS_HANDLE_TYPE_OPERATION|7,1000);break;
    case VIEW_EXIT_ZERO:invoke(BOS_CALL_EXIT,0,0);break;
    case VIEW_EXIT_NONZERO:invoke(BOS_CALL_EXIT,27,0);break;
    case VIEW_EXIT_STOP_VALUE:invoke(BOS_CALL_EXIT,(unsigned)PROCESS_TASK_STOPPED,0);break;
    case VIEW_TIMER:case VIEW_PENDING:timer_return();break;
    case VIEW_ERROR:finish(-3,PROCESS_EXIT_ERROR);break;
    default:assert(!"Unknown ordinary publication case");
    }
    assert(!"Boundary must return to the kernel");
}
static AppView saved_view;
static unsigned char saved_pixels[APP_CANVAS_PIXELS];
static void reject_old_output(const ProcessIO *io){
    saved_view=views[owner_slot];memcpy(saved_pixels,published_canvases[owner_slot],sizeof saved_pixels);
    unsigned lines=line_count[owner_slot];
    io->print(&io->binding,"Late output");io->plot(&io->binding,1,2,99);
    io->rect(&io->binding,0,0,160,100,99);assert(io->resize(&io->binding,160,100)<0);
    io->present(&io->binding);
    assert(!memcmp(&views[owner_slot],&saved_view,sizeof saved_view));
    assert(!memcmp(published_canvases[owner_slot],saved_pixels,sizeof saved_pixels));
    assert(line_count[owner_slot]==lines);
}
static void scheduler_turn(int owner,int ran){
    unsigned before[PROCESS_TASKS],before_runs=runs;
    memcpy(before,owner_slices,sizeof before);
    AppViewUpdate update=app_view_poll_update();
    assert(update.slot==owner&&runs==before_runs+(ran>=0));
    for(unsigned slot=0;slot<PROCESS_TASKS;slot++)
        assert(owner_slices[slot]==before[slot]+((int)slot==ran));
    if(owner>=0&&ran<0)assert(update.flags&APP_VIEW_LIFECYCLE);
}
static ProcessHandle start_owner(int slot){
    assert(!app_view_start_file(slot,1,7,0,0,APP_VIEW_OUTPUT_TERMINAL));
    return views[slot].binding.process;
}
static void real_view_scheduler(void){
    view_mode=VIEW_SCHEDULER;schedule_next=0;
    ProcessHandle a=start_owner(2),b=start_owner(6);
    scheduler_turn(2,2);scheduler_turn(6,6);scheduler_turn(2,2);
    assert(owner_slices[2]==2&&owner_slices[6]==1);
    assert(app_view_frame(2).pixels[161]==2&&app_view_frame(6).pixels[161]==1);
    /* Retained DONE is consumed before the still-ready owner's next slice. */
    assert(process_request_stop(b)&&process_status(b)==PROCESS_TASK_DONE);
    scheduler_turn(6,-1);assert(process_status(b)==PROCESS_TASK_EMPTY);
    scheduler_turn(2,2);assert(process_status(a)==PROCESS_TASK_READY);

    b=start_owner(6);now=100;owner_action[2]=SCHEDULE_SLEEP;
    scheduler_turn(6,6);scheduler_turn(2,2);
    assert(process_status(a)==PROCESS_TASK_SLEEPING);
    unsigned sleeping_slices=owner_slices[2];scheduler_turn(6,6);
    assert(owner_slices[2]==sleeping_slices);
    assert(process_request_stop(b));ProcessHandle c=start_owner(4);
    /* The pre-slice drain also wins with one sleeping and one ready peer. */
    scheduler_turn(6,-1);scheduler_turn(4,4);
    assert(owner_slices[2]==sleeping_slices&&process_status(a)==PROCESS_TASK_SLEEPING);
    assert(app_view_close(4));assert(process_status(c)==PROCESS_TASK_EMPTY);
    scheduler_turn(-1,-1); /* A sole sleeper does not consume a slice. */

    now+=TIMER_HZ;owner_action[2]=SCHEDULE_WAIT;stub_poll=BOS_PENDING;
    scheduler_turn(2,2);
    assert(process_status(a)==PROCESS_TASK_SLEEPING&&task_lookup(a)->wait_kind==TASK_WAIT_SYNC);
    unsigned waiting_slices=owner_slices[2];
    assert(app_view_frame(2).pixels[161]==waiting_slices); /* Existing SYNC WAIT publication. */
    scheduler_turn(-1,-1);
    b=start_owner(6);scheduler_turn(6,6);
    assert(owner_slices[2]==waiting_slices);
    assert(process_request_stop(b));c=start_owner(4);
    /* A separate wait cannot delay retained DONE or advance ahead of readiness. */
    scheduler_turn(6,-1);scheduler_turn(4,4);
    assert(owner_slices[2]==waiting_slices&&process_status(a)==PROCESS_TASK_SLEEPING);
    stub_poll=BOS_OK;owner_action[2]=SCHEDULE_YIELD;scheduler_turn(2,2);
    assert(process_status(a)==PROCESS_TASK_READY&&owner_slices[2]==waiting_slices+1);
    assert(app_view_frame(2).pixels[161]==owner_slices[2]);
    assert(app_view_close(2)&&app_view_close(4));
    assert(process_status(a)==PROCESS_TASK_EMPTY&&process_status(c)==PROCESS_TASK_EMPTY);
    puts("Real view scheduler: two ready owners advance one slice per turn; retained DONE drains before ready peers beside sleeping/sync-waiting owners; waits remain independent.");
}
int main(int argc,char **argv){
    unsigned ram=argc>1?(unsigned)atoi(argv[1]):64;host_physmem_init(ram);
    ProcessIO previous={0};unsigned generation=0;
    for(view_mode=VIEW_PRESENT;view_mode<=VIEW_PENDING;view_mode++){
        int mode=view_mode;
        assert(!app_view_start_file((int)owner_slot,1,7,0,0,APP_VIEW_OUTPUT_TERMINAL));
        ProcessHandle handle=views[owner_slot].binding.process;NativeTask *task=task_lookup(handle);
        assert(task&&views[owner_slot].binding.generation==++generation);
        ProcessIO io=task->io;
        if(previous.binding.process)reject_old_output(&previous);
        view_mode=VIEW_INITIAL;AppViewUpdate initial=app_view_poll_update();
        assert(initial.slot==(int)owner_slot&&(initial.flags&APP_VIEW_LAYOUT));frame_is(160,100,0);
        unsigned copies=outgoing,released_before=release_events,runs_before=runs;
        view_mode=mode;assert(process_step(handle));
        if(mode==VIEW_PENDING){
            assert(process_status(handle)==PROCESS_TASK_READY);frame_is(160,100,0);
            assert(process_request_stop(handle)); /* Ordinary idle Stop cannot publish. */
        }else assert(outgoing==copies); /* No final user-image copy after deferred Stop/exit. */
        assert(process_status(handle)==PROCESS_TASK_DONE&&release_events==released_before+2);
        assert(!process_binding_live(&io.binding)&&!process_step(handle)&&runs==runs_before+1);
        ProcessResult result;assert(process_get_result(handle,&result));
        if(mode>=VIEW_EXIT_ZERO&&mode<=VIEW_EXIT_STOP_VALUE)
            assert(result.reason==PROCESS_EXIT_APP&&result.value==
                   (mode==VIEW_EXIT_ZERO?0:mode==VIEW_EXIT_NONZERO?27:PROCESS_TASK_STOPPED));
        else if(mode==VIEW_ERROR)assert(result.reason==PROCESS_EXIT_ERROR&&result.value==-3);
        else assert(result.reason==PROCESS_EXIT_STOP&&result.value==PROCESS_TASK_STOPPED);
        int published=mode<=VIEW_EXIT_STOP_VALUE;
        frame_is(published?320:160,published?200:100,published);
        assert(!host_physmem_stats().allocated);reject_old_output(&io); /* Retained DONE. */
        AppViewUpdate update=app_view_poll_update();
        assert(update.slot==(int)owner_slot&&(update.flags&APP_VIEW_LIFECYCLE));
        assert(!views[owner_slot].binding.process&&!views[owner_slot].canvas.pending);
        assert(process_status(handle)==PROCESS_TASK_EMPTY&&runs==runs_before+1);
        frame_is(published?320:160,published?200:100,published);reject_old_output(&io);
        previous=io;
    }
    real_view_scheduler();
    assert(backing_allocations==backing_releases&&!host_physmem_stats().allocated);
    host_physmem_destroy();
    printf("Real process + app view: %u MiB; deferred Stop preserves exact present/yield/sleep/SYNC_WAIT/APP-exit frames and output; UI revokes immediately; timer/error/idle Stop never publish; DONE/reaped/reused output is refused.\n",ram);
    return 0;
}
