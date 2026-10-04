#include "program.h"
#include "audio.h"
#include "platform.h"
#include "fs.h"
#include "native_files.h"
#include "native_sync.h"
#include "../sdk/baseos_abi.h"
extern unsigned char gdt_user_code[],gdt_user_data[],gdt_tss[];
extern int process_enter(unsigned entry);
extern int process_resume(const uint32_t *frame);
extern void process_leave(void) __attribute__((noreturn));
unsigned process_saved_sp;
int process_result;
static int active;
static uint32_t began;
static const ProgramIO *output;
static unsigned canvas_width,canvas_height;
/* Only ring-3 execution is preempted. Syscalls finish atomically on the kernel
 * stack before the desktop regains control. Task images never overlap USER_BASE. */
#define TASK_KEYS 32
#define FRAME_WORDS 19
#define TASK_MAX_SLEEP_MS 60000u
typedef struct { unsigned char bytes[108]; } FpuState;
typedef struct {
    unsigned char image[USER_CAPACITY];
    uint32_t frame[FRAME_WORDS];
    FpuState fpu;
    ProcessIO io;
    uint32_t wake;
    BosHandle owner_id, wait_operation;
    unsigned canvas_width,canvas_height;
    int state, result, fpu_ready;
    unsigned exit_reason, legacy_task_id;
    int resources_live, bound, stop_requested;
    unsigned key_head, key_count;
    unsigned char keys[TASK_KEYS];
    unsigned argument_length;
    char argument[PROCESS_ARGUMENT_MAX+1];
} NativeTask;
static NativeTask *tasks=(NativeTask *)TASK_BASE;
static NativeTask *current_task;
static int tasks_ready, have_fpu;
static unsigned schedule_next;
static unsigned owner_serial;
static BosHandle synchronous_owner;
/* Allocation domains never wrap or reset, even when a display slot is reused. */
static BosHandle allocate_owner(void) {
    if(owner_serial==BOS_HANDLE_SERIAL_MAX)return BOS_HANDLE_INVALID;
    return BOS_HANDLE_TYPE_PROCESS | ++owner_serial;
}
static BosHandle current_owner(void) {
    return current_task?current_task->owner_id:synchronous_owner;
}
static void release_owner(BosHandle owner) {
    if(!owner)return;
    native_files_release_owner(owner);
    native_sync_owner_release(owner);
}
/* A completion retains its identity/result until the display consumes it.
 * Resource release is legal only after the active process has returned. */
static void task_release(NativeTask *task) {
    if(task->resources_live){release_owner(task->owner_id);task->resources_live=0;}
    task->wait_operation=0;
    task->key_head=task->key_count=0;
}
static void task_mark_exit(NativeTask *task,int result,unsigned reason) {
    if(task->state==PROCESS_TASK_DONE||task->state==PROCESS_TASK_EXITING)return;
    task->result=result;task->exit_reason=reason;task->state=PROCESS_TASK_EXITING;
}
static void task_finalize(NativeTask *task) {
    if(active||task->state!=PROCESS_TASK_EXITING)return;
    task_release(task);
    task->state=PROCESS_TASK_DONE;
}
static FpuState kernel_fpu, sync_fpu;
static unsigned kernel_cr0;
_Static_assert(TASK_BASE+sizeof(NativeTask)*PROCESS_TASKS<=TASK_INTERRUPT_STACK_BASE,"native tasks overlap syscall stack");
#ifdef TASK_PAGE_METADATA_BASE
_Static_assert(TASK_BASE+sizeof(NativeTask)*PROCESS_TASKS<=TASK_PAGE_METADATA_BASE,"native tasks overlap owned-page metadata");
#endif
_Static_assert(TASK_INTERRUPT_STACK_BASE+TASK_INTERRUPT_STACK_CAPACITY<=TASK_BASE+TASK_CAPACITY,"syscall stack exceeds task arena");
_Static_assert(TASK_BASE+TASK_CAPACITY<=RAM_REQUIRED_END,"native task arena must be boot-validated");
static void fpu_enter(NativeTask *task) {
    if(!have_fpu)return;
    __asm__ volatile("mov %%cr0,%0":"=r"(kernel_cr0));
    unsigned enabled=(kernel_cr0&~12u)|0x22u; /* x87 available, #MF exceptions */
    __asm__ volatile("mov %0,%%cr0"::"r"(enabled):"memory");
    __asm__ volatile("fnsave %0":"=m"(kernel_fpu)::"memory");
    if(task&&task->fpu_ready)__asm__ volatile("frstor %0"::"m"(task->fpu):"memory");
    else {
        /* FNINIT alone marks registers empty but leaves their old bits readable
         * through FNSAVE. Zero all eight slots before exposing a fresh context. */
        __asm__ volatile("fninit\n\tfldz\n\tfldz\n\tfldz\n\tfldz\n\tfldz\n\tfldz\n\tfldz\n\tfldz\n\tfninit":::"memory");
    }
}
static void fpu_leave(NativeTask *task) {
    if(!have_fpu)return;
    FpuState *state=task?&task->fpu:&sync_fpu;
    __asm__ volatile("fnsave %0":"=m"(*state)::"memory");
    if(task)task->fpu_ready=1;
    __asm__ volatile("frstor %0"::"m"(kernel_fpu):"memory");
    __asm__ volatile("mov %0,%%cr0"::"r"(kernel_cr0):"memory");
}
/* Syscall-side device polling can decode an MP3 frame (about 17 KiB stack).
 * Keep a separately reserved 64 KiB exception/syscall stack out of kernel BSS. */
