/* Optional 3 MiB ordinary workload derived from address_space_app.c. All pointers are inside declared text/data/workspace/stack.
 * Both owners must reach stage1 before the runner sends either verification key.
 * No guards, absent pages, invalid pointers or intentional CPU faults are read. */
#include "baseos_app2.h"
static volatile unsigned initialized_word=0x53706163u;
static BosAbiInfo abi;
static BosMemoryInfo memory;
static BosFileInfo output;
static BosHandle operation;
static unsigned display,stage,steps,checksum,checks;
static int sync_result=BOS_E_STALE;
/* Ordinary page records: a unique 32-bit little-endian page identity
 * followed by patterned payload. Every one of the 768 pages differs. */
static unsigned pattern(unsigned i){
    unsigned lane=i&4095u,page=i>>12;
    if(lane<4)return ((page^(display*0x9e3779b9u))>>(lane*8))&255u;
    return (i*37u+display*13u+page*17u)&255u;
}
static unsigned source_byte(unsigned i){return (i*29u+7u)&255u;}
static void report(void){
    unsigned values[16]={abi.process,display,stage,checksum,steps,output.handle,operation,
        (unsigned)sync_result,memory.mapped_pages,memory.owned_pages,memory.table_pages,
        bos_workspace_bytes(),(unsigned)bos_workspace(),(unsigned)__stack_bottom,checks,0};
    for(unsigned group=0;group<2;group++){
        char line[76];unsigned n=0;line[n++]='A';line[n++]=(char)('0'+group);line[n++]=' ';
        for(unsigned i=0;i<8;i++){
            for(int shift=28;shift>=0;shift-=4)line[n++]="0123456789abcdef"[(values[group*8+i]>>shift)&15];
            line[n++]=i==7?'\n':' ';
        }
        line[n]=0;bos_print(line);
    }
}
static int verify(void){
    unsigned char *work=bos_workspace();
    for(unsigned i=0;i<bos_workspace_bytes();i++)if(work[i]!=pattern(i))return 0;
    checks++;return 1;
}
static int reads(void){
    /* Choose an interior stack-page boundary: both sides remain in this array. */
    unsigned char local[12288];
    unsigned char *cross=(unsigned char *)(((unsigned)local+8191u)&~4095u)-32;
    if(bos_argument((char *)cross,128)!=21)return 0;
    const char expected[]="/Documents/source.bin";
    for(unsigned i=0;i<sizeof expected;i++)if(cross[i]!=(unsigned char)expected[i])return 0;
    BosFileInfo source;
    if(bos_file_open((char *)cross,BOS_FILE_OPEN_READ,&source)!=BOS_OK)return 0;
    if(bos_file_read_at(source.handle,cross,128,33)!=128)return 0;
    for(unsigned i=0;i<128;i++)if(cross[i]!=source_byte(33+i))return 0;
    unsigned char *work=bos_workspace();
    /* Every interior workspace boundary is exercised. The first client's
     * workspace necessarily crosses allocator holes on the normal profiles. */
    for(unsigned boundary=4096;boundary<bos_workspace_bytes();boundary+=4096){
        unsigned offset=boundary-32;
        if(bos_file_read_at(source.handle,work+offset,128,boundary&4095u)!=128)return 0;
        for(unsigned i=0;i<128;i++)if(work[offset+i]!=source_byte(i))return 0;
        for(unsigned i=0;i<128;i++)work[offset+i]=(unsigned char)pattern(offset+i);
    }
    return bos_file_close(source.handle)==BOS_OK;
}
int main(void){
    if(stage||steps||checksum||checks||operation||output.handle||abi.struct_size||memory.struct_size)return 39;
    if(bos_abi_query(&abi,sizeof abi)!=BOS_OK||bos_memory_info(&memory,sizeof memory)!=BOS_OK)return 41;
    if(initialized_word!=0x53706163u)return 40;
    display=bos_task_id();
    if(display<1||display>8||abi.user_bytes!=4194304||abi.context!=BOS_CONTEXT_DESKTOP_TASK||
       memory.format!=2||memory.page_bytes!=4096||memory.table_pages!=2||
       memory.owned_pages!=memory.mapped_pages+2||memory.virtual_bytes!=4194304||
       memory.policy_pages!=1024||memory.region_count<3||memory.region_count>4)return 42;
    unsigned found=0;
    for(unsigned i=0;i<memory.region_count;i++)if(memory.regions[i].purpose==BOS_MEMORY_WORKSPACE){
        if(memory.regions[i].offset!=(unsigned)bos_workspace()||
           memory.regions[i].bytes!=bos_workspace_bytes()||memory.regions[i].protection!=3)return 42;
        found++;
    }
    if(found!=1)return 42;
    unsigned char *work=bos_workspace();
    for(unsigned i=0;i<bos_workspace_bytes();i++)if(work[i])return 43;
    for(unsigned i=0;i<bos_workspace_bytes();i++){work[i]=(unsigned char)pattern(i);checksum+=work[i];}
    stage=1;report();
    for(;;){
        int key;while((key=bos_key())){
            if(key=='q')return 0;
            if(key=='e')return 23;
            if(key=='v'){
                if(!verify())return 44;
                if(!reads())return 45;
                char path[]="/Documents/as-0.bin";path[14]=(char)('0'+display);
                unsigned flags=BOS_FILE_OPEN_READ|BOS_FILE_OPEN_WRITE;
                int opened=bos_file_open(path,flags,&output);
                if(opened==BOS_E_NOT_FOUND)opened=bos_file_open(path,flags|BOS_FILE_OPEN_CREATE,&output);
                if(opened!=BOS_OK)return 46;
                if(bos_file_replace(output.handle,work+4096-32,32768,&output)!=BOS_OK)return 47;
                stage=2;
            }
            if(key=='s'){
                sync_result=bos_sync_begin(&operation);if(sync_result!=BOS_OK)return 48;stage=3;
            }
            if(key=='w')sync_result=bos_sync_wait(operation,60000);
        }
        if(stage>=2&&!verify())return 49;
        steps++;report();if(bos_yield())return 50;
    }
}
