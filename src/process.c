#include "program.h"
#include "platform.h"
#include "fs.h"
extern unsigned char gdt_user_code[],gdt_user_data[],gdt_tss[];
extern int process_enter(unsigned entry);
extern void process_leave(void) __attribute__((noreturn));
unsigned process_saved_sp;
int process_result;
static int active;
static uint32_t began;
static const ProgramIO *output;
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
    descriptor(gdt_user_code,USER_BASE,USER_CAPACITY-1,0xfa,0x40);
    descriptor(gdt_user_data,USER_BASE,USER_CAPACITY-1,0xf2,0x40);
    *(uint32_t *)(tss+4)=(uintptr_t)(kernel_stack+sizeof(kernel_stack));
    *(uint32_t *)(tss+8)=0x10;
    *(uint16_t *)(tss+102)=sizeof(tss);
    descriptor(gdt_tss,(uintptr_t)tss,sizeof(tss)-1,0x89,0);
    __asm__ volatile("ltr %%ax"::"a"(0x28):"memory");
}
int process_run(const void *file,unsigned bytes,const ProgramIO *io){
    uint32_t h[4];
    if(active||bytes<16||bytes>FS_MAX_SIZE-1)return -2;
    kmemcpy(h,file,16);
    if(h[0]!=0x31584542||h[1]<16||h[1]>=bytes||h[2]!=bytes||h[3])return -2;
    protect_memory();
    kmemset((void *)USER_BASE,0,USER_CAPACITY);
    kmemcpy((void *)USER_BASE,file,bytes);
    output=io;process_result=0;began=timer_ticks();active=1;
    int result=process_enter(h[1]);active=0;
    return result;
}
/* Offsets match InterruptFrame: 8 registers, four segment slots, vector/error,
 * then EIP, CS, EFLAGS and the user SS/ESP on a privilege transition. */
int process_interrupt(uint32_t *r){
    if(!active || (r[15]&3)!=3)return 0;
    unsigned vector=r[12];
    if(vector==32){
        if(timer_ticks()-began<2*TIMER_HZ)return 1;
        process_result=-3;process_leave();
    }
    if(vector!=128){process_result=-(int)vector-100;process_leave();}
    unsigned call=r[7],a=r[4],b=r[6],c=r[5]; /* eax, ebx, ecx, edx */
    if(call==0){process_result=(int)a;process_leave();}
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
    else if(call==4)r[7]=output->key?output->key():0;
    else r[7]=(unsigned)-1;
    return 1;
}
_Static_assert(USER_CAPACITY==65536,"native ABI needs a 64KB segment");
