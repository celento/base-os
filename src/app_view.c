#include "app_view.h"
#include "app_storage.h"
#include "fs.h"
#include "platform.h"
static AppView *views=((AppStorage *)TERM_MEMORY)->views;
static unsigned char (*published_canvases)[APP_CANVAS_PIXELS]=(void *)NATIVE_CANVAS_MEMORY;
_Static_assert(APP_VIEW_NAME_LEN>=FS_NAME_LEN,"app-view filename buffer too small");
static int valid_slot(int slot){return slot>=0&&slot<PROCESS_TASKS;}
void app_view_mark(int slot,unsigned flags){if(valid_slot(slot))views[slot].task_dirty|=flags;}
/* The only sink in this extraction is a fixed trusted Terminal adapter. Store
 * its small policy value, never a borrowed user/stack pointer or selected slot. */
static void output_write(int slot,AppViewOutput output,const char *text){
    if(output==APP_VIEW_OUTPUT_TERMINAL)term_write_at(slot,text);
}
static void view_write(int slot,const char *text){output_write(slot,views[slot].output,text);}
AppCanvasFrame app_view_frame(int slot){
    if(!valid_slot(slot))return (AppCanvasFrame){0,0,0};
    return app_canvas_frame(&views[slot].canvas,published_canvases[slot]);
}
int app_view_canvas_size(int slot,int *width,int *height){
    if(width)*width=0;
    if(height)*height=0;
    if(!valid_slot(slot)||!width||!height)return 0;
    AppCanvasFrame frame=app_view_frame(slot);
    if(!frame.pixels)return 0;
    *width=frame.width;*height=frame.height;return 1;
}
void app_view_canvas_reset(int slot){
    if(valid_slot(slot))app_view_mark(slot,app_canvas_reset(&views[slot].canvas));
}
void app_view_canvas_unbuffered(int slot){if(valid_slot(slot))views[slot].canvas.buffered=0;}
void app_view_plot(int slot,int x,int y,int color){
    if(valid_slot(slot))app_view_mark(slot,app_canvas_plot(&views[slot].canvas,x,y,color));
}
void app_view_rect(int slot,int x,int y,int width,int height,int color){
    if(valid_slot(slot))app_view_mark(slot,app_canvas_rect(&views[slot].canvas,x,y,width,height,color));
}
int app_view_resize(int slot,int width,int height){
    if(!valid_slot(slot))return -1;
    int dirty=app_canvas_resize(&views[slot].canvas,width,height);
    if(dirty<0)return -1;
    app_view_mark(slot,(unsigned)dirty);return 0;
}
static void view_publish(int slot){
    app_view_mark(slot,app_canvas_publish(&views[slot].canvas,published_canvases[slot]));
}
int app_view_reset(int slot){
    if(!valid_slot(slot))return 0;
    app_storage_init();
    if(!app_view_close(slot))return 0;
    unsigned generation=views[slot].binding.generation;
    kmemset(&views[slot],0,sizeof views[slot]);
    views[slot].binding.slot=(unsigned)slot;views[slot].binding.generation=generation;
    app_view_canvas_reset(slot);return 1;
}
/* Resolve all three fields; callbacks for an old process/binding are discarded.
 * No callback changes selected or dispatches another process. */