static unsigned char *const kernel_stack=(unsigned char *)TASK_INTERRUPT_STACK_BASE;
/* 32-bit TSS; I/O bitmap offset equals descriptor size, denying all ports. */
static unsigned char tss[104] __attribute__((aligned(4)));
static int paging_ready;
static void protect_memory(void){
    if(paging_ready)return;
    uint32_t *directory=(uint32_t *)PAGING_BASE,*user=(uint32_t *)(PAGING_BASE+4096);
    /* Identity-map the kernel and devices as supervisor-only 4MB pages.
     * The user region uses 4KB pages, with only its 64KB marked accessible. */
    for(unsigned i=0;i<1024;i++){directory[i]=(i<<22)|0x83;user[i]=0;}
    for(unsigned i=0;i<USER_CAPACITY/4096;i++)user[i]=(USER_BASE+i*4096)|7;
    directory[USER_BASE>>22]=(PAGING_BASE+4096)|7;
    unsigned cr4,cr0;
    __asm__ volatile("mov %%cr4,%0":"=r"(cr4));
    cr4|=0x10; /* PSE: Pentium or newer. */
    __asm__ volatile("mov %0,%%cr4"::"r"(cr4):"memory");
    __asm__ volatile("mov %0,%%cr3"::"r"(PAGING_BASE):"memory");
    __asm__ volatile("mov %%cr0,%0":"=r"(cr0));
    cr0|=0x80010000u;
    __asm__ volatile("mov %0,%%cr0"::"r"(cr0):"memory");
    paging_ready=1;
}
static void descriptor(unsigned char *p,unsigned base,unsigned limit,unsigned access,unsigned flags){
    p[0]=limit;p[1]=limit>>8;p[2]=base;p[3]=base>>8;p[4]=base>>16;
    p[5]=access;p[6]=((limit>>16)&15)|flags;p[7]=base>>24;
}
void process_init(void){
    if(active)return;
    if(tasks_ready){
        for(unsigned i=0;i<PROCESS_TASKS;i++)task_release(tasks+i);
        kmemset(tasks,0,sizeof(NativeTask)*PROCESS_TASKS);
        tasks_ready=0;
    }
    schedule_next=0;current_task=0;output=0;
    release_owner(synchronous_owner);synchronous_owner=0;
    native_files_init();
    unsigned a,b,c,d;
    __asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(1));
    have_fpu=(d&1)!=0;
    descriptor(gdt_user_code,USER_BASE,USER_CAPACITY-1,0xfa,0x40);
    descriptor(gdt_user_data,USER_BASE,USER_CAPACITY-1,0xf2,0x40);
    *(uint32_t *)(tss+4)=(uintptr_t)(kernel_stack+TASK_INTERRUPT_STACK_CAPACITY);
    *(uint32_t *)(tss+8)=0x10;
    *(uint16_t *)(tss+102)=sizeof(tss);
    descriptor(gdt_tss,(uintptr_t)tss,sizeof(tss)-1,0x89,0);
    __asm__ volatile("ltr %%ax"::"a"(0x28):"memory");
}
static int valid_image(const void *file,unsigned bytes,uint32_t h[4]) {
    if(!file||bytes<16||bytes>PROCESS_IMAGE_LIMIT)return 0;
    kmemcpy(h,file,16);
    return h[0]==0x31584542&&h[1]>=16&&h[1]<bytes&&h[2]==bytes&&!h[3];
}
int process_run(const void *file,unsigned bytes,const ProgramIO *io){
    uint32_t h[4];
    if(active||!io||!io->print||!io->plot||!valid_image(file,bytes,h))return -2;
    synchronous_owner=allocate_owner();
    if(!synchronous_owner)return -2;
    protect_memory();
    kmemset((void *)USER_BASE,0,USER_CAPACITY);
    kmemcpy((void *)USER_BASE,file,bytes);
    canvas_width=PROGRAM_CANVAS_DEFAULT_WIDTH;canvas_height=PROGRAM_CANVAS_DEFAULT_HEIGHT;
    output=io;process_result=0;began=timer_ticks();current_task=0;active=1;
    int resume_audio=audio_status()->state==AUDIO_PLAYING;
    if(resume_audio)audio_pause(1);
    fpu_enter(0);
    int result=process_enter(h[1]);
    fpu_leave(0);active=0;
    release_owner(synchronous_owner);synchronous_owner=0;output=0;
    if(resume_audio)audio_pause(0);
    return result;
}
static NativeTask *task_lookup(ProcessHandle process) {
    if(!tasks_ready||!process)return 0;
    for(unsigned i=0;i<PROCESS_TASKS;i++)
        if(tasks[i].state!=PROCESS_TASK_EMPTY&&tasks[i].owner_id==process)return tasks+i;
    return 0;
}
int process_create(const void *file,unsigned bytes,const char *argument,
                   unsigned argument_length,ProcessHandle *out_process) {
    uint32_t h[4];
    if(active||!out_process||!valid_image(file,bytes,h))return -2;
    if(argument_length>PROCESS_ARGUMENT_MAX||
       (argument_length&&(!argument||argument[0]!='/')))return -2;
    for(unsigned i=0;i<argument_length;i++)
        if(argument[i]<32||argument[i]>126)return -2;
    if(!tasks_ready){kmemset(tasks,0,sizeof(NativeTask)*PROCESS_TASKS);tasks_ready=1;}
    NativeTask *task=0;
    for(unsigned i=0;i<PROCESS_TASKS;i++)
        if(tasks[i].state==PROCESS_TASK_EMPTY){task=tasks+i;break;}
    if(!task)return -1;
    BosHandle owner_id=allocate_owner();
    if(!owner_id)return -2;
    kmemset(task,0,sizeof(*task));
    task->state=PROCESS_TASK_CREATING;task->owner_id=owner_id;task->resources_live=1;
    kmemcpy(task->image,file,bytes);
    if(argument_length)kmemcpy(task->argument,argument,argument_length);
    task->argument_length=argument_length;
    task->frame[8]=task->frame[9]=task->frame[10]=task->frame[11]=0x23;
    task->frame[14]=h[1];task->frame[15]=0x1b;task->frame[16]=0x202;
    task->frame[17]=USER_CAPACITY-16;task->frame[18]=0x23;
    task->canvas_width=PROGRAM_CANVAS_DEFAULT_WIDTH;task->canvas_height=PROGRAM_CANVAS_DEFAULT_HEIGHT;
    task->state=PROCESS_TASK_CREATED;
    *out_process=owner_id;
    return 0;
}
int process_bind(ProcessHandle process,const ProcessIO *io) {
    NativeTask *task=task_lookup(process);
    if(active||!task||task->state!=PROCESS_TASK_CREATED||task->bound||!io||
       !io->print||!io->plot||io->binding.process!=process||
       io->binding.slot>=PROCESS_TASKS||!io->binding.generation)return 0;
    task->io=*io;task->legacy_task_id=io->binding.slot+1;task->bound=1;
    return 1;
}
int process_unbind(ProcessHandle process) {
    NativeTask *task=task_lookup(process);
    if(active||!task||task->state!=PROCESS_TASK_CREATED)return 0;
    kmemset(&task->io,0,sizeof task->io);task->legacy_task_id=0;task->bound=0;
    return 1;
}
int process_start(ProcessHandle process) {
    NativeTask *task=task_lookup(process);
    if(active||!task||task->state!=PROCESS_TASK_CREATED||!task->bound)return 0;
    task->state=PROCESS_TASK_READY;return 1;
}
int process_status(ProcessHandle process) {
    NativeTask *task=task_lookup(process);return task?task->state:PROCESS_TASK_EMPTY;
}
int process_get_result(ProcessHandle process,ProcessResult *out) {
    NativeTask *task=task_lookup(process);
    if(!out||!task||task->state!=PROCESS_TASK_DONE)return 0;
    out->value=task->result;out->reason=task->exit_reason;return 1;
}
void process_counts(ProcessCounts *out) {
    if(!out)return;
    kmemset(out,0,sizeof *out);
    if(!tasks_ready)return;
    for(unsigned i=0;i<PROCESS_TASKS;i++){
        const NativeTask *task=tasks+i;
        if(task->state==PROCESS_TASK_EMPTY)continue;
        out->records++;out->owned+=task->resources_live!=0;
        if(task->state==PROCESS_TASK_CREATED||task->state==PROCESS_TASK_CREATING)out->created++;
        else if(task->state==PROCESS_TASK_READY||task->state==PROCESS_TASK_SLEEPING)out->live++;
        else if(task->state==PROCESS_TASK_EXITING)out->exiting++;
        else if(task->state==PROCESS_TASK_DONE)out->done++;
    }
}
/* Polling an owned completion never drives disk I/O. A sleeping operation
 * waiter stores no borrowed pointer or live kernel stack between slices. */
