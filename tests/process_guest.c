#include "program.h"
#include "platform.h"
#include "fs.h"
static int messages,pixels,test;
static void hex(unsigned v){char s[9];for(int i=0;i<8;i++)s[i]="0123456789ABCDEF"[(v>>(28-i*4))&15];s[8]=0;platform_log(s);}
static void say(const char *s){(void)s;messages++;}
static void pixel(int x,int y,int c){(void)x;(void)y;(void)c;pixels++;}
static int key(void){return 42;}
static void show(void){}
static unsigned char image[256];
static void check(const unsigned char *code,int length,int expected){
    unsigned header[]={0x31584542,16,16+(unsigned)length,0};
    kmemcpy(image,header,16);kmemcpy(image+16,code,length);
    ProgramIO io={say,pixel,key,show};
    int rc=process_run(image,length+16,&io);
    test++;platform_log("test ");hex(test);platform_log(" result ");hex(rc);platform_log("\n");
    if(rc!=expected){platform_log("PROCESS-FAIL\n");panic("process result mismatch");}
}
void process_guest(void){
    platform_validate_memory();
    const unsigned char exit_ok[]={0x31,0xc0,0x31,0xdb,0xcd,0x80};check(exit_ok,sizeof exit_ok,0);
    platform_log("exit passed\n");
    const unsigned char ud[]={0x0f,0x0b};check(ud,sizeof ud,-106);
    const unsigned char memory[]={0xa1,0,0,1,0};check(memory,sizeof memory,-114);
    const unsigned char kernel_memory[]={0xa1,0,0,0,0xff};check(kernel_memory,sizeof kernel_memory,-114);
    const unsigned char port[]={0xe4,0x80};check(port,sizeof port,-113);
    const unsigned char cli[]={0xfa};check(cli,sizeof cli,-113);
    const unsigned char seg[]={0x66,0xb8,0x10,0,0x8e,0xd8};check(seg,sizeof seg,-113);
    const unsigned char loop[]={0xeb,0xfe};check(loop,sizeof loop,-3);
    /* Invalid syscall pointer must return -1, then exit with that result. */
    const unsigned char pointer[]={0xb8,1,0,0,0,0xbb,0xff,0xff,0,0,0xb9,2,0,0,0,0xcd,0x80,0x89,0xc3,0x31,0xc0,0xcd,0x80};check(pointer,sizeof pointer,-1);
    const unsigned char write[]={0xb8,1,0,0,0,0xbb,0,0,0,0,0xb9,4,0,0,0,0xcd,0x80,0x31,0xdb,0x31,0xc0,0xcd,0x80};check(write,sizeof write,0);
    if(messages!=1)panic("write syscall");
    check(exit_ok,sizeof exit_ok,0);
    platform_log("PROCESS-ISOLATION-PASS\n");
    for(;;)__asm__ volatile("hlt");
}
