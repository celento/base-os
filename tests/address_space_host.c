/* Pure table inspection using deterministic layouts; no guest memory accesses. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "address_space.h"
int main(void){
    uint32_t table[1024]={0};
    table[1]=0x680005; /* Read-only text; null page stays absent. */
    table[2]=0x510007;table[3]=0xf03007; /* Adjacent offsets, scattered frames. */
    table[768]=0x681007; /* High workspace. */
    table[1023]=0x511007; /* Top stack; its preceding page remains absent. */
    assert(address_space_span(0,65536,0,65536,USER_WRITE));
    assert(address_space_span(0,65536,65536,0,USER_READ));
    assert(!address_space_span(0,65536,65536,1,USER_READ));
    assert(!address_space_span(0,65536,UINT32_MAX,2,USER_READ));
    assert(address_space_span(table,4194304,4096,4096,USER_READ));
    assert(!address_space_span(table,4194304,4096,1,USER_WRITE));
    assert(!address_space_span(table,4194304,0,1,USER_READ));
    assert(address_space_span(table,4194304,8192+4090,12,USER_WRITE));
    assert(address_space_span(table,4194304,768*4096,4096,USER_WRITE));
    assert(address_space_span(table,4194304,4194304-64,64,USER_WRITE));
    assert(!address_space_span(table,4194304,4194304-4096-1,2,USER_WRITE));
    assert(!address_space_span(table,4194304,8192+4096,8192,USER_READ));
    assert(!address_space_span(table,4194304,4096,8192,USER_WRITE));
    assert(address_space_span(table,4194304,4194304,0,USER_WRITE));
    assert(!address_space_span(table,4194304,4194303,2,USER_READ));
    assert(!address_space_span(table,4194304,4096,UINT32_MAX,USER_READ));
    puts("checked spans: BEX1 boundaries, sparse text/data/workspace/stack, scattered pages and directional permissions passed");
}
