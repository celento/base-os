/* Execute the actual example source with normal success/error syscall results. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define BASEOS_SDK_H
#define BOS_TICKS_PER_SECOND 70u
static char output[2048],written[128];
static const char *keys;
static int write_result,sync_result,writes,syncs;
static unsigned bos_strlen(const char *s){return (unsigned)strlen(s);}
static int bos_print(const char *s){strcat(output,s);return (int)strlen(s);}
static unsigned bos_task_id(void){return 1;}
static int bos_read_file(const char *p,void *out,unsigned n){(void)p;(void)out;(void)n;return -1;}
static int bos_write_file(const char *p,const void *data,unsigned n){
    (void)p;writes++;if(write_result<0)return -1;
    memcpy(written,data,n);written[n]=0;return (int)n;
}
static int bos_sync(void){syncs++;return sync_result;}
static unsigned bos_ticks(void){return 0;}
static int bos_key(void){return *keys?*keys++:0;}
static int bos_sleep(unsigned ms){(void)ms;return -1;}
static void bos_rect(int x,int y,unsigned w,unsigned h,unsigned color){(void)x;(void)y;(void)w;(void)h;(void)color;}
static void bos_present(void){}
#define main counter_main
#include "../examples/c/counter.c"
#undef main
#define main notebook_main
#include "../examples/c/notebook.c"
#undef main
static void prepare(int write_error,int sync_error){
    output[0]=written[0]=0;writes=syncs=0;write_result=write_error;sync_result=sync_error;keys="sq";value=42;
}
int main(void){
    prepare(0,0);assert(!counter_main());assert(writes==1&&syncs==1&&!strcmp(written,"42\n"));
    assert(strstr(output,"Saved /Documents/counter-1.txt"));
    prepare(0,-1);assert(!counter_main());assert(writes==1&&syncs==1&&!strcmp(written,"42\n"));
    assert(strstr(output,"RAM only")&&!strstr(output,"Saved /"));
    prepare(-1,0);assert(!counter_main());assert(writes==1&&!syncs&&!written[0]);
    assert(strstr(output,"Save failed")&&!strstr(output,"Saved /"));
    prepare(0,0);assert(!notebook_main());assert(writes==1&&syncs==1);
    assert(strstr(output,"Saved /Documents/sdk-note.txt"));
    prepare(0,-1);assert(notebook_main()==1);assert(writes==1&&syncs==1&&written[0]);
    assert(strstr(output,"RAM only")&&!strstr(output,"Saved /"));
    prepare(-1,0);assert(notebook_main()==1);assert(writes==1&&!syncs&&!written[0]);
    assert(strstr(output,"Could not save")&&!strstr(output,"Saved /"));
    puts("native saves: Counter and Notebook require explicit successful sync for Saved; RAM-only and write failures stay honest");
}
