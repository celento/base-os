#ifndef BASEOS_SDK_H
#define BASEOS_SDK_H
#include "baseos_abi.h"
/* BEX1 user pointers are offsets in the process's 64 KiB segment. */
typedef unsigned int bos_uint;
static inline int bos_call(unsigned call, unsigned a, unsigned b, unsigned c,
                           unsigned d, unsigned e) {
    __asm__ volatile("int $0x80" : "+a"(call) : "b"(a), "c"(b), "d"(c),
                     "S"(d), "D"(e) : "memory", "cc");
    return (int)call;
}
static inline unsigned bos_strlen(const char *s) { unsigned n=0; while(s[n])++n; return n; }
static inline void bos_exit(int result) { bos_call(BOS_CALL_EXIT,(unsigned)result,0,0,0,0); for(;;){} }
static inline int bos_write(const char *text, unsigned bytes) { return bos_call(BOS_CALL_WRITE,(unsigned)text,bytes,0,0,0); }
static inline int bos_print(const char *text) { return bos_write(text,bos_strlen(text)); }
static inline void bos_plot(int x,int y,unsigned color) { bos_call(BOS_CALL_PLOT,(unsigned)x,(unsigned)y,color,0,0); }
static inline unsigned bos_ticks(void) { return (unsigned)bos_call(BOS_CALL_TICKS,0,0,0,0,0); }
static inline int bos_key(void) { return bos_call(BOS_CALL_KEY,0,0,0,0,0); }
static inline void bos_present(void) { bos_call(BOS_CALL_PRESENT,0,0,0,0,0); }
static inline int bos_read_file(const char *path,void *out,unsigned capacity) {
    return bos_call(BOS_CALL_READ_FILE,(unsigned)path,bos_strlen(path),(unsigned)out,capacity,0);
}
#define BOS_ERR_BUSY (-2) /* Temporary snapshot lease; retry the write shortly. */
/* Atomic RAM writes, confined to /Documents and its existing subfolders.
 * BOS_ERR_BUSY leaves the target and source bytes unchanged. Reads still work.
 * A successful return is not durable until bos_sync() succeeds. */
static inline int bos_write_file(const char *path,const void *data,unsigned bytes) {
    return bos_call(BOS_CALL_WRITE_FILE,(unsigned)path,bos_strlen(path),(unsigned)data,bytes,0);
}
static inline int bos_file_size(const char *path) {
    return bos_call(BOS_CALL_FILE_SIZE,(unsigned)path,bos_strlen(path),0,0,0);
}
static inline void bos_rect(int x,int y,unsigned w,unsigned h,unsigned color) {
    bos_call(BOS_CALL_RECT,(unsigned)x,(unsigned)y,w,h,color);
}
/* Desktop task mode (start): present and yield return control to the desktop.
 * Sleep uses milliseconds, 0..60000. Legacy synchronous exec returns -1. */
static inline int bos_yield(void) { return bos_call(BOS_CALL_YIELD,0,0,0,0,0); }
static inline int bos_sleep(unsigned milliseconds) { return bos_call(BOS_CALL_SLEEP,milliseconds,0,0,0,0); }
/* One-based terminal slot for a task; 0 for synchronous exec. */
static inline unsigned bos_task_id(void) { return (unsigned)bos_call(BOS_CALL_TASK_ID,0,0,0,0,0); }
/* Offset is a byte offset, not a pointer. A bounded read returns 0 at/beyond EOF.
 * Each call resolves the path again: files edited between chunks are not snapshots. */
static inline int bos_read_file_at(const char *path,void *out,unsigned capacity,unsigned offset) {
    return bos_call(BOS_CALL_READ_FILE_AT,(unsigned)path,bos_strlen(path),(unsigned)out,capacity,offset);
}
/* Exactly 160x100 (default) or 320x200. Success clears every canvas pixel.
 * A new program always starts at 160x100, even in a reused Terminal. */
static inline int bos_canvas_size(unsigned width,unsigned height) {
    return bos_call(BOS_CALL_CANVAS_SIZE,width,height,0,0,0);
}
/* Replace one /Documents file in RAM, atomically, with up to 32 KiB.
 * IDE storage is required above 16383 bytes. Call bos_sync for durable storage.
 * BOS_ERR_BUSY leaves the target and source bytes unchanged; retry shortly. */
