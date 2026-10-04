#ifndef BASEOS_SDK_H
#define BASEOS_SDK_H
/* BEX1 user pointers are offsets in the process's 64 KiB segment. */
typedef unsigned int bos_uint;
static inline int bos_call(unsigned call, unsigned a, unsigned b, unsigned c,
                           unsigned d, unsigned e) {
    __asm__ volatile("int $0x80" : "+a"(call) : "b"(a), "c"(b), "d"(c),
                     "S"(d), "D"(e) : "memory", "cc");
    return (int)call;
}
static inline unsigned bos_strlen(const char *s) { unsigned n=0; while(s[n])++n; return n; }
static inline void bos_exit(int result) { bos_call(0,(unsigned)result,0,0,0,0); for(;;){} }
static inline int bos_write(const char *text, unsigned bytes) { return bos_call(1,(unsigned)text,bytes,0,0,0); }
static inline int bos_print(const char *text) { return bos_write(text,bos_strlen(text)); }
static inline void bos_plot(int x,int y,unsigned color) { bos_call(2,(unsigned)x,(unsigned)y,color,0,0); }
static inline unsigned bos_ticks(void) { return (unsigned)bos_call(3,0,0,0,0,0); }
static inline int bos_key(void) { return bos_call(4,0,0,0,0,0); }
static inline void bos_present(void) { bos_call(5,0,0,0,0,0); }
static inline int bos_read_file(const char *path,void *out,unsigned capacity) {
    return bos_call(6,(unsigned)path,bos_strlen(path),(unsigned)out,capacity,0);
}
/* Atomic RAM writes, confined to /Documents and its existing subfolders.
 * A successful return is not durable until bos_sync() succeeds. */
static inline int bos_write_file(const char *path,const void *data,unsigned bytes) {
    return bos_call(7,(unsigned)path,bos_strlen(path),(unsigned)data,bytes,0);
}
static inline int bos_file_size(const char *path) {
    return bos_call(8,(unsigned)path,bos_strlen(path),0,0,0);
}
static inline void bos_rect(int x,int y,unsigned w,unsigned h,unsigned color) {
    bos_call(9,(unsigned)x,(unsigned)y,w,h,color);
}
/* Desktop task mode (start): present and yield return control to the desktop.
 * Sleep uses milliseconds, 0..60000. Legacy synchronous exec returns -1. */
static inline int bos_yield(void) { return bos_call(10,0,0,0,0,0); }
static inline int bos_sleep(unsigned milliseconds) { return bos_call(11,milliseconds,0,0,0,0); }
/* One-based terminal slot for a task; 0 for synchronous exec. */
static inline unsigned bos_task_id(void) { return (unsigned)bos_call(12,0,0,0,0,0); }
/* Offset is a byte offset, not a pointer. A bounded read returns 0 at/beyond EOF.
 * Each call resolves the path again: files edited between chunks are not snapshots. */
static inline int bos_read_file_at(const char *path,void *out,unsigned capacity,unsigned offset) {
    return bos_call(13,(unsigned)path,bos_strlen(path),(unsigned)out,capacity,offset);
}
/* Exactly 160x100 (default) or 320x200. Success clears every canvas pixel.
 * A new program always starts at 160x100, even in a reused Terminal. */
static inline int bos_canvas_size(unsigned width,unsigned height) {
    return bos_call(14,width,height,0,0,0);
}
/* Replace one /Documents file in RAM, atomically, with up to 32 KiB.
 * IDE storage is required above 16383 bytes. Call bos_sync for durable storage. */
static inline int bos_replace_file(const char *path,const void *data,unsigned bytes) {
    return bos_call(15,(unsigned)path,bos_strlen(path),(unsigned)data,bytes,0);
}
/* Flush the filesystem snapshot: 0 is durable success; -1 leaves RAM pending.
 * This flushes other applications' pending files too and can delay scheduling. */
static inline int bos_sync(void) { return bos_call(16,0,0,0,0,0); }
/* Copy the optional startup document path, including a trailing NUL. Returns
 * its byte length (not counting NUL), 0 if absent, or -1 without a partial copy
 * if capacity is too small. With capacity 0, only query the length. Legacy exec
 * has no argument. The immutable source belongs to this task, not the filesystem. */
static inline int bos_argument(char *out,unsigned capacity) {
    return bos_call(17,(unsigned)out,capacity,0,0,0);
}
#define BOS_ARGUMENT_MAX 128u
#define BOS_DOCUMENT_MAX 32768u
#define BOS_FILE_CHUNK_MAX 4096u
#define BOS_IMAGE_MAX 49152u
#define BOS_TICKS_PER_SECOND 70u
#endif
