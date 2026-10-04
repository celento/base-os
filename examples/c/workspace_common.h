#ifndef BASEOS_WORKSPACE_EXAMPLE_H
#define BASEOS_WORKSPACE_EXAMPLE_H
#include "baseos_app2.h"
/* Shared support for opt-in BEX2 examples. They keep 4096-byte file transfers
 * and small conditional report replacements even with a MiB of private RAM. */
static unsigned append_text(char *out,unsigned at,const char *s) {
    while(*s)out[at++]=*s++;
    return at;
}
static unsigned append_number(char *out,unsigned at,unsigned value) {
    char reverse[10];unsigned n=0;
    do{reverse[n++]=(char)('0'+value%10u);value/=10u;}while(value);
    while(n)out[at++]=reverse[--n];
    return at;
}
static int open_input(BosFileInfo *input) {
    char path[BOS_ARGUMENT_MAX+1];
    int length=bos_argument(path,sizeof path);
    if(length<0)return length;
    if(!length){
        static const char fallback[]="/Documents/stats-sample.txt";
        for(unsigned i=0;i<sizeof fallback;i++)path[i]=fallback[i];
    }
    return bos_file_open(path,BOS_FILE_OPEN_READ,input);
}
static int save_report(const char *prefix,const char *report,unsigned bytes) {
    char path[96];unsigned n=append_text(path,0,"/Documents/");
    n=append_text(path,n,prefix);n=append_text(path,n,"-");
    n=append_number(path,n,bos_task_id());n=append_text(path,n,".txt");path[n]=0;
    BosFileInfo file;
    int result;
    for(unsigned attempt=0;;attempt++){
        result=bos_file_open(path,BOS_FILE_OPEN_READ|BOS_FILE_OPEN_WRITE,&file);
        if(result==BOS_E_NOT_FOUND)
            result=bos_file_open(path,BOS_FILE_OPEN_READ|BOS_FILE_OPEN_WRITE|BOS_FILE_OPEN_CREATE,&file);
        if(result!=BOS_E_BUSY||attempt==5999)break;
        if(bos_sleep(10)<0)break;
    }
    if(result!=BOS_OK)return result;
    for(unsigned attempt=0;;attempt++){
        result=bos_file_replace(file.handle,report,bytes,&file);
        if(result!=BOS_E_BUSY||attempt==5999)break;
        if(bos_sleep(10)<0)break;
    }
    int closed=bos_file_close(file.handle);
    if(result!=BOS_OK)return result;
    if(closed!=BOS_OK)return closed;
    BosHandle operation;
    result=bos_sync_begin(&operation);
    if(result!=BOS_OK)return result;
    do{result=bos_sync_wait(operation,60000);}while(result==BOS_E_TIMEOUT);
    int released=bos_sync_release(operation);
    if(result!=BOS_OK)return result;
    if(released!=BOS_OK)return released;
    bos_print("Saved and synchronized: ");bos_print(path);bos_print("\n");
    return BOS_OK;
}
static int workspace_ready(void) {
    if(!bos_task_id()||bos_workspace_bytes()<1048576u){
        bos_print("This BEX2 example needs a desktop task and at least 1 MiB workspace.\n");
        return 0;
    }
    return 1;
}
#endif
