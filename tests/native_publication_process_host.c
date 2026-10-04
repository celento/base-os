/* Exact production dispatcher/state transitions with ordinary host callbacks.
 * No guest, machine interrupt, protected register, or native image is executed. */
#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "program.h"
#include "platform.h"
#include "fs.h"
#include "native_publication_process_types.inc"

static unsigned char user_memory[USER_CAPACITY];
#undef USER_BASE
#define USER_BASE ((uintptr_t)user_memory)
static NativeTask task_memory[PROCESS_TASKS];
static NativeTask *tasks=task_memory,*current_task;
static const ProgramIO *output;
static int tasks_ready,active,process_result;
static uint32_t began,now;
static unsigned canvas_width,canvas_height;
static jmp_buf leave_target;
static unsigned publications,leaves,plots;
static int checking_suspend;
static char events[8];
static unsigned event_count;

void kmemcpy(void *to,const void *from,int bytes){memcpy(to,from,(size_t)bytes);}
uint32_t timer_ticks(void){return now;}
int fs_sync(void){assert(!"Publication tests must not execute the fs_sync shim");return -1;}
static int file_call(unsigned call,unsigned a,unsigned b,unsigned c,unsigned d,unsigned e){
    (void)call;(void)a;(void)b;(void)c;(void)d;(void)e;
    assert(!"Publication tests must not enter filesystem stubs");return -1;
}
static void event(char value){assert(event_count+1<sizeof events);events[event_count++]=value;}
static void process_leave(void) __attribute__((noreturn));
static void process_leave(void){leaves++;event('L');longjmp(leave_target,1);}

#include "native_publication_process_ops.inc"