static int task_wake(NativeTask *task) {
    if(task->state!=PROCESS_TASK_SLEEPING)return task->state==PROCESS_TASK_READY;
    if(task->wait_operation){
        int result=native_sync_poll(task->owner_id,task->wait_operation);
        if(result==BOS_PENDING){
            if((int32_t)(timer_ticks()-task->wake)<0)return 0;
            result=BOS_E_TIMEOUT;
        }
        task->frame[7]=(unsigned)result;
        task->wait_operation=0;
    }else if((int32_t)(timer_ticks()-task->wake)<0)return 0;
    task->state=PROCESS_TASK_READY;
    return 1;
}
int process_step(ProcessHandle process) {
    NativeTask *task=task_lookup(process);
    if(active||!task)return 0;
    if(task->stop_requested){
        task_mark_exit(task,PROCESS_TASK_STOPPED,PROCESS_EXIT_STOP);task_finalize(task);return 0;
    }
    if(!task_wake(task))return 0;
    protect_memory();
    kmemcpy((void *)USER_BASE,task->image,USER_CAPACITY);
    canvas_width=task->canvas_width;canvas_height=task->canvas_height;
    output=0;current_task=task;active=1;process_result=0;
    fpu_enter(task);
    process_resume(task->frame);
    /* First return to the kernel context, then save or release owned backing.
     * A completed image is never copied through a future freed page list. */
    fpu_leave(task);active=0;current_task=0;
    if(task->stop_requested)task_mark_exit(task,PROCESS_TASK_STOPPED,PROCESS_EXIT_STOP);
    if(task->state==PROCESS_TASK_EXITING)task_finalize(task);
    else kmemcpy(task->image,(const void *)USER_BASE,USER_CAPACITY);
    return 1;
}
ProcessHandle process_schedule_one(void) {
    if(active||!tasks_ready)return 0;
    for(unsigned i=0;i<PROCESS_TASKS;i++){
        unsigned index=(schedule_next+i)%PROCESS_TASKS;
        NativeTask *task=tasks+index;
        if(!process_step(task->owner_id))continue;
        schedule_next=(index+1)%PROCESS_TASKS;
        return task->owner_id;
    }
    return 0;
}
int process_key(ProcessHandle process,int key) {
    NativeTask *task=task_lookup(process);
    if(!task||(task->state!=PROCESS_TASK_READY&&task->state!=PROCESS_TASK_SLEEPING))return 0;
    /* A full queue drops the newest key; task input never escapes to another owner. */
    if(key>0&&key<=255&&task->key_count<TASK_KEYS){
        task->keys[(task->key_head+task->key_count)%TASK_KEYS]=(unsigned char)key;
        task->key_count++;
    }
    return 1;
}
int process_request_stop(ProcessHandle process) {
    NativeTask *task=task_lookup(process);
    if(!task)return 1;
    if(active){task->stop_requested=1;return 0;}
    task_mark_exit(task,PROCESS_TASK_STOPPED,PROCESS_EXIT_STOP);
    task_finalize(task);return 1;
}
int process_reap(ProcessHandle process) {
    NativeTask *task=task_lookup(process);
    if(!task)return 1;
    if(active||task->state!=PROCESS_TASK_DONE)return 0;
    kmemset(task,0,sizeof(*task));return 1;
}
/* Native callbacks carry a copied attachment. Synchronous BASIC/exec keeps its
 * small ProgramIO adapter and never borrows a desktop process's callbacks. */
