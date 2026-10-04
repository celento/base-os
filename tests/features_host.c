#define main foundation_main
#include "fs_host.c"
#undef main
static unsigned char terminal_arena[0x100000];
#define TERM_MEMORY ((uintptr_t)terminal_arena)
#include "../src/app_storage.c"
#include "../src/app_canvas.c"
#include "../src/app_view.c"
#include "../src/term.c"
#define V (views[selected])
#include "../src/basic.c"
#include "../src/history.c"
void timer_delay(unsigned t){now+=t;}
int program_key(void){return 65;}
void program_present(void){}
int process_run(const void *p,unsigned n,const ProgramIO *io){(void)p;(void)n;(void)io;return -1;}
int process_create(const void *p,unsigned n,const char *argument,unsigned length,ProcessHandle *out){(void)p;(void)n;(void)argument;(void)length;(void)out;return -1;}
int process_create_mode(const void *file,unsigned bytes,const char *argument,unsigned length,
                        unsigned mode,ProcessHandle *out){
    return mode==PROCESS_LAUNCH_HOSTED?process_create(file,bytes,argument,length,out):PROCESS_CREATE_UNSUPPORTED;
}

int process_bind(ProcessHandle p,const ProcessIO *io){(void)p;(void)io;return 0;}
int process_unbind(ProcessHandle p){(void)p;return 0;}
int process_start(ProcessHandle p){(void)p;return 0;}
int process_step(ProcessHandle p){(void)p;return 0;}
ProcessHandle process_schedule_one(void){return 0;}
int process_binding_live(const ProcessBinding *binding){(void)binding;return 0;}
int process_status(ProcessHandle p){(void)p;return PROCESS_TASK_EMPTY;}
int process_get_result(ProcessHandle p,ProcessResult *out){(void)p;(void)out;return 0;}
int process_key(ProcessHandle p,int key){(void)p;(void)key;return 0;}
int process_request_stop(ProcessHandle p){(void)p;return 1;}
int process_reap(ProcessHandle p){(void)p;return 1;}
void process_counts(ProcessCounts *out){if(out)memset(out,0,sizeof *out);}
static char result[192];static int plotted;
static void output_line(const char *s){snprintf(result,sizeof result,"%s",s);}
static void output_plot(int x,int y,int c){assert(x>=0&&x<160&&y>=0&&y<100&&c==7);plotted++;}
static void command(const char *s){while(*s)term_char(*s++);term_enter();}
int main(void){
    reset();
    unsigned char items[3*sizeof(int)];int value=0;History h={0,0,3,sizeof value,items};
    for(int i=1;i<=5;i++){history_record(&h,&value);value=i;}
    assert(history_step(&h,&value,0)&&value==4);assert(history_step(&h,&value,0)&&value==3);
    assert(history_step(&h,&value,1)&&value==4);history_record(&h,&value);value=99;
    assert(!history_step(&h,&value,1));assert(history_step(&h,&value,0)&&value==4);
    ProgramIO io={output_line,output_plot,program_key,program_present,0,0};
    const char *source="10 LET A=2+3*4\n20 PRINT A\n30 RECT 0,0,2,3,7\n40 END\n";
    assert(!basic_run(source,strlen(source),&io));assert(!strcmp(result,"14")&&plotted==6);
    source="10 LET A=0\n20 LET A=A+1\n30 IF A < 5 THEN 20\n40 PRINT A\n";
    assert(!basic_run(source,strlen(source),&io)&&!strcmp(result,"5"));
    source="10 GOTO 10";assert(basic_run(source,strlen(source),&io)==-2);
    const char *bad[]={"10 LET","10 LET A","10 PRINT 1/0","10 IF","10 INKEY","10 PLOT 1,","10 PRINT (1","10 LET A=1\n10 END","10 PRINT 1/(-1+1)","10 GOTO 999"};
    for(unsigned i=0;i<sizeof bad/sizeof *bad;i++)assert(basic_run(bad[i],strlen(bad[i]),&io)==-1);
    /* Every truncation of valid statements must remain memory-safe. */
    char fuzz[192];source="10 LET A=1\n20 IF A < 3 THEN 40\n30 RECT 1,2,3,4,7\n40 PRINT \"ok\"\n";
    for(unsigned i=0;i<strlen(source);i++){memcpy(fuzz,source,i);fuzz[i]=0;basic_run(fuzz,i,&io);}
    int dir=fs_mkdir(0,"test folder"),f=fs_create(dir,"hello.txt");fs_write(f,"contents",8);
    assert(fs_resolve(0,"/test folder/../test folder/hello.txt")==f);
    char name[24];assert(fs_destination(0,"/test folder/new.txt",name)==dir&&!strcmp(name,"new.txt"));
    assert(fs_destination(0,"/test folder/1234567890123456789012345",name)<0);
    term_select(0);term_reset();command("cd \"/test folder\"");assert(term_cwd()==dir);
    term_char('c');term_char('a');term_char('t');term_complete();assert(!strcmp(term_input(),"cat "));
    term_char('h');term_complete();assert(!strcmp(term_input(),"cat hello.txt "));term_enter();assert(!strcmp(term_get(term_count()-1),"contents"));
    term_history(-1);assert(!strcmp(term_input(),"cat hello.txt "));term_history(1);assert(!*term_input());
    term_select(1);term_reset();command("echo separate");assert(term_cwd()==0);
    term_select(0);assert(term_cwd()==dir&&!strcmp(term_get(term_count()-1),"contents"));
    int script_id=fs_create(dir,"script");source="echo first\ncd /\necho last\n";fs_write(script_id,source,strlen(source));command("run script");assert(term_cwd()==0&&!strcmp(term_get(term_count()-1),"last"));
    source="run /test folder/script"; /* use quoted path for recursive call */
    source="run \"/test folder/script\"";fs_write(script_id,source,strlen(source));command(source);assert(strstr(term_get(term_count()-1),"Error:"));
    command("df");assert(term_count()>0);
    for(unsigned i=0;i<sizeof commands/sizeof *commands;i++){int count_before=term_count();assert(!manual(commands[i].name));assert(term_count()>count_before||term_count()==TERM_LINES);}
    command("help");term_set_rows(8);term_scroll(8);assert(term_scroll_offset()==8);term_scroll(-8);assert(!term_scroll_offset());
    command("man basic");assert(!strcmp(term_get(term_count()-1),"Limit: 256 lines, 10000 statements, ten seconds."));
    command("man not-a-command");assert(strstr(term_get(term_count()-1),"Error:"));
    int old=fs_create(0,"identity-test");unsigned identity=fs_identity(old);assert(!fs_delete(old));int newer=fs_create(0,"replacement");assert(newer==old&&fs_identity(newer)!=identity);
    int deep=0;for(int i=0;i<12;i++){deep=fs_mkdir(deep,"nested-folder");assert(deep>=0);}char path[FS_PATH_LEN];fs_path(deep,path,sizeof path);assert(strlen(path)>64&&fs_resolve(0,path)==deep);
    int original = fs_mkdir(0,"original");
    term_set_cwd(original);assert(term_cwd()==original);
    assert(fs_delete(original)==0);
    int replacement = fs_mkdir(0,"replacement-dir");assert(replacement==original);
    assert(term_cwd()==fs_root());
    puts("features: bounded undo/redo, BASIC syntax/loops/truncation, paths, terminal isolation/history/completion/scripts passed");
}
