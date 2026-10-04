/* Real kernel factory/preflight and real view storage. Process execution and
 * WM geometry/focus are explicit bounded spies; no guest code is executed. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "platform.h"
#include "program.h"
#include "../sdk/baseos_abi.h"
static unsigned char arena[0xC0000],published[NATIVE_CANVAS_CAPACITY];
#define TERM_MEMORY ((uintptr_t)arena)
#define NATIVE_CANVAS_MEMORY ((uintptr_t)published)
#include "../src/app_storage.c"
#include "../src/app_canvas.c"
#include "../src/app_view.c"
#define TASK_BACKING_PAGES 16u

static unsigned char gui_file[8192];
static const unsigned char hosted_file[]={0x42,0x45,0x58,0x31,16,0,0,0,18,0,0,0,0,0,0,0,0xeb,0xfe};
void kmemcpy(void *out,const void *in,int bytes){memcpy(out,in,(size_t)bytes);}
void kmemset(void *out,int value,int bytes){memset(out,value,(size_t)bytes);}
void kstrcpy(char *out,const char *in){strcpy(out,in);}
uint32_t timer_ticks(void){return 77;}
int fs_valid(int id){return id==1||id==2;}
int fs_is_dir(int id){return id==0;}
int fs_is_app(int id){(void)id;return 0;}
unsigned fs_identity(int id){return fs_valid(id)?(unsigned)id+8:0;}
const char *fs_data(int id){assert(fs_valid(id));return (const char *)(id==1?gui_file:hosted_file);}
int fs_size(int id){assert(fs_valid(id));return id==1?sizeof gui_file:sizeof hosted_file;}
const char *fs_name(int id){assert(fs_valid(id));return id==1?"window.bex":"hosted.bex";}
int fs_parent(int id){assert(fs_valid(id));return 0;}
unsigned fs_file_limit(void){return BOS_BEX2_FILE_MAX;}
#include "native_window_probe.inc"

#define MAX_WIN 8
enum { WK_TERM=1,WK_NATIVE=2 };
typedef struct {int x,y,w,h,open,kind,seq;} Win;
static Win wins[MAX_WIN];
static unsigned window_state[MAX_WIN];
static int native_output_scroll[MAX_WIN],wm_seq,wm_z,context_slot=7,selected=7;
static int dirty,open_dlg,input_available=1,focused,terminal_windows,terminal_starts,terminal_lines;
static const char *native_launch_status="";
static int native_ui_available(void){return input_available;}
static void layout_window(int kind,int *x,int *y,int *w,int *h){
    assert(kind==WK_NATIVE);*x=40;*y=50;*w=360;*h=240;
}
static void win_clamp(Win *w){assert(w->w==360&&w->h==240);}
static void win_focus(int slot){assert(wins[slot].open);context_slot=slot;selected=slot;focused++;wm_z++;}
static int win_open(int kind){assert(kind==WK_TERM);terminal_windows++;return 7;}
void term_set_cwd(int id){assert(id==0);}
int term_task_start_file(int slot,int id,unsigned identity){
    assert(slot==7&&id==2&&identity==10);terminal_starts++;return 0;
}
int term_task_start_file_with_arg(int slot,int id,unsigned identity,const char *argument,unsigned length){
    assert(slot==7&&id==2&&identity==10&&argument&&length);terminal_starts++;return 0;
}
void term_write_at(int slot,const char *text){assert(slot>=0&&slot<8&&text);terminal_lines++;}

typedef struct {ProcessHandle handle;ProcessIO io;ProcessResult result;int state;} Record;
static Record records[MAX_WIN];
static unsigned serial,created,reaped;
static int refuse_create,refuse_bind,refuse_start;
static Record *record(ProcessHandle handle){
    for(unsigned i=0;i<MAX_WIN;i++)if(handle&&records[i].handle==handle)return records+i;
    return 0;
}
static unsigned record_count(void){unsigned n=0;for(unsigned i=0;i<MAX_WIN;i++)n+=records[i].handle!=0;return n;}
int process_create_mode(const void *file,unsigned bytes,const char *argument,unsigned length,
                        unsigned mode,ProcessHandle *out){
    unsigned actual=0;int validation=process_probe_launch(file,bytes,&actual);
    if(validation)return validation;
    assert(actual==mode&&mode==PROCESS_LAUNCH_OWNED_WINDOW&&out);
    assert(!length||(argument&&argument[0]=='/'));
    created++;
    if(refuse_create)return refuse_create;
    for(unsigned i=0;i<MAX_WIN;i++)if(!records[i].handle){
        records[i]=(Record){.handle=0x10000000u|++serial,.state=PROCESS_TASK_CREATED};
        *out=records[i].handle;return 0;
    }
    return -1;
}
int process_create(const void *file,unsigned bytes,const char *argument,unsigned length,ProcessHandle *out){
    (void)file;(void)bytes;(void)argument;(void)length;(void)out;assert(!"Factory must not create a hosted process");return -1;
}
int process_bind(ProcessHandle handle,const ProcessIO *io){
    Record *p=record(handle);assert(p&&p->state==PROCESS_TASK_CREATED);
    if(refuse_bind)return 0;
    p->io=*io;return 1;
}
int process_start(ProcessHandle handle){
    Record *p=record(handle);assert(p&&p->state==PROCESS_TASK_CREATED);
    assert(!wins[p->io.binding.slot].open); /* Never expose a half-started task. */
    if(refuse_start)return 0;
    p->state=PROCESS_TASK_READY;return 1;
}
int process_status(ProcessHandle handle){Record *p=record(handle);return p?p->state:PROCESS_TASK_EMPTY;}
int process_key(ProcessHandle handle,int key){(void)key;return record(handle)!=0;}
int process_request_stop(ProcessHandle handle){
    Record *p=record(handle);if(p){p->state=PROCESS_TASK_DONE;p->result=(ProcessResult){PROCESS_TASK_STOPPED,PROCESS_EXIT_STOP};}return 1;
}
int process_get_result(ProcessHandle handle,ProcessResult *out){
    Record *p=record(handle);if(!p||p->state!=PROCESS_TASK_DONE)return 0;*out=p->result;return 1;
}
int process_reap(ProcessHandle handle){Record *p=record(handle);if(p){assert(p->state==PROCESS_TASK_DONE);memset(p,0,sizeof *p);reaped++;}return 1;}
ProcessHandle process_schedule_one(void){assert(!"Factory must not schedule while reserving or starting");return 0;}
#include "native_window_factory.inc"

