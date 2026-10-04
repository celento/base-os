/* Valid native-task lifecycle and metadata with the real Terminal/filesystem. */
#define main filesystem_fixture_main
#include "fs_host.c"
#undef main
static unsigned char terminal_arena[0x100000];
static unsigned char published_arena[NATIVE_CANVAS_CAPACITY];
#define NATIVE_CANVAS_MEMORY ((uintptr_t)published_arena)
#define TERM_MEMORY ((uintptr_t)terminal_arena)
#include "../src/term.c"
#include "../sdk/baseos_abi.h"
/* Exact handles resolve records first. Slot-indexed views keep the Terminal
 * assertions readable without making a display slot the process identity. */
static int states[PROCESS_TASKS],exit_next[PROCESS_TASKS],steps[PROCESS_TASKS];
static int stop_deferred[PROCESS_TASKS];
static ProcessIO callbacks[PROCESS_TASKS];
static char arguments[PROCESS_TASKS][PROCESS_ARGUMENT_MAX+1];
typedef struct {
    ProcessHandle handle;
    int slot,state;
    ProcessResult result;
    char argument[PROCESS_ARGUMENT_MAX+1];
} FixtureProcess;
static FixtureProcess fixture_processes[PROCESS_TASKS];
static ProcessHandle next_handle=0x100;
static unsigned schedule_cursor;
static int create_result,exec_result;
static unsigned exec_calls;
static FixtureProcess *fixture_process(ProcessHandle handle){
    for(unsigned i=0;i<PROCESS_TASKS;i++)
        if(handle&&fixture_processes[i].handle==handle)return &fixture_processes[i];
    return 0;
}
static int fixture_state(const FixtureProcess *p){return p->slot>=0?states[p->slot]:p->state;}
static void fixture_set_state(FixtureProcess *p,int state){
    p->state=state;if(p->slot>=0)states[p->slot]=state;
}
int basic_run(const char *s,int n,const ProgramIO *io){(void)s;(void)n;(void)io;return 0;}
int program_key(void){return 0;}
void program_present(void){}
int process_run(const void *p,unsigned n,const ProgramIO *io){(void)p;(void)n;(void)io;exec_calls++;return exec_result;}
int process_create(const void *file,unsigned bytes,const char *argument,unsigned length,ProcessHandle *out){
    assert(file&&bytes>=16&&out);
    if(create_result)return create_result;
    assert(length<=PROCESS_ARGUMENT_MAX&&(!length||(argument&&argument[0]=='/')));
    for(unsigned i=0;i<PROCESS_TASKS;i++)if(!fixture_processes[i].handle){
        FixtureProcess *p=&fixture_processes[i];memset(p,0,sizeof *p);
        p->handle=BOS_HANDLE_TYPE_PROCESS|++next_handle;p->slot=-1;p->state=PROCESS_TASK_CREATED;
        if(length)memcpy(p->argument,argument,length);
        p->argument[length]=0;*out=p->handle;return 0;
    }
    return -1;
}
int process_bind(ProcessHandle handle,const ProcessIO *io){
    FixtureProcess *p=fixture_process(handle);
    if(!p||fixture_state(p)!=PROCESS_TASK_CREATED||p->slot>=0||!io||!io->print||!io->plot||
       io->binding.process!=handle||io->binding.slot>=PROCESS_TASKS||!io->binding.generation)return 0;
    int slot=(int)io->binding.slot;
    for(unsigned i=0;i<PROCESS_TASKS;i++)
        if(fixture_processes[i].handle&&fixture_processes[i].slot==slot)return 0;
    p->slot=slot;callbacks[slot]=*io;strcpy(arguments[slot],p->argument);
    states[slot]=p->state;return 1;
}
int process_unbind(ProcessHandle handle){
    FixtureProcess *p=fixture_process(handle);
    if(!p||fixture_state(p)!=PROCESS_TASK_CREATED)return 0;
    if(p->slot>=0)states[p->slot]=PROCESS_TASK_EMPTY;
    p->slot=-1;return 1;
}
int process_start(ProcessHandle handle){
    FixtureProcess *p=fixture_process(handle);
    if(!p||fixture_state(p)!=PROCESS_TASK_CREATED||p->slot<0)return 0;
    fixture_set_state(p,PROCESS_TASK_READY);return 1;
}
int process_step(ProcessHandle handle){
    FixtureProcess *p=fixture_process(handle);
    if(!p||fixture_state(p)!=PROCESS_TASK_READY)return 0;
    int owner=p->slot;
    if(owner<0)return 1;
    steps[owner]++;
    if(exit_next[owner]){
        exit_next[owner]=0;p->result=(ProcessResult){0,PROCESS_EXIT_APP};
        fixture_set_state(p,PROCESS_TASK_DONE);
    }else {
#ifdef TERM_TASK_STEP_HOOK
        TERM_TASK_STEP_HOOK(owner);
#else
        callbacks[owner].print(&callbacks[owner].binding,"Task progress.");
#endif
    }
    return 1;
}
ProcessHandle process_schedule_one(void){
    for(unsigned n=0;n<PROCESS_TASKS;n++){
        unsigned i=(schedule_cursor+n)%PROCESS_TASKS;
        FixtureProcess *p=&fixture_processes[i];
        if(p->handle&&process_step(p->handle)){
            schedule_cursor=(i+1)%PROCESS_TASKS;return p->handle;
        }
    }
    return 0;
}
int process_status(ProcessHandle handle){
    FixtureProcess *p=fixture_process(handle);return p?fixture_state(p):PROCESS_TASK_EMPTY;
}
int process_get_result(ProcessHandle handle,ProcessResult *out){
    FixtureProcess *p=fixture_process(handle);
    if(!p||fixture_state(p)!=PROCESS_TASK_DONE||!out)return 0;
    *out=p->result;return 1;
}
int process_key(ProcessHandle handle,int key){
    (void)key;int state=process_status(handle);
    return state==PROCESS_TASK_READY||state==PROCESS_TASK_SLEEPING;
}
int process_request_stop(ProcessHandle handle){
    FixtureProcess *p=fixture_process(handle);
    if(!p)return 1;
    if(p->slot>=0&&stop_deferred[p->slot])return 0;
    if(fixture_state(p)!=PROCESS_TASK_DONE){
        p->result=(ProcessResult){PROCESS_TASK_STOPPED,PROCESS_EXIT_STOP};
        fixture_set_state(p,PROCESS_TASK_DONE);
    }
    return 1;
}
int process_reap(ProcessHandle handle){
    FixtureProcess *p=fixture_process(handle);
    if(!p)return 1;
    if(fixture_state(p)!=PROCESS_TASK_DONE)return 0;
    if(p->slot>=0)states[p->slot]=PROCESS_TASK_EMPTY;
    memset(p,0,sizeof *p);return 1;
}
void process_counts(ProcessCounts *out){
    if(!out)return;
    memset(out,0,sizeof *out);
    for(unsigned i=0;i<PROCESS_TASKS;i++)if(fixture_processes[i].handle){
        int state=fixture_state(&fixture_processes[i]);out->records++;
        if(state==PROCESS_TASK_CREATED)out->created++;
        if(state==PROCESS_TASK_READY||state==PROCESS_TASK_SLEEPING)out->live++;
        if(state==PROCESS_TASK_EXITING)out->exiting++;
        if(state==PROCESS_TASK_DONE)out->done++;
        if(state!=PROCESS_TASK_DONE)out->owned++;
    }
}
static void command(const char *s){while(*s)term_char(*s++);term_enter();}
static int executable(const char *name){
    /* A complete BEX1 file is used. Process execution itself has separate tests. */
    const unsigned char bex[]={0x42,0x45,0x58,0x31,16,0,0,0,18,0,0,0,0,0,0,0,0xeb,0xfe};
    int id=fs_create(fs_root(),name);assert(id>=0);
    assert(fs_write(id,(const char *)bex,sizeof bex)==sizeof bex);return id;
}
#ifndef TERM_TASK_FIXTURE_ONLY
int main(void){
    reset();int first=executable("counter.bex");executable("another.bex");
    term_select(0);term_reset();exec_result=-5;command("exec /counter.bex");
    assert(exec_calls==1&&!strcmp(term_get(term_count()-2),"Program stopped (error, fault, or execution limit)."));
    exec_result=0;
    unsigned char extended[8192]={0};
    BosBex2Header header={BOS_BEX2_MAGIC,64,1,0,8192,4096,2,8192,0,0,0,65536,1,1,0,0};
    memcpy(extended,&header,sizeof header);extended[4096]=0xeb;extended[4097]=0xfe;
    int second_format=fs_create(fs_root(),"extended.bex");assert(second_format>=0);
    assert(fs_write(second_format,(const char *)extended,sizeof extended)==sizeof extended);
    command("exec /extended.bex");
    assert(exec_calls==1&&!strcmp(term_get(term_count()-2),"Legacy exec supports BEX1 only; use start for this program."));
    term_select(0);term_reset();canvas_resize(320,200);plot(17,23,9);
    unsigned generation=terms[0].binding_generation;
    create_result=PROCESS_CREATE_MEMORY;command("start /counter.bex");
    assert(!term_task_running(0)&&!terms[0].process&&terms[0].binding_generation==generation);
    assert(term_canvas_width()==320&&term_canvas_height()==200&&term_canvas()[23*320+17]==9);
    assert(!strcmp(term_get(term_count()-2),"Cannot start: native backing memory is unavailable."));
    create_result=0;
    term_select(0);term_reset();now=70;command("start /counter.bex");
    TermTaskInfo info,copy;assert(term_task_info(0,&info));copy=info;
    assert(!strcmp(info.name,"counter.bex")&&info.owner==0&&info.state==PROCESS_TASK_READY);
    assert(info.started_ticks==70&&info.elapsed_sec==0&&info.instance);
    term_select(1);term_reset();command("start /another.bex");
    unsigned second_instance;assert(term_task_info(1,&info));second_instance=info.instance;
    assert(second_instance!=copy.instance);
    term_char('x');int selected_before=selected,count_before=term_count();
    now+=140;assert(term_task_info(0,&info)&&info.elapsed_sec==2);
    assert(selected==selected_before&&!strcmp(term_input(),"x")&&term_count()==count_before);
    states[0]=PROCESS_TASK_SLEEPING;assert(term_task_info(0,&info)&&info.state==PROCESS_TASK_SLEEPING);
    assert(!fs_rename(first,"renamed.bex"));assert(term_task_info(0,&info)&&!strcmp(info.name,"counter.bex"));
    assert(!fs_delete(first));assert(term_task_info(0,&info)&&!strcmp(info.name,"counter.bex"));
    /* An ordinary second start on a busy terminal is rejected without new metadata. */
    term_select(0);command("start /another.bex");assert(term_task_info(0,&info));
    assert(info.instance==copy.instance&&info.started_ticks==copy.started_ticks&&!strcmp(info.name,"counter.bex"));
    term_select(1);term_task_stop(0);
    assert(selected==1&&!strcmp(term_input(),"x")&&term_task_running(1)&&!term_task_running(0));
    memset(&info,0xa5,sizeof info);assert(!term_task_info(0,&info));
    TermTaskInfo zero={0};assert(!memcmp(&info,&zero,sizeof info));
    assert(!terms[0].task_name[0]&&!terms[0].task_started&&!terms[0].process);
    term_select(0);assert(!strcmp(term_get(term_count()-1),"Native task stopped."));
    command("echo Terminal is still alive");assert(!strcmp(term_get(term_count()-1),"Terminal is still alive"));
    command("start /another.bex");assert(term_task_info(0,&info));
    assert(info.instance!=copy.instance&&info.started_ticks==now&&info.elapsed_sec==0&&!strcmp(info.name,"another.bex"));
    copy=info;term_task_stop(0);command("start /another.bex");
    assert(term_task_info(0,&info)&&info.started_ticks==copy.started_ticks&&info.instance!=copy.instance);
    /* Normal completion clears metadata and leaves the caller's selection alone. */
    term_select(1);exit_next[0]=1;int before_steps=steps[1];
    for(int i=0;i<3&&term_task_running(0);i++)term_task_poll();
    assert(!term_task_running(0)&&!terms[0].task_name[0]&&selected==1&&!strcmp(term_input(),"x"));
    assert(term_task_running(1)&&steps[1]>=before_steps);
    term_task_close(1);assert(!term_task_info(1,&info)&&!terms[1].process);
    term_backspace();command("start /another.bex");assert(term_task_info(1,&info));
    term_reset();assert(!term_task_info(1,&info)&&!terms[1].task_name[0]);
    /* Lifetime remains correct across the unsigned PIT counter rollover. */
    now=UINT32_MAX-10;command("start /another.bex");now+=70;
    assert(term_task_info(1,&info)&&info.elapsed_sec==1);
    term_task_close(1);
    /* pwd retains the complete valid path rather than a 63-byte prefix. */
    term_reset();
    int folder=fs_root();
    for(int level=0;level<8;level++){
        folder=fs_mkdir(folder,"long-folder-name-123456");
        assert(folder>=0);
    }
    term_set_cwd(folder);
    char expected[FS_PATH_LEN], actual[FS_PATH_LEN];
    fs_path(folder,expected,sizeof expected);
    assert(strlen(expected)>160);
    int before=term_count();command("pwd");
    unsigned used=0;
    for(int row=before+1;row<term_count();row++){
        unsigned length=(unsigned)strlen(term_get(row));
        memcpy(actual+used,term_get(row),length);used+=length;
    }
    actual[used]=0;
    assert(!strcmp(expected,actual));
    puts("task metadata: copied names, live status/lifetime, selection preservation, busy start, stop/exit/close/reset and restart identities passed");
}

#endif
