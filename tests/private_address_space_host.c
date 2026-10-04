/* Actual production process logic, pure tables and owned allocator. Hardware
 * entry/root changes are explicit host-buffer adapters; no guest code executes. */
#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "program.h"
#include "platform.h"
#include "audio.h"
#include "fs.h"
#include "native_platform_service_stubs.h"
static unsigned root_is_user;
static void release_check(unsigned owner){assert(owner&&!root_is_user);}
#define PROCESS_BACKING_RELEASE_HOOK(owner) release_check(owner)
#include "process_backing_host.h"
#include "private_space_types.inc"
static unsigned char user_memory[BOS_BEX2_VIRTUAL_BYTES];
#undef USER_BASE
#define USER_BASE ((uintptr_t)user_memory)
static NativeTask task_memory[PROCESS_TASKS],*tasks=task_memory,*current_task,*root_task;
static unsigned owner_serial,schedule_next,canvas_width,canvas_height;
static BosHandle synchronous_owner;
static const ProgramIO *output;
static int tasks_ready,active,process_result;
static uint32_t began,now;
static unsigned char gdt_user_code[8],gdt_user_data[8];
static jmp_buf returned;
static unsigned production_image_copies,enters,restores,fpu_entries,fpu_returns;
static unsigned publications,legacy_writes;
static unsigned char disk_bytes[4096];
static AudioStatus sound;
static int slice_exit,slice_value;
static void (*slice_check)(void);
static int task_private(const NativeTask *);
static uint32_t process_user_root(const NativeTask *);
static int file_call(unsigned,unsigned,unsigned,unsigned,unsigned,unsigned);
void kmemcpy(void *to,const void *from,int bytes){
    uintptr_t a=(uintptr_t)to,b=(uintptr_t)from,base=(uintptr_t)user_memory;
    if((host_physmem_is_frame(from)&&a>=base&&a<base+sizeof user_memory)||
       (host_physmem_is_frame(to)&&b>=base&&b<base+sizeof user_memory))production_image_copies++;
    host_physmem_copy(to,from,(unsigned)bytes);
}
void kmemset(void *to,int value,int bytes){memset(to,value,(size_t)bytes);}
uint32_t timer_ticks(void){return now;}
int fs_sync(void){return 0;}
int fs_root(void){return 0;}
int fs_valid(int id){return id>=0&&id<=2;}
int fs_is_dir(int id){return id==0||id==1;}
int fs_is_app(int id){(void)id;return 0;}
int fs_find_child(int parent,const char *name){return parent==0&&!strcmp(name,"Documents")?1:-1;}
int fs_parent(int id){return id==2?1:id==1?0:-1;}
int fs_resolve(int cwd,const char *path){(void)cwd;return !strcmp(path,"/Documents/data.txt")?2:-1;}
int fs_destination(int cwd,const char *path,char *name){(void)cwd;(void)path;strcpy(name,"data.txt");return 1;}
int fs_size(int id){assert(id==2);return sizeof disk_bytes;}
const char *fs_data(int id){assert(id==2);return (const char *)disk_bytes;}
int fs_create(int parent,const char *name){(void)name;assert(parent==1);return 2;}
int fs_write(int id,const char *data,int length){assert(id==2);assert(length>=0);if(length)assert(data);legacy_writes++;return length;}
int fs_delete(int id){assert(id==2);return 0;}
const AudioStatus *audio_status(void){return &sound;}
void audio_pause(int paused){sound.state=paused?AUDIO_PAUSED:AUDIO_PLAYING;}
static void protect_memory(void){assert(!root_is_user);}
static uint32_t segment_limit(const unsigned char p[8]){
    uint32_t result=p[0]|(uint32_t)p[1]<<8|(uint32_t)(p[6]&15)<<16;
    return p[6]&0x80?(result<<12)|4095:result;
}
static void process_user_context(const NativeTask *task){
    assert(active&&!root_is_user);root_task=(NativeTask *)task;root_is_user=1;enters++;
    uint32_t root=process_user_root(task);
    if(task_private(task)){
        assert(root==task->directory);
        assert(segment_limit(gdt_user_code)==task->plan.code_limit);
        assert(segment_limit(gdt_user_data)==BOS_BEX2_VIRTUAL_BYTES-1);
        assert(gdt_user_data[6]==0xc0&&gdt_user_data[1]==3&&gdt_user_data[0]==255);
        const uint32_t *table=space_frame_pointer(task->table);
        for(unsigned page=0;page<1024;page++)if(table[page]&1)
            memcpy(user_memory+page*4096,host_physmem_pointer((void *)(uintptr_t)(table[page]&~4095u),4096),4096);
    }else{
        assert(root==PAGING_BASE);
        assert(segment_limit(gdt_user_code)==65535&&segment_limit(gdt_user_data)==65535);
    }
}
static void process_kernel_context(void){
    assert(active&&root_is_user);restores++;
    if(task_private(root_task)){
        const uint32_t *table=space_frame_pointer(root_task->table);
        for(unsigned page=0;page<1024;page++)if((table[page]&3)==3)
            memcpy(host_physmem_pointer((void *)(uintptr_t)(table[page]&~4095u),4096),user_memory+page*4096,4096);
    }
    root_task=0;root_is_user=0;
}
static void fpu_enter(NativeTask *task){assert(active&&root_is_user&&task==current_task);fpu_entries++;}
static void fpu_leave(NativeTask *task){assert(active&&!root_is_user&&task==current_task);fpu_returns++;}
static void process_leave(void) __attribute__((noreturn));
static void process_leave(void){assert(active&&root_is_user);longjmp(returned,1);}
static int invoke(unsigned call,unsigned a,unsigned b,unsigned c,unsigned d,unsigned e){
    uint32_t r[FRAME_WORDS]={0};r[7]=call;r[4]=a;r[6]=b;r[5]=c;r[1]=d;r[0]=e;r[12]=128;r[15]=0x1b;
    assert(process_interrupt(r)==1);return (int)r[7];
}
static int process_resume(const uint32_t *frame){
    assert(active&&root_is_user&&current_task&&frame==current_task->frame);
    if(setjmp(returned))return process_result;
    if(slice_check)slice_check();
    invoke(slice_exit?BOS_CALL_EXIT:BOS_CALL_YIELD,(unsigned)slice_value,0,0,0,0);return 0;
}
static int process_enter(unsigned entry){
    assert(active&&root_is_user&&!current_task&&entry==16);
    if(setjmp(returned))return process_result;
    assert(invoke(BOS_CALL_TASK_ID,0,0,0,0,0)==0);
    assert(invoke(BOS_CALL_MEMORY_INFO,1024,sizeof(BosMemoryInfo),1,0,0)==BOS_OK);
    BosMemoryInfo memory;memcpy(&memory,user_memory+1024,sizeof memory);
    assert(memory.mapped_pages==16&&!memory.owned_pages&&!memory.table_pages&&!memory.policy_pages);
    assert(invoke(BOS_CALL_ABI_QUERY,1024,sizeof(BosAbiInfo),1,0,0)==BOS_OK);
    BosAbiInfo abi;memcpy(&abi,user_memory+1024,sizeof abi);
    assert((abi.features&BOS_FEATURE_MEMORY_INFO)&&!(abi.features&BOS_FEATURE_BEX2));
    invoke(BOS_CALL_EXIT,0,0,0,0,0);return 0;
}
#include "private_space_ops.inc"
#include "private_space_helpers.inc"
static void line(const ProcessBinding *b,const char *text){assert(b->process==current_task->owner_id);assert(text);}
static void pixel(const ProcessBinding *b,int x,int y,int c){(void)b;(void)x;(void)y;(void)c;}
static void present(const ProcessBinding *b){assert(b->process==current_task->owner_id);publications++;}
static void legacy_line(const char *text){(void)text;}
static void legacy_pixel(int x,int y,int c){(void)x;(void)y;(void)c;}
static const ProgramIO legacy_io={legacy_line,legacy_pixel,0,0,0,0};
static const unsigned char bex1[]={0x42,0x45,0x58,0x31,16,0,0,0,18,0,0,0,0,0,0,0,0xeb,0xfe};
static ProcessHandle launch(const void *file,unsigned bytes,unsigned slot){
    ProcessHandle result=0;const char *argument="/Documents/data.txt";
    assert(process_create(file,bytes,argument,strlen(argument),&result)==0);
    ProcessIO io={{result,slot,1},line,pixel,present,0,0};
    assert(process_bind(result,&io)&&process_start(result));return result;
}
static void stop(ProcessHandle handle){assert(process_request_stop(handle)&&process_reap(handle));}
static void check_tables(NativeTask *task,const unsigned char *image){
    const ExecutablePlan *plan=&task->plan;
    const uint32_t *table=space_frame_pointer(task->table),*directory=space_frame_pointer(task->directory);
    assert(task_private(task)&&task->frame[14]==plan->entry_offset&&task->frame[17]==4194288);
    assert(physmem_core_owner_pages(&backing_core,task->owner_id)==plan->owned_pages);
    assert(!table[0]&&!table[plan->guard_offset/4096]);
    unsigned mapped=0;
    for(unsigned i=0;i<1024;i++){
        if(i!=USER_DIRECTORY_INDEX)assert(directory[i]==((i<<22)|0x83));
        else assert(directory[i]==(task->table|7));
        if(!(table[i]&1))continue;
        mapped++;uint32_t frame=table[i]&~4095u;
        assert(backing_owners[frame/4096]==task->owner_id&&backing_kinds[frame/4096]==PHYS_USER_IMAGE);
        assert(frame<0x1000000||frame>=0x1400000);
        unsigned flags=i>=plan->text.offset/4096&&i<(plan->text.offset+plan->text.bytes)/4096?5:7;
        assert((table[i]&7)==flags);
    }
    assert(mapped==plan->mapped_pages);
    const ExecutableRegion *regions[]={&plan->text,&plan->data,&plan->workspace,&plan->stack};
    unsigned index=0;
    for(unsigned r=0;r<4;r++)for(unsigned p=0;p<regions[r]->pages;p++,index++){
        const unsigned char *contents=host_physmem_pointer((void *)(uintptr_t)task->private_frames[index],4096);
        for(unsigned offset=0;offset<4096;offset++){
            unsigned logical=p*4096+offset,loaded=r==0?plan->text_file_bytes:r==1?plan->data_file_bytes:0;
            unsigned char expected=logical<loaded?image[regions[r]->offset+logical]:0;
            assert(contents[offset]==expected);
        }
    }
}
static void check_query(unsigned destination){
    BosAbiInfo abi;BosMemoryInfo memory;
    assert(invoke(BOS_CALL_ABI_QUERY,destination,sizeof abi,1,0,0)==BOS_OK);
    memcpy(&abi,user_memory+destination,sizeof abi);
    assert(abi.abi_major==1&&abi.abi_minor==1&&abi.user_bytes==4194304);
    assert((abi.features&(BOS_FEATURE_BEX2|BOS_FEATURE_MEMORY_INFO))==(BOS_FEATURE_BEX2|BOS_FEATURE_MEMORY_INFO));
    assert(abi.file_chunk_bytes==4096&&abi.replace_bytes==32768&&abi.path_bytes==128);
    assert(invoke(BOS_CALL_MEMORY_INFO,destination,sizeof memory,1,0,0)==BOS_OK);
    memcpy(&memory,user_memory+destination,sizeof memory);
    assert(memory.struct_size==128&&memory.version==1&&memory.format==2&&memory.page_bytes==4096);
    assert(memory.virtual_bytes==4194304&&memory.mapped_pages==current_task->plan.mapped_pages);
    assert(memory.owned_pages==memory.mapped_pages+2&&memory.table_pages==2&&memory.policy_pages==1024);
    assert(memory.pool_free_pages==host_physmem_stats().free&&memory.pool_total_pages==host_physmem_stats().total);
    for(unsigned i=0;i<4;i++)assert(!memory.reserved[i]);
    unsigned last=0;
    for(unsigned i=0;i<memory.region_count;i++){
        assert(memory.regions[i].bytes&&memory.regions[i].offset>=last);
        last=memory.regions[i].offset+memory.regions[i].bytes;
        assert(memory.regions[i].protection==(i==0?5:3));
    }
    for(unsigned i=memory.region_count;i<4;i++)assert(!memory.regions[i].bytes&&!memory.regions[i].offset&&!memory.regions[i].purpose&&!memory.regions[i].protection);
    memset(user_memory+destination,0xa5,sizeof memory+16);
    assert(invoke(BOS_CALL_MEMORY_INFO,destination,16,1,0,0)==BOS_OK);
    for(unsigned i=16;i<sizeof memory+16;i++)assert(user_memory[destination+i]==0xa5);
    memset(user_memory+destination,0xa5,sizeof memory+16);
    assert(invoke(BOS_CALL_MEMORY_INFO,destination,sizeof memory,2,0,0)==BOS_E_UNSUPPORTED);
    assert(invoke(BOS_CALL_MEMORY_INFO,destination,15,1,0,0)==BOS_E_INVALID);
    assert(invoke(BOS_CALL_MEMORY_INFO,destination,sizeof memory,1,1,0)==BOS_E_INVALID);
    for(unsigned i=0;i<sizeof memory+16;i++)assert(user_memory[destination+i]==0xa5);
}
static void copy_sites(void){
    unsigned work=current_task->plan.workspace.offset+0x80000,stack=4194304-2048;
    unsigned data=current_task->plan.data.offset,text=current_task->plan.text.offset;
    const char *path="/Documents/data.txt";unsigned length=strlen(path);
    unsigned places[]={data,work,stack};
    for(unsigned i=0;i<3;i++){
        unsigned p=places[i];memcpy(user_memory+p,path,length+1);
        assert(invoke(BOS_CALL_WRITE,p,length,0,0,0)==(int)length);
        assert(invoke(BOS_CALL_ARGUMENT,p,129,0,0,0)==(int)length);
        assert(!strcmp((char *)user_memory+p,path));
        assert(invoke(BOS_CALL_FILE_SIZE,p,length,0,0,0)==4096);
        assert(invoke(BOS_CALL_READ_FILE,p,length,p+512,12,0)==12);
        assert(!memcmp(user_memory+p+512,disk_bytes,12));
        assert(invoke(BOS_CALL_READ_FILE_AT,p,length,p+512,12,10)==12);
        assert(!memcmp(user_memory+p+512,disk_bytes+10,12));
        assert(invoke(BOS_CALL_WRITE_FILE,p,length,p+512,12,0)==12);
        assert(invoke(BOS_CALL_REPLACE_FILE,p,length,p+512,12,0)==12);
        stub_result=BOS_OK;
        assert(invoke(BOS_CALL_FILE_OPEN,p,length,3,p+256,sizeof(BosFileInfo))==BOS_OK);
        assert(invoke(BOS_CALL_FILE_INFO,BOS_HANDLE_TYPE_FILE|1,p+256,32,0,0)==BOS_OK);
        stub_result=3;assert(invoke(BOS_CALL_FILE_READ_AT,BOS_HANDLE_TYPE_FILE|1,p+512,3,0,0)==3);
        stub_result=BOS_OK;assert(invoke(BOS_CALL_FILE_REPLACE,BOS_HANDLE_TYPE_FILE|1,p+512,3,p+256,32)==BOS_OK);
        assert(invoke(BOS_CALL_SYNC_BEGIN,p+256,0,0,0,0)==BOS_OK);
        assert(invoke(BOS_CALL_SYNC_RELEASE,BOS_HANDLE_TYPE_OPERATION|1,0,0,0,0)==BOS_OK);
        check_query(p+512);
    }
    /* Entire virtual spans are checked before filesystem effects; table-only
     * rejections never dereference an absent or read-only host/guest address. */
    unsigned calls=stub_file_calls,writes=legacy_writes;
    assert(invoke(BOS_CALL_FILE_REPLACE,1,work,12,text,32)==BOS_E_INVALID);
    assert(stub_file_calls==calls);
    assert(invoke(BOS_CALL_FILE_READ_AT,1,text,12,0,0)==BOS_E_INVALID);
    assert(stub_file_calls==calls);
    assert(invoke(BOS_CALL_READ_FILE,work,length,text,12,0)==-1&&legacy_writes==writes);
    assert(invoke(BOS_CALL_WRITE,text,8,0,0,0)==8);
    assert(invoke(BOS_CALL_ARGUMENT,UINT32_MAX,0,0,0,0)==(int)length);
    assert(invoke(BOS_CALL_READ_FILE_AT,work,length,work+4090,12,0)==12);
    assert(!memcmp(user_memory+work+4090,disk_bytes,12));
    assert(invoke(BOS_CALL_FILE_READ_AT,1,work+4090,4097,0,0)==BOS_E_INVALID);
    assert(invoke(BOS_CALL_FILE_REPLACE,1,work,32769,stack,32)==BOS_E_INVALID);
    assert(invoke(BOS_CALL_MEMORY_INFO,text,128,1,0,0)==BOS_E_INVALID);
    user_memory[current_task->plan.workspace.offset]=(unsigned char)current_task->legacy_task_id;
}
static void check_persisted(void){
    assert(user_memory[current_task->plan.workspace.offset]==current_task->legacy_task_id);
}
static void *load(const char *path,unsigned *bytes){
    FILE *f=fopen(path,"rb");assert(f);assert(!fseek(f,0,SEEK_END));long n=ftell(f);assert(n>0);rewind(f);
    void *data=malloc((size_t)n);assert(data&&fread(data,1,(size_t)n,f)==(size_t)n);fclose(f);*bytes=(unsigned)n;return data;
}
int main(int argc,char **argv){
    assert(argc==5);unsigned ram=(unsigned)atoi(argv[1]),bytes,large_bytes,three_bytes;
    unsigned char *image=load(argv[2],&bytes),*large=load(argv[3],&large_bytes),*three=load(argv[4],&three_bytes);
    ProcessHandle untouched=0xabcdef;
    assert(process_create(image,bytes,0,0,&untouched)==PROCESS_CREATE_MEMORY&&untouched==0xabcdef);
    host_physmem_init(ram);
    for(unsigned i=0;i<sizeof disk_bytes;i++)disk_bytes[i]=(unsigned char)(i*13);
    ProcessHandle a=launch(image,bytes,5),b=launch(image,bytes,2),old=launch(bex1,sizeof bex1,7);
    NativeTask *ta=task_lookup(a),*tb=task_lookup(b);check_tables(ta,image);check_tables(tb,image);
    assert(ta->directory!=tb->directory&&ta->table!=tb->table);
    assert(host_physmem_stats().allocated==2*ta->plan.owned_pages+16);
    slice_check=copy_sites;slice_exit=0;
    unsigned copies=production_image_copies;
    assert(process_step(a)&&process_step(b)&&production_image_copies==copies);
    slice_check=check_persisted;assert(process_step(a)&&process_step(b));
    assert(production_image_copies==copies);
    assert(process_run(image,bytes,&legacy_io)==PROCESS_CREATE_UNSUPPORTED);
    assert(!process_run(bex1,sizeof bex1,&legacy_io));
    slice_check=check_persisted;assert(process_step(a)&&process_step(b));
    if(ram!=256){
        PhysmemStats before=host_physmem_stats();untouched=0xabcdef;
        assert(process_create(large,large_bytes,0,0,&untouched)==PROCESS_CREATE_MEMORY&&untouched==0xabcdef);
        PhysmemStats after=host_physmem_stats();assert(!memcmp(&before,&after,sizeof before));
        assert(process_status(a)==PROCESS_TASK_READY&&process_status(b)==PROCESS_TASK_READY);
        stop(a);ProcessHandle admitted=launch(large,large_bytes,5);check_tables(task_lookup(admitted),large);stop(admitted);
    }else{
        stop(a);ProcessHandle c=launch(three,three_bytes,5),d=launch(three,three_bytes,6);
        check_tables(task_lookup(c),three);check_tables(task_lookup(d),three);
        slice_check=copy_sites;assert(process_step(c)&&process_step(d));stop(c);stop(d);
    }
    slice_check=check_persisted;slice_exit=1;slice_value=7;assert(process_step(b));
    assert(!root_is_user&&process_status(b)==PROCESS_TASK_DONE);
    ProcessResult result;assert(process_get_result(b,&result)&&result.value==7&&result.reason==PROCESS_EXIT_APP);
    assert(process_reap(b));stop(old);assert(!host_physmem_stats().allocated);
    /* Pending owned-save wait cleanup is inactive and does not cancel service
     * state. Other-client durable behavior belongs to native_sync's real suite. */
    a=launch(image,bytes,0);b=launch(image,bytes,1);
    task_lookup(a)->state=PROCESS_TASK_SLEEPING;task_lookup(a)->wait_operation=BOS_HANDLE_TYPE_OPERATION|7;
    task_lookup(a)->wake=1000;stub_poll=BOS_PENDING;stop(a);assert(task_lookup(b)->resources_live);stop(b);
    assert(!host_physmem_stats().allocated&&enters==restores&&fpu_entries==fpu_returns);
    assert(!root_is_user);host_physmem_destroy();free(image);free(large);free(three);
    printf("Private address spaces: %u MiB profile, owned tables and sparse mappings, every syscall copy class, mixed BEX1/exec, inactive cleanup and capacity/reuse passed.\n",ram);
}
