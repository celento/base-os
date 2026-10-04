/* Valid native-task lifecycle and metadata with the real Terminal/filesystem. */
#define main filesystem_fixture_main
#include "fs_host.c"
#undef main
static unsigned char terminal_arena[0x100000];
#define TERM_MEMORY ((uintptr_t)terminal_arena)
#include "../src/term.c"
static int states[PROCESS_TASKS],exit_next[PROCESS_TASKS],steps[PROCESS_TASKS];
static ProgramIO callbacks[PROCESS_TASKS];
int basic_run(const char *s,int n,const ProgramIO *io){(void)s;(void)n;(void)io;return 0;}
int program_key(void){return 0;}
void program_present(void){}
int process_run(const void *p,unsigned n,const ProgramIO *io){(void)p;(void)n;(void)io;return 0;}
int process_task_start(int owner,const void *p,unsigned n,const ProgramIO *io){
    assert(owner>=0&&owner<8&&p&&n>=16&&io);
    if(states[owner]==PROCESS_TASK_READY||states[owner]==PROCESS_TASK_SLEEPING)return -1;
    states[owner]=PROCESS_TASK_READY;callbacks[owner]=*io;return 0;
}
int process_task_step(int owner){
    if(states[owner]!=PROCESS_TASK_READY)return 0;
    steps[owner]++;
    if(exit_next[owner]){exit_next[owner]=0;states[owner]=PROCESS_TASK_DONE;}
    else callbacks[owner].print("Task progress.");
    return 1;
}
int process_task_status(int owner){return owner>=0&&owner<8?states[owner]:PROCESS_TASK_EMPTY;}
int process_task_result(int owner){(void)owner;return 0;}
int process_task_key(int owner,int key){(void)key;return term_task_running(owner);}
void process_task_stop(int owner){states[owner]=PROCESS_TASK_DONE;}
void process_task_clear(int owner){if(owner>=0&&owner<8)states[owner]=PROCESS_TASK_EMPTY;}
static void command(const char *s){while(*s)term_char(*s++);term_enter();}
static int executable(const char *name){
    /* A complete BEX1 file is used. Process execution itself has separate tests. */
    const unsigned char bex[]={0x42,0x45,0x58,0x31,16,0,0,0,18,0,0,0,0,0,0,0,0xeb,0xfe};
    int id=fs_create(fs_root(),name);assert(id>=0);
    assert(fs_write(id,(const char *)bex,sizeof bex)==sizeof bex);return id;
}
int main(void){
    reset();int first=executable("counter.bex");executable("another.bex");
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
    assert(!terms[0].task_name[0]&&!terms[0].task_started&&!terms[0].task_instance);
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
    term_task_close(1);assert(!term_task_info(1,&info)&&!terms[1].task_instance);
    term_backspace();command("start /another.bex");assert(term_task_info(1,&info));
    term_reset();assert(!term_task_info(1,&info)&&!terms[1].task_name[0]);
    /* Lifetime remains correct across the unsigned PIT counter rollover. */
    now=UINT32_MAX-10;command("start /another.bex");now+=70;
    assert(term_task_info(1,&info)&&info.elapsed_sec==1);
    term_task_close(1);
    puts("task metadata: copied names, live status/lifetime, selection preservation, busy start, stop/exit/close/reset and restart identities passed");
}