static inline int bos_replace_file(const char *path,const void *data,unsigned bytes) {
    return bos_call(BOS_CALL_REPLACE_FILE,(unsigned)path,bos_strlen(path),(unsigned)data,bytes,0);
}
/* Flush the filesystem snapshot: 0 is durable success; -1 leaves RAM pending.
 * This flushes other applications' pending files too and can delay scheduling. */
static inline int bos_sync(void) { return bos_call(BOS_CALL_SYNC,0,0,0,0,0); }
/* Copy the optional startup document path, including a trailing NUL. Returns
 * its byte length (not counting NUL), 0 if absent, or -1 without a partial copy
 * if capacity is too small. With capacity 0, only query the length. Legacy exec
 * has no argument. The immutable source belongs to this task, not the filesystem. */
static inline int bos_argument(char *out,unsigned capacity) {
    return bos_call(BOS_CALL_ARGUMENT,(unsigned)out,capacity,0,0,0);
}
#define BOS_ARGUMENT_MAX 128u
#define BOS_DOCUMENT_MAX 32768u
#define BOS_FILE_CHUNK_MAX 4096u
#define BOS_IMAGE_MAX 49152u
#define BOS_TICKS_PER_SECOND 70u
/* Additive ABI discovery. Old kernels return -1. Test result before reading
 * output, and inspect feature bits instead of assuming all new services exist. */
static inline int bos_abi_query(BosAbiInfo *out,unsigned capacity) {
    return bos_call(BOS_CALL_ABI_QUERY,(unsigned)out,capacity,BOS_ABI_MAJOR,0,0);
}
static inline int bos_memory_info(BosMemoryInfo *out,unsigned capacity) {
    return bos_call(BOS_CALL_MEMORY_INFO,(unsigned)out,capacity,BOS_MEMORY_INFO_VERSION,0,0);
}
/* Task + asynchronous IDE only. Begin reserves an owned completion without
 * doing whole-operation I/O. A handle remains valid until release or exit.
 * Releasing it abandons this receipt, never cancels somebody else's save. */
static inline int bos_sync_begin(BosHandle *out) {
    return bos_call(BOS_CALL_SYNC_BEGIN,(unsigned)out,0,0,0,0);
}
static inline int bos_sync_poll(BosHandle operation) {
    return bos_call(BOS_CALL_SYNC_POLL,operation,0,0,0,0);
}
/* 0 is a poll. 1..60000 suspends only this task; TIMEOUT leaves the operation
 * alive. Completion is checked before the deadline. No user pointer is retained. */
static inline int bos_sync_wait(BosHandle operation,unsigned milliseconds) {
    return bos_call(BOS_CALL_SYNC_WAIT,operation,milliseconds,0,0,0);
}
static inline int bos_sync_release(BosHandle operation) {
    return bos_call(BOS_CALL_SYNC_RELEASE,operation,0,0,0,0);
}
static inline int bos_file_open(const char *path,unsigned flags,BosFileInfo *out) {
    return bos_call(BOS_CALL_FILE_OPEN,(unsigned)path,bos_strlen(path),flags,
                    (unsigned)out,sizeof(*out));
}
static inline int bos_file_info(BosHandle file,BosFileInfo *out) {
    return bos_call(BOS_CALL_FILE_INFO,file,(unsigned)out,sizeof(*out),0,0);
}
/* The opened revision is checked before any copy, including EOF/zero reads.
 * CHANGED never partially copies data. Close and reopen to accept a new version. */
static inline int bos_file_read_at(BosHandle file,void *out,unsigned capacity,unsigned offset) {
    return bos_call(BOS_CALL_FILE_READ_AT,file,(unsigned)out,capacity,offset,0);
}
/* Conditional atomic RAM replacement. BOS_OK refreshes this handle's version;
 * durability requires sync. Output metadata is unchanged on every error. */