static void print_line(const char *line){(void)line;}
static void pixel(int x,int y,int color){(void)x;(void)y;(void)color;plots++;}
static void publish(void){
    publications++;event('P');
    if(checking_suspend){
        assert(current_task&&current_task->state==PROCESS_TASK_READY);
        for(unsigned i=0;i<FRAME_WORDS;i++)assert(current_task->frame[i]==0xa5a5a5a5u);
    }
}
static void prepare(uint32_t frame[FRAME_WORDS],unsigned call,unsigned argument){
    memset(task_memory,0,sizeof task_memory);
    memset(events,0,sizeof events);event_count=publications=leaves=plots=0;
    current_task=tasks+3;tasks_ready=active=1;process_result=17;
    current_task->state=PROCESS_TASK_READY;
    current_task->io=(ProgramIO){print_line,pixel,0,publish,0};output=&current_task->io;
    for(unsigned i=0;i<FRAME_WORDS;i++){
        frame[i]=0x100u+i;current_task->frame[i]=0xa5a5a5a5u;
    }
    frame[7]=call;frame[4]=argument;frame[12]=128;frame[15]=0x1b;
    canvas_width=PROGRAM_CANVAS_DEFAULT_WIDTH;canvas_height=PROGRAM_CANVAS_DEFAULT_HEIGHT;
    current_task->canvas_width=canvas_width;current_task->canvas_height=canvas_height;
    current_task->wake=0x12345678u;now=100;began=0;checking_suspend=0;
}
/* -1 means the production path called process_leave, not a syscall error. */
static int interrupt(uint32_t *frame){
    if(setjmp(leave_target))return -1;
    return process_interrupt(frame);
}
static void finish_directly(int result){
    if(setjmp(leave_target))return;
    finish(result);
}
static void suspension(unsigned call,unsigned milliseconds,uint32_t tick){
    uint32_t frame[FRAME_WORDS];prepare(frame,call,milliseconds);now=tick;
    checking_suspend=1;
    assert(interrupt(frame)==-1);
    assert(publications==1&&leaves==1&&!strcmp(events,"PL"));
    assert(frame[7]==0&&!memcmp(current_task->frame,frame,sizeof current_task->frame));
    if(call==11&&milliseconds){
        assert(current_task->state==PROCESS_TASK_SLEEPING);
        assert(current_task->wake==tick+(milliseconds*TIMER_HZ+999u)/1000u);
    }else{
        assert(current_task->state==PROCESS_TASK_READY);
        assert(current_task->wake==0x12345678u);
    }
    assert(process_result==17);
}
static void nonpublishing_paths(void){
    uint32_t frame[FRAME_WORDS];
    /* A completed pixel syscall and ordinary timer preemption cannot publish. */
    prepare(frame,2,12);assert(interrupt(frame)==1&&plots==1&&frame[7]==0);
    assert(!publications&&!leaves);
    frame[12]=32;assert(interrupt(frame)==-1);
    assert(!publications&&leaves==1&&!strcmp(events,"L"));
    assert(current_task->state==PROCESS_TASK_READY);
    assert(!memcmp(current_task->frame,frame,sizeof current_task->frame));
    /* Generic error completion is tested directly, without provoking a guest
     * fault. Explicit application exit is a different publication boundary. */
    for(int error=-3;error>=-4;error--){
        prepare(frame,0,0);finish_directly(error);
        assert(!publications&&leaves==1&&!strcmp(events,"L"));
        assert(current_task->state==PROCESS_TASK_DONE);
        assert(current_task->result==error&&process_result==error);
    }
    for(unsigned invalid=60001;invalid;invalid=invalid==60001?UINT32_MAX:0){
        prepare(frame,11,invalid);assert(interrupt(frame)==1);
        assert(frame[7]==UINT32_MAX&&!publications&&!leaves);
        assert(current_task->state==PROCESS_TASK_READY&&current_task->wake==0x12345678u);
        for(unsigned i=0;i<FRAME_WORDS;i++)assert(current_task->frame[i]==0xa5a5a5a5u);
    }
    prepare(frame,5,0);frame[15]=0x10;assert(interrupt(frame)==0);
    assert(!publications&&!leaves&&frame[7]==5);
    prepare(frame,5,0);active=0;assert(interrupt(frame)==0);
    assert(!publications&&!leaves&&frame[7]==5);
    for(int state=PROCESS_TASK_READY;state<=PROCESS_TASK_SLEEPING;state++){
        prepare(frame,5,0);active=0;current_task->state=state;
        current_task->key_head=7;current_task->key_count=9;
        process_task_stop(3);
        assert(!publications&&!leaves&&current_task->state==PROCESS_TASK_DONE);
        assert(current_task->result==PROCESS_TASK_STOPPED);
        assert(!current_task->key_head&&!current_task->key_count);
        assert(tasks[2].state==PROCESS_TASK_EMPTY&&tasks[4].state==PROCESS_TASK_EMPTY);
    }
    prepare(frame,5,0);process_task_stop(3); /* Cannot stop while a slice is active. */
    assert(current_task->state==PROCESS_TASK_READY&&!publications&&!leaves);
}
static void completion_and_compatibility(void){
    uint32_t frame[FRAME_WORDS];
    for(unsigned exit_code=0;exit_code<=42;exit_code=exit_code?43:42){
        prepare(frame,0,exit_code);assert(interrupt(frame)==-1);
        assert(publications==1&&leaves==1&&!strcmp(events,"PL"));
        assert(current_task->state==PROCESS_TASK_DONE);
        assert(current_task->result==(int)exit_code&&process_result==(int)exit_code);
    }
    /* Optional callback: the same native ABI still suspends/completes normally. */
    for(unsigned call=0;call<=11;call=call==0?5:call==5?10:call+1){
        prepare(frame,call,0);current_task->io.present=0;
        assert(interrupt(frame)==-1&&leaves==1&&!publications);
        assert(current_task->state==(call?PROCESS_TASK_READY:PROCESS_TASK_DONE));
    }
    /* Legacy exec present remains synchronous; yield/sleep remain unsupported. */
    prepare(frame,5,0);current_task=0;assert(interrupt(frame)==1);
    assert(publications==1&&!leaves&&frame[7]==0&&!strcmp(events,"P"));
    for(unsigned call=10;call<=11;call++){
        prepare(frame,call,0);current_task=0;assert(interrupt(frame)==1);
        assert(frame[7]==UINT32_MAX&&!publications&&!leaves);
    }
    prepare(frame,0,0);current_task=0;assert(interrupt(frame)==-1);
    assert(!publications&&leaves==1&&!process_result);
}
int main(void){
    suspension(5,0,100);suspension(10,0,100);suspension(11,0,100);
    suspension(11,1,100);suspension(11,1000,100);suspension(11,60000,100);
    suspension(11,1000,UINT32_MAX-10u);
    nonpublishing_paths();completion_and_compatibility();
    puts("Native publication process: exact dispatcher present/yield/sleep/explicit-exit boundaries, publication-before-suspend, timer/stop/generic-error isolation and legacy exec passed.");
}