static AppStorage saved;
static unsigned char saved_published[sizeof published];
static Win saved_wins[MAX_WIN];
static Record saved_records[MAX_WIN];
static unsigned saved_window_state[MAX_WIN];
static int saved_scroll[MAX_WIN],saved_context,saved_selected,saved_seq,saved_z,saved_focus;
static int saved_terminal_windows,saved_terminal_starts,saved_terminal_lines;
static void save(void){
    memcpy(&saved,arena,sizeof saved);memcpy(saved_published,published,sizeof published);
    memcpy(saved_wins,wins,sizeof wins);memcpy(saved_window_state,window_state,sizeof window_state);
    memcpy(saved_records,records,sizeof records);
    memcpy(saved_scroll,native_output_scroll,sizeof saved_scroll);
    saved_context=context_slot;saved_selected=selected;saved_seq=wm_seq;saved_z=wm_z;saved_focus=focused;
    saved_terminal_windows=terminal_windows;saved_terminal_starts=terminal_starts;saved_terminal_lines=terminal_lines;
}
static void unchanged(void){
    assert(!memcmp(&saved,arena,sizeof saved)&&!memcmp(saved_published,published,sizeof published));
    assert(!memcmp(saved_wins,wins,sizeof wins)&&!memcmp(saved_window_state,window_state,sizeof window_state));
    assert(!memcmp(saved_scroll,native_output_scroll,sizeof saved_scroll));
    assert(context_slot==saved_context&&selected==saved_selected&&wm_seq==saved_seq&&wm_z==saved_z&&focused==saved_focus);
    assert(!memcmp(saved_records,records,sizeof records));
    assert(terminal_windows==saved_terminal_windows&&terminal_starts==saved_terminal_starts&&terminal_lines==saved_terminal_lines);
}
static void valid_gui(void){
    memset(gui_file,0,sizeof gui_file);
    BosBex2Header header={BOS_BEX2_MAGIC,64,1,BOS_BEX2_FLAG_NATIVE_WINDOW_V1,
        sizeof gui_file,4096,2,8192,0,0,0,16384,1,2,0,0};
    memcpy(gui_file,&header,sizeof header);gui_file[4096]=0xeb;gui_file[4097]=0xfe;
}
int main(void){
    app_storage_init();valid_gui();
    AppStorage *storage=(AppStorage *)arena;
    for(unsigned i=0;i<MAX_WIN;i++){
        snprintf(storage->terminals[i].input,sizeof storage->terminals[i].input,"Preserve Terminal %u draft",i);
        window_state[i]=0x12340000u+i;native_output_scroll[i]=(int)i+5;
    }
    /* Production allocation-free preflight rejects before touching WM/views. */
    const unsigned offsets[]={0,12,12,52,4},values[]={0,2,3,1,60};
    for(unsigned i=0;i<sizeof offsets/sizeof *offsets;i++){
        valid_gui();memcpy(gui_file+offsets[i],values+i,4);save();unsigned before=created;
        open_native_file(1);assert(native_launch_status[0]&&created==before);unchanged();
    }
    valid_gui();save();unsigned mode=99;
    assert(native_file_mode(1,10,&mode)==APP_VIEW_START_INVALID&&mode==99);unchanged();
    input_available=0;save();open_native_file(1);unchanged();assert(strstr(native_launch_status,"input is unavailable"));
    input_available=1;
    for(unsigned i=0;i<MAX_WIN;i++)wins[i]=(Win){.open=1,.kind=WK_TERM,.seq=(int)i+1};
    save();open_native_file(1);unchanged();assert(strstr(native_launch_status,"close a window"));
    memset(wins,0,sizeof wins);
    for(unsigned which=0;which<2;which++){
        if(which)wm_z=0x7fffffff;else wm_seq=0x7fffffff;
        save();open_native_file(1);unchanged();assert(strstr(native_launch_status,"identities are exhausted"));
        wm_seq=wm_z=0;
    }
    const int failures[]={-1,PROCESS_CREATE_MEMORY,PROCESS_CREATE_UNSUPPORTED,PROCESS_CREATE_LAYOUT,-2};
    for(unsigned i=0;i<sizeof failures/sizeof *failures;i++){
        refuse_create=failures[i];save();open_native_file(1);unchanged();assert(native_launch_status[0]);
    }
    refuse_create=0;refuse_bind=1;save();open_native_file(1);unchanged();assert(reaped==1);
    refuse_bind=0;refuse_start=1;save();open_native_file(1);unchanged();assert(reaped==2);
    refuse_start=0;
    views[0].binding.generation=UINT32_MAX;save();open_native_file(1);unchanged();
    assert(strstr(native_launch_status,"binding identities"));views[0].binding.generation=0;
    save();assert(native_window_start(1,9,"relative",8)==APP_VIEW_START_ARGUMENT);unchanged();
    /* Success publishes exactly one independent WM window after START. */
    TerminalText terminals[MAX_WIN];memcpy(terminals,storage->terminals,sizeof terminals);
    open_dlg=1;open_native_file(1);
    assert(wins[0].open&&wins[0].kind==WK_NATIVE&&!wins[1].open&&record_count()==1);
    assert(!open_dlg&&!native_launch_status[0]&&focused==1&&context_slot==0);
    assert(app_view_backend(0)==APP_VIEW_BACKEND_OWNED&&!terminal_windows&&!terminal_starts&&!terminal_lines);
    assert(!memcmp(terminals,storage->terminals,sizeof terminals));
    ProcessBinding first=views[0].binding;
    /* A failed second admission cannot disturb the already-live first owner. */
    refuse_create=PROCESS_CREATE_MEMORY;save();open_native_file(1);unchanged();refuse_create=0;
    refuse_bind=1;save();open_native_file(1);unchanged();refuse_bind=0;
    refuse_start=1;save();open_native_file(1);unchanged();refuse_start=0;
    assert(record_count()==1&&views[0].binding.process==first.process&&focused==1);
    assert(!terminal_native_launch(7,1,9,"/Documents/draft.txt",20));
    assert(wins[1].open&&wins[1].kind==WK_NATIVE&&record_count()==2&&focused==2);
    assert(views[1].binding.process!=first.process&&!strcmp(views[1].task_document,"draft.txt"));
    assert(!memcmp(terminals,storage->terminals,sizeof terminals));
    assert(app_view_close(0));wins[0].open=0;open_native_file(1);
    assert(wins[0].open&&record_count()==2&&views[0].binding.process!=first.process);
    assert(views[0].binding.generation>first.generation&&focused==3);
    assert(!memcmp(terminals,storage->terminals,sizeof terminals));
    assert(!terminal_windows&&!terminal_starts&&!terminal_lines);
    /* Unflagged BEX1 still selects the old Files and Terminal routes. */
    open_native_file(2);assert(terminal_windows==1&&terminal_starts==1);
    assert(!terminal_native_launch(7,2,10,"/Documents/draft.txt",20)&&terminal_starts==2);
    puts("Native window factory: real preflight, invisible refusal rollback, bounded capacity, one/two owned windows, preserved Terminal bytes and fresh slot reuse passed.");
}
