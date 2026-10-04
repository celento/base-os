/* Production functions, not a model: hardware entry/FPU are explicit spies.
 * All inputs represent ordinary supported lifecycle and finite-capacity work. */
#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "program.h"
#include "platform.h"
#include "audio.h"
#include "fs.h"
static void released(unsigned owner);
#define NATIVE_OWNER_RELEASE_HOOK(owner) released(owner)
#include "native_platform_service_stubs.h"
static void backing_released(unsigned owner);
#define PROCESS_BACKING_RELEASE_HOOK(owner) backing_released(owner)
#include "process_backing_host.h"
#include "process_lifetime_types.inc"
static unsigned char user_memory[USER_CAPACITY];
#undef USER_BASE
#define USER_BASE ((uintptr_t)user_memory)
static NativeTask task_memory[PROCESS_TASKS];
static NativeTask *tasks=task_memory,*current_task;
static unsigned owner_serial,schedule_next;
static BosHandle synchronous_owner;
static const ProgramIO *output;
static unsigned canvas_width,canvas_height;
static int tasks_ready,active,process_result;
static uint32_t began,now;
static jmp_buf returned;
static unsigned incoming,outgoing,incoming_bytes,outgoing_bytes,leaves,fpu_saves,fpu_restores,publications;
static unsigned release_events,last_release;
static int kernel_restored=1;
static AudioStatus sound;
static unsigned audio_pauses;
static int slice_kind,slice_value;
enum { SLICE_YIELD, SLICE_SLEEP, SLICE_EXIT, SLICE_STOP, SLICE_WAIT, SLICE_STOP_YIELD, SLICE_STOP_EXIT, SLICE_SCATTER };
static unsigned char expected_image[USER_CAPACITY];
static int check_image;
static ProcessBinding seen_binding;
static ProcessHandle run_order[64];
static unsigned runs;
void kmemcpy(void *to,const void *from,int bytes){
    if(host_physmem_is_frame(from)){
        assert(bytes==(int)PHYS_PAGE_BYTES&&!active&&kernel_restored);
        if(to==user_memory)incoming++;
        if((uintptr_t)to>=(uintptr_t)user_memory&&
           (uintptr_t)to<(uintptr_t)user_memory+USER_CAPACITY)incoming_bytes+=bytes;
    }
    if(host_physmem_is_frame(to)&&
       (uintptr_t)from>=(uintptr_t)user_memory&&
       (uintptr_t)from<(uintptr_t)user_memory+USER_CAPACITY){
        assert(!active&&kernel_restored);outgoing_bytes+=bytes;
        if(from==user_memory)outgoing++;
    }
    host_physmem_copy(to,from,(unsigned)bytes);
}
void kmemset(void *to,int value,int bytes){memset(to,value,(size_t)bytes);}
uint32_t timer_ticks(void){return now;}
int fs_sync(void){return 0;}
static int file_call(unsigned call,unsigned a,unsigned b,unsigned c,unsigned d,unsigned e){
    (void)call;(void)a;(void)b;(void)c;(void)d;(void)e;return -1;
}
static void released(unsigned owner){
    assert(owner&&!active&&kernel_restored);
    release_events++;last_release=owner;
}
static void backing_released(unsigned owner){
    assert(owner&&!active&&kernel_restored);
    assert(physmem_core_owner_pages(&backing_core,owner)==TASK_BACKING_PAGES);
}
static void protect_memory(void){}
static void fpu_enter(NativeTask *task){
    assert(active&&kernel_restored&&task==current_task);kernel_restored=0;fpu_saves++;
}
static void fpu_leave(NativeTask *task){
    assert(active&&!kernel_restored&&task==current_task);
    assert(!task||task->resources_live);
    assert(!task||physmem_core_owner_pages(&backing_core,task->owner_id)==16);
    kernel_restored=1;fpu_restores++;
}
const AudioStatus *audio_status(void){return &sound;}
void audio_pause(int paused){sound.state=paused?AUDIO_PAUSED:AUDIO_PLAYING;audio_pauses++;}
static void process_leave(void) __attribute__((noreturn));
static void process_leave(void){
    assert(active&&!kernel_restored);
    assert(current_task?current_task->resources_live:synchronous_owner!=0);
    leaves++;longjmp(returned,1);
}
static int invoke(unsigned call,unsigned a,unsigned b){
    uint32_t r[FRAME_WORDS]={0};r[7]=call;r[4]=a;r[6]=b;r[12]=128;r[15]=0x1b;
    assert(process_interrupt(r)==1);return (int)r[7];
}
static int process_resume(const uint32_t *frame){
    assert(active&&current_task&&frame==current_task->frame);
    assert(physmem_core_owner_pages(&backing_core,current_task->owner_id)==16);
    if(check_image)assert(!memcmp(user_memory,expected_image,USER_CAPACITY));
    assert(runs<sizeof run_order/sizeof *run_order);run_order[runs++]=current_task->owner_id;
    assert(invoke(BOS_CALL_TASK_ID,0,0)==(int)current_task->io.binding.slot+1);
    if(setjmp(returned))return process_result;
    user_memory[400]++;
    switch(slice_kind){
    case SLICE_SCATTER:
        for(unsigned page=0;page<TASK_BACKING_PAGES;page++){
            unsigned offset=page*PHYS_PAGE_BYTES;
            user_memory[offset]=(unsigned char)(page+61);
            user_memory[offset+PHYS_PAGE_BYTES-1]=(unsigned char)(page+93);
        }
        invoke(BOS_CALL_YIELD,0,0);break;
    case SLICE_SLEEP:invoke(BOS_CALL_SLEEP,(unsigned)slice_value,0);break;
    case SLICE_EXIT:invoke(BOS_CALL_EXIT,(unsigned)slice_value,0);break;
    case SLICE_WAIT:invoke(BOS_CALL_SYNC_WAIT,BOS_HANDLE_TYPE_OPERATION|7,1000);break;
    case SLICE_STOP:case SLICE_STOP_YIELD:case SLICE_STOP_EXIT:
        assert(!process_request_stop(current_task->owner_id));
        assert(current_task->resources_live&&current_task->state==PROCESS_TASK_READY);
        if(slice_kind==SLICE_STOP_YIELD)invoke(BOS_CALL_YIELD,0,0);
        if(slice_kind==SLICE_STOP_EXIT)invoke(BOS_CALL_EXIT,27,0);
        /* Ordinary PIT suspension is not an explicit publication boundary. */
        {uint32_t timer[FRAME_WORDS]={0};timer[12]=32;timer[15]=0x1b;
         process_interrupt(timer);}
        break;
    default:invoke(BOS_CALL_YIELD,0,0);break;
    }
    assert(!"Each ordinary test slice must yield or exit");return 0;
}
static int process_enter(unsigned entry){
    assert(entry==16&&active&&!current_task&&synchronous_owner);
    assert(!physmem_core_owner_pages(&backing_core,synchronous_owner));
    assert(invoke(BOS_CALL_TASK_ID,0,0)==0);
    if(setjmp(returned))return process_result;
    invoke(BOS_CALL_EXIT,19,0);return 0;
}
#include "process_lifetime_ops.inc"
static unsigned char backing_byte(const NativeTask *task,unsigned offset){
    return *(unsigned char *)host_physmem_pointer((const void *)(uintptr_t)
        (task->backing[offset/PHYS_PAGE_BYTES]+offset%PHYS_PAGE_BYTES),1);
}
static uint32_t backing_hash(const NativeTask *task){
    uint32_t hash=2166136261u;
    for(unsigned offset=0;offset<USER_CAPACITY;offset++)hash=(hash^backing_byte(task,offset))*16777619u;
    return hash;
}
static void line(const ProcessBinding *binding,const char *text){(void)text;seen_binding=*binding;}
static void pixel(const ProcessBinding *binding,int x,int y,int color){(void)x;(void)y;(void)color;seen_binding=*binding;}
static void present(const ProcessBinding *binding){seen_binding=*binding;publications++;}
static void legacy_line(const char *text){(void)text;}
static void legacy_pixel(int x,int y,int color){(void)x;(void)y;(void)color;}
static const ProgramIO legacy_io={legacy_line,legacy_pixel,0,0,0,0};
static const unsigned char image[]={0x42,0x45,0x58,0x31,16,0,0,0,18,0,0,0,0,0,0,0,0xeb,0xfe};
static ProcessHandle create(unsigned slot,int start){
    ProcessHandle handle=0;assert(!process_create(image,sizeof image,0,0,&handle));
    ProcessIO io={{handle,slot,17},line,pixel,present,0,0};
    assert(process_bind(handle,&io));if(start)assert(process_start(handle));
    return handle;
}
static void reap(ProcessHandle handle,unsigned reason,int value){
    ProcessResult result={123,123};assert(process_get_result(handle,&result));
    assert(result.reason==reason&&result.value==value);
    assert(process_reap(handle)&&process_reap(handle));
    assert(process_status(handle)==PROCESS_TASK_EMPTY);
}
static void owned_binding(void){
    char argument[]="/Documents/copied.txt";ProcessHandle handle=0;
    assert(!process_create(image,sizeof image,argument,strlen(argument),&handle));
    NativeTask *task=task_lookup(handle);assert(task==tasks&&handle!=1);
    assert(process_status(handle)==PROCESS_TASK_CREATED&&!process_schedule_one());
    argument[1]='X';assert(!strcmp(task->argument,"/Documents/copied.txt"));
    assert(physmem_core_owner_pages(&backing_core,handle)==16);
    assert(host_physmem_stats().allocated==16);
    for(unsigned i=sizeof image;i<USER_CAPACITY;i++)assert(backing_byte(task,i)==0);
    assert(task->frame[17]==65520);
    ProcessIO io={{handle,5,31},line,pixel,present,0,0};
    unsigned released_before=release_events;
    assert(process_bind(handle,&io)&&task->legacy_task_id==6);
    assert(process_unbind(handle)&&task->resources_live&&release_events==released_before);
    assert(!process_start(handle)&&process_bind(handle,&io)&&process_start(handle));
    memset(&io,0,sizeof io); /* Callback table and context were copied. */
    slice_kind=SLICE_YIELD;unsigned copies=outgoing;
    assert(process_schedule_one()==handle&&outgoing==copies+1);
    assert(seen_binding.process==handle&&seen_binding.slot==5&&seen_binding.generation==31);
    assert(backing_byte(task,400)==1&&process_status(handle)==PROCESS_TASK_READY);
    assert(process_key(handle,'A')&&task->key_count==1);
    assert(process_request_stop(handle)&&release_events==released_before+2);
    assert(!physmem_core_owner_pages(&backing_core,handle));
    for(unsigned i=0;i<TASK_BACKING_PAGES;i++)assert(!task->backing[i]);
    assert(!task->key_count&&task->owner_id==handle&&!task->resources_live);
    assert(process_request_stop(handle)&&release_events==released_before+2);
    reap(handle,PROCESS_EXIT_STOP,PROCESS_TASK_STOPPED);
    ProcessHandle fresh=create(5,1);assert(fresh!=handle&&task_lookup(fresh)==task);
    assert(!process_key(handle,'B')&&process_request_stop(handle)&&process_reap(handle));
    assert(process_status(fresh)==PROCESS_TASK_READY&&task->key_count==0);
    assert(process_request_stop(fresh));reap(fresh,PROCESS_EXIT_STOP,PROCESS_TASK_STOPPED);
}
static void scheduling_and_lifetime(void){
    ProcessHandle a=create(5,1),b=create(1,1);schedule_next=0;slice_kind=SLICE_YIELD;
    assert(task_lookup(a)==tasks&&task_lookup(b)==tasks+1);
    assert(process_schedule_one()==a&&process_schedule_one()==b&&process_schedule_one()==a);
    assert(process_key(a,'x')&&process_key(b,'z'));
    now=100;slice_kind=SLICE_SLEEP;slice_value=1000;
    assert(process_step(a)&&process_status(a)==PROCESS_TASK_SLEEPING);
    assert(!process_step(a));now=169;assert(!process_step(a));
    slice_kind=SLICE_YIELD;now=170;assert(process_step(a));
    assert(task_lookup(a)->key_count==1&&task_lookup(b)->key_count==1);
    slice_kind=SLICE_WAIT;stub_poll=BOS_PENDING;assert(process_step(a));
    assert(process_status(a)==PROCESS_TASK_SLEEPING&&!process_step(a));
    stub_poll=BOS_OK;slice_kind=SLICE_YIELD;assert(process_step(a));
    assert(!task_lookup(a)->wait_operation&&task_lookup(a)->frame[7]==0);
    unsigned copies=outgoing,released_before=release_events,pubs=publications;
    slice_kind=SLICE_EXIT;slice_value=PROCESS_TASK_STOPPED;
    assert(process_step(a)&&outgoing==copies&&release_events==released_before+2);
    assert(publications==pubs+1&&process_status(a)==PROCESS_TASK_DONE);
    reap(a,PROCESS_EXIT_APP,PROCESS_TASK_STOPPED); /* App -4 is not Stop. */
    slice_kind=SLICE_STOP;copies=outgoing;released_before=release_events;
    assert(process_step(b)&&outgoing==copies&&release_events==released_before+2);
    reap(b,PROCESS_EXIT_STOP,PROCESS_TASK_STOPPED);
    assert(kernel_restored&&fpu_saves==fpu_restores&&incoming==leaves);
}
static void deferred_boundaries(void){
    /* An internal stop request made during an active slice is deferred. The
     * same slice's explicit boundary keeps its normal publication contract. */
    for(int mode=SLICE_STOP_YIELD;mode<=SLICE_STOP_EXIT;mode++){
        ProcessHandle handle=create(4,1);slice_kind=mode;
        unsigned copies=outgoing,pubs=publications,released_before=release_events;
        assert(process_step(handle)&&outgoing==copies);
        assert(publications==pubs+1&&release_events==released_before+2);
        if(mode==SLICE_STOP_EXIT)reap(handle,PROCESS_EXIT_APP,27);
        else reap(handle,PROCESS_EXIT_STOP,PROCESS_TASK_STOPPED);
    }
}
static void capacity_and_exec(void){
    ProcessHandle handles[PROCESS_TASKS];
    for(unsigned i=0;i<PROCESS_TASKS;i++)handles[i]=create(PROCESS_TASKS-1-i,1);
    ProcessCounts counts;process_counts(&counts);
    assert(counts.records==8&&counts.live==8&&counts.owned==8&&!counts.done);
    assert(host_physmem_stats().allocated==128&&host_physmem_stats().by_kind[PHYS_BEX1_BACKING]==128);
    ProcessHandle unchanged=0x12345678;
    assert(process_create(image,sizeof image,0,0,&unchanged)==-1&&unchanged==0x12345678);
    uint32_t checksums[PROCESS_TASKS];
    for(unsigned i=0;i<PROCESS_TASKS;i++)checksums[i]=backing_hash(task_lookup(handles[i]));
    sound.state=AUDIO_PLAYING;unsigned before=release_events;
    assert(process_run(image,sizeof image,&legacy_io)==19&&release_events==before+2);
    assert(!synchronous_owner&&audio_pauses==2&&sound.state==AUDIO_PLAYING);
    assert(host_physmem_stats().allocated==128);
    for(unsigned i=0;i<PROCESS_TASKS;i++)
        assert(backing_hash(task_lookup(handles[i]))==checksums[i]&&process_status(handles[i])==PROCESS_TASK_READY);
    for(unsigned i=0;i<PROCESS_TASKS;i++)assert(process_request_stop(handles[i]));
    process_counts(&counts);assert(counts.records==8&&counts.done==8&&!counts.owned&&!counts.live);
    assert(process_create(image,sizeof image,0,0,&unchanged)==-1&&unchanged==0x12345678);
    for(unsigned i=0;i<PROCESS_TASKS;i++)reap(handles[i],PROCESS_EXIT_STOP,PROCESS_TASK_STOPPED);
    process_counts(&counts);ProcessCounts zero={0};assert(!memcmp(&counts,&zero,sizeof counts));
    ProcessHandle abandoned=create(3,0);before=release_events;
    assert(process_unbind(abandoned)&&release_events==before);
    assert(process_request_stop(abandoned)&&release_events==before+2);
    reap(abandoned,PROCESS_EXIT_STOP,PROCESS_TASK_STOPPED);
    assert(last_release==abandoned&&stub_release_files==stub_release_sync);
    assert(!host_physmem_stats().allocated);
}
static void not_ready(void){
    ProcessHandle untouched=0x12345678;unsigned serial=owner_serial;
    assert(process_create(image,sizeof image,0,0,&untouched)==PROCESS_CREATE_MEMORY);
    assert(untouched==0x12345678&&owner_serial==serial+1&&!backing_allocations);
    ProcessCounts counts,zero={0};process_counts(&counts);
    assert(!memcmp(&counts,&zero,sizeof counts));
}
static void scattered_copy(void){
    /* Valid fragmentation: a kernel owner retains alternating frames. */
    BosHandle keeper=allocate_owner();uint32_t held[32];
    assert(physmem_core_alloc(&backing_core,keeper,PHYS_BEX1_BACKING,32,held)==PHYS_OK);
    for(unsigned i=0;i<32;i+=2)
        assert(physmem_core_release(&backing_core,keeper,PHYS_BEX1_BACKING,held+i,1)==PHYS_OK);
    unsigned char file[PROCESS_IMAGE_LIMIT];
    for(unsigned i=0;i<sizeof file;i++)file[i]=(unsigned char)(i*37+i/PHYS_PAGE_BYTES);
    const uint32_t header[]={0x31584542,16,sizeof file,0};memcpy(file,header,sizeof header);
    unsigned zeros=backing_zeroes;ProcessHandle process=0;
    assert(!process_create(file,sizeof file,0,0,&process)&&backing_zeroes==zeros+16);
    NativeTask *task=task_lookup(process);
    for(unsigned i=0;i<TASK_BACKING_PAGES;i++)assert(task->backing[i]==held[i*2]);
    memset(expected_image,0,sizeof expected_image);memcpy(expected_image,file,sizeof file);
    unsigned char actual[USER_CAPACITY];task_image_read(task,actual);
    assert(!memcmp(actual,expected_image,sizeof actual));
    ProcessIO io={{process,7,32},line,pixel,present,0,0};
    assert(process_bind(process,&io)&&process_start(process));
    check_image=1;slice_kind=SLICE_SCATTER;unsigned in=incoming_bytes,out=outgoing_bytes;
    assert(process_step(process));
    assert(incoming_bytes==in+65536&&outgoing_bytes==out+65536);
    expected_image[400]++;
    for(unsigned i=0;i<TASK_BACKING_PAGES;i++){
        expected_image[i*PHYS_PAGE_BYTES]=(unsigned char)(i+61);
        expected_image[(i+1)*PHYS_PAGE_BYTES-1]=(unsigned char)(i+93);
    }
    task_image_read(task,actual);assert(!memcmp(actual,expected_image,sizeof actual));
    slice_kind=SLICE_YIELD;assert(process_step(process));check_image=0;
    assert(process_request_stop(process));reap(process,PROCESS_EXIT_STOP,PROCESS_TASK_STOPPED);
    assert(physmem_core_release_owner(&backing_core,keeper)==PHYS_OK);
    assert(!host_physmem_stats().allocated);
    /* Partial last image page: bytes immediately after the file remain zero. */
    uint32_t short_size=PHYS_PAGE_BYTES+3;memcpy(file+8,&short_size,sizeof short_size);
    assert(!process_create(file,short_size,0,0,&process));task=task_lookup(process);
    task_image_read(task,actual);assert(!memcmp(actual,file,short_size));
    for(unsigned i=short_size;i<USER_CAPACITY;i++)assert(!actual[i]);
    assert(process_request_stop(process));reap(process,PROCESS_EXIT_STOP,PROCESS_TASK_STOPPED);
    /* A re-used physical frame is zeroed for a fresh independent owner. */
    process=create(0,0);task=task_lookup(process);
    for(unsigned i=sizeof image;i<USER_CAPACITY;i++)assert(!backing_byte(task,i));
    assert(process_request_stop(process));reap(process,PROCESS_EXIT_STOP,PROCESS_TASK_STOPPED);
}
static void finite_backing_capacity(void){
    ProcessHandle survivor=create(2,1);NativeTask *task=task_lookup(survivor);
    unsigned char before[USER_CAPACITY],after[USER_CAPACITY];task_image_read(task,before);
    BosHandle keeper=allocate_owner();uint32_t held[PHYS_BATCH_MAX];
    unsigned count=host_physmem_stats().free-15;
    assert(count<=PHYS_BATCH_MAX);
    assert(physmem_core_alloc(&backing_core,keeper,PHYS_BEX1_BACKING,count,held)==PHYS_OK);
    PhysmemStats prior=host_physmem_stats();unsigned serial=owner_serial,zeros=backing_zeroes;
    unsigned files=release_events;ProcessHandle untouched=0x12345678;
    assert(process_create(image,sizeof image,0,0,&untouched)==PROCESS_CREATE_MEMORY);
    assert(untouched==0x12345678&&owner_serial==serial+1&&backing_zeroes==zeros&&release_events==files);
    PhysmemStats after_failure=host_physmem_stats();assert(!memcmp(&prior,&after_failure,sizeof prior));
    ProcessCounts counts;process_counts(&counts);assert(counts.records==1&&counts.live==1&&counts.owned==1);
    task_image_read(task,after);assert(!memcmp(before,after,sizeof before));
    assert(physmem_core_release(&backing_core,keeper,PHYS_BEX1_BACKING,held,count)==PHYS_OK);
    ProcessHandle retry=create(3,0);assert(retry!=survivor);
    assert(process_request_stop(retry));reap(retry,PROCESS_EXIT_STOP,PROCESS_TASK_STOPPED);
    assert(process_request_stop(survivor));reap(survivor,PROCESS_EXIT_STOP,PROCESS_TASK_STOPPED);
    assert(!host_physmem_stats().allocated);
}
static void failed_attachment_and_reset(void){
    ProcessHandle failed=0;assert(!process_create(image,sizeof image,0,0,&failed));
    /* Ordinary launch rollback: an unavailable callback prevents binding. */
    ProcessIO missing={{failed,1,1},0,pixel,present,0,0};unsigned releases=backing_releases;
    assert(!process_bind(failed,&missing)&&!process_start(failed));
    assert(process_request_stop(failed)&&backing_releases==releases+1);
    reap(failed,PROCESS_EXIT_STOP,PROCESS_TASK_STOPPED);
    assert(backing_releases==releases+1&&!host_physmem_stats().allocated);
    ProcessHandle a=create(2,1),b=create(4,0);unsigned serial=owner_serial;
    releases=backing_releases;task_reset_all();
    assert(backing_releases==releases+2&&!host_physmem_stats().allocated&&!tasks_ready);
    assert(owner_serial==serial&&process_status(a)==PROCESS_TASK_EMPTY&&process_status(b)==PROCESS_TASK_EMPTY);
    task_reset_all();assert(backing_releases==releases+2);
    ProcessHandle fresh=create(2,0);assert(fresh!=a&&fresh!=b);
    assert(process_request_stop(a)&&process_reap(a)&&process_status(fresh)==PROCESS_TASK_CREATED);
    assert(process_request_stop(fresh));reap(fresh,PROCESS_EXIT_STOP,PROCESS_TASK_STOPPED);
}
int main(int argc,char **argv){
    unsigned ram=argc>1?(unsigned)atoi(argv[1]):64;
    not_ready();host_physmem_init(ram);
    assert(host_physmem_stats().total==(ram==64?798u:1054u));
    owned_binding();scheduling_and_lifetime();deferred_boundaries();capacity_and_exec();scattered_copy();finite_backing_capacity();failed_attachment_and_reset();
    assert(backing_allocations==backing_releases&&!host_physmem_stats().allocated);
    printf("BEX1 owned backing: %u MiB, %u total/free pages, 16 per task, real scattered-page copy, finite exhaustion, rollback and reset passed.\n",ram,host_physmem_stats().free);
    host_physmem_destroy();
    puts("Process lifetime: independent records/slots, copied attachments, exact stale lookup, round-robin, sleep/wait/input, deferred cleanup/no final copy, active-stop publication/exit precedence, retained completion, finite capacity and legacy exec passed.");
    return 0;
}
