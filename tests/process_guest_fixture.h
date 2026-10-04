#ifndef PROCESS_GUEST_FIXTURE_H
#define PROCESS_GUEST_FIXTURE_H
/* Guest-only lifecycle helpers: display labels are explicit bindings, while
 * every operation after creation uses the exact returned process handle. */
static void guest_process_print(const ProcessBinding *binding,const char *text){
    (void)binding;(void)text;
}
static void guest_process_plot(const ProcessBinding *binding,int x,int y,int color){
    (void)binding;(void)x;(void)y;(void)color;
}
static int guest_process_launch(ProcessHandle *out,unsigned display,const void *file,
                                unsigned bytes,const char *argument,unsigned length){
    if(!out||*out)return -1;
    ProcessHandle process=0;
    int result=process_create(file,bytes,argument,length,&process);
    if(result)return result;
    ProcessIO io={.binding={process,display,1},
                  .print=guest_process_print,.plot=guest_process_plot};
    if(!process_bind(process,&io)||!process_start(process)){
        process_request_stop(process);process_reap(process);return -1;
    }
    *out=process;return 0;
}
static int guest_process_result(ProcessHandle process,unsigned reason,int value){
    ProcessResult result;
    return process_get_result(process,&result)&&result.reason==reason&&result.value==value;
}
static int guest_process_release(ProcessHandle *process){
    if(!*process)return 1;
    ProcessResult result;
    if(!process_request_stop(*process)||!process_get_result(*process,&result)||
       !process_reap(*process))return 0;
    *process=0;return 1;
}
#endif