static void output_print(const char *text) {
    if(current_task)current_task->io.print(&current_task->io.binding,text);
    else if(output&&output->print)output->print(text);
}
static void output_plot(int x,int y,int color) {
    if(current_task)current_task->io.plot(&current_task->io.binding,x,y,color);
    else if(output&&output->plot)output->plot(x,y,color);
}
static void output_present(void) {
    if(current_task){
        if(current_task->io.present)current_task->io.present(&current_task->io.binding);
    }else if(output&&output->present)output->present();
}
static int output_resize(int width,int height) {
    if(current_task)return current_task->io.resize?
        current_task->io.resize(&current_task->io.binding,width,height):-1;
    return output&&output->resize?output->resize(width,height):-1;
}
static void output_rect(unsigned x,unsigned y,unsigned width,unsigned height,int color) {
    if(current_task&&current_task->io.rect)
        current_task->io.rect(&current_task->io.binding,(int)x,(int)y,(int)width,(int)height,color);
    else if(!current_task&&output&&output->rect)
        output->rect((int)x,(int)y,(int)width,(int)height,color);
    else for(unsigned row=0;row<height;row++)for(unsigned col=0;col<width;col++)
        output_plot((int)(x+col),(int)(y+row),color);
}
static void task_suspend(uint32_t *frame,int state) __attribute__((noreturn));
static void task_suspend(uint32_t *frame,int state) {
    kmemcpy(current_task->frame,frame,FRAME_WORDS*sizeof(uint32_t));
    current_task->state=state;
    process_leave();
}
static void finish(int result,unsigned reason) __attribute__((noreturn));
static void finish(int result,unsigned reason) {
    process_result=result;
    if(current_task)task_mark_exit(current_task,result,reason);
    /* Synchronous and desktop owners remain live through process_leave. */
    process_leave();
}
static int user_range(unsigned offset, unsigned bytes) {
    return offset <= USER_CAPACITY && bytes <= USER_CAPACITY-offset;
}
static int user_path(unsigned offset, unsigned length, char path[129]) {
    if (!length || length > 128 || !user_range(offset,length)) return 0;
    const char *source=(const char *)(USER_BASE+offset);
    for(unsigned i=0;i<length;i++) {
        if(source[i]<32 || source[i]>126) return 0;
        path[i]=source[i];
    }
    path[length]=0;
    return path[0]=='/';
}
static int document_parent(int parent) {
    int documents=fs_find_child(fs_root(),"Documents");
    if(documents<0 || !fs_is_dir(documents)) return 0;
    for(int i=0;i<FS_MAX_NODES && parent>=0;i++) {
        if(parent==documents) return 1;
        parent=fs_parent(parent);
    }
    return 0;
}
static int file_call(unsigned call,unsigned path_offset,unsigned path_length,
                     unsigned buffer,unsigned length,unsigned offset) {
    char path[129];
    if(!user_path(path_offset,path_length,path)) return -1;
    unsigned limit=call==BOS_CALL_REPLACE_FILE?PROCESS_DOCUMENT_MAX:PROCESS_FILE_CHUNK_MAX;
    if(call!=8 && (length>limit || !user_range(buffer,length))) return -1;
    int id=fs_resolve(fs_root(),path);
    if(call==BOS_CALL_READ_FILE || call==BOS_CALL_FILE_SIZE || call==BOS_CALL_READ_FILE_AT) {
        if(!fs_valid(id)||fs_is_dir(id)||fs_is_app(id)) return -1;
        if(call==BOS_CALL_FILE_SIZE) return fs_size(id);
        unsigned size=(unsigned)fs_size(id);
        /* Subtract before adding: even an offset near UINT_MAX is a safe EOF. */
        if(offset>=size)return 0;
        size-=offset;if(size>length)size=length;
        kmemcpy((void *)(USER_BASE+buffer),fs_data(id)+offset,(int)size);
        return (int)size;
    }
    char name[FS_NAME_LEN];int parent=fs_destination(fs_root(),path,name);
    if(!document_parent(parent)) return -1;
    int created=id<0;
    if(created)id=fs_create(parent,name);
    if(id<0)return id;
    int result=fs_write(id,(const char *)(USER_BASE+buffer),(int)length);
    if(result<0&&created)fs_delete(id);
    return result;
}
/* Query can be consumed through a known prefix without exposing physical
 * arenas, terminal slots, raw filesystem ids or implementation tickets. */
