/* Dispatcher-only fixtures: deterministic service results, no device I/O. Real
 * filesystem/coordinator behavior is covered separately by their service tests. */
#include "../sdk/baseos_abi.h"
#include "native_files.h"
#include "native_sync.h"
#include "native_ui_service_stubs.h"
static unsigned stub_sync_calls,stub_file_calls,stub_release_files,stub_release_sync;
static unsigned stub_owner,stub_handle,stub_offset,stub_length;
static unsigned stub_last_released,stub_limit=2097152;
static int stub_available=1,stub_result=BOS_OK,stub_poll=BOS_PENDING;
static char stub_path[129];
static const void *stub_data;
unsigned fs_file_limit(void){return stub_limit;}
int native_sync_available(void){return stub_available;}
unsigned native_sync_capacity(void){return NATIVE_SYNC_CAPACITY;}
unsigned native_sync_per_owner_limit(void){return NATIVE_SYNC_PER_OWNER;}
int native_sync_begin(unsigned owner,unsigned *handle){
    stub_sync_calls++;stub_owner=owner;
    if(stub_result==BOS_OK)*handle=BOS_HANDLE_TYPE_OPERATION|1;
    return stub_result;
}
int native_sync_poll(unsigned owner,unsigned handle){
    stub_sync_calls++;stub_owner=owner;stub_handle=handle;return stub_poll;
}
int native_sync_release(unsigned owner,unsigned handle){
    stub_sync_calls++;stub_owner=owner;stub_handle=handle;return stub_result;
}
void native_sync_owner_release(unsigned owner){
#ifdef NATIVE_OWNER_RELEASE_HOOK
    NATIVE_OWNER_RELEASE_HOOK(owner);
#endif
    stub_release_sync++;stub_last_released=owner;
}
void native_files_release_owner(unsigned owner){
#ifdef NATIVE_OWNER_RELEASE_HOOK
    NATIVE_OWNER_RELEASE_HOOK(owner);
#endif
    stub_release_files++;stub_last_released=owner;
}
static void stub_info(BosFileInfo *info){
    memset(info,0,sizeof(*info));info->struct_size=sizeof(*info);
    info->handle=BOS_HANDLE_TYPE_FILE|1;info->size=7;info->flags=3;info->revision=19;
}
int native_file_open(unsigned owner,const char *path,unsigned flags,BosFileInfo *out){
    stub_file_calls++;stub_owner=owner;stub_length=flags;strcpy(stub_path,path);
    if(stub_result==BOS_OK)stub_info(out);
    return stub_result;
}
int native_file_info(unsigned owner,unsigned handle,BosFileInfo *out){
    stub_file_calls++;stub_owner=owner;stub_handle=handle;
    if(stub_result==BOS_OK)stub_info(out);
    return stub_result;
}
int native_file_read_at(unsigned owner,unsigned handle,unsigned offset,void *out,unsigned capacity){
    stub_file_calls++;stub_owner=owner;stub_handle=handle;stub_offset=offset;stub_length=capacity;stub_data=out;
    if(stub_result>=0&&capacity)memcpy(out,"abc",capacity<3?capacity:3);
    return stub_result;
}
int native_file_replace(unsigned owner,unsigned handle,const void *data,unsigned length,BosFileInfo *out){
    stub_file_calls++;stub_owner=owner;stub_handle=handle;stub_data=data;stub_length=length;
    if(stub_result==BOS_OK)stub_info(out);
    return stub_result;
}
int native_file_close(unsigned owner,unsigned handle){
    stub_file_calls++;stub_owner=owner;stub_handle=handle;return stub_result;
}
