#define FEATURE_TEST
#include "../src/kernel.c"
#include "process_guest_fixture.h"
static ProcessHandle argument_handles[3];
static void check(int ok,const char *why){if(!ok)panic(why);}
static void quiet(const char *text){(void)text;}
static void pixel(int x,int y,int color){(void)x;(void)y;(void)color;}
static ProgramIO io={quiet,pixel,0,0,0,0};
static int file(const char *path){return fs_resolve(fs_root(),path);}
static void command(const char *s){while(*s)term_char(*s++);term_enter();}
static void record(int owner,const char *expected){
    char path[]="/Documents/argument-1.txt";path[20]=(char)('0'+owner);
    int id=file(path);check(id>=0&&fs_size(id)==kstrlen(expected)&&!kstrcmp(fs_data(id),expected),"copied argument record differs");
}
static void tick(void){poll_time();platform_poll();__asm__ volatile("hlt");}
static int has_line(const char *s){for(int i=0;i<term_count();i++)if(!kstrcmp(term_get(i),s))return 1;return 0;}
void feature_test(void){
    int app=file("/Programs/arguments.bex");check(app>=0,"argument app missing");
    if(file("/Documents/arguments-done.txt")>=0){
        record(1,"/Documents/a sample.txt");record(3,"none");
        platform_log("NATIVE-ARGUMENTS-REBOOT-PASS\n");return;
    }
    char first[]="/Documents/a sample.txt",second[PROCESS_ARGUMENT_MAX+1],saved_second[PROCESS_ARGUMENT_MAX+1];
    second[0]='/';for(unsigned i=1;i<PROCESS_ARGUMENT_MAX;i++)second[i]=(char)('a'+i%26);
    second[PROCESS_ARGUMENT_MAX]=0;kstrcpy(saved_second,second);
    check(!guest_process_launch(&argument_handles[0],0,fs_data(app),fs_size(app),first,kstrlen(first)),"argument first start");
    check(!guest_process_launch(&argument_handles[1],1,fs_data(app),fs_size(app),second,PROCESS_ARGUMENT_MAX),"128-byte argument start");
    check(!guest_process_launch(&argument_handles[2],2,fs_data(app),fs_size(app),0,0),"legacy argument-free start");
    check(!process_start(argument_handles[0]),"busy task argument replaced");
    kmemset(first,'x',sizeof first-1);kmemset(second,'y',PROCESS_ARGUMENT_MAX);
    unsigned began=timer_ticks();
    while(timer_ticks()-began<5*TIMER_HZ){
        int done=0;
        for(int owner=0;owner<3;owner++){
            process_step(argument_handles[owner]);if(process_status(argument_handles[owner])==PROCESS_TASK_DONE)done++;
        }
        if(done==3)break;tick();
    }
    for(int owner=0;owner<3;owner++)check(process_status(argument_handles[owner])==PROCESS_TASK_DONE&&guest_process_result(argument_handles[owner],PROCESS_EXIT_APP,0),"argument runtime copy or bounded output failed");
    record(1,"/Documents/a sample.txt");record(2,saved_second);record(3,"none");
    check(!process_run(fs_data(app),fs_size(app),&io),"legacy exec argument absence");
    /* A new exact process bound to the same display must drop its old path. */
    check(guest_process_release(&argument_handles[1]),"argument owner release");
    check(!guest_process_launch(&argument_handles[1],1,fs_data(app),fs_size(app),0,0),"argument-free owner restart");
    began=timer_ticks();
    while(process_status(argument_handles[1])!=PROCESS_TASK_DONE&&timer_ticks()-began<5*TIMER_HZ){process_step(argument_handles[1]);tick();}
    check(process_status(argument_handles[1])==PROCESS_TASK_DONE&&guest_process_result(argument_handles[1],PROCESS_EXIT_APP,0),"argument-free restarted app");record(2,"none");
    for(int owner=0;owner<3;owner++)
        check(guest_process_release(&argument_handles[owner]),"argument record release");
    platform_log("NATIVE-ARGUMENT-COPY-ISOLATION-PASS\n");
    /* Two real Terminal paths, one quoted and one relative. Their source image
     * and argument bytes remain owned after ordinary source edits/deletion. */
    int renamed=fs_create(file("/Programs"),"argument tool.bex");
    check(renamed>=0&&fs_write(renamed,fs_data(app),fs_size(app))==fs_size(app),"quoted program copy");
    term_select(0);term_reset();command("start \"/Programs/argument tool.bex\" \"/Documents/a sample.txt\"");
    term_select(1);term_reset();term_set_cwd(file("/Documents"));command("start ../Programs/arguments.bex \"a sample.txt\"");
    TermTaskInfo one,two;char title[TERM_TASK_TITLE_LEN];
    check(term_task_info(0,&one)&&term_task_info(1,&two)&&one.instance!=two.instance,"quoted launch owners");
    check(term_task_title(0,title,sizeof title)&&!kstrcmp(title,"argument tool.bex - a sample.txt"),"copied document title");
    check(!fs_delete(renamed),"delete copied executable source");
    int document=file("/Documents/a sample.txt");check(!fs_rename(document,"renamed sample.txt"),"rename startup document");
    check(term_task_title(0,title,sizeof title)&&!kstrcmp(title,"argument tool.bex - a sample.txt"),"source edit changed launch title");
    began=timer_ticks();
    while((term_task_running(0)||term_task_running(1))&&timer_ticks()-began<5*TIMER_HZ){term_task_poll();tick();}
    check(!term_task_running(0)&&!term_task_running(1),"Terminal argument apps did not finish");
    term_select(0);check(has_line("Native task finished."),"quoted argument execution failed");
    term_select(1);check(has_line("Native task finished."),"relative argument execution failed");
    record(1,"/Documents/a sample.txt");record(2,"/Documents/a sample.txt");
    check(!term_task_title(0,title,sizeof title)&&!title[0],"finished title leaked");
    term_select(2);term_reset();command("start /Programs/arguments.bex");
    began=timer_ticks();while(term_task_running(2)&&timer_ticks()-began<5*TIMER_HZ){term_task_poll();tick();}
    check(has_line("Native task finished."),"no-argument slot reuse");record(3,"none");
    int done=fs_create(file("/Documents"),"arguments-done.txt");
    check(done>=0&&fs_write(done,"done",4)==4&&!fs_sync(),"argument reports sync");
    platform_log("NATIVE-ARGUMENTS-PASS\n");
}
