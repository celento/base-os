/* Ordinary valid-app fixture. Real runtime objects; serial diagnostics only.
 * No memory observation, fault probes, forced low-memory knobs or debugger.
 * kernel.c is resolved from the explicitly supplied clean runtime checkout. */
#define FEATURE_TEST
#include "kernel.c"
#include "native_files.h"
#include "native_sync.h"
#include "executable.h"
static ProcessHandle as_handles[PROCESS_TASKS];
static unsigned as_reports[PROCESS_TASKS][16],as_pending[PROCESS_TASKS][8],as_generation[PROCESS_TASKS];
static unsigned as_pages[PROCESS_TASKS],as_mapped[PROCESS_TASKS];
static unsigned as_baseline,as_high,as_max_launch,as_max_close,as_max_slice;
static void as_check(int ok,const char *why){if(!ok)panic(why);}
static int as_file(const char *path){return fs_resolve(fs_root(),path);}
static void as_max(unsigned *out,unsigned began){unsigned dt=timer_ticks()-began;if(dt>*out)*out=dt;}
static void as_print(const ProcessBinding *binding,const char *text){
    as_check(binding&&binding->slot<PROCESS_TASKS&&as_handles[binding->slot]==binding->process&&
             as_generation[binding->slot]==binding->generation,"AS callback owner mismatch");
    if(!as_mapped[binding->slot])return; /* unchanged frozen BEX1 output */
    as_check(text[0]=='A'&&(text[1]=='0'||text[1]=='1')&&text[2]==' '&&kstrlen(text)==74,"AS report format");
    unsigned group=(unsigned)(text[1]-'0');
    for(unsigned i=0;i<8;i++){
        unsigned value=0;for(unsigned j=0;j<8;j++){
            char ch=text[3+i*9+j];as_check((ch>='0'&&ch<='9')||(ch>='a'&&ch<='f'),"AS report digit");
            value=(value<<4)|(unsigned)(ch<='9'?ch-'0':ch-'a'+10);
        }
        if(group)as_reports[binding->slot][8+i]=value;else as_pending[binding->slot][i]=value;
    }
    if(!group)return;
    for(unsigned i=0;i<8;i++)as_reports[binding->slot][i]=as_pending[binding->slot][i];
    unsigned *r=as_reports[binding->slot];
    as_check(r[0]==binding->process&&r[1]==binding->slot+1&&r[8]==as_mapped[binding->slot]&&
             r[9]==as_pages[binding->slot]&&r[10]==2&&!r[15],"AS public memory identity");
}
static void as_plot(const ProcessBinding *binding,int x,int y,int color){(void)binding;(void)x;(void)y;(void)color;}
static void as_legacy_print(const char *text){(void)text;}
static void as_legacy_plot(int x,int y,int color){(void)x;(void)y;(void)color;}
static const ProgramIO as_legacy={as_legacy_print,as_legacy_plot,0,0,0,0};
static void as_checkpoint(const char *label){
    unsigned pages=0,mapped=0,tables=0,backing=0,records=0;
    for(unsigned i=0;i<PROCESS_TASKS;i++)if(as_handles[i]){
        pages+=as_pages[i];records++;
        if(as_mapped[i]){mapped+=as_mapped[i];tables++;}else backing+=16;
        as_check(physmem_owner_pages(as_handles[i])==as_pages[i],"AS per-owner accounting");
    }
    PhysmemStats stats;ProcessCounts counts;
    as_check(physmem_stats(&stats)==PHYS_OK,"AS stats unavailable");process_counts(&counts);
    if(pages>as_high)as_high=pages;
    as_check(stats.total==as_baseline&&stats.free==as_baseline-pages&&stats.allocated==pages&&
             stats.high_water==as_high&&stats.by_kind[PHYS_BEX1_BACKING]==backing&&
             stats.by_kind[PHYS_USER_IMAGE]==mapped&&stats.by_kind[PHYS_PAGE_TABLE]==tables&&
             stats.by_kind[PHYS_PAGE_DIRECTORY]==tables,"AS page-kind accounting");
    as_check(counts.records==records&&counts.owned==records&&counts.live+counts.created==records&&
             !counts.done&&!counts.exiting,"AS process accounting");
    kprint_debug("AS-COUNT ");kprint_debug(label);kprint_debug(" total=");kprint_uint(stats.total);
    kprint_debug(" free=");kprint_uint(stats.free);kprint_debug(" allocated=");kprint_uint(stats.allocated);
    kprint_debug(" high_water=");kprint_uint(stats.high_water);kprint_debug(" backing=");kprint_uint(backing);
    kprint_debug(" mapped=");kprint_uint(mapped);kprint_debug(" pt=");kprint_uint(tables);
    kprint_debug(" pd=");kprint_uint(tables);kprint_debug(" records=");kprint_uint(records);serial_write('\n');
}
static ProcessHandle as_launch(unsigned slot,int app,int bex2){
    as_check(!as_handles[slot],"AS occupied fixture slot");
    unsigned owned=16,mapped=0;
    if(bex2){
        ExecutablePlan plan;ExecutablePolicy policy={1,1,262144,1024};
        as_check(executable_plan_bex2(fs_data(app),fs_size(app),&policy,&plan)==EXECUTABLE_OK,"AS valid plan");
        owned=plan.owned_pages;mapped=plan.mapped_pages;
    }
    ProcessHandle process=0;unsigned began=timer_ticks();
    const char *argument=bex2?"/Documents/source.bin":0;
    as_check(!process_create(fs_data(app),fs_size(app),argument,bex2?21:0,&process),"AS valid create");
    ProcessIO io={.binding={process,slot,++as_generation[slot]},.print=as_print,.plot=as_plot};
    as_check(process_bind(process,&io)&&process_start(process),"AS bind/start");
    as_handles[slot]=process;as_pages[slot]=owned;as_mapped[slot]=mapped;
    kmemset(as_reports[slot],0,sizeof as_reports[slot]);as_max(&as_max_launch,began);
    as_check(physmem_owner_pages(process)==owned,"AS launch pages");return process;
}
static void as_step(unsigned slot){
    unsigned began=timer_ticks();process_step(as_handles[slot]);as_max(&as_max_slice,began);
}
static void as_until(unsigned slot,unsigned field,unsigned wanted){
    unsigned began=timer_ticks();
    while(as_reports[slot][field]<wanted){
        as_check(timer_ticks()-began<15*TIMER_HZ,"AS client report deadline");
        as_step(slot);
        if(process_status(as_handles[slot])==PROCESS_TASK_DONE){
            ProcessResult result;as_check(process_get_result(as_handles[slot],&result),"AS result missing");
            kprint_debug("AS-EARLY-EXIT slot=");kprint_uint(slot);kprint_debug(" value=");kprint_uint((unsigned)result.value);
            serial_write('\n');panic("AS valid client exited early");
        }
    }
}
static void as_finish(unsigned slot,unsigned reason,int value){
    ProcessHandle process=as_handles[slot];ProcessResult result;
    as_check(process_get_result(process,&result)&&result.reason==reason&&result.value==value,"AS completion mismatch");
    as_check(!physmem_owner_pages(process)&&process_reap(process)&&process_reap(process),"AS inactive cleanup/reap");
    as_handles[slot]=0;as_pages[slot]=as_mapped[slot]=0;
}
static void as_stop(unsigned slot){
    unsigned began=timer_ticks();as_check(process_request_stop(as_handles[slot]),"AS inactive stop");
    as_check(process_request_stop(as_handles[slot]),"AS repeated inactive close");
    as_max(&as_max_close,began);as_finish(slot,PROCESS_EXIT_STOP,PROCESS_TASK_STOPPED);
}
static void as_exit(unsigned slot,int code){
    as_check(process_key(as_handles[slot],code?'e':'q'),"AS exit key");unsigned began=timer_ticks();
    while(process_status(as_handles[slot])!=PROCESS_TASK_DONE){
        as_check(timer_ticks()-began<5*TIMER_HZ,"AS exit deadline");as_step(slot);
    }
    as_finish(slot,PROCESS_EXIT_APP,code);
}
static void as_exact_files(void){
    for(unsigned display=2;display<=6;display+=4){
        char path[]="/Documents/as-0.bin";path[14]=(char)('0'+display);int id=as_file(path);
        as_check(id>=0&&fs_size(id)==32768,"AS persisted length");
        const unsigned char *bytes=(const unsigned char *)fs_data(id);
        for(unsigned i=0;i<32768;i++){
            unsigned j=i+4064;as_check(bytes[i]==((j*37u+display*13u+(j>>12)*17u)&255u),"AS persisted exact bytes");
        }
    }
    int counter=as_file("/Documents/counter-4.txt");
    as_check(counter>=0&&fs_size(counter)==3&&fs_data(counter)[0]=='1'&&
             fs_data(counter)[1]=='0'&&fs_data(counter)[2]=='\n',"AS reboot frozen input result");
    int id=as_file("/Documents/sdk-note.txt");const char note[]="This note was saved by a protected C application.\n";
    as_check(id>=0&&fs_size(id)==sizeof note-1,"AS frozen exec output size");
    for(unsigned i=0;i<sizeof note-1;i++)as_check(fs_data(id)[i]==note[i],"AS frozen exec exact bytes");
}
void feature_test(void){
    PhysmemStats initial;as_check(physmem_stats(&initial)==PHYS_OK&&!initial.allocated&&!initial.high_water,"AS initial pages");
    as_baseline=initial.total;as_check(as_baseline==797||as_baseline==1021,"AS default/large real pool");as_checkpoint("baseline");
    if(as_file("/Documents/as-complete.txt")>=0){as_exact_files();as_checkpoint("reboot");platform_log("ADDRESS-SPACE-REBOOT-PASS\n");return;}
    as_check(!fs_sync(),"AS initial sync");
    int app=as_file("/Programs/address-space.bex"),big=as_file("/Programs/address-large.bex");
    int counter=as_file("/Programs/counter.bex"),notebook=as_file("/Programs/notebook.bex");
    as_check(app>=0&&big>=0&&counter>=0&&notebook>=0,"AS fixture apps missing");
    ProcessHandle first=as_launch(5,app,1);as_until(5,2,1);as_checkpoint("one-filled");
    ProcessHandle second=as_launch(1,app,1);as_until(1,2,1);as_checkpoint("both-filled-barrier");
    /* Neither verification occurs until BOTH complete writes have been reported. */
    as_check(as_reports[5][2]==1&&as_reports[1][2]==1&&!as_reports[5][14]&&!as_reports[1][14],"AS verification barrier");
    as_launch(3,counter,0);as_step(3);as_checkpoint("mixed-frozen-bex1");
    as_check(process_key(first,'v')&&process_key(second,'v'),"AS verify keys");
    as_until(5,2,2);as_until(1,2,2);as_until(5,14,as_reports[5][14]+2);
    int fpu_a=as_file("/Programs/fpu-a.bex"),fpu_b=as_file("/Programs/fpu-b.bex");
    as_check(fpu_a>=0&&fpu_b>=0,"AS normal x87 apps missing");
    as_launch(2,fpu_a,0);as_launch(4,fpu_b,0);as_checkpoint("mixed-x87-owners");
    for(int i=0;i<20;i++){
        int expected=900+i,actual=0;
        __asm__ volatile("fninit\n\tfildl %0"::"m"(expected):"memory");
        as_step(5);as_step(2);as_step(1);as_step(4);
        __asm__ volatile("fistpl %0":"=m"(actual)::"memory");
        as_check(actual==expected&&process_status(as_handles[2])==PROCESS_TASK_READY&&
                 process_status(as_handles[4])==PROCESS_TASK_READY,"AS mixed x87 context");
    }
    as_exit(2,0);as_exit(4,0);as_checkpoint("mixed-x87-cleanup");
    as_check(process_key(as_handles[3],'=')&&process_key(as_handles[3],'s'),"AS frozen input");
    unsigned counter_started=timer_ticks();
    while(as_file("/Documents/counter-4.txt")<0){
        as_check(timer_ticks()-counter_started<5*TIMER_HZ,"AS frozen counter progress");as_step(3);
    }
    int counter_output=as_file("/Documents/counter-4.txt");
    as_check(fs_size(counter_output)==3&&fs_data(counter_output)[0]=='1'&&
             fs_data(counter_output)[1]=='0'&&fs_data(counter_output)[2]=='\n',"AS frozen input exact output");
    unsigned first_file=as_reports[5][5],second_file=as_reports[1][5];BosFileInfo info;
    as_check(native_file_info(first,first_file,&info)==BOS_OK&&native_file_info(second,first_file,&info)==BOS_E_STALE,"AS file ownership");
    as_check(process_run(fs_data(notebook),fs_size(notebook),&as_legacy)==0,"AS frozen legacy exec");
    as_until(5,14,as_reports[5][14]+2);as_until(1,14,as_reports[1][14]+2);as_step(3);as_checkpoint("mixed-exec-retained");
    ProcessHandle unchanged=0x12345678;
    as_check(process_create(fs_data(big),fs_size(big),0,0,&unchanged)==PROCESS_CREATE_MEMORY&&unchanged==0x12345678,"AS natural capacity result");
    as_check(native_file_info(first,first_file,&info)==BOS_OK&&native_file_info(second,second_file,&info)==BOS_OK,"AS rejection kept file handles");
    as_until(5,14,as_reports[5][14]+2);as_until(1,14,as_reports[1][14]+2);as_checkpoint("capacity-rejected-survivors");
    as_stop(1);as_checkpoint("capacity-owner-closed");
    as_launch(1,big,1);as_until(1,2,1);as_checkpoint("same-request-admitted");
    as_until(5,14,as_reports[5][14]+2);as_check(process_key(as_handles[1],'v'),"AS larger verify key");as_until(1,2,2);
    as_stop(1);as_checkpoint("larger-closed");
    second=as_launch(1,app,1);as_until(1,2,1);as_check(process_key(second,'v'),"AS reused verify");as_until(1,2,2);
    second_file=as_reports[1][5];as_checkpoint("zero-reuse");as_stop(3);as_checkpoint("frozen-closed");
    int anchor=fs_create(as_file("/Documents"),"as-anchor.txt");
    as_check(anchor>=0&&fs_write(anchor,"shared ordinary save\n",21)==21,"AS dirty sync anchor");
    as_check(process_key(first,'s')&&process_key(second,'s'),"AS sync keys");as_until(5,2,3);as_until(1,2,3);
    unsigned first_op=as_reports[5][6],second_op=as_reports[1][6];
    as_check(first_op!=second_op&&native_sync_poll(first,first_op)==BOS_PENDING&&native_sync_poll(second,second_op)==BOS_PENDING,"AS shared pending receipts");
    as_check(process_key(second,'w'),"AS waiter key");unsigned began=timer_ticks();
    while(process_status(second)!=PROCESS_TASK_SLEEPING){as_check(timer_ticks()-began<5*TIMER_HZ,"AS wait deadline");as_step(1);}
    as_stop(5);as_checkpoint("pending-close-released-pd-pt");
    as_check(native_file_info(first,first_file,&info)==BOS_E_STALE&&native_sync_poll(first,first_op)==BOS_E_STALE&&
             native_file_info(second,second_file,&info)==BOS_OK&&native_sync_poll(second,second_op)==BOS_PENDING,"AS close kept survivor");
    began=timer_ticks();while(native_sync_poll(second,second_op)==BOS_PENDING){
        as_check(timer_ticks()-began<120*TIMER_HZ,"AS durable save deadline");storage_poll();platform_poll();
    }
    as_check(native_sync_poll(second,second_op)==BOS_OK,"AS surviving receipt durable");as_until(1,14,as_reports[1][14]+2);
    ProcessHandle replacement=as_launch(5,app,1);as_until(5,2,1);
    as_check(replacement!=first&&process_request_stop(first)&&process_reap(first),"AS stale close reused owner");
    as_check(process_key(replacement,'v'),"AS replacement verify");as_until(5,2,2);as_checkpoint("post-sync-zero-reuse");
    as_exit(1,23);as_checkpoint("nonzero-exit");
    as_check(native_file_info(second,second_file,&info)==BOS_E_STALE&&native_sync_poll(second,second_op)==BOS_E_STALE,"AS exit owner cleanup");
    as_exit(5,0);as_checkpoint("normal-exit");as_exact_files();
    int marker=fs_create(as_file("/Documents"),"as-complete.txt");const char complete[]="BEX2 ordinary address spaces complete\n";
    as_check(marker>=0&&fs_write(marker,complete,sizeof complete-1)==sizeof complete-1&&!fs_sync(),"AS completion sync");
    as_checkpoint("final");kprint_debug("AS-LATENCY ticks_hz=70 launch=");kprint_uint(as_max_launch);
    kprint_debug(" close=");kprint_uint(as_max_close);kprint_debug(" slice=");kprint_uint(as_max_slice);serial_write('\n');
    platform_log("ADDRESS-SPACE-FUNCTIONAL-PASS\n");
}