static int abi_query(unsigned buffer,unsigned capacity,unsigned major,unsigned reserved0,unsigned reserved1) {
    if(major!=BOS_ABI_MAJOR)return BOS_E_UNSUPPORTED;
    if(reserved0||reserved1||capacity<BOS_ABI_QUERY_MIN_SIZE||!user_range(buffer,capacity))
        return BOS_E_INVALID;
    BosAbiInfo info;
    kmemset(&info,0,sizeof(info));
    info.struct_size=sizeof(info);info.abi_major=BOS_ABI_MAJOR;info.abi_minor=BOS_ABI_MINOR;
    info.features=BOS_FEATURE_VERSIONED_FILES|BOS_FEATURE_PROCESS_ID;
    info.context=current_task?BOS_CONTEXT_DESKTOP_TASK:BOS_CONTEXT_LEGACY_EXEC;
    info.process=current_owner();
    info.user_bytes=USER_CAPACITY;info.image_bytes=PROCESS_IMAGE_LIMIT;
    info.stack_reserved_bytes=16384;info.path_bytes=NATIVE_FILE_PATH_MAX;
    info.file_chunk_bytes=NATIVE_FILE_READ_MAX;
    info.replace_bytes=fs_file_limit()<NATIVE_FILE_REPLACE_MAX?fs_file_limit():NATIVE_FILE_REPLACE_MAX;
    info.file_bytes=fs_file_limit();
    info.files_per_process=NATIVE_FILE_PER_OWNER;info.files_total=NATIVE_FILE_CAPACITY;
    info.ticks_per_second=TIMER_HZ;info.processes_total=PROCESS_TASKS+1;
    if(current_task&&native_sync_available()){
        info.features|=BOS_FEATURE_OWNED_SYNC|BOS_FEATURE_OPERATION_WAIT;
        info.operations_per_process=native_sync_per_owner_limit();
        info.operations_total=native_sync_capacity();info.wait_milliseconds=TASK_MAX_SLEEP_MS;
    }
    unsigned bytes=capacity<sizeof(info)?capacity:sizeof(info);
    kmemcpy((void *)(USER_BASE+buffer),&info,(int)bytes);
    return BOS_OK;
}
static int native_file_call(unsigned call,unsigned a,unsigned b,unsigned c,unsigned d,unsigned e) {
    BosFileInfo info;int result;
    BosHandle owner=current_owner();
    if(call==BOS_CALL_FILE_OPEN){
        char path[NATIVE_FILE_PATH_MAX+1];
        if(e<sizeof(info)||!user_range(d,e)||!user_path(a,b,path))return BOS_E_INVALID;
        result=native_file_open(owner,path,c,&info);
        if(result==BOS_OK)kmemcpy((void *)(USER_BASE+d),&info,sizeof(info));
        return result;
    }
    if(call==BOS_CALL_FILE_INFO){
        if(d||e||c<sizeof(info)||!user_range(b,c))return BOS_E_INVALID;
        result=native_file_info(owner,a,&info);
        if(result==BOS_OK)kmemcpy((void *)(USER_BASE+b),&info,sizeof(info));
        return result;
    }
    if(call==BOS_CALL_FILE_READ_AT){
        if(e||c>NATIVE_FILE_READ_MAX||!user_range(b,c))return BOS_E_INVALID;
        return native_file_read_at(owner,a,d,(void *)(USER_BASE+b),c);
    }
    if(call==BOS_CALL_FILE_REPLACE){
        if(c>NATIVE_FILE_REPLACE_MAX||!user_range(b,c)||e<sizeof(info)||!user_range(d,e))
            return BOS_E_INVALID;
        result=native_file_replace(owner,a,(const void *)(USER_BASE+b),c,&info);
        if(result==BOS_OK)kmemcpy((void *)(USER_BASE+d),&info,sizeof(info));
        return result;
    }
    if(b||c||d||e)return BOS_E_INVALID;
    return native_file_close(owner,a);
}
/* Offsets match InterruptFrame: 8 registers, four segment slots, vector/error,
 * then EIP, CS, EFLAGS and the user SS/ESP on a privilege transition. */