static inline int bos_file_replace(BosHandle file,const void *data,unsigned bytes,BosFileInfo *out) {
    return bos_call(BOS_CALL_FILE_REPLACE,file,(unsigned)data,bytes,(unsigned)out,sizeof(*out));
}
static inline int bos_file_close(BosHandle file) {
    return bos_call(BOS_CALL_FILE_CLOSE,file,0,0,0,0);
}
/* Chunked conditional RAM publication. Query negotiates the general feature
 * first and explicitly returns UNSUPPORTED on old runtimes. Other operations
 * require successful discovery; there is never an automatic legacy fallback. */
static inline int bos_file_transaction_query(BosFileTransactionInfoV1 *out,unsigned capacity) {
    BosAbiInfo abi;
    int result=bos_abi_query(&abi,sizeof abi);
    if(result==-1||result==BOS_E_UNSUPPORTED||
       (result==BOS_OK&&!(abi.features&BOS_FEATURE_FILE_TRANSACTIONS)))return BOS_E_UNSUPPORTED;
    if(result!=BOS_OK)return result;
    return bos_call(BOS_CALL_FILE_TRANSACTION,BOS_FILE_TRANSACTION_QUERY,
                    BOS_FILE_TRANSACTION_MAJOR,(unsigned)out,capacity,0);
}
static inline int bos_file_transaction_begin_replace(BosHandle file,unsigned revision,unsigned total,
                                                      BosFileTransactionStatusV1 *out) {
    BosFileTransactionBeginV1 input={0};
    input.struct_size=sizeof input;input.version=BOS_FILE_TRANSACTION_VERSION;
    input.source_handle=file;input.expected_revision=revision;input.total_bytes=total;
    return bos_call(BOS_CALL_FILE_TRANSACTION,BOS_FILE_TRANSACTION_BEGIN_REPLACE,
                    (unsigned)&input,sizeof input,(unsigned)out,sizeof *out);
}
static inline int bos_file_transaction_begin_create(const char *path,unsigned total,BosFileTransactionStatusV1 *out) {
    BosFileTransactionBeginV1 input={0};
    input.struct_size=sizeof input;input.version=BOS_FILE_TRANSACTION_VERSION;
    input.path_offset=(unsigned)path;input.path_bytes=bos_strlen(path);input.total_bytes=total;
    return bos_call(BOS_CALL_FILE_TRANSACTION,BOS_FILE_TRANSACTION_BEGIN_CREATE,
                    (unsigned)&input,sizeof input,(unsigned)out,sizeof *out);
}
/* length is 1..chunk_bytes; offset must equal received_bytes. Returns exactly
 * length or changes nothing within the same mount incarnation. Reuse the buffer
 * immediately after return. Empty stages are complete without an APPEND. */
static inline int bos_file_transaction_append(BosHandle stage,const void *data,unsigned length,unsigned offset) {
    return bos_call(BOS_CALL_FILE_TRANSACTION,BOS_FILE_TRANSACTION_APPEND,stage,(unsigned)data,length,offset);
}
static inline int bos_file_transaction_info(BosHandle stage,BosFileTransactionStatusV1 *out) {
    return bos_call(BOS_CALL_FILE_TRANSACTION,BOS_FILE_TRANSACTION_INFO,stage,(unsigned)out,sizeof *out,0);
}
/* Synchronous atomic RAM publication, never durable or an elapsed-time promise.
 * Success consumes stage and returns the updated/new file handle and revision.
 * BUSY/CAPACITY/conflict leave the private stage intact in the same incarnation.
 * Mount invalidation instead releases frames and reports INVALIDATED in INFO.
 * Closing the source file also aborts its replacement stage. */
static inline int bos_file_transaction_accept_ram(BosHandle stage,BosFileInfo *out) {
    return bos_call(BOS_CALL_FILE_TRANSACTION,BOS_FILE_TRANSACTION_ACCEPT_RAM,stage,(unsigned)out,sizeof *out,0);
}
static inline int bos_file_transaction_abort(BosHandle stage) {
    return bos_call(BOS_CALL_FILE_TRANSACTION,BOS_FILE_TRANSACTION_ABORT,stage,0,0,0);
}
/* Explicit durability request for the captured accepted CONTENT revision.
 * Begins a new owned receipt; success also checks the same handle still reports
 * exactly that revision. It does not confirm later rename/move metadata.
 * A later edit can yield CHANGED even if this version was previously durable.
 * This opt-in helper waits only the calling task and never falls back to exec. */
