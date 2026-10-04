/* Ordinary file dispatch/lifecycle in the real kernel, with valid SDK programs. */
#define FEATURE_TEST
#include "../src/kernel.c"
static void launch_check(int ok,const char *why){if(!ok)panic(why);}
static void launch_round(unsigned ticks){
    unsigned start=timer_ticks();
    while(timer_ticks()-start<ticks){term_task_poll();poll_time();platform_poll();asm volatile("hlt");}
}
static int launch_text(int owner,const char *text){
    term_select(owner);for(int i=0;i<term_count();i++)if(!kstrcmp(term_get(i),text))return 1;return 0;
}
static int launch_value(const char *path){
    int id=fs_resolve(fs_root(),path),n=0;
    launch_check(id>=0&&fs_size(id)>1,"saved counter missing");
    const char *s=fs_data(id);for(int i=0;i<fs_size(id)&&s[i]>='0'&&s[i]<='9';i++)n=n*10+s[i]-'0';
    return n;
}
static void launch_query(const char *name){
    launcher_open();kstrcpy(launch_buf,name);launch_len=kstrlen(name);launcher_refresh();
    launch_check(launch_n==1,"native search must have one match");launcher_run(0);
}
void feature_test(void){
    int programs=fs_resolve(fs_root(),"/Programs"),counter=fs_resolve(fs_root(),"/Programs/counter.bex");
    int done=fs_resolve(fs_root(),"/Documents/native-launch-ok");
    if(done>=0){
        launch_check(launch_value("/Documents/counter-2.txt")>=3,"first counter did not survive reboot");
        launch_check(launch_value("/Documents/counter-3.txt")>=23,"second counter did not survive reboot");
        int note=fs_resolve(fs_root(),"/Documents/sdk-note.txt");
        launch_check(note>=0&&!kstrcmp(fs_data(note),"This note was saved by a protected C application.\n"),"notebook did not survive reboot");
        platform_log("NATIVE-LAUNCH-REBOOT-PASS\n");return;
    }
    open_files(programs);int files=context_slot;
    for(int row=0;row<fm_vis_count();row++)if(fm_row_id(row)==counter)fm_selected=row;
    fm_open_selected();int first=context_slot;
    launch_check(files==0&&first==1&&wins[first].kind==WK_TERM&&term_task_running(first),"Files native dispatch");
    launch_query("counter.bex");int second=context_slot;
    launch_check(second==2&&term_task_running(first)&&term_task_running(second),"launcher native dispatch");
    TermTaskInfo a,b;launch_check(term_task_info(first,&a)&&term_task_info(second,&b)&&a.instance!=b.instance,"independent launch owners");
    launch_round(3*TIMER_HZ+10);
    launch_check(term_task_key(second,'+')&&term_task_key(second,'+'),"counter input");
    launch_check(term_task_key(first,'s')&&term_task_key(second,'s'),"counter saves");launch_round(TIMER_HZ);
    launch_check(launch_value("/Documents/counter-2.txt")>=3&&launch_value("/Documents/counter-3.txt")>=23,"independent counter values");
    launch_check(!fs_needs_sync(),"counter Save must explicitly sync");
    launch_check(launch_text(first,"Saved /Documents/counter-2.txt")&&launch_text(second,"Saved /Documents/counter-3.txt"),"counter Saved output");
    win_minimize(first);launch_round(TIMER_HZ);
    launch_check(term_task_running(first)&&term_task_running(second),"minimize keeps tasks running");
    term_task_stop(first);launch_check(!term_task_running(first)&&term_task_running(second),"stop only owner");
    win_close(first);win_close(second);launch_check(!term_task_running(second),"close cancels owner");
    launch_query("notebook.bex");int notebook=context_slot;launch_round(TIMER_HZ);
    launch_check(!term_task_running(notebook)&&launch_text(notebook,"Saved /Documents/sdk-note.txt")&&!fs_needs_sync(),"Notebook explicit sync and natural exit");
    win_close(notebook);win_close(files);
    /* All eight windows are legitimate owners; a ninth never replaces one. */
    for(int i=0;i<MAX_WIN;i++)open_fs_file(counter);
    for(int i=0;i<MAX_WIN;i++)launch_check(term_task_running(i),"eight native owners");
    term_task_info(0,&a);open_fs_file(counter);term_task_info(0,&b);
    launch_check(a.instance==b.instance&&native_launch_status[0],"full desktop preserves task");
    win_close(0);launch_check(!native_launch_status[0],"closing frees capacity warning");
    for(int i=1;i<MAX_WIN;i++)win_close(i);
    /* A copied program remains independent when its ordinary source is removed. */
    int copy=fs_create(programs,"copy with spaces.BEX");
    launch_check(copy>=0&&fs_write(copy,(const char *)sdk_counter,sizeof sdk_counter)==sizeof sdk_counter,"install named program");
    open_fs_file(copy);int owner=context_slot;
    launch_check(term_task_info(owner,&a)&&!kstrcmp(a.name,"copy with spaces.BEX"),"spaces and uppercase suffix");
    launch_check(!fs_delete(copy),"remove copied source");launch_round(TIMER_HZ);
    launch_check(term_task_info(owner,&b)&&!kstrcmp(b.name,a.name)&&b.instance==a.instance,"owned image/name survived source removal");
    launch_check(term_task_key(owner,'q'),"graceful exit key");launch_round(TIMER_HZ);
    launch_check(!term_task_running(owner)&&launch_text(owner,"Native task finished."),"graceful native exit");win_close(owner);
    done=fs_create(fs_resolve(fs_root(),"/Documents"),"native-launch-ok");
    launch_check(done>=0&&fs_write(done,"ok\n",3)==3&&!fs_sync(),"completion marker persistence");
    platform_log("NATIVE-LAUNCH-FUNCTIONAL-PASS\n");
}
