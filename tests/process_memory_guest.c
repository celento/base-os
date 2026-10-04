/* Ordinary valid-app fixture kernel. This checks the real process/allocator
 * objects and public client output; it is separate from PS/2 desktop evidence.
 * kernel.c is found through the supplied, provenance-checked runtime source. */
#define FEATURE_TEST
#include "kernel.c"
#include "native_files.h"
#include "native_sync.h"
#define PM_BYTES 24576u
static ProcessHandle pm_handles[PROCESS_TASKS];
static unsigned pm_reports[PROCESS_TASKS][8],pm_generations[PROCESS_TASKS];
static unsigned pm_baseline,pm_max_launch,pm_max_close,pm_max_slice;
static void pm_check(int ok,const char *message){if(!ok)panic(message);}
static void pm_max(unsigned *out,unsigned began){unsigned elapsed=timer_ticks()-began;if(elapsed>*out)*out=elapsed;}
static int pm_file(const char *path){return fs_resolve(fs_root(),path);}
static void pm_print(const ProcessBinding *binding,const char *text){
    pm_check(binding&&binding->slot<PROCESS_TASKS&&pm_handles[binding->slot]==binding->process&&
             pm_generations[binding->slot]==binding->generation,"PM callback attachment mismatch");
    pm_check(text[0]=='P'&&text[1]=='M'&&text[2]==' '&&kstrlen(text)==74,"PM public report format");
    for(unsigned i=0;i<8;i++){
        unsigned value=0;for(unsigned j=0;j<8;j++){
            char ch=text[3+i*9+j];pm_check((ch>='0'&&ch<='9')||(ch>='a'&&ch<='f'),"PM report digit");
            value=(value<<4)|(unsigned)(ch<='9'?ch-'0':ch-'a'+10);
        }
        pm_reports[binding->slot][i]=value;
    }
    pm_check(pm_reports[binding->slot][0]==binding->process&&
             pm_reports[binding->slot][1]==binding->slot+1&&pm_reports[binding->slot][2]==1,
             "PM independent owner/display/zero report");
}
static void pm_plot(const ProcessBinding *binding,int x,int y,int color){(void)binding;(void)x;(void)y;(void)color;}
static void pm_legacy_print(const char *text){(void)text;}
static void pm_legacy_plot(int x,int y,int color){(void)x;(void)y;(void)color;}
static const ProgramIO pm_legacy={pm_legacy_print,pm_legacy_plot,0,0,0,0};
static void pm_checkpoint(const char *label,unsigned pages,unsigned high,unsigned records,unsigned done){
    PhysmemStats stats;ProcessCounts counts;
    pm_check(physmem_stats(&stats)==PHYS_OK,"PM stats unavailable");process_counts(&counts);
    pm_check(stats.total==pm_baseline&&stats.free==pm_baseline-pages&&stats.allocated==pages&&
             stats.high_water==high&&stats.by_kind[PHYS_BEX1_BACKING]==pages,"PM page accounting mismatch");
    for(unsigned kind=PHYS_USER_IMAGE;kind<PHYS_KIND_LIMIT;kind++)pm_check(!stats.by_kind[kind],"PM unexpected page kind");
    pm_check(counts.records==records&&counts.done==done&&counts.owned==pages/16&&
             counts.live+counts.created+done==records&&!counts.exiting,"PM process accounting mismatch");
    kprint_debug("PM-COUNT ");kprint_debug(label);kprint_debug(" total=");kprint_uint(stats.total);
    kprint_debug(" free=");kprint_uint(stats.free);kprint_debug(" allocated=");kprint_uint(stats.allocated);
    kprint_debug(" high_water=");kprint_uint(stats.high_water);kprint_debug(" backing=");kprint_uint(stats.by_kind[PHYS_BEX1_BACKING]);
    kprint_debug(" records=");kprint_uint(counts.records);kprint_debug(" done=");kprint_uint(counts.done);serial_write('\n');
}
static ProcessHandle pm_launch(unsigned slot,int app){
    ProcessHandle process=0;unsigned began=timer_ticks();
    pm_check(!pm_handles[slot]&&!process_create(fs_data(app),fs_size(app),0,0,&process),"PM valid launch failed");
    ProcessIO io={.binding={process,slot,++pm_generations[slot]},.print=pm_print,.plot=pm_plot};
    pm_check(process_bind(process,&io)&&process_start(process),"PM bind/start failed");
    pm_handles[slot]=process;kmemset(pm_reports[slot],0,sizeof pm_reports[slot]);
    pm_check(physmem_owner_pages(process)==16,"PM launch must own sixteen pages");pm_max(&pm_max_launch,began);
    return process;
}
static void pm_step(unsigned slot){unsigned began=timer_ticks();process_step(pm_handles[slot]);pm_max(&pm_max_slice,began);}
static void pm_report_until(unsigned slot,unsigned field,unsigned wanted){
    unsigned began=timer_ticks();
    while(pm_reports[slot][field]<wanted){
        pm_check(process_status(pm_handles[slot])!=PROCESS_TASK_DONE,"PM client exited before report");
        pm_check(timer_ticks()-began<5*TIMER_HZ,"PM client report deadline");pm_step(slot);
    }
}
static void pm_finish(unsigned slot,unsigned reason,int value){
    ProcessHandle process=pm_handles[slot];ProcessResult result;
    pm_check(process_get_result(process,&result)&&result.reason==reason&&result.value==value,"PM exact completion mismatch");
    pm_check(!physmem_owner_pages(process),"PM completion retained backing");
    pm_check(process_reap(process)&&process_reap(process),"PM completion reap");pm_handles[slot]=0;
}
static void pm_stop(unsigned slot){
    unsigned began=timer_ticks();pm_check(process_request_stop(pm_handles[slot]),"PM inactive stop");
    pm_max(&pm_max_close,began);pm_finish(slot,PROCESS_EXIT_STOP,PROCESS_TASK_STOPPED);
}
static void pm_app_exit(unsigned slot,int code){
    pm_check(process_key(pm_handles[slot],code?'e':'q'),"PM exit key");unsigned began=timer_ticks();
    while(process_status(pm_handles[slot])!=PROCESS_TASK_DONE){
        pm_check(timer_ticks()-began<5*TIMER_HZ,"PM app exit deadline");pm_step(slot);
    }
    pm_finish(slot,PROCESS_EXIT_APP,code);
}
static void pm_exact_files(void){
    for(unsigned display=2;display<=6;display+=4){
        char path[]="/Documents/pm-0.bin";path[14]=(char)('0'+display);int id=pm_file(path);
        pm_check(id>=0&&fs_size(id)==PM_BYTES,"PM persisted workspace length");
        const unsigned char *bytes=(const unsigned char *)fs_data(id);
        for(unsigned i=0;i<PM_BYTES;i++)pm_check(bytes[i]==((i*37u+display*13u)&255u),"PM persisted workspace bytes");
    }
    int note=pm_file("/Documents/sdk-note.txt");
    const char expected[]="This note was saved by a protected C application.\n";
    pm_check(note>=0&&fs_size(note)==(int)sizeof expected-1,"PM frozen notebook length");
    for(unsigned i=0;i<sizeof expected-1;i++)pm_check(fs_data(note)[i]==expected[i],"PM frozen notebook bytes");
}
void feature_test(void){
    PhysmemStats initial;pm_check(physmem_stats(&initial)==PHYS_OK&&!initial.allocated&&!initial.high_water,"PM initial allocator not clean");
    pm_baseline=initial.total;pm_check(pm_baseline>=128,"PM profile needs eight normal clients");
    pm_checkpoint("baseline",0,0,0,0);
    if(pm_file("/Documents/pm-complete.txt")>=0){
        pm_exact_files();pm_checkpoint("reboot",0,0,0,0);platform_log("PROCESS-MEMORY-REBOOT-PASS\n");return;
    }
    pm_check(!fs_sync(),"PM initial disk sync");
    int app=pm_file("/Programs/process-memory.bex");pm_check(app>=0,"PM valid client missing");
    ProcessHandle first=pm_launch(5,app);pm_checkpoint("one",16,16,1,0);
    ProcessHandle second=pm_launch(1,app);pm_checkpoint("two",32,32,2,0);
    pm_check(first!=second&&first!=6&&second!=2,"PM independent opaque owners");
    pm_report_until(5,4,3);pm_report_until(1,4,3);
    unsigned first_file=pm_reports[5][5],second_file=pm_reports[1][5];BosFileInfo info;
    pm_check(native_file_info(first,first_file,&info)==BOS_OK&&native_file_info(second,first_file,&info)==BOS_E_STALE,
             "PM file handle not owner bound");
    unsigned prior_steps=pm_reports[5][4];
    int notebook=pm_file("/Programs/notebook.bex");pm_check(notebook>=0,"PM frozen notebook missing");
    pm_check(process_run(fs_data(notebook),fs_size(notebook),&pm_legacy)==0,"PM frozen exec failed");
    pm_checkpoint("exec-retained",32,32,2,0);pm_report_until(5,4,prior_steps+2);
    for(unsigned slot=0;slot<PROCESS_TASKS;slot++)if(slot!=5&&slot!=1)pm_launch(slot,app);
    pm_checkpoint("eight",128,128,8,0);
    ProcessHandle unchanged=0x12345678;pm_check(process_create(fs_data(app),fs_size(app),0,0,&unchanged)!=0&&unchanged==0x12345678,"PM full-capacity launch changed output");
    for(unsigned slot=0;slot<PROCESS_TASKS;slot++)pm_check(physmem_owner_pages(pm_handles[slot])==16&&process_status(pm_handles[slot])==PROCESS_TASK_READY,"PM capacity changed owner");
    pm_check(native_file_info(first,first_file,&info)==BOS_OK&&native_file_info(second,second_file,&info)==BOS_OK,"PM capacity changed file owner");
    pm_checkpoint("rejected-ninth",128,128,8,0);
    for(unsigned slot=0;slot<PROCESS_TASKS;slot++)if(slot!=5&&slot!=1)pm_stop(slot);
    pm_checkpoint("six-stopped",32,128,2,0);
    int anchor=fs_create(pm_file("/Documents"),"pm-sync-anchor.txt");
    pm_check(anchor>=0&&fs_write(anchor,"two owners share this ordinary save\n",36)==36,"PM dirty save anchor");
    pm_check(process_key(first,'s')&&process_key(second,'s'),"PM begin keys");
    pm_report_until(5,6,1);pm_report_until(1,6,1);
    unsigned first_op=pm_reports[5][6],second_op=pm_reports[1][6];
    pm_check(first_op!=second_op&&native_sync_poll(first,first_op)==BOS_PENDING&&native_sync_poll(second,second_op)==BOS_PENDING,
             "PM two pending save receipts");
    pm_check(native_sync_poll(second,first_op)==BOS_E_STALE,"PM receipt not owner bound");
    pm_check(process_key(second,'w'),"PM wait key");unsigned began=timer_ticks();
    while(process_status(second)!=PROCESS_TASK_SLEEPING){pm_check(timer_ticks()-began<5*TIMER_HZ,"PM waiter deadline");pm_step(1);}
    began=timer_ticks();pm_check(process_request_stop(first),"PM inactive pending close");pm_max(&pm_max_close,began);
    pm_checkpoint("close-pending-done",16,128,2,1);pm_finish(5,PROCESS_EXIT_STOP,PROCESS_TASK_STOPPED);
    pm_checkpoint("close-pending",16,128,1,0);
    pm_check(native_file_info(first,first_file,&info)==BOS_E_STALE&&native_sync_poll(first,first_op)==BOS_E_STALE&&
             native_file_info(second,second_file,&info)==BOS_OK&&native_sync_poll(second,second_op)==BOS_PENDING,
             "PM stopped owner cleanup damaged survivor");
    began=timer_ticks();while(native_sync_poll(second,second_op)==BOS_PENDING){
        pm_check(timer_ticks()-began<120*TIMER_HZ,"PM shared save deadline");storage_poll();platform_poll();
    }
    pm_check(native_sync_poll(second,second_op)==BOS_OK,"PM surviving save did not complete");
    pm_report_until(1,4,pm_reports[1][4]+2);
    ProcessHandle replacement=pm_launch(5,app);pm_check(replacement!=first,"PM owner reused");
    pm_check(process_request_stop(first)&&process_reap(first)&&process_status(replacement)==PROCESS_TASK_READY,"PM stale close affected replacement");
    pm_report_until(5,4,3);pm_check(native_sync_poll(replacement,second_op)==BOS_E_STALE,"PM new owner inherited receipt");
    pm_checkpoint("zero-reuse",32,128,2,0);
    pm_app_exit(1,23);pm_checkpoint("nonzero-exit",16,128,1,0);
    pm_check(native_file_info(second,second_file,&info)==BOS_E_STALE&&native_sync_poll(second,second_op)==BOS_E_STALE,"PM app exit owner cleanup");
    pm_app_exit(5,0);pm_checkpoint("normal-exit",0,128,0,0);
    int counter=pm_file("/Programs/counter.bex");pm_check(counter>=0,"PM frozen counter missing");
    for(unsigned mode=0;mode<3;mode++){
        pm_check(!term_task_start_file(0,counter,fs_identity(counter)),"PM Terminal frozen launch");
        pm_checkpoint("terminal-start",16,128,1,0);TermTaskInfo task;pm_check(term_task_info(0,&task),"PM Terminal owner info");
        if(mode==0)term_task_stop(0);else if(mode==1)pm_check(term_task_close(0),"PM Terminal close");else {term_select(0);term_reset();}
        pm_check(!term_task_running(0)&&!physmem_owner_pages(task.instance),"PM Terminal cleanup");pm_checkpoint("terminal-clean",0,128,0,0);
    }
    pm_exact_files();int marker=fs_create(pm_file("/Documents"),"pm-complete.txt");
    const char complete[]="C1/C2 ordinary guest complete\n";
    pm_check(marker>=0&&fs_write(marker,complete,sizeof complete-1)==sizeof complete-1&&!fs_sync(),"PM completion persistence");
    pm_checkpoint("final",0,128,0,0);
    kprint_debug("PM-LATENCY ticks_hz=70 launch=");kprint_uint(pm_max_launch);kprint_debug(" close=");kprint_uint(pm_max_close);
    kprint_debug(" slice=");kprint_uint(pm_max_slice);serial_write('\n');
    platform_log("PROCESS-MEMORY-FUNCTIONAL-PASS\n");
}
