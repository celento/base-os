/* Deterministic format/layout arithmetic tests. No guest code executes. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "executable.h"
static unsigned char file[BOS_BEX2_FILE_MAX+1];
static void put(unsigned index,uint32_t value) {
    for(unsigned b=0;b<4;b++)file[index*4+b]=(unsigned char)(value>>(b*8));
}
static void normal(void) {
    memset(file,0,sizeof file);
    uint32_t words[]={BOS_BEX2_MAGIC,64,1,0,8196,4096,97,8192,4,4097,
                      1048576,65536,1,1,0,0};
    for(unsigned i=0;i<16;i++)put(i,words[i]);
}
static ExecutablePolicy policy={1,1,BOS_BEX2_FILE_MAX,1024};
static ExecutablePlan plan;
static int parse(uint32_t bytes) { return executable_plan_bex2(file,bytes,&policy,&plan); }
static void rejection(unsigned field,uint32_t value,uint32_t bytes,int expected) {
    normal();put(field,value);
    memset(&plan,0xa5,sizeof plan);ExecutablePlan before=plan;
    assert(parse(bytes)==expected);assert(!memcmp(&plan,&before,sizeof plan));
}
int main(void) {
    normal();assert(parse(8196)==EXECUTABLE_OK);
    assert(plan.kind==BOS_EXECUTABLE_BEX2&&plan.file_bytes==8196&&plan.entry_offset==4096);
    assert(plan.virtual_bytes==4194304&&plan.initial_sp==4194288);
    assert(plan.text.offset==4096&&plan.text.bytes==4096&&plan.text.pages==1);
    assert(plan.text_file_bytes==97&&plan.data_file_bytes==4);
    assert(plan.data.offset==8192&&plan.data.bytes==8192&&plan.data.pages==2);
    assert(plan.workspace.offset==16384&&plan.workspace.bytes==1048576&&plan.workspace.pages==256);
    assert(plan.stack.offset==4128768&&plan.stack.bytes==65536&&plan.stack.pages==16);
    assert(plan.guard_offset==plan.stack.offset-4096&&plan.code_limit==8191&&plan.data_limit==4194303);
    assert(plan.mapped_pages==275&&plan.table_pages==2&&plan.owned_pages==277);
    /* Exact capacity succeeds; a smaller caller cap is rejected unchanged. */
    policy.owned_pages_max=277;assert(parse(8196)==EXECUTABLE_OK);
    policy.owned_pages_max=276;rejection(4,8196,8196,EXECUTABLE_CAPACITY);policy.owned_pages_max=1024;
    policy.file_bytes_max=8196;normal();assert(parse(8196)==EXECUTABLE_OK);
    policy.file_bytes_max=8195;rejection(4,8196,8196,EXECUTABLE_CAPACITY);policy.file_bytes_max=BOS_BEX2_FILE_MAX;
    /* Empty data/workspace needs no page, and retains distinct fixed offsets. */
    normal();put(4,8192);put(8,0);put(9,0);put(10,0);put(11,16384);
    assert(parse(8192)==0&&plan.data.pages==0&&plan.workspace.pages==0&&plan.owned_pages==7);
    assert(plan.workspace.offset==8192&&plan.stack.bytes==16384);
    /* Maximum virtual commitment ends exactly at guard. The absent null and
     * guard pages leave 1022 mapped pages plus two table pages. */
    normal();put(10,4157440);put(11,16384);
    assert(parse(8196)==0&&plan.workspace.offset+plan.workspace.bytes==plan.guard_offset);
    assert(plan.mapped_pages==1022&&plan.owned_pages==1024);
    put(10,4161536);assert(parse(8196)==EXECUTABLE_CAPACITY);
    /* Maximum file and maximum stack are independent inclusive limits. */
    normal();put(4,262144);put(8,253952);put(9,253952);put(10,0);put(11,262144);
    assert(parse(262144)==0&&plan.stack.bytes==262144&&plan.file_bytes==262144);
    put(4,262145);put(8,253953);put(9,253953);assert(parse(262145)==EXECUTABLE_CAPACITY);
    /* A large BSS counts in pages even when absent from the file. */
    normal();put(9,1048580);assert(parse(8196)==0&&plan.data.pages==257);
    /* Fixed field and arithmetic contracts, never fed to guest execution. */
    rejection(0,BOS_BEX1_MAGIC,8196,EXECUTABLE_FORMAT);
    rejection(1,60,8196,EXECUTABLE_UNSUPPORTED);
    rejection(2,2,8196,EXECUTABLE_UNSUPPORTED);
    rejection(3,1,8196,EXECUTABLE_UNSUPPORTED);
    rejection(14,1,8196,EXECUTABLE_UNSUPPORTED);
    rejection(15,1,8196,EXECUTABLE_UNSUPPORTED);
    rejection(12,2,8196,EXECUTABLE_UNSUPPORTED);
    rejection(13,2,8196,EXECUTABLE_UNSUPPORTED);
    rejection(4,8195,8196,EXECUTABLE_FORMAT);
    rejection(5,4095,8196,EXECUTABLE_FORMAT);
    rejection(5,4193,8196,EXECUTABLE_FORMAT);
    rejection(6,0,8196,EXECUTABLE_FORMAT);
    rejection(6,0xffffffffu,8196,EXECUTABLE_FORMAT);
    rejection(7,8193,8196,EXECUTABLE_FORMAT);
    rejection(8,4098,8196,EXECUTABLE_FORMAT);
    rejection(9,0xffffffffu,8196,EXECUTABLE_CAPACITY);
    rejection(10,1048577,8196,EXECUTABLE_FORMAT);
    rejection(10,0xfffff000u,8196,EXECUTABLE_CAPACITY);
    rejection(11,16383,8196,EXECUTABLE_FORMAT);
    rejection(11,12288,8196,EXECUTABLE_CAPACITY);
    rejection(11,266240,8196,EXECUTABLE_CAPACITY);
    normal();memset(&plan,0xa5,sizeof plan);ExecutablePlan before=plan;
    assert(parse(63)==EXECUTABLE_FORMAT&&!memcmp(&plan,&before,sizeof plan));
    assert(executable_plan_bex2(0,8196,&policy,&plan)==EXECUTABLE_FORMAT);
    assert(executable_plan_bex2(file,8196,0,&plan)==EXECUTABLE_FORMAT);
    assert(executable_plan_bex2(file,8196,&policy,0)==EXECUTABLE_FORMAT);
    /* ABI compatibility is caller supplied, not compiled into this module. */
    policy.abi_major=7;policy.abi_minor=3;put(12,7);put(13,2);assert(parse(8196)==0);
    puts("BEX2 production parser: deterministic field/layout/capacity contracts passed");
}