static AppView *binding_identity(const ProcessBinding *binding){
    if(!binding||binding->slot>=PROCESS_TASKS||!binding->process||!binding->generation)return 0;
    AppView *t=views+binding->slot;
    return t->binding.process==binding->process&&t->binding.slot==binding->slot&&t->binding.generation==binding->generation?t:0;
}
int app_view_binding_matches(const ProcessBinding *binding){return binding_identity(binding)!=0;}
static AppView *binding_view(const ProcessBinding *binding){
    AppView *v=binding_identity(binding);
    return v&&process_binding_live(binding)?v:0;
}
static void native_print(const ProcessBinding *binding,const char *text){
    AppView *t=binding_view(binding);if(t)view_write((int)binding->slot,text);
}
static void native_plot(const ProcessBinding *binding,int x,int y,int color){
    AppView *t=binding_view(binding);if(t)app_view_plot((int)binding->slot,x,y,color);
}
static void native_present(const ProcessBinding *binding){
    AppView *t=binding_view(binding);if(t)view_publish((int)binding->slot);
}
static int native_resize(const ProcessBinding *binding,int width,int height){
    AppView *t=binding_view(binding);return t?app_view_resize((int)binding->slot,width,height):-1;
}
static void native_rect(const ProcessBinding *binding,int x,int y,int width,int height,int color){
    AppView *t=binding_view(binding);if(t)app_view_rect((int)binding->slot,x,y,width,height,color);
}
int app_view_running(int slot){
    if(slot<0||slot>=PROCESS_TASKS)return 0;
    int state=process_status(views[slot].binding.process);
    return state==PROCESS_TASK_READY||state==PROCESS_TASK_SLEEPING;
}
int app_view_start_file(int slot,int file,unsigned identity,
                                   const char *argument,unsigned argument_length,AppViewOutput output){
    if(slot<0||slot>=PROCESS_TASKS)return -1;
    app_storage_init();
    AppView *v=&views[slot];
    int result=-1;
    if(v->binding.process&&process_status(v->binding.process)==PROCESS_TASK_DONE)app_view_close(slot);
    if(!identity||!fs_valid(file)||fs_is_dir(file)||fs_is_app(file)||fs_identity(file)!=identity)
        output_write(slot,output,"Cannot start: the selected program changed or is no longer a file.");
    else if(v->binding.process)
        output_write(slot,output,"Cannot start: this terminal already has a native task.");
    else if(argument_length>PROCESS_ARGUMENT_MAX||
            (argument_length&&(!argument||argument[0]!='/')))
        output_write(slot,output,"Cannot start: use an absolute document path of at most 128 bytes.");
    else if(v->binding.generation==UINT32_MAX)
        output_write(slot,output,"Cannot start: this terminal's binding identities are exhausted.");
    else {
        ProcessHandle process=0;
        /* No task runs between this identity check and the loader's owned copy. */
        int created=process_create(fs_data(file),fs_size(file),argument,argument_length,&process);
        if(created)
            output_write(slot,output,created==PROCESS_CREATE_MEMORY?"Cannot start: native backing memory is unavailable.":
                 created==-1?"Cannot start: all native process records are in use.":
                 created==PROCESS_CREATE_UNSUPPORTED?"Cannot start: this executable format or ABI is not enabled.":
                 created==PROCESS_CREATE_LAYOUT?"Cannot start: executable exceeds this context's file or memory policy.":
                 "Cannot start: invalid native executable header or layout.");
        else {
            ProcessIO io={{process,(unsigned)slot,v->binding.generation+1},
                          native_print,native_plot,native_present,native_resize,native_rect};
            if(!process_bind(process,&io)){
                process_request_stop(process);process_reap(process);
                output_write(slot,output,"Cannot start: native output attachment is unavailable.");
                return -1;
            }
            /* Commit both sides before it can run; START never invokes output. */
            v->binding=io.binding;v->output=output;
            if(!process_start(process)){
                v->binding.process=0;process_request_stop(process);process_reap(process);
                output_write(slot,output,"Cannot start: native process is unavailable.");
                return -1;
            }
            kstrcpy(v->task_name,fs_name(file));v->task_started=timer_ticks();
            unsigned first=0;
            for(unsigned i=0;i<argument_length;i++)if(argument[i]=='/')first=i+1;
            unsigned n=argument_length-first;
            if(n>=sizeof v->task_document)n=sizeof v->task_document-1;
            if(n)kmemcpy(v->task_document,argument+first,n);
            v->task_document[n]=0;
            v->task_dirty|=APP_VIEW_LIFECYCLE;
            app_view_canvas_reset(slot);v->canvas.buffered=1;
            output_write(slot,output,v->task_name);
            output_write(slot,output,"Native task started. Ctrl+C stops; close ends it.");
            result=0;
        }
    }
    return result;
}
int app_view_info(int slot,AppViewInfo *out){
    if(!out)return 0;
    kmemset(out,0,sizeof *out);
    if(slot<0||slot>=PROCESS_TASKS)return 0;
    int state=process_status(views[slot].binding.process);
    if(state!=PROCESS_TASK_READY&&state!=PROCESS_TASK_SLEEPING)return 0;
    const AppView *t=&views[slot];
    kstrcpy(out->name,t->task_name[0]?t->task_name:"Native task");
    kstrcpy(out->document,t->task_document);
    out->owner=slot;out->state=state;
    out->started_ticks=t->task_started;out->instance=t->binding.process;
    out->elapsed_sec=t->binding.process?(unsigned)(timer_ticks()-t->task_started)/TIMER_HZ:0;
    return 1;
}
int app_view_title(int slot,char *out,int capacity){
    if(!out||capacity<=0)return 0;
    out[0]=0;AppViewInfo info;
    if(!app_view_info(slot,&info))return 0;
    const char *parts[3]={info.name,info.document[0]?" - ":"",info.document};
    int used=0;
    for(int p=0;p<3;p++)for(int i=0;parts[p][i]&&used<capacity-1;i++)out[used++]=parts[p][i];
    out[used]=0;return 1;
}
static void task_metadata_clear(int slot){
    if(slot<0||slot>=PROCESS_TASKS)return;
    if(views[slot].binding.process)views[slot].task_dirty|=APP_VIEW_LIFECYCLE;
    kmemset(views[slot].task_name,0,sizeof views[slot].task_name);
    kmemset(views[slot].task_document,0,sizeof views[slot].task_document);
    views[slot].task_started=views[slot].binding.process=0;
}
int app_view_key(int slot,int key){
    return slot>=0&&slot<PROCESS_TASKS?process_key(views[slot].binding.process,key):0;
}
int app_view_close(int slot){
    if(slot<0||slot>=PROCESS_TASKS)return 1;
    ProcessHandle process=views[slot].binding.process;
    if(process){
        if(!process_request_stop(process))return 0;
        ProcessResult result;
        /* Completion is consumed before record reuse, even for explicit close. */
        process_get_result(process,&result);
        if(!process_reap(process))return 0;
    }
    task_metadata_clear(slot);
    /* Stop/error/close discards unfinished work, retaining the published view. */
    views[slot].canvas.pending=0;return 1;
}
void app_view_stop(int slot){
    if(!app_view_running(slot))return;
    if(!process_request_stop(views[slot].binding.process))return;
    view_write(slot,"Native task stopped.");app_view_close(slot);
}
AppViewUpdate app_view_poll_update(void){
    AppViewUpdate update={-1,0};
    int slot=-1;
    /* Consume retained completions before another slice, so a continuously
     * runnable peer cannot starve a deferred stop's display/reap. */
    for(unsigned i=0;i<PROCESS_TASKS;i++)
        if(views[i].binding.process&&process_status(views[i].binding.process)==PROCESS_TASK_DONE){slot=(int)i;break;}
    if(slot<0){
        ProcessHandle ran=process_schedule_one();
        for(unsigned i=0;i<PROCESS_TASKS;i++)
            if(ran&&views[i].binding.process==ran){slot=(int)i;break;}
        /* A queued stop may complete without running a slice. */
        if(slot<0)for(unsigned i=0;i<PROCESS_TASKS;i++)
            if(views[i].binding.process&&process_status(views[i].binding.process)==PROCESS_TASK_DONE){slot=(int)i;break;}
    }
    if(slot>=0){
        AppView *t=views+slot;ProcessResult result;
        if(process_get_result(t->binding.process,&result)){
            if(result.reason==PROCESS_EXIT_STOP)view_write(slot,"Native task stopped.");
            else if(result.reason==PROCESS_EXIT_APP&&!result.value)view_write(slot,"Native task finished.");
            else view_write(slot,"Native task ended with an error or fault.");
            app_view_close(slot);
        }
        update.slot=slot;update.flags=t->task_dirty;t->task_dirty=0;
    }
    return update;
}
int app_view_poll(void){return app_view_poll_update().flags!=0;}
