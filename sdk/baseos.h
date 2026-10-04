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
/* Writes are confined to /Documents and its existing subfolders. */
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
#define BOS_TICKS_PER_SECOND 70u
#endif