static inline int bos_file_sync_revision(BosHandle file,unsigned accepted_revision) {
    if(!accepted_revision)return BOS_E_INVALID;
    BosHandle operation;
    int result=bos_sync_begin(&operation);
    if(result!=BOS_OK)return result;
    do { result=bos_sync_wait(operation,60000u); } while(result==BOS_E_TIMEOUT);
    int released=bos_sync_release(operation);
    if(result!=BOS_OK)return result;
    if(released!=BOS_OK)return released;
    BosFileInfo info;
    result=bos_file_info(file,&info);
    if(result!=BOS_OK)return result;
    return info.revision==accepted_revision?BOS_OK:BOS_E_CHANGED;
}
/* Explicit opt-in compatibility helper for synchronous application save code.
 * On a supported task it sleeps only the caller while the desktop progresses.
 * allow_legacy_blocking=1 permits old syscall16 on an old kernel or unsupported
 * context/backend; 0 returns UNSUPPORTED instead. Resource BUSY/CAPACITY and I/O
 * failures NEVER trigger a blocking fallback. Existing bos_sync remains exact. */
static inline int bos_sync_compatible(unsigned allow_legacy_blocking) {
    BosAbiInfo abi;
    int result=bos_abi_query(&abi,sizeof(abi));
    if(result==-1 || result==BOS_E_UNSUPPORTED ||
       (result==BOS_OK && !(abi.features&BOS_FEATURE_OWNED_SYNC))) {
        return allow_legacy_blocking ? (bos_sync()==0?BOS_OK:BOS_E_IO) : BOS_E_UNSUPPORTED;
    }
    if(result!=BOS_OK)return result;
    BosHandle operation;
    result=bos_sync_begin(&operation);
    if(result!=BOS_OK)return result;
    do { result=bos_sync_wait(operation,60000u); } while(result==BOS_E_TIMEOUT);
    int released=bos_sync_release(operation);
    return result==BOS_OK ? released : result;
}
/* Pointer service. Drain events and bos_key(), explicitly present your
 * frame, then wait. UI wait never publishes an unfinished working canvas. */
static inline int bos_ui_query(BosUiInfoV1 *out,unsigned capacity) {
    return bos_call(BOS_CALL_UI,BOS_UI_QUERY,BOS_UI_MAJOR,(unsigned)out,capacity,0);
}
static inline int bos_ui_host_open(unsigned subscriptions,BosUiTargetInfoV1 *out) {
    return bos_call(BOS_CALL_UI,BOS_UI_HOST_OPEN,BOS_UI_MAJOR,(unsigned)out,sizeof *out,subscriptions);
}
/* GUI BEX2 only: adopt the existing primary window endpoint. This does not
 * create a window. Releasing the endpoint leaves the window and task alive. */
static inline int bos_ui_window_adopt(unsigned subscriptions,BosUiTargetInfoV1 *out) {
    return bos_call(BOS_CALL_UI,BOS_UI_WINDOW_ADOPT,BOS_UI_MAJOR,(unsigned)out,sizeof *out,subscriptions);
}
static inline int bos_ui_info(BosHandle target,BosUiTargetInfoV1 *out) {
    return bos_call(BOS_CALL_UI,BOS_UI_INFO,target,(unsigned)out,sizeof *out,0);
}
static inline int bos_ui_read(BosHandle target,BosUiEventV1 *out) {
    return bos_call(BOS_CALL_UI,BOS_UI_READ,target,(unsigned)out,sizeof *out,0);
}
static inline int bos_ui_wait(BosHandle target,unsigned readiness,unsigned milliseconds) {
    return bos_call(BOS_CALL_UI,BOS_UI_WAIT,target,readiness,milliseconds,0);
}
static inline int bos_ui_release(BosHandle target) {
    return bos_call(BOS_CALL_UI,BOS_UI_RELEASE,target,0,0,0);
}
#endif
