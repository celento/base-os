#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "program.h"
#include "platform.h"
#include "fs.h"
#include "native_platform_service_stubs.h"
#include "native_platform_types.inc"
static unsigned char user_memory[USER_CAPACITY];
#undef USER_BASE
#define USER_BASE ((uintptr_t)user_memory)
static NativeTask task_memory[PROCESS_TASKS];
static NativeTask *tasks=task_memory,*current_task;
static unsigned owner_serial;
static BosHandle synchronous_owner;
static const ProgramIO *output;
static unsigned canvas_width,canvas_height;
static int tasks_ready,active,process_result;
static uint32_t began,now;
static unsigned publications,legacy_syncs;
static jmp_buf leave_target;
void kmemcpy(void *to,const void *from,int bytes){memcpy(to,from,(size_t)bytes);}
void kmemset(void *to,int value,int bytes){memset(to,value,(size_t)bytes);}
uint32_t timer_ticks(void){return now;}
int fs_sync(void){legacy_syncs++;return 0;}
static int file_call(unsigned call,unsigned a,unsigned b,unsigned c,unsigned d,unsigned e){
    (void)call;(void)a;(void)b;(void)c;(void)d;(void)e;return -1;
}
static void process_leave(void) __attribute__((noreturn));
static void process_leave(void){longjmp(leave_target,1);}
#include "native_platform_ops.inc"
static void print_line(const char *text){(void)text;}
static void pixel(int x,int y,int color){(void)x;(void)y;(void)color;}
static void publish(void){publications++;}
static ProgramIO io={print_line,pixel,0,publish,0};
static const unsigned char program[]={0x42,0x45,0x58,0x31,16,0,0,0,18,0,0,0,0,0,0,0,0xeb,0xfe};
static void select_task(unsigned slot){
    current_task=tasks+slot;active=1;output=&current_task->io;
    canvas_width=160;canvas_height=100;
}
static int invoke(unsigned call,unsigned a,unsigned b,unsigned c,unsigned d,unsigned e){
    uint32_t r[FRAME_WORDS]={0};
    r[7]=call;r[4]=a;r[6]=b;r[5]=c;r[1]=d;r[0]=e;r[12]=128;r[15]=0x1b;
    if(setjmp(leave_target))return 12345; /* Host suspension marker, not ABI. */
    assert(process_interrupt(r)==1);
    return (int)r[7];
}
static void start(unsigned slot){
    active=0;
    assert(process_task_start_with_arg(slot,program,sizeof program,&io,0,0)==0);
    select_task(slot);
}
static void query(void){
    start(2);BosAbiInfo info;
    memset(user_memory,0xa5,sizeof(user_memory));
    assert(invoke(BOS_CALL_ABI_QUERY,64,sizeof info,BOS_ABI_MAJOR,0,0)==BOS_OK);
    memcpy(&info,user_memory+64,sizeof info);
    assert(info.struct_size==96&&info.abi_major==1&&info.abi_minor==0&&info.features==15);
    assert(info.context==BOS_CONTEXT_DESKTOP_TASK&&info.process==current_task->owner_id);
    assert(info.user_bytes==65536&&info.image_bytes==49152&&info.stack_reserved_bytes==16384);
    assert(info.file_bytes==2097152&&info.replace_bytes==32768&&info.file_chunk_bytes==4096);
    assert(info.files_total==64&&info.files_per_process==8&&info.operations_total==32);
    assert(info.operations_per_process==4&&info.wait_milliseconds==60000&&info.ticks_per_second==70);
    for(unsigned i=0;i<4;i++)assert(!info.reserved[i]);
    memset(user_memory+64,0xa5,sizeof info);
    assert(invoke(BOS_CALL_ABI_QUERY,64,16,BOS_ABI_MAJOR,0,0)==BOS_OK);
    for(unsigned i=80;i<64+sizeof info;i++)assert(user_memory[i]==0xa5);
    memset(user_memory+64,0xa5,sizeof info);
    assert(invoke(BOS_CALL_ABI_QUERY,64,sizeof info,2,0,0)==BOS_E_UNSUPPORTED);
    assert(invoke(BOS_CALL_ABI_QUERY,64,15,1,0,0)==BOS_E_INVALID);
    assert(invoke(BOS_CALL_ABI_QUERY,64,sizeof info,1,1,0)==BOS_E_INVALID);
    assert(invoke(BOS_CALL_ABI_QUERY,USER_CAPACITY-16,32,1,0,0)==BOS_E_INVALID);
    for(unsigned i=64;i<64+sizeof info;i++)assert(user_memory[i]==0xa5);
    stub_available=0;stub_limit=16383;
    assert(invoke(BOS_CALL_ABI_QUERY,64,sizeof info,1,0,0)==BOS_OK);
    memcpy(&info,user_memory+64,sizeof info);
    assert(info.features==(BOS_FEATURE_VERSIONED_FILES|BOS_FEATURE_PROCESS_ID));
    assert(!info.operations_total&&!info.operations_per_process&&!info.wait_milliseconds&&info.replace_bytes==16383);
    current_task=0;synchronous_owner=allocate_owner();stub_available=1;
    assert(invoke(BOS_CALL_ABI_QUERY,64,sizeof info,1,0,0)==BOS_OK);
    memcpy(&info,user_memory+64,sizeof info);
    assert(info.context==BOS_CONTEXT_LEGACY_EXEC&&info.process==synchronous_owner&&info.features==9);
    assert(invoke(BOS_CALL_SYNC_BEGIN,0,0,0,0,0)==BOS_E_UNSUPPORTED);
    assert(invoke(BOS_CALL_SYNC,0,0,0,0,0)==0&&legacy_syncs==1);
    assert(invoke(999,0,0,0,0,0)==-1);
    release_owner(synchronous_owner);synchronous_owner=0;select_task(2);stub_limit=2097152;
}
static void file_dispatch(void){
    strcpy((char *)user_memory+10,"/Documents/demo.txt");
    BosFileInfo info;
    stub_result=BOS_OK;unsigned calls=stub_file_calls;
    assert(invoke(BOS_CALL_FILE_OPEN,10,19,3,128,sizeof info)==BOS_OK);
    memcpy(&info,user_memory+128,sizeof info);
    assert(info.struct_size==32&&info.handle==(BOS_HANDLE_TYPE_FILE|1)&&info.revision==19);
    assert(stub_file_calls==calls+1&&stub_owner==current_task->owner_id&&stub_length==3);
    assert(!strcmp(stub_path,"/Documents/demo.txt"));
    memset(user_memory+128,0xa5,sizeof info);stub_result=BOS_E_CHANGED;
    assert(invoke(BOS_CALL_FILE_INFO,info.handle,128,sizeof info,0,0)==BOS_E_CHANGED);
    for(unsigned i=128;i<128+sizeof info;i++)assert(user_memory[i]==0xa5);
    calls=stub_file_calls;
    assert(invoke(BOS_CALL_FILE_OPEN,10,19,3,128,31)==BOS_E_INVALID);
    assert(invoke(BOS_CALL_FILE_READ_AT,info.handle,65530,10,0,0)==BOS_E_INVALID);
    assert(invoke(BOS_CALL_FILE_REPLACE,info.handle,65530,10,128,32)==BOS_E_INVALID);
    assert(stub_file_calls==calls);
    stub_result=3;
    assert(invoke(BOS_CALL_FILE_READ_AT,info.handle,200,3,4096,0)==3);
    assert(!memcmp(user_memory+200,"abc",3)&&stub_data==user_memory+200&&stub_offset==4096);
    stub_result=BOS_OK;
    assert(invoke(BOS_CALL_FILE_REPLACE,info.handle,200,3,128,32)==BOS_OK);
    assert(stub_data==user_memory+200&&stub_length==3);
    assert(invoke(BOS_CALL_FILE_CLOSE,info.handle,0,0,0,0)==BOS_OK);
}
static void waits(void){
    unsigned before=stub_sync_calls;BosHandle op=0;
    stub_result=BOS_E_BUSY;memset(user_memory+256,0xa5,4);
    assert(invoke(BOS_CALL_SYNC_BEGIN,256,0,0,0,0)==BOS_E_BUSY);
    for(unsigned i=256;i<260;i++)assert(user_memory[i]==0xa5);
    assert(legacy_syncs==1&&stub_sync_calls==before+1);
    stub_result=BOS_OK;
    assert(invoke(BOS_CALL_SYNC_BEGIN,256,0,0,0,0)==BOS_OK);
    memcpy(&op,user_memory+256,4);assert(op==(BOS_HANDLE_TYPE_OPERATION|1));
    now=100;stub_poll=BOS_PENDING;unsigned previous=publications;
    assert(invoke(BOS_CALL_SYNC_WAIT,op,1000,0,0,0)==12345);
    assert(current_task->state==PROCESS_TASK_SLEEPING&&current_task->wait_operation==op);
    assert(current_task->wake==170&&publications==previous+1);
    assert(!task_wake(current_task));now=169;assert(!task_wake(current_task));
    stub_poll=BOS_OK;
    assert(task_wake(current_task)&&current_task->frame[7]==0&&!current_task->wait_operation);
    assert(current_task->state==PROCESS_TASK_READY);
    stub_poll=BOS_PENDING;now=UINT32_MAX-5;
    assert(invoke(BOS_CALL_SYNC_WAIT,op,1000,0,0,0)==12345);
    now+=69;assert(!task_wake(current_task));now++;
    assert(task_wake(current_task)&&(int)current_task->frame[7]==BOS_E_TIMEOUT);
    assert(invoke(BOS_CALL_SYNC_POLL,op,0,0,0,0)==BOS_PENDING);
    assert(invoke(BOS_CALL_SYNC_WAIT,op,0,0,0,0)==BOS_PENDING);
    assert(invoke(BOS_CALL_SYNC_WAIT,op,60001,0,0,0)==BOS_E_INVALID);
    assert(invoke(BOS_CALL_SYNC_RELEASE,op,0,0,0,0)==BOS_OK);
    assert(legacy_syncs==1);
}
static void lifetimes(void){
    volatile BosHandle old=current_task->owner_id;
    unsigned f=stub_release_files,s=stub_release_sync;
    assert(invoke(BOS_CALL_EXIT,27,0,0,0,0)==12345);
    assert(current_task->state==PROCESS_TASK_DONE&&current_task->result==27);
    assert(!current_task->owner_id&&stub_release_files==f+1&&stub_release_sync==s+1&&stub_last_released==old);
    start(2);assert(current_task->owner_id!=old);old=current_task->owner_id;
    stub_poll=BOS_PENDING;assert(invoke(BOS_CALL_SYNC_WAIT,BOS_HANDLE_TYPE_OPERATION|2,1000,0,0,0)==12345);
    assert(process_task_key(2,'x'));active=0;process_task_stop(2);
    assert(!current_task->owner_id&&!current_task->wait_operation&&stub_last_released==old);
    assert(!current_task->key_count&&current_task->state==PROCESS_TASK_DONE);
    start(2);old=current_task->owner_id;active=0;process_task_clear(2);
    assert(current_task->state==PROCESS_TASK_EMPTY&&!current_task->owner_id&&stub_last_released==old);
    /* Generic error completion invokes the same cleanup. No fault is injected. */
    start(2);old=current_task->owner_id;
    if(!setjmp(leave_target))finish(-3);
    assert(current_task->state==PROCESS_TASK_DONE&&!current_task->owner_id&&stub_last_released==old);
    /* Direct allocator boundary, no runtime fault or malformed app involved. */
    owner_serial=BOS_HANDLE_SERIAL_MAX-1;
    assert(allocate_owner()==(BOS_HANDLE_TYPE_PROCESS|BOS_HANDLE_SERIAL_MAX));
    assert(allocate_owner()==BOS_HANDLE_INVALID&&allocate_owner()==BOS_HANDLE_INVALID);
    active=0;
    assert(process_task_start_with_arg(2,program,sizeof program,&io,0,0)==-2);
    assert(current_task->state==PROCESS_TASK_DONE&&!current_task->owner_id);
    puts("Native platform dispatcher: negotiation, bounded copy, wait/timeout, context support and owner cleanup passed.");
}
int main(void){query();file_dispatch();waits();lifetimes();return 0;}
