#ifndef PROCESS_GUEST_FIXTURE_H
#define PROCESS_GUEST_FIXTURE_H
#include "physmem.h"
/* FEATURE_TEST guests include the real kmain: validated RAM/video and late
 * physmem initialization have already run. Never initialize a ready core twice. */
static inline PhysmemStats guest_process_memory(void){
    PhysmemStats stats;
    if(physmem_stats(&stats)!=PHYS_OK||stats.free+stats.allocated!=stats.total)
        panic("guest owned-page accounting unavailable");
    return stats;
}
static inline void guest_process_memory_restored(const PhysmemStats *before){
    PhysmemStats after=guest_process_memory();
    if(after.total!=before->total||after.free!=before->free||
       after.allocated!=before->allocated||after.high_water<before->high_water)
        panic("guest owned-page baseline drift");
    for(unsigned kind=0;kind<PHYS_KIND_LIMIT;kind++)
        if(after.by_kind[kind]!=before->by_kind[kind])panic("guest owned-page kind drift");
}
/* Guest-only lifecycle helpers: display labels are explicit bindings, while
 * every operation after creation uses the exact returned process handle. */
static inline void guest_process_print(const ProcessBinding *binding,const char *text){
    (void)binding;(void)text;
}
static inline void guest_process_plot(const ProcessBinding *binding,int x,int y,int color){
    (void)binding;(void)x;(void)y;(void)color;
}
static inline int guest_process_launch(ProcessHandle *out,unsigned display,const void *file,
                                unsigned bytes,const char *argument,unsigned length){
    if(!out||*out)return -1;
    PhysmemStats before=guest_process_memory();
    ProcessHandle process=0;
    int result=process_create(file,bytes,argument,length,&process);
    if(result){guest_process_memory_restored(&before);return result;}
    PhysmemStats after=guest_process_memory();
    if(physmem_owner_pages(process)!=16||after.allocated!=before.allocated+16||
       after.free+16!=before.free||after.by_kind[PHYS_BEX1_BACKING]!=before.by_kind[PHYS_BEX1_BACKING]+16)
        panic("guest BEX1 must own exactly 16 backing pages");
    ProcessIO io={.binding={process,display,1},
                  .print=guest_process_print,.plot=guest_process_plot};
    if(!process_bind(process,&io)||!process_start(process)){
        process_request_stop(process);process_reap(process);
        guest_process_memory_restored(&before);return -1;
    }
    *out=process;return 0;
}
static inline int guest_process_result(ProcessHandle process,unsigned reason,int value){
    ProcessResult result;
    return process_get_result(process,&result)&&result.reason==reason&&result.value==value;
}
static inline int guest_process_release(ProcessHandle *process){
    if(!*process)return 1;
    ProcessResult result;
    if(!process_request_stop(*process)||!process_get_result(*process,&result)||
       !process_reap(*process))return 0;
    if(physmem_owner_pages(*process))panic("guest stopped process retained pages");
    *process=0;return 1;
}
#endif
