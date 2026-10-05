/* Safe host spans and ordinary buffers exercise the exact extracted dispatcher.
 * There is no guest-pointer probing, instruction execution or invalid memory access. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "native_files.h"
static unsigned char user_bytes[4096];
#undef USER_BASE
#define USER_BASE ((uintptr_t)user_bytes)
enum UserAccess { USER_READ, USER_WRITE };
static int current_task=1,available=1,calls,result;
static unsigned span_count,last_span_bytes;
static BosFileTransactionBeginV1 received;
static char received_path[129];
static int user_span(unsigned offset,unsigned bytes,enum UserAccess access){
    (void)access;span_count++;last_span_bytes=bytes;
    return offset<=sizeof user_bytes&&bytes<=sizeof user_bytes-offset;
}
static int user_path(unsigned offset,unsigned length,char path[129]){
    if(!length||length>128||!user_span(offset,length,USER_READ))return 0;
    for(unsigned i=0;i<length;++i){if(user_bytes[offset+i]<32||user_bytes[offset+i]>126)return 0;path[i]=user_bytes[offset+i];}
    path[length]=0;return path[0]=='/';
}
static unsigned current_owner(void){return BOS_HANDLE_TYPE_PROCESS|1;}
void kmemcpy(void *to,const void *from,int length){memcpy(to,from,(unsigned)length);}
int native_file_transactions_available(void){return available;}
void native_file_transactions_query(BosFileTransactionInfoV1 *out){
    calls++;memset(out,0,sizeof *out);out->struct_size=sizeof *out;out->major=1;out->capabilities=3;
}
int native_file_transaction_begin(unsigned owner,unsigned mode,const BosFileTransactionBeginV1 *input,
                                  const char *path,BosFileTransactionStatusV1 *out){
    assert(owner==current_owner());calls++;received=*input;
    if(path)strcpy(received_path,path);
    if(result==BOS_OK){memset(out,0,sizeof *out);out->struct_size=sizeof *out;out->handle=0x50000001;out->mode=mode;}
    return result;
}
int native_file_transaction_append(unsigned owner,unsigned handle,const void *data,unsigned length,unsigned offset){
    assert(owner==current_owner()&&handle==0x50000001&&data==user_bytes+512&&length==4&&offset==12);calls++;return 4;
}
int native_file_transaction_info(unsigned owner,unsigned handle,BosFileTransactionStatusV1 *out){
    assert(owner==current_owner()&&handle==0x50000001);calls++;
    if(result==BOS_OK){memset(out,0,sizeof *out);out->struct_size=sizeof *out;out->handle=handle;}
    return result;
}
int native_file_transaction_accept(unsigned owner,unsigned handle,BosFileInfo *out){
    assert(owner==current_owner()&&handle==0x50000001);calls++;
    if(result==BOS_OK){memset(out,0,sizeof *out);out->struct_size=sizeof *out;out->revision=7;}
    return result;
}
int native_file_transaction_abort(unsigned owner,unsigned handle){
    assert(owner==current_owner()&&handle==0x50000001);calls++;return result;
}
#include "transaction_dispatch.inc"
static void prepare(void){memset(user_bytes,0xa5,sizeof user_bytes);calls=span_count=0;result=BOS_OK;current_task=available=1;}
static void tail(unsigned offset,unsigned start,unsigned end){for(unsigned i=start;i<end;++i)assert(user_bytes[offset+i]==0xa5);}
int main(void){
    for(unsigned capacity=16;capacity<=112;capacity+=16){
        prepare();assert(native_file_transaction_call(BOS_FILE_TRANSACTION_QUERY,1,128,capacity,0)==BOS_OK);
        assert(last_span_bytes==capacity&&calls==1&&*(unsigned *)(user_bytes+128)==96);
        tail(128,capacity<96?capacity:96,112);
    }
    prepare();assert(native_file_transaction_call(BOS_FILE_TRANSACTION_QUERY,2,128,96,0)==BOS_E_UNSUPPORTED);
    assert(!calls);tail(128,0,96);
    prepare();available=0;assert(native_file_transaction_call(BOS_FILE_TRANSACTION_QUERY,1,128,96,0)==BOS_E_UNSUPPORTED);
    assert(!calls);tail(128,0,96);
    BosFileTransactionBeginV1 input={0};input.struct_size=64;input.version=1;input.total_bytes=262144;
    input.path_offset=256;input.path_bytes=14;
    prepare();memcpy(user_bytes+128,&input,64);memcpy(user_bytes+256,"/Documents/new",14);
    /* Output aliases the complete input record; copied request remains exact. */
    assert(native_file_transaction_call(BOS_FILE_TRANSACTION_BEGIN_CREATE,128,64,128,80)==BOS_OK);
    assert(calls==1&&!memcmp(&received,&input,64)&&!strcmp(received_path,"/Documents/new"));tail(128,64,80);
    prepare();memcpy(user_bytes+128,&input,64);memcpy(user_bytes+256,"/Documents/new",14);
    /* Output aliases the path, including a larger valid caller-owned tail. */
    assert(native_file_transaction_call(BOS_FILE_TRANSACTION_BEGIN_CREATE,128,64,256,80)==BOS_OK);
    assert(!strcmp(received_path,"/Documents/new"));tail(256,64,80);
    for(unsigned word=0;word<8;++word){
        prepare();input.reserved[word]=1;memcpy(user_bytes+128,&input,64);
        assert(native_file_transaction_call(BOS_FILE_TRANSACTION_BEGIN_CREATE,128,64,512,80)==BOS_E_INVALID);
        assert(!calls);tail(512,0,80);input.reserved[word]=0;
    }
    prepare();current_task=0;input.path_bytes=12;memcpy(user_bytes+128,&input,64);memcpy(user_bytes+256,"/Documents/.",12);
    assert(native_file_transaction_call(BOS_FILE_TRANSACTION_BEGIN_CREATE,128,64,512,80)==BOS_E_UNSUPPORTED);
    assert(!calls);tail(512,0,80);input.path_bytes=14;
    prepare();input.version=2;memcpy(user_bytes+128,&input,64);
    assert(native_file_transaction_call(BOS_FILE_TRANSACTION_BEGIN_CREATE,128,64,512,80)==BOS_E_UNSUPPORTED);
    assert(!calls);tail(512,0,80);
    prepare();input.version=1;input.path_offset=input.path_bytes=0;input.source_handle=0x20000001;input.expected_revision=11;
    memcpy(user_bytes+128,&input,64);
    assert(native_file_transaction_call(BOS_FILE_TRANSACTION_BEGIN_REPLACE,128,64,512,80)==BOS_OK);
    assert(!memcmp(&received,&input,64));tail(512,64,80);
    prepare();assert(native_file_transaction_call(BOS_FILE_TRANSACTION_APPEND,0x50000001,512,4,12)==4&&calls==1);
    for(unsigned op=4;op<=5;++op){
        prepare();assert(native_file_transaction_call(op,0x50000001,512,80,0)==BOS_OK);
        assert(last_span_bytes==80);tail(512,op==4?64:32,80);
        prepare();result=BOS_E_CHANGED;assert(native_file_transaction_call(op,0x50000001,512,80,0)==BOS_E_CHANGED);tail(512,0,80);
        /* Preexisting retained handles remain usable when backend support ends. */
        prepare();available=0;assert(native_file_transaction_call(op,0x50000001,512,80,0)==BOS_OK&&calls==1);
        prepare();current_task=0;assert(native_file_transaction_call(op,0x50000001,512,80,0)==BOS_E_UNSUPPORTED&&!calls);
    }
    prepare();assert(native_file_transaction_call(BOS_FILE_TRANSACTION_ABORT,0x50000001,0,0,0)==BOS_OK&&calls==1);
    puts("exact dispatcher valid-span/prefix/tail, copied aliased input/path, reserved/version and contextual contracts passed");return 0;
}