int process_interrupt(uint32_t *r){
    if(!active || (r[15]&3)!=3)return 0;
    unsigned vector=r[12];
    if(vector==32){
        if(current_task)task_suspend(r,PROCESS_TASK_READY);
        if(timer_ticks()-began<2*TIMER_HZ)return 1;
        finish(-3,PROCESS_EXIT_ERROR);
    }
    if(vector!=128)finish(-(int)vector-100,PROCESS_EXIT_ERROR);
    unsigned call=r[7],a=r[4],b=r[6],c=r[5],d=r[1],e=r[0]; /* eax, ebx, ecx, edx */
    if(call==BOS_CALL_EXIT){
        /* Explicit application completion publishes even a nonzero return code.
         * Exceptions, watchdogs and external stops do not use this boundary. */
        if(current_task)output_present();
        finish((int)a,PROCESS_EXIT_APP);
    }
    if(call==BOS_CALL_WRITE){
        if(a>=USER_CAPACITY||b>4096||b>USER_CAPACITY-a){r[7]=(unsigned)-1;return 1;}
        char line[81];unsigned n=0;
        for(unsigned i=0;i<b;i++){
            char ch=*(char *)(USER_BASE+a+i);
            if(ch=='\n'||n==80){line[n]=0;output_print(line);n=0;if(ch=='\n')continue;}
            line[n++]=(ch>=32&&ch<=126)?ch:'.';
        }
        if(n){line[n]=0;output_print(line);}r[7]=b;
    }else if(call==BOS_CALL_PLOT){output_plot((int)a,(int)b,(int)c);r[7]=0;}
    else if(call==BOS_CALL_TICKS)r[7]=timer_ticks();
    else if(call==BOS_CALL_KEY){
        if(current_task){
            r[7]=0;
            if(current_task->key_count){
                r[7]=current_task->keys[current_task->key_head];
                current_task->key_head=(current_task->key_head+1)%TASK_KEYS;
                current_task->key_count--;
            }
        }else r[7]=output->key?output->key():0;
    }
    else if(call==BOS_CALL_PRESENT){
        r[7]=0;
        output_present();
        if(current_task)task_suspend(r,PROCESS_TASK_READY);
    }
    else if(call>=BOS_CALL_READ_FILE&&call<=BOS_CALL_FILE_SIZE)r[7]=(unsigned)file_call(call,a,b,c,d,0);
    else if(call==BOS_CALL_RECT){
        if(c>canvas_width||d>canvas_height){r[7]=(unsigned)-1;return 1;}
        if(c&&d)output_rect(a,b,c,d,(int)e);
        r[7]=0;
    }
    else if(call==BOS_CALL_YIELD||call==BOS_CALL_SLEEP){
        /* Sleep is bounded and wrap-safe; zero milliseconds is a yield. */
        if(!current_task||(call==BOS_CALL_SLEEP&&a>TASK_MAX_SLEEP_MS)){r[7]=(unsigned)-1;return 1;}
        r[7]=0;
        output_present();
        current_task->wait_operation=0;
        if(call==BOS_CALL_SLEEP&&a){
            current_task->wake=timer_ticks()+(a*TIMER_HZ+999u)/1000u;
            task_suspend(r,PROCESS_TASK_SLEEPING);
        }
        task_suspend(r,PROCESS_TASK_READY);
    }
    else if(call==BOS_CALL_TASK_ID)r[7]=current_task?current_task->legacy_task_id:0;
    else if(call==BOS_CALL_READ_FILE_AT)r[7]=(unsigned)file_call(call,a,b,c,d,e);
    else if(call==BOS_CALL_CANVAS_SIZE){
        int supported=(a==PROGRAM_CANVAS_DEFAULT_WIDTH&&b==PROGRAM_CANVAS_DEFAULT_HEIGHT)||
                      (a==PROGRAM_CANVAS_MAX_WIDTH&&b==PROGRAM_CANVAS_MAX_HEIGHT);
        if(!supported||output_resize((int)a,(int)b)){
            r[7]=(unsigned)-1;return 1;
        }
        canvas_width=a;canvas_height=b;
        if(current_task){current_task->canvas_width=a;current_task->canvas_height=b;}
        r[7]=0;
    }
    else if(call==BOS_CALL_REPLACE_FILE)r[7]=(unsigned)file_call(call,a,b,c,d,0);
    else if(call==BOS_CALL_SYNC){
        /* The interrupt gate masks PIT ticks, but floppy motor delays need
         * them. Ring-0 IRQ frames cannot preempt this syscall (see above). */
        unsigned flags;
        __asm__ volatile("pushfl; popl %0; sti":"=r"(flags)::"memory");
        r[7]=(unsigned)fs_sync();
        __asm__ volatile("pushl %0; popfl"::"r"(flags):"memory","cc");
    }
    else if(call==BOS_CALL_ARGUMENT){
        unsigned length=current_task?current_task->argument_length:0;
        /* A zero-capacity query never touches a user pointer. No partial copies:
         * even the terminator must fit in a completely checked user range. */
        if(b&&(!user_range(a,b)||b<=length)){r[7]=(unsigned)-1;return 1;}
        if(b){
            if(length)kmemcpy((void *)(USER_BASE+a),current_task->argument,length);
            *(char *)(USER_BASE+a+length)=0;
        }
        r[7]=length;

    }
    else if(call==BOS_CALL_ABI_QUERY)r[7]=(unsigned)abi_query(a,b,c,d,e);
    else if(call>=BOS_CALL_SYNC_BEGIN&&call<=BOS_CALL_SYNC_RELEASE){
        if(!current_task){r[7]=(unsigned)BOS_E_UNSUPPORTED;return 1;}
        if(c||d||e||(call!=BOS_CALL_SYNC_WAIT&&b)){
            r[7]=(unsigned)BOS_E_INVALID;return 1;
        }
        if(call==BOS_CALL_SYNC_BEGIN){
            if(!user_range(a,sizeof(BosHandle))){r[7]=(unsigned)BOS_E_INVALID;return 1;}
            BosHandle operation;
            int result=native_sync_begin(current_owner(),&operation);
            if(result==BOS_OK)kmemcpy((void *)(USER_BASE+a),&operation,sizeof(operation));
            r[7]=(unsigned)result;
        }else if(call==BOS_CALL_SYNC_RELEASE)r[7]=(unsigned)native_sync_release(current_owner(),a);
        else {
            if(call==BOS_CALL_SYNC_WAIT&&b>TASK_MAX_SLEEP_MS){r[7]=(unsigned)BOS_E_INVALID;return 1;}
            int result=native_sync_poll(current_owner(),a);
            r[7]=(unsigned)result;
            if(call==BOS_CALL_SYNC_WAIT&&b&&result==BOS_PENDING){
                current_task->wait_operation=a;
                current_task->wake=timer_ticks()+(b*TIMER_HZ+999u)/1000u;
                output_present();
                task_suspend(r,PROCESS_TASK_SLEEPING);
            }
        }
    }
    else if(call>=BOS_CALL_FILE_OPEN&&call<=BOS_CALL_FILE_CLOSE)
        r[7]=(unsigned)native_file_call(call,a,b,c,d,e);
    else r[7]=(unsigned)-1;
    return 1;
}
_Static_assert(USER_CAPACITY==65536,"native ABI needs a 64KB segment");

_Static_assert(PROCESS_IMAGE_LIMIT+16384u==USER_CAPACITY,"reserve 16 KiB above native image");
