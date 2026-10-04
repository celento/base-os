#include "program.h"
#include "audio.h"
#include "platform.h"
#include "fs.h"
extern unsigned char gdt_user_code[],gdt_user_data[],gdt_tss[];
extern int process_enter(unsigned entry);
extern int process_resume(const uint32_t *frame);
extern void process_leave(void) __attribute__((noreturn));
unsigned process_saved_sp;
int process_result;
static int active;
static uint32_t began;
static const ProgramIO *output;
/* Only ring-3 execution is preempted. Syscalls finish atomically on the kernel
 * stack before the desktop regains control. Task images never overlap USER_BASE. */
#ifndef TASK_BASE
#define TASK_BASE 0x3000000
#define TASK_CAPACITY 0x100000
#endif
#define TASK_KEYS 32
#define FRAME_WORDS 19
#define TASK_MAX_SLEEP_MS 60000u
typedef struct { unsigned char bytes[108]; } FpuState;
typedef struct {
    unsigned char image[USER_CAPACITY];
    uint32_t frame[FRAME_WORDS];
    FpuState fpu;
    ProgramIO io;
    uint32_t wake;
    int state, result, fpu_ready;
    unsigned key_head, key_count;
    unsigned char keys[TASK_KEYS];
} NativeTask;
static NativeTask *tasks=(NativeTask *)TASK_BASE;
static NativeTask *current_task;
static int tasks_ready, have_fpu;
static FpuState kernel_fpu, sync_fpu;
static unsigned kernel_cr0;
_Static_assert(sizeof(NativeTask)*PROCESS_TASKS<=TASK_CAPACITY,"native task arena overflow");
static void fpu_enter(NativeTask *task) {
    if(!have_fpu)return;
    __asm__ volatile("mov %%cr0,%0":"=r"(kernel_cr0));
    unsigned enabled=(kernel_cr0&~12u)|0x22u; /* x87 available, #MF exceptions */
    __asm__ volatile("mov %0,%%cr0"::"r"(enabled):"memory");
    __asm__ volatile("fnsave %0":"=m"(kernel_fpu)::"memory");
    if(task&&task->fpu_ready)__asm__ volatile("frstor %0"::"m"(task->fpu):"memory");
    else __asm__ volatile("fninit":::"memory");
}
static void fpu_leave(NativeTask *task) {
    if(!have_fpu)return;
    FpuState *state=task?&task->fpu:&sync_fpu;
    __asm__ volatile("fnsave %0":"=m"(*state)::"memory");
    if(task)task->fpu_ready=1;
    __asm__ volatile("frstor %0"::"m"(kernel_fpu):"memory");
    __asm__ volatile("mov %0,%%cr0"::"r"(kernel_cr0):"memory");
}
static unsigned char kernel_stack[16384] __attribute__((aligned(16)));
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
    unsigned a,b,c,d;
    __asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(1));
    have_fpu=(d&1)!=0;
    descriptor(gdt_user_code,USER_BASE,USER_CAPACITY-1,0xfa,0x40);
    descriptor(gdt_user_data,USER_BASE,USER_CAPACITY-1,0xf2,0x40);
    *(uint32_t *)(tss+4)=(uintptr_t)(kernel_stack+sizeof(kernel_stack));
    *(uint32_t *)(tss+8)=0x10;
    *(uint16_t *)(tss+102)=sizeof(tss);
    descriptor(gdt_tss,(uintptr_t)tss,sizeof(tss)-1,0x89,0);
    __asm__ volatile("ltr %%ax"::"a"(0x28):"memory");
}
static int valid_image(const void *file,unsigned bytes,uint32_t h[4]) {
    if(!file||bytes<16||bytes>16383u)return 0;
    kmemcpy(h,file,16);
    return h[0]==0x31584542&&h[1]>=16&&h[1]<bytes&&h[2]==bytes&&!h[3];
}
int process_run(const void *file,unsigned bytes,const ProgramIO *io){
    uint32_t h[4];
    if(active||!io||!io->print||!io->plot||!valid_image(file,bytes,h))return -2;
    protect_memory();
    kmemset((void *)USER_BASE,0,USER_CAPACITY);
    kmemcpy((void *)USER_BASE,file,bytes);
    output=io;process_result=0;began=timer_ticks();current_task=0;active=1;
    int resume_audio=audio_status()->state==AUDIO_PLAYING;
    if(resume_audio)audio_pause(1);
    fpu_enter(0);
    int result=process_enter(h[1]);active=0;
    fpu_leave(0);
    if(resume_audio)audio_pause(0);
    return result;
}
static NativeTask *task_at(int owner) {
    return tasks_ready&&owner>=0&&owner<PROCESS_TASKS?tasks+owner:0;
}
int process_task_start(int owner,const void *file,unsigned bytes,const ProgramIO *io) {
    uint32_t h[4];
    if(active||owner<0||owner>=PROCESS_TASKS||!io||!io->print||!io->plot||
       !valid_image(file,bytes,h))return -2;
    if(!tasks_ready){kmemset(tasks,0,sizeof(NativeTask)*PROCESS_TASKS);tasks_ready=1;}
    NativeTask *task=tasks+owner;
    if(task->state==PROCESS_TASK_READY||task->state==PROCESS_TASK_SLEEPING)return -1;
    kmemset(task,0,sizeof(*task));
    kmemcpy(task->image,file,bytes);task->io=*io;
    task->frame[8]=task->frame[9]=task->frame[10]=task->frame[11]=0x23;
    task->frame[14]=h[1];task->frame[15]=0x1b;task->frame[16]=0x202;
    task->frame[17]=USER_CAPACITY-16;task->frame[18]=0x23;
    task->state=PROCESS_TASK_READY;
    return 0;
}
int process_task_status(int owner) {
    NativeTask *task=task_at(owner);return task?task->state:PROCESS_TASK_EMPTY;
}
int process_task_result(int owner) {
    NativeTask *task=task_at(owner);return task?task->result:0;
}
int process_task_step(int owner) {
    NativeTask *task=task_at(owner);
    if(active||!task)return 0;
    if(task->state==PROCESS_TASK_SLEEPING){
        if((int32_t)(timer_ticks()-task->wake)<0)return 0;
        task->state=PROCESS_TASK_READY;
    }
    if(task->state!=PROCESS_TASK_READY)return 0;
    protect_memory();
    kmemcpy((void *)USER_BASE,task->image,USER_CAPACITY);
    output=&task->io;current_task=task;active=1;process_result=0;
    fpu_enter(task);
    process_resume(task->frame);
    active=0;fpu_leave(task);current_task=0;output=0;
    /* Preserve changes exactly once, including completed syscall effects. */
    kmemcpy(task->image,(const void *)USER_BASE,USER_CAPACITY);
    return 1;
}
int process_task_key(int owner,int key) {
    NativeTask *task=task_at(owner);
    if(!task||(task->state!=PROCESS_TASK_READY&&task->state!=PROCESS_TASK_SLEEPING))return 0;
    /* A full queue drops the newest key; task input never escapes to another owner. */
    if(key>0&&key<=255&&task->key_count<TASK_KEYS){
        task->keys[(task->key_head+task->key_count)%TASK_KEYS]=(unsigned char)key;
        task->key_count++;
    }
    return 1;
}
void process_task_stop(int owner) {
    NativeTask *task=task_at(owner);
    if(active||!task)return;
    if(task->state==PROCESS_TASK_READY||task->state==PROCESS_TASK_SLEEPING){
        task->state=PROCESS_TASK_DONE;task->result=PROCESS_TASK_STOPPED;
        task->key_head=task->key_count=0;
    }
}
void process_task_clear(int owner) {
    NativeTask *task=task_at(owner);
    if(!active&&task)kmemset(task,0,sizeof(*task));
}
static void task_suspend(uint32_t *frame,int state) __attribute__((noreturn));
static void task_suspend(uint32_t *frame,int state) {
    kmemcpy(current_task->frame,frame,FRAME_WORDS*sizeof(uint32_t));
    current_task->state=state;
    process_leave();
}
static void finish(int result) __attribute__((noreturn));
static void finish(int result) {
    process_result=result;
    if(current_task){current_task->result=result;current_task->state=PROCESS_TASK_DONE;}
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
                     unsigned buffer,unsigned length) {
    char path[129];
    if(!user_path(path_offset,path_length,path)) return -1;
    if(call!=8 && (length>4096 || !user_range(buffer,length))) return -1;
    int id=fs_resolve(fs_root(),path);
    if(call==6 || call==8) {
        if(!fs_valid(id)||fs_is_dir(id)||fs_is_app(id)) return -1;
        if(call==8) return fs_size(id);
        unsigned size=(unsigned)fs_size(id);if(size>length)size=length;
        kmemcpy((void *)(USER_BASE+buffer),fs_data(id),(int)size);
        return (int)size;
    }
    char name[FS_NAME_LEN];int parent=fs_destination(fs_root(),path,name);
    if(!document_parent(parent)) return -1;
    if(id<0)id=fs_create(parent,name);
    return id<0?-1:fs_write(id,(const char *)(USER_BASE+buffer),(int)length);
}
/* Offsets match InterruptFrame: 8 registers, four segment slots, vector/error,
 * then EIP, CS, EFLAGS and the user SS/ESP on a privilege transition. */
