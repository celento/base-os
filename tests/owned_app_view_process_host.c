/* Real BEX2 admission, owned pages, dispatch, publication and app-view cleanup.
 * Hardware entry/root mapping use the existing explicit host-only adapters. */
/* The included fixture main relies on C's implicit main return. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#define main private_address_space_fixture_main
#define fs_valid private_fixture_fs_valid
#define fs_is_dir private_fixture_fs_is_dir
#define fs_size private_fixture_fs_size
#define fs_data private_fixture_fs_data
#include "private_address_space_host.c"
#undef fs_data
#undef fs_size
#undef fs_is_dir
#undef fs_valid
#undef main
#pragma GCC diagnostic pop
static unsigned char *view_image;
static unsigned view_image_bytes;
int fs_valid(int id){return id==42;}
int fs_is_dir(int id){(void)id;return 0;}
int fs_size(int id){assert(id==42);return (int)view_image_bytes;}
const char *fs_data(int id){assert(id==42);return (const char *)view_image;}
const char *fs_name(int id){assert(id==42);return "owned-view.bex";}
unsigned fs_identity(int id){return id==42?21:0;}
void kstrcpy(char *to,const char *from){strcpy(to,from);}
static unsigned char view_arena[0xC0000],published_arena[NATIVE_CANVAS_CAPACITY];
#define TERM_MEMORY ((uintptr_t)view_arena)
#define NATIVE_CANVAS_MEMORY ((uintptr_t)published_arena)
#include "../src/app_storage.c"
#include "../src/app_canvas.c"
#include "../src/app_view.c"
void term_write_at(int slot,const char *text){(void)slot;(void)text;assert(!"Owned output must never reach Terminal");}
enum { INITIAL, STOP_PRESENT, STOP_YIELD, STOP_SLEEP, STOP_SYNC, STOP_APP,
       CLOSE_PRESENT, APP_SUCCESS, APP_NONZERO, ERROR_RESULT };
static int mode;
static unsigned selected_slot=4;
static void owned_slice(void){
    assert(current_task&&task_private(current_task));
    assert(current_task->io.binding.slot==selected_slot);
    assert(process_launch_mode(current_task->owner_id)==PROCESS_LAUNCH_OWNED_WINDOW);
    if(mode==INITIAL){
        assert(!invoke(BOS_CALL_PLOT,1,1,11,0,0));invoke(BOS_CALL_PRESENT,0,0,0,0,0);return;
    }
    assert(!invoke(BOS_CALL_PLOT,1,1,22,0,0));
    assert(app_view_frame((int)selected_slot).pixels[161]==11);
    if(mode>=STOP_PRESENT&&mode<=STOP_APP)app_view_stop((int)selected_slot);
    if(mode==CLOSE_PRESENT)assert(!app_view_close((int)selected_slot));
    if(mode>=STOP_PRESENT&&mode<=CLOSE_PRESENT)assert(!process_binding_live(&current_task->io.binding));
    const char row[]="Copied active-slice owned output";
    unsigned address=current_task->plan.stack.offset+128;
    memcpy(user_memory+address,row,sizeof row-1);
    assert(invoke(BOS_CALL_WRITE,address,sizeof row-1,0,0,0)==sizeof row-1);
    assert(!strcmp(app_view_output_line((int)selected_slot,2),row));
    switch(mode){
    case STOP_PRESENT:case CLOSE_PRESENT:invoke(BOS_CALL_PRESENT,0,0,0,0,0);break;
    case STOP_YIELD:invoke(BOS_CALL_YIELD,0,0,0,0,0);break;
    case STOP_SLEEP:invoke(BOS_CALL_SLEEP,1000,0,0,0,0);break;
    case STOP_SYNC:stub_poll=BOS_PENDING;invoke(BOS_CALL_SYNC_WAIT,BOS_HANDLE_TYPE_OPERATION|7,1000,0,0,0);break;
    case STOP_APP:invoke(BOS_CALL_EXIT,(unsigned)-4,0,0,0,0);break;
    case APP_SUCCESS:invoke(BOS_CALL_EXIT,0,0,0,0,0);break;
    case APP_NONZERO:invoke(BOS_CALL_EXIT,27,0,0,0,0);break;
    case ERROR_RESULT:finish(-3,PROCESS_EXIT_ERROR);break;
    default:assert(!"Unknown valid lifecycle action");
    }
    assert(!"Publication/completion returns to inactive kernel context");
}
int main(int argc,char **argv){
    assert(argc==3);unsigned ram=(unsigned)atoi(argv[1]);view_image=load(argv[2],&view_image_bytes);
    host_physmem_init(ram);slice_check=owned_slice;
    ProcessIO old={0};unsigned generations=0;
    for(int action=STOP_PRESENT;action<=ERROR_RESULT;action++){
        assert(!app_view_start_owned_file((int)selected_slot,42,21,0,0));
        ProcessHandle handle=views[selected_slot].binding.process;NativeTask *task=task_lookup(handle);
        assert(task&&task_private(task)&&task->plan.flags==BOS_BEX2_FLAG_NATIVE_WINDOW_V1);
        assert(views[selected_slot].binding.generation==++generations);
        unsigned admitted=task->plan.owned_pages;assert(host_physmem_stats().allocated==admitted&&admitted>2);
        if(old.binding.process){unsigned count=(unsigned)app_view_output_count((int)selected_slot);
            old.print(&old.binding,"Prior incarnation row");assert((unsigned)app_view_output_count((int)selected_slot)==count);}
        old=task->io;mode=INITIAL;
        AppViewUpdate first=app_view_poll_update();assert(first.slot==(int)selected_slot);
        assert(app_view_frame((int)selected_slot).pixels[161]==11);
        unsigned copied=production_image_copies;mode=action;AppViewUpdate update=app_view_poll_update();
        assert(update.slot==(int)selected_slot&&production_image_copies==copied);
        assert(!host_physmem_stats().allocated&&!root_is_user&&enters==restores&&fpu_entries==fpu_returns);
        ProcessCounts counts;process_counts(&counts);assert(!counts.records&&!counts.owned&&!counts.live);
        AppViewResult result;assert(app_view_result((int)selected_slot,&result)&&result.instance==handle);
        assert(!views[selected_slot].binding.process&&!views[selected_slot].canvas.pending&&!process_binding_live(&old.binding));
        assert(app_view_frame((int)selected_slot).pixels[161]==(action==ERROR_RESULT?11:22));
        assert(!!(update.flags&APP_VIEW_AUTO_CLOSE)==(action==CLOSE_PRESENT||action==APP_SUCCESS));
        assert(result.reason==(action==ERROR_RESULT?PROCESS_EXIT_ERROR:
               action==STOP_APP||action==APP_SUCCESS||action==APP_NONZERO?PROCESS_EXIT_APP:PROCESS_EXIT_STOP));
        assert(result.value==(action==ERROR_RESULT?-3:action==APP_SUCCESS?0:action==APP_NONZERO?27:-4));
        unsigned count=(unsigned)app_view_output_count((int)selected_slot);old.print(&old.binding,"Reaped row");
        old.plot(&old.binding,1,1,99);old.present(&old.binding);
        assert((unsigned)app_view_output_count((int)selected_slot)==count);
        assert(app_view_frame((int)selected_slot).pixels[161]==(action==ERROR_RESULT?11:22));
        assert(app_view_close((int)selected_slot));
    }
    host_physmem_destroy();free(view_image);
    printf("Owned BEX2 app views: %u MiB; real mode admission/pages/dispatcher, deferred Stop/Close publication, sync-wait, APP -4, success/nonzero/error results and inactive zero-resource cleanup passed.\n",ram);
    return 0;
}
