#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../sdk/baseos_abi.h"
static int abi_result,call_result,begin_result,wait_result,release_result,info_result;
static unsigned features,query_calls,begins,waits,releases,infos,current_revision,timeout_first;
static int bos_abi_query(BosAbiInfo *out,unsigned capacity){
    assert(capacity==sizeof *out);if(abi_result==BOS_OK){memset(out,0,sizeof *out);out->features=features;}return abi_result;
}
static int bos_call(unsigned call,unsigned op,unsigned a,unsigned b,unsigned c,unsigned d){
    assert(call==30&&op==0&&a==1&&b&&c==96&&!d);query_calls++;return call_result;
}
static int bos_sync_begin(BosHandle *out){begins++;if(begin_result==BOS_OK)*out=0x30000001;return begin_result;}
static int bos_sync_wait(BosHandle handle,unsigned delay){assert(handle==0x30000001&&delay==60000);waits++;if(timeout_first&&waits==1)return BOS_E_TIMEOUT;return wait_result;}
static int bos_sync_release(BosHandle handle){assert(handle==0x30000001);releases++;return release_result;}
static int bos_file_info(BosHandle handle,BosFileInfo *out){assert(handle==0x20000001);infos++;if(info_result==BOS_OK){memset(out,0,sizeof *out);out->revision=current_revision;}return info_result;}
#include "transaction_sdk.inc"
static void reset(void){abi_result=call_result=begin_result=wait_result=release_result=info_result=BOS_OK;features=BOS_FEATURE_FILE_TRANSACTIONS;
    query_calls=begins=waits=releases=infos=timeout_first=0;current_revision=17;}
int main(void){
    BosFileTransactionInfoV1 info,before;memset(&info,0xa5,sizeof info);before=info;
    reset();abi_result=-1;assert(bos_file_transaction_query(&info,sizeof info)==BOS_E_UNSUPPORTED&&!query_calls&&!memcmp(&info,&before,sizeof info));
    reset();features=0;assert(bos_file_transaction_query(&info,sizeof info)==BOS_E_UNSUPPORTED&&!query_calls);
    reset();assert(bos_file_transaction_query(&info,sizeof info)==BOS_OK&&query_calls==1);
    reset();assert(bos_file_sync_revision(0x20000001,17)==BOS_OK&&begins==1&&waits==1&&releases==1&&infos==1);
    reset();timeout_first=1;assert(bos_file_sync_revision(0x20000001,17)==BOS_OK&&waits==2&&releases==1);
    reset();current_revision=18;assert(bos_file_sync_revision(0x20000001,17)==BOS_E_CHANGED&&infos==1);
    reset();info_result=BOS_E_CHANGED;assert(bos_file_sync_revision(0x20000001,17)==BOS_E_CHANGED&&releases==1);
    reset();begin_result=BOS_E_BUSY;assert(bos_file_sync_revision(0x20000001,17)==BOS_E_BUSY&&!waits&&!releases&&!infos);
    reset();assert(bos_file_sync_revision(0x20000001,0)==BOS_E_INVALID&&!begins);
    puts("SDK old-runtime refusal and explicit new-receipt exact-content-revision confirmation passed");return 0;
}
