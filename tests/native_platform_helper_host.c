#include <assert.h>
#include <stdio.h>
#include "../sdk/baseos_abi.h"
static int query_result,begin_result,wait_result,release_result,legacy_result;
static unsigned features,legacy_calls,begin_calls,wait_calls,release_calls,timeouts;
static int bos_abi_query(BosAbiInfo *out,unsigned capacity){assert(capacity==sizeof(*out));out->features=features;return query_result;}
static int bos_sync(void){legacy_calls++;return legacy_result;}
static int bos_sync_begin(BosHandle *out){begin_calls++;*out=9;return begin_result;}
static int bos_sync_wait(BosHandle handle,unsigned timeout){assert(handle==9&&timeout==60000);wait_calls++;if(timeouts){timeouts--;return BOS_E_TIMEOUT;}return wait_result;}
static int bos_sync_release(BosHandle handle){assert(handle==9);release_calls++;return release_result;}
#include "native_platform_helper.inc"
int main(void){
    query_result=-1;assert(bos_sync_compatible(0)==BOS_E_UNSUPPORTED&&!legacy_calls);
    assert(bos_sync_compatible(1)==BOS_OK&&legacy_calls==1);
    query_result=BOS_OK;features=0;assert(bos_sync_compatible(0)==BOS_E_UNSUPPORTED);
    legacy_result=-1;assert(bos_sync_compatible(1)==BOS_E_IO&&legacy_calls==2);
    features=BOS_FEATURE_OWNED_SYNC;
    begin_result=BOS_E_BUSY;assert(bos_sync_compatible(1)==BOS_E_BUSY&&legacy_calls==2&&!wait_calls);
    begin_result=BOS_E_CAPACITY;assert(bos_sync_compatible(1)==BOS_E_CAPACITY&&legacy_calls==2);
    begin_result=BOS_OK;wait_result=BOS_OK;timeouts=2;
    assert(bos_sync_compatible(0)==BOS_OK&&wait_calls==3&&release_calls==1);
    wait_result=BOS_E_IO;assert(bos_sync_compatible(1)==BOS_E_IO&&release_calls==2&&legacy_calls==2);
    query_result=BOS_E_INVALID;assert(bos_sync_compatible(1)==BOS_E_INVALID&&legacy_calls==2);
    puts("Native SDK helper: opt-in old-kernel/context fallback, cooperative waits, no busy/capacity fallback passed.");
}