int process_interrupt(uint32_t *r){
    if(!active || (r[15]&3)!=3)return 0;
    unsigned vector=r[12];
    if(vector==32){
        if(current_task)task_suspend(r,PROCESS_TASK_READY);
        if(timer_ticks()-began<2*TIMER_HZ)return 1;
        finish(-3);
    }
    if(vector!=128)finish(-(int)vector-100);
    unsigned call=r[7],a=r[4],b=r[6],c=r[5],d=r[1],e=r[0]; /* eax, ebx, ecx, edx */
    if(call==0)finish((int)a);
    if(call==1){
        if(a>=USER_CAPACITY||b>4096||b>USER_CAPACITY-a){r[7]=(unsigned)-1;return 1;}
        char line[81];unsigned n=0;
        for(unsigned i=0;i<b;i++){
            char ch=*(char *)(USER_BASE+a+i);
            if(ch=='\n'||n==80){line[n]=0;output->print(line);n=0;if(ch=='\n')continue;}
            line[n++]=(ch>=32&&ch<=126)?ch:'.';
        }
        if(n){line[n]=0;output->print(line);}r[7]=b;
    }else if(call==2){output->plot((int)a,(int)b,(int)c);r[7]=0;}
    else if(call==3)r[7]=timer_ticks();
    else if(call==4){
        if(current_task){
            r[7]=0;
            if(current_task->key_count){
                r[7]=current_task->keys[current_task->key_head];
                current_task->key_head=(current_task->key_head+1)%TASK_KEYS;
                current_task->key_count--;
            }
        }else r[7]=output->key?output->key():0;
    }
    else if(call==5){
        r[7]=0;
        if(current_task)task_suspend(r,PROCESS_TASK_READY);
        if(output->present)output->present();
    }
    else if(call>=6&&call<=8)r[7]=(unsigned)file_call(call,a,b,c,d);
    else if(call==9){
        if(c>160||d>100){r[7]=(unsigned)-1;return 1;}
        for(unsigned y=0;y<d;y++)for(unsigned x=0;x<c;x++)
            output->plot((int)(a+x),(int)(b+y),(int)e);
        r[7]=0;
    }
    else if(call==10||call==11){
        /* Sleep is bounded and wrap-safe; zero milliseconds is a yield. */
        if(!current_task||(call==11&&a>TASK_MAX_SLEEP_MS)){r[7]=(unsigned)-1;return 1;}
        r[7]=0;
        if(call==11&&a){
            current_task->wake=timer_ticks()+(a*TIMER_HZ+999u)/1000u;
            task_suspend(r,PROCESS_TASK_SLEEPING);
        }
        task_suspend(r,PROCESS_TASK_READY);
    }
    else if(call==12)r[7]=current_task?(unsigned)(current_task-tasks)+1:0;
    else r[7]=(unsigned)-1;
    return 1;
}
_Static_assert(USER_CAPACITY==65536,"native ABI needs a 64KB segment");
