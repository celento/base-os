/* Ordinary owned-view lifecycle and finite-resource refusal tests. Process
 * entry/backing are explicit service spies; app-view, canvas and log are real. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "program.h"
#include "platform.h"
#include "layout.h"
static unsigned char arena[0xC0000],published[NATIVE_CANVAS_CAPACITY];
#define TERM_MEMORY ((uintptr_t)arena)
#define NATIVE_CANVAS_MEMORY ((uintptr_t)published)
#include "../src/app_storage.c"
#include "../src/app_canvas.c"
#include "../src/app_view.c"

typedef struct {
    ProcessHandle handle;
    ProcessIO io;
    ProcessResult result;
    unsigned mode;
    int state,resources,stop_requested;
} TestProcess;
static TestProcess records[PROCESS_TASKS];
static unsigned serial,now,creates,slices,reaps,term_lines[PROCESS_TASKS];
static int refuse_create,refuse_bind,refuse_start,active_slot=-1;
static int next_action;
enum { ACTION_PRINT, ACTION_STOP, ACTION_CLOSE, ACTION_APP_STOP_VALUE };
static char filename[24]="owned.bex";
static const char file_bytes[64]="Valid executable fixture bytes";
static TestProcess *find(ProcessHandle handle){
    for(unsigned i=0;i<PROCESS_TASKS;i++)if(handle&&records[i].handle==handle)return records+i;
    return 0;
}
static void finish_process(TestProcess *p,unsigned reason,int value){
    assert(p&&p->state!=PROCESS_TASK_EMPTY);
    p->result=(ProcessResult){value,reason};p->state=PROCESS_TASK_DONE;p->resources=0;
}
void kmemcpy(void *to,const void *from,int bytes){memcpy(to,from,(size_t)bytes);}
void kmemset(void *to,int value,int bytes){memset(to,value,(size_t)bytes);}
void kstrcpy(char *to,const char *from){strcpy(to,from);}
uint32_t timer_ticks(void){return now;}
int fs_valid(int file){return file==1;}
int fs_is_dir(int file){(void)file;return 0;}
int fs_is_app(int file){(void)file;return 0;}
unsigned fs_identity(int file){return file==1?9:0;}
const char *fs_data(int file){assert(file==1);return file_bytes;}
int fs_size(int file){assert(file==1);return sizeof file_bytes;}
const char *fs_name(int file){assert(file==1);return filename;}
void term_write_at(int slot,const char *text){assert(text);term_lines[slot]++;app_view_mark(slot,APP_VIEW_TEXT);}
int process_create_mode(const void *file,unsigned bytes,const char *argument,unsigned length,
                        unsigned mode,ProcessHandle *out){
    assert(file==file_bytes&&bytes==sizeof file_bytes&&out);
    assert(length<=PROCESS_ARGUMENT_MAX&&(!length||(argument&&argument[0]=='/')));
    assert(mode==PROCESS_LAUNCH_HOSTED||mode==PROCESS_LAUNCH_OWNED_WINDOW);creates++;
    if(refuse_create)return refuse_create;
    for(unsigned i=0;i<PROCESS_TASKS;i++)if(!records[i].handle){
        records[i]=(TestProcess){.handle=0x10000000u|++serial,.mode=mode,
                                .state=PROCESS_TASK_CREATED,.resources=1};
        *out=records[i].handle;return 0;
    }
    return -1;
}
int process_create(const void *file,unsigned bytes,const char *argument,unsigned length,ProcessHandle *out){
    return process_create_mode(file,bytes,argument,length,PROCESS_LAUNCH_HOSTED,out);
}
int process_bind(ProcessHandle handle,const ProcessIO *io){
    TestProcess *p=find(handle);assert(p&&p->state==PROCESS_TASK_CREATED);
    if(refuse_bind)return 0;
    p->io=*io;return 1;
}
int process_start(ProcessHandle handle){
    TestProcess *p=find(handle);assert(p&&p->state==PROCESS_TASK_CREATED&&p->io.binding.process==handle);
    if(refuse_start)return 0;
    p->state=PROCESS_TASK_READY;return 1;
}
int process_status(ProcessHandle handle){TestProcess *p=find(handle);return p?p->state:PROCESS_TASK_EMPTY;}
int process_key(ProcessHandle handle,int key){(void)key;TestProcess *p=find(handle);return p&&!p->stop_requested&&p->resources;}
int process_binding_live(const ProcessBinding *binding){
    TestProcess *p=binding?find(binding->process):0;
    return p&&!p->stop_requested&&p->resources&&p->io.binding.slot==binding->slot&&
           p->io.binding.generation==binding->generation;
}
int process_request_stop(ProcessHandle handle){
    TestProcess *p=find(handle);if(!p)return 1;
    p->stop_requested=1;
    if(active_slot>=0&&p->io.binding.slot==(unsigned)active_slot)return 0;
    if(p->state!=PROCESS_TASK_DONE)finish_process(p,PROCESS_EXIT_STOP,PROCESS_TASK_STOPPED);
    return 1;
}
int process_get_result(ProcessHandle handle,ProcessResult *out){
    TestProcess *p=find(handle);if(!p||p->state!=PROCESS_TASK_DONE||!out)return 0;
    *out=p->result;return 1;
}
int process_reap(ProcessHandle handle){
    TestProcess *p=find(handle);if(!p)return 1;
    if(p->state!=PROCESS_TASK_DONE)return 0;
    assert(!p->resources);memset(p,0,sizeof *p);reaps++;return 1;
}
ProcessHandle process_schedule_one(void){
    for(unsigned i=0;i<PROCESS_TASKS;i++)if(records[i].state==PROCESS_TASK_READY){
        TestProcess *p=records+i;ProcessHandle handle=p->handle;ProcessIO *io=&p->io;
        active_slot=(int)io->binding.slot;slices++;
        if(next_action==ACTION_CLOSE)assert(!app_view_close(active_slot));
        else if(next_action==ACTION_STOP||next_action==ACTION_APP_STOP_VALUE)app_view_stop(active_slot);
        if(next_action!=ACTION_PRINT){
            assert(!process_binding_live(&io->binding));
            /* A deferred stop revokes UI, not already-active trusted output. */
            io->plot(&io->binding,3,4,41);io->present(&io->binding);
        }
        io->print(&io->binding,"One ordinary active slice.");
        active_slot=-1;
        if(next_action==ACTION_APP_STOP_VALUE)finish_process(p,PROCESS_EXIT_APP,PROCESS_TASK_STOPPED);
        else if(p->stop_requested)finish_process(p,PROCESS_EXIT_STOP,PROCESS_TASK_STOPPED);
        next_action=ACTION_PRINT;return handle;
    }
    return 0;
}
static unsigned record_count(void){unsigned count=0;for(unsigned i=0;i<PROCESS_TASKS;i++)count+=records[i].handle!=0;return count;}
static AppStorage saved_storage;
static unsigned char saved_published[NATIVE_CANVAS_CAPACITY];
static void save_storage(void){memcpy(&saved_storage,arena,sizeof saved_storage);memcpy(saved_published,published,sizeof published);}
static void unchanged_storage(void){assert(!memcmp(&saved_storage,arena,sizeof saved_storage));assert(!memcmp(saved_published,published,sizeof published));}
static ProcessIO start_owned(int slot){
    assert(!app_view_start_owned_file(slot,1,9,0,0));
    TestProcess *p=find(views[slot].binding.process);assert(p&&p->mode==PROCESS_LAUNCH_OWNED_WINDOW);
    return p->io;
}
static void reject_callbacks(const ProcessIO *io){
    save_storage();io->print(&io->binding,"Late row");io->plot(&io->binding,1,1,99);
    io->rect(&io->binding,0,0,160,100,99);assert(io->resize(&io->binding,320,200)<0);
    io->present(&io->binding);unchanged_storage();
}
static void transactional_launch(void){
    app_storage_init();assert(app_view_reset(4));
    AppStorage *storage=(AppStorage *)arena;
    strcpy(storage->terminals[4].input,"preserve invoking draft");storage->terminals[4].len=23;
    app_view_plot(4,7,8,13);save_storage();unsigned before=creates;
    assert(app_view_start_owned_file(4,1,8,0,0)==APP_VIEW_START_INVALID);unchanged_storage();
    assert(app_view_start_owned_file(4,1,9,"relative",8)==APP_VIEW_START_ARGUMENT);unchanged_storage();
    assert(creates==before);
    const int errors[]={-1,PROCESS_CREATE_MEMORY,PROCESS_CREATE_UNSUPPORTED,PROCESS_CREATE_LAYOUT,-2};
    for(unsigned i=0;i<sizeof errors/sizeof *errors;i++){
        refuse_create=errors[i];assert(app_view_start_owned_file(4,1,9,0,0)==errors[i]);
        unchanged_storage();assert(!record_count());
    }
    refuse_create=0;refuse_bind=1;
    assert(app_view_start_owned_file(4,1,9,0,0)==APP_VIEW_START_ATTACHMENT);unchanged_storage();assert(!record_count());
    refuse_bind=0;refuse_start=1;
    assert(app_view_start_owned_file(4,1,9,0,0)==APP_VIEW_START_UNAVAILABLE);unchanged_storage();assert(!record_count());
    refuse_start=0;views[4].binding.generation=UINT32_MAX;save_storage();before=creates;
    assert(app_view_start_owned_file(4,1,9,0,0)==APP_VIEW_START_GENERATION);unchanged_storage();assert(creates==before);
    views[4].binding.generation=2;
    char argument[]="/Documents/draft.txt";
    assert(!app_view_start_owned_file(4,1,9,argument,sizeof argument-1));
    assert(views[4].binding.generation==3&&views[4].executable_identity==9);
    assert(!strcmp(storage->terminals[4].input,"preserve invoking draft")&&!term_lines[4]);
    memset(argument,'X',sizeof argument);strcpy(filename,"renamed.bex");
    char title[APP_VIEW_TITLE_LEN];assert(app_view_title(4,title,sizeof title));assert(!strcmp(title,"owned.bex - draft.txt"));
    save_storage();before=creates;
    assert(app_view_start_owned_file(4,1,9,0,0)==APP_VIEW_START_BUSY);unchanged_storage();assert(creates==before);
    assert(app_view_close(4));assert(!record_count()&&!app_view_frame(4).pixels);
    assert(views[4].binding.generation==3&&app_view_backend(4)==APP_VIEW_BACKEND_NONE);
    assert(!strcmp(storage->terminals[4].input,"preserve invoking draft"));
    strcpy(filename,"owned.bex");
}
static void bounded_output_and_stop(void){
    assert(!app_view_start_file(1,1,9,0,0,APP_VIEW_OUTPUT_TERMINAL));
    ProcessIO hosted=find(views[1].binding.process)->io;
    ProcessIO own=start_owned(4);unsigned terminal_before=term_lines[1];
    assert(app_view_owned_binding(&own.binding)&&!app_view_owned_binding(&hosted.binding));
    AppView other=views[1];
    own.plot(&own.binding,2,3,17);own.present(&own.binding);
    own.plot(&own.binding,2,3,19);views[4].task_dirty=0;
    for(int i=0;i<52;i++){char row[81];snprintf(row,sizeof row,"row %d",i);own.print(&own.binding,row);}
    assert(app_view_output_count(4)==48&&app_view_output_dropped(4)==6);
    assert(!strcmp(app_view_output_line(4,0),"row 4")&&!strcmp(app_view_output_line(4,47),"row 51"));
    assert(!strcmp(app_view_output_line(4,48),"")&&!strcmp(app_view_output_line(-1,0),""));
    assert(views[4].task_dirty==APP_VIEW_OUTPUT&&!app_view_output_visible(4));
    assert(!memcmp(&other,views+1,sizeof other)&&term_lines[1]==terminal_before&&!term_lines[4]);
    logs[4].dropped=UINT32_MAX;own.print(&own.binding,"");assert(app_view_output_dropped(4)==UINT32_MAX);
    app_view_output_show(4,1);assert(app_view_output_visible(4)&&(views[4].task_dirty&APP_VIEW_LAYOUT));
    app_view_output_show(4,0);assert(!app_view_output_visible(4)&&app_view_frame(4).pixels[3*160+2]==17);
    now+=3*TIMER_HZ;app_view_stop(4);AppViewResult result;
    assert(app_view_result(4,&result)&&result.instance==own.binding.process&&result.reason==PROCESS_EXIT_STOP&&result.value==-4&&result.elapsed_sec==3);
    assert(record_count()==1&&!app_view_running(4)&&!views[4].binding.process&&!views[4].canvas.pending);
    assert(app_view_frame(4).pixels[3*160+2]==17&&!app_view_output_visible(4));
    assert(strstr(app_view_output_line(4,47),"STOP (3), value -4"));
    char title[APP_VIEW_TITLE_LEN];assert(app_view_title(4,title,sizeof title)&&strstr(title,"[Stopped]"));
    AppViewInfo info;assert(!app_view_info(4,&info)&&!info.instance);reject_callbacks(&own);
    unsigned before=slices;AppViewUpdate update=app_view_poll_update();
    assert(update.slot==4&&(update.flags&APP_VIEW_LIFECYCLE)&&!(update.flags&APP_VIEW_AUTO_CLOSE)&&slices==before);
    assert(app_view_close(4)&&!app_view_output_count(4)&&!app_view_output_dropped(4));
    ProcessIO replacement=start_owned(4);assert(replacement.binding.generation==own.binding.generation+1);reject_callbacks(&own);
    assert(app_view_close(4)&&app_view_close(1)&&!record_count());
}
static void result_lifetimes(void){
    const unsigned reasons[]={PROCESS_EXIT_APP,PROCESS_EXIT_APP,PROCESS_EXIT_ERROR,PROCESS_EXIT_STOP};
    const int values[]={0,-4,-3,-4};
    for(unsigned n=0;n<4;n++){
        ProcessIO own=start_owned(4);TestProcess *p=find(own.binding.process);
        finish_process(p,reasons[n],values[n]);reject_callbacks(&own);
        unsigned before=slices;AppViewUpdate update=app_view_poll_update();AppViewResult result;
        assert(update.slot==4&&slices==before&&app_view_result(4,&result));
        assert(result.reason==reasons[n]&&result.value==values[n]&&!record_count());
        assert(!!(update.flags&APP_VIEW_AUTO_CLOSE)==(n==0));
        assert(app_view_output_visible(4)==(n!=0));
        assert(!views[4].binding.process&&!app_view_running(4));reject_callbacks(&own);
        assert(app_view_close(4)&&!app_view_result(4,&result));
    }
    for(int action=ACTION_STOP;action<=ACTION_APP_STOP_VALUE;action++){
        ProcessIO own=start_owned(4);next_action=action;AppViewUpdate update=app_view_poll_update();
        AppViewResult result;assert(app_view_result(4,&result)&&!record_count());
        assert(app_view_frame(4).pixels[4*160+3]==41);assert(app_view_output_count(4)==4);
        assert(!strcmp(app_view_output_line(4,2),"One ordinary active slice."));
        assert(!!(update.flags&APP_VIEW_AUTO_CLOSE)==(action==ACTION_CLOSE));
        assert(result.reason==(action==ACTION_APP_STOP_VALUE?PROCESS_EXIT_APP:PROCESS_EXIT_STOP)&&result.value==-4);
        reject_callbacks(&own);assert(app_view_close(4));
    }
}
int main(void){
    transactional_launch();bounded_output_and_stop();result_lifetimes();
    assert(!record_count());
    printf("Owned app views: transactional refusal, copied titles, 48-row bounded log, retained results, single-consumer deferred Stop/Close, stale callbacks and generation reuse passed; %u reaps.\n",reaps);
    return 0;
}
