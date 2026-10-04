/* Ordinary BEX1 client: public ABI only, fixed BSS workspace and text reports.
 * This is not a heap, a larger image, a memory probe or a kernel test hook. */
#include "baseos.h"
#define PM_BYTES 24576u
static unsigned char workspace[PM_BYTES];
static BosAbiInfo abi;
static BosFileInfo file;
static BosHandle operation;
static unsigned display, steps, checksum;
static int sync_result=BOS_E_STALE;
static unsigned expected(unsigned i){return (i*37u+display*13u)&255u;}
static void report(void){
    unsigned values[8]={abi.process,display,1,checksum,steps,file.handle,operation,(unsigned)sync_result};
    char line[76];unsigned n=0;line[n++]='P';line[n++]='M';line[n++]=' ';
    for(unsigned i=0;i<8;i++){
        for(int shift=28;shift>=0;shift-=4)line[n++]="0123456789abcdef"[(values[i]>>shift)&15];
        line[n++]=i==7?'\n':' ';
    }
    line[n]=0;bos_print(line);
}
int main(void){
    if(bos_abi_query(&abi,sizeof abi)!=BOS_OK)return 41;
    display=bos_task_id();
    if(display<1||display>8||abi.user_bytes!=65536||abi.image_bytes!=49152||
       abi.stack_reserved_bytes!=16384||abi.context!=BOS_CONTEXT_DESKTOP_TASK)return 42;
    for(unsigned i=0;i<PM_BYTES;i++)if(workspace[i])return 43;
    for(unsigned i=0;i<PM_BYTES;i++){workspace[i]=(unsigned char)expected(i);checksum+=workspace[i];}
    char path[]="/Documents/pm-0.bin";path[14]=(char)('0'+display);
    int opened=bos_file_open(path,BOS_FILE_OPEN_READ|BOS_FILE_OPEN_WRITE,&file);
    if(opened==BOS_E_NOT_FOUND)opened=bos_file_open(path,BOS_FILE_OPEN_READ|BOS_FILE_OPEN_WRITE|BOS_FILE_OPEN_CREATE,&file);
    if(opened!=BOS_OK)return 44;
    if(bos_file_replace(file.handle,workspace,PM_BYTES,&file)!=BOS_OK)return 45;
    for(;;){
        for(unsigned i=0;i<PM_BYTES;i++)if(workspace[i]!=expected(i))return 46;
        int key;while((key=bos_key())){
            if(key=='q')return 0;
            if(key=='e')return 23;
            if(key=='s')sync_result=bos_sync_begin(&operation);
            if(key=='w')sync_result=bos_sync_wait(operation,60000);
        }
        steps++;report();
        if(bos_yield())return 47;
    }
}
