/* Valid BEX2 files are ordinary unsupported input to an older BEX1 kernel.
 * This fixture checks clean refusal without attempting BEX2 execution. */
#define FEATURE_TEST
#include "kernel.c"
#ifndef AS_ROLLBACK_EXPECT
#define AS_ROLLBACK_EXPECT (-2)
#endif
static unsigned rb_total,rb_printed;
static void rb_check(int ok,const char *why){if(!ok)panic(why);}
static void rb_print(const ProcessBinding *binding,const char *text){
    rb_check(binding&&binding->slot==2&&text,"AS rollback frozen callback");rb_printed++;
}
static void rb_plot(const ProcessBinding *binding,int x,int y,int color){(void)binding;(void)x;(void)y;(void)color;}
static void rb_legacy_print(const char *text){(void)text;}
static void rb_legacy_plot(int x,int y,int color){(void)x;(void)y;(void)color;}
static const ProgramIO rb_legacy={rb_legacy_print,rb_legacy_plot,0,0,0,0};
static void rb_count(const char *label){
    PhysmemStats stats;ProcessCounts count;rb_check(physmem_stats(&stats)==PHYS_OK,"AS rollback stats");process_counts(&count);
    rb_check(stats.total==rb_total&&stats.free==rb_total&&!stats.allocated&&!count.records&&!count.owned,"AS rollback leaked resources");
    for(unsigned i=PHYS_BEX1_BACKING;i<PHYS_KIND_LIMIT;i++)rb_check(!stats.by_kind[i],"AS rollback kind leaked");
    kprint_debug("AS-COUNT ");kprint_debug(label);kprint_debug(" total=");kprint_uint(stats.total);
    kprint_debug(" free=");kprint_uint(stats.free);kprint_debug(" allocated=0 high_water=");kprint_uint(stats.high_water);
    kprint_debug(" backing=0 mapped=0 pt=0 pd=0 records=0\n");
}
void feature_test(void){
    PhysmemStats initial;rb_check(physmem_stats(&initial)==PHYS_OK&&!initial.allocated,"AS rollback baseline");
    rb_total=initial.total;rb_count("rollback-baseline");
    const char *names[]={"/Programs/address-space.bex","/Programs/address-large.bex"};
    for(unsigned i=0;i<2;i++){
        int file=fs_resolve(fs_root(),names[i]);rb_check(file>=0&&fs_size(file)>64,"AS rollback valid file missing");
        ProcessHandle unchanged=0x12345678;
        int result=process_create(fs_data(file),fs_size(file),0,0,&unchanged);
        rb_check(result==AS_ROLLBACK_EXPECT&&unchanged==0x12345678,"AS rollback create refusal changed");
        rb_check(process_run(fs_data(file),fs_size(file),&rb_legacy)==AS_ROLLBACK_EXPECT,"AS rollback exec refusal changed");
        rb_count("rollback-refused-valid-bex2");
    }
    int file=fs_resolve(fs_root(),"/Programs/hello-c.bex");rb_check(file>=0,"AS rollback frozen file missing");
    ProcessHandle process=0;rb_check(!process_create(fs_data(file),fs_size(file),0,0,&process),"AS rollback BEX1 create");
    ProcessIO io={.binding={process,2,1},.print=rb_print,.plot=rb_plot};
    rb_check(process_bind(process,&io)&&process_start(process),"AS rollback BEX1 start");
    unsigned began=timer_ticks();
    while(process_status(process)!=PROCESS_TASK_DONE){
        rb_check(timer_ticks()-began<5*TIMER_HZ,"AS rollback BEX1 deadline");process_step(process);
    }
    ProcessResult result;
    rb_check(rb_printed&&process_get_result(process,&result)&&result.reason==PROCESS_EXIT_APP&&
             !result.value&&process_reap(process),"AS rollback frozen completion");
    rb_count("rollback-final");platform_log("ADDRESS-SPACE-ROLLBACK-PASS\n");
}
