#define FEATURE_TEST
#include "../src/kernel.c"
#include "task_examples.h"
static void task_check(int ok,const char *why){if(!ok)panic(why);}
static void task_print(const char *text){(void)text;}
static void task_pixel(int x,int y,int color){(void)x;(void)y;(void)color;}
static ProgramIO task_io={task_print,task_pixel,0,0,0};
typedef struct {unsigned owner,value,steps,keys,last_key,checksum;} TaskRecord;
static TaskRecord record(int owner){
    char path[]="/Documents/task-1.dat";path[16]=(char)('0'+owner);
    int id=fs_resolve(fs_root(),path);TaskRecord r;
    task_check(id>=0&&fs_size(id)==sizeof r,"task file missing");
    kmemcpy(&r,fs_data(id),sizeof r);return r;
}
static void round_for(unsigned duration){
    unsigned start=timer_ticks();
    while(timer_ticks()-start<duration){
        for(int owner=0;owner<PROCESS_TASKS;owner++)process_task_step(owner);
        poll_time();platform_poll();
        __asm__ volatile("hlt");
    }
}
static void command(const char *text){while(*text)term_char(*text++);term_enter();}
void feature_test(void){
    task_check(!process_task_start(0,task_app,sizeof task_app,&task_io),"start first task");
    task_check(!process_task_start(1,task_app,sizeof task_app,&task_io),"start second task");
    task_check(process_task_start(0,task_app,sizeof task_app,&task_io)<0,"replace running task");
    task_check(process_task_key(0,'a')&&process_task_key(0,'b')&&process_task_key(1,'z'),"route task input");
    unsigned start=timer_ticks();round_for(3*TIMER_HZ);
    TaskRecord one=record(1),two=record(2);
    task_check(timer_ticks()-start>=3*TIMER_HZ,"long task duration");
    task_check(one.owner==1&&two.owner==2&&one.steps>20&&two.steps>20,"independent task progress");
    task_check(one.value==295&&two.value==322&&one.keys==2&&two.keys==1&&one.checksum==195&&two.checksum==122,"isolated keys and file effects");
    task_check(process_task_status(0)!=PROCESS_TASK_DONE&&process_task_status(1)!=PROCESS_TASK_DONE,"unexpected watchdog in task mode");
    platform_log("TASK-TWO-LONG-RUNNING-PASS\n");
    /* A sleeping task does not run early or wake just because input arrives. */
    task_check(!process_task_start(5,task_sleep,sizeof task_sleep,&task_io),"start sleeper");
    unsigned sleep_start=timer_ticks();
    task_check(process_task_step(5)&&process_task_status(5)==PROCESS_TASK_SLEEPING,"sleep did not suspend");
    task_check(process_task_key(5,'x'),"sleeping task input");
    round_for(20);
    task_check(process_task_status(5)==PROCESS_TASK_SLEEPING,"sleep returned too early");
    round_for(TIMER_HZ);
    task_check(timer_ticks()-sleep_start>=TIMER_HZ&&process_task_status(5)==PROCESS_TASK_DONE&&!process_task_result(5),"sleep deadline or input lost");
    platform_log("TASK-SLEEP-DEADLINE-PASS\n");
    /* A CPU-bound task does not need to cooperate. Each step returns at PIT. */
    const unsigned char spin[]={0x42,0x45,0x58,0x31,16,0,0,0,18,0,0,0,0,0,0,0,0xeb,0xfe};
    task_check(!process_task_start(2,spin,sizeof spin,&task_io),"start bounded CPU task");
    unsigned before=timer_ticks();task_check(process_task_step(2),"CPU task did not run");
    task_check(timer_ticks()-before<=3,"CPU task slice unbounded");
    round_for(TIMER_HZ);
    task_check(record(1).steps>one.steps&&record(2).steps>two.steps,"CPU task starved apps");
    process_task_stop(2);task_check(process_task_status(2)==PROCESS_TASK_DONE&&process_task_result(2)==PROCESS_TASK_STOPPED,"CPU task stop");
    platform_log("TASK-PREEMPTION-PASS\n");
    /* Context includes x87 state, while the desktop itself also uses x87. */
    task_check(!process_task_start(3,task_fpu_a,sizeof task_fpu_a,&task_io),"FPU first task");
    task_check(!process_task_start(4,task_fpu_b,sizeof task_fpu_b,&task_io),"FPU second task");
    for(int i=0;i<40;i++){
        int value=900+i,actual=0;
        __asm__ volatile("fninit\n\tfildl %0"::"m"(value):"memory");
        process_task_step(3);process_task_step(4);
        __asm__ volatile("fistpl %0":"=m"(actual)::"memory");
        task_check(actual==value,"kernel FPU context lost");
        task_check(process_task_status(3)==PROCESS_TASK_READY&&process_task_status(4)==PROCESS_TASK_READY,"app FPU context lost");
    }
    process_task_key(3,'q');process_task_key(4,'q');round_for(3);
    task_check(process_task_status(3)==PROCESS_TASK_DONE&&!process_task_result(3)&&process_task_status(4)==PROCESS_TASK_DONE&&!process_task_result(4),"FPU task exit");
    platform_log("TASK-FPU-ISOLATION-PASS\n");
    process_task_key(0,'q');round_for(5);
    task_check(process_task_status(0)==PROCESS_TASK_DONE&&!process_task_result(0),"task graceful exit");
    one=record(1);task_check(one.keys==3&&one.last_key=='q'&&one.checksum==308,"key replayed on resume");
    process_task_stop(1);two=record(2);round_for(5);
    task_check(record(2).steps==two.steps,"stopped task changed file");
    task_check(!process_task_start(0,task_app,sizeof task_app,&task_io),"task restart");round_for(5);
    one=record(1);task_check(one.keys==0&&one.value==100,"restart inherited stale image or input");
    /* Legacy exec still times out and cannot overwrite suspended task images. */
    unsigned before_exec=timer_ticks(),before_steps=one.steps;
    task_check(process_run(spin,sizeof spin,&task_io)==-3,"legacy watchdog changed");
    task_check(timer_ticks()-before_exec>=2*TIMER_HZ,"legacy watchdog returned early");
    round_for(5);one=record(1);
    task_check(one.value==100&&one.keys==0&&one.steps>before_steps,"legacy exec damaged a saved task");
    platform_log("TASK-LEGACY-WATCHDOG-PASS\n");
    process_task_clear(0);
    task_check(!process_task_start(0,task_app,sizeof task_app,&task_io),"queue test start");
    for(int key=33;key<73;key++)process_task_key(0,key);
    round_for(5);one=record(1);
    task_check(one.keys==32&&one.last_key==64&&one.checksum==1552,"key queue overflow order");
    process_task_key(0,'A');round_for(5);one=record(1);
    task_check(one.keys==33&&one.last_key=='A'&&one.checksum==1617,"key queue wrap");
    for(int owner=0;owner<PROCESS_TASKS;owner++)process_task_clear(owner);
    platform_log("TASK-STOP-RESTART-PASS\n");
    /* Terminal owner selection survives polling; close/reset discards a task. */
    int programs=fs_find_child(fs_root(),"Programs"),id=fs_create(programs,"task-test.bex");
    task_check(id>=0&&fs_write(id,(const char *)task_app,sizeof task_app)==sizeof task_app,"install task fixture");
    term_select(0);term_reset();command("start /Programs/task-test.bex");
    term_select(1);term_reset();command("start /Programs/task-test.bex");
    unsigned until=timer_ticks()+10;
    while((int)(timer_ticks()-until)<0){term_task_poll();__asm__ volatile("hlt");}
    task_check(term_task_running(0)&&term_task_running(1),"terminal task launch");
    task_check(!kstrcmp(term_input(),""),"terminal selection changed");
    term_task_close(0);task_check(!term_task_running(0)&&term_task_running(1),"close wrong owner");
    term_task_stop(1);task_check(!term_task_running(1),"terminal stop failed");
    command("start /Programs/task-test.bex");task_check(term_task_running(1),"terminal restart failed");
    term_reset();task_check(!term_task_running(1),"reset did not cancel task");
    /* Notebook also synchronizes the floppy through syscall 16: motor delays
     * must receive PIT ticks even though the syscall enters an interrupt gate. */
    command("exec /Programs/notebook.bex");
    task_check(!kstrcmp(term_get(term_count()-1),"Program finished."),"legacy synchronous exec regression");
    task_check(fs_sync()==0,"task document persistence");
    platform_log("NATIVE-TASK-RUNTIME-PASS\n");
}
