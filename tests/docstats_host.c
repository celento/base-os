/* Execute the production DocStats source with ordinary SDK results. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define BASEOS_SDK_H
#define BOS_FILE_CHUNK_MAX 4096u
#define BOS_ARGUMENT_MAX 128u
static const char *argument,*configuration,*input_name,*keys;
static unsigned char *input;
static unsigned input_size,reads,yields,presents,config_reads,syncs;
static int changed_length;
static char output[8192],saved[512];
static unsigned bos_strlen(const char *s){return (unsigned)strlen(s);}
static int bos_print(const char *s){assert(strlen(output)+strlen(s)<sizeof output);strcat(output,s);return (int)strlen(s);}
static unsigned bos_task_id(void){return 1;}
static int bos_argument(char *out,unsigned capacity){
    unsigned n=(unsigned)strlen(argument);assert(n<capacity);memcpy(out,argument,n+1);return (int)n;
}
static int bos_file_size(const char *path){
    if(!strcmp(path,"/Documents/stats-path.txt")){config_reads++;return configuration?(int)strlen(configuration):-1;}
    if(strcmp(path,input_name))return -1;
    return (int)input_size+(changed_length&&reads?1:0);
}
static int bos_read_file(const char *path,void *out,unsigned capacity){
    assert(!strcmp(path,"/Documents/stats-path.txt")&&configuration);
    unsigned n=(unsigned)strlen(configuration);assert(n<=capacity);memcpy(out,configuration,n);return (int)n;
}
static int bos_read_file_at(const char *path,void *out,unsigned capacity,unsigned offset){
    assert(!strcmp(path,input_name)&&capacity==4096);reads++;
    if(offset>=input_size)return 0;
    unsigned n=input_size-offset;if(n>capacity)n=capacity;memcpy(out,input+offset,n);return (int)n;
}
static int bos_replace_file(const char *path,const void *data,unsigned n){
    assert(!strcmp(path,"/Documents/stats-1.txt")&&n<sizeof saved);memcpy(saved,data,n);saved[n]=0;return (int)n;
}
static int bos_sync(void){syncs++;return 0;}
static int bos_key(void){assert(*keys);return *keys++;}
static int bos_sleep(unsigned ms){assert(ms==30);return 0;}
static int bos_yield(void){yields++;return 0;}
static int bos_canvas_size(unsigned w,unsigned h){assert(w==320&&h==200);return 0;}
static void bos_rect(int x,int y,unsigned w,unsigned h,unsigned color){
    (void)x;(void)y;(void)color;assert(w<=320&&h<=200);
}
static void bos_present(void){presents++;}
#define main docstats_main
#include "../examples/c/docstats.c"
#undef main
static void check(unsigned size,const char *arg,const char *config,const char *name){
    input=realloc(input,size?size:1);assert(input);input_size=size;
    argument=arg;configuration=config;input_name=name;keys="sq";
    reads=yields=presents=config_reads=syncs=0;output[0]=saved[0]=0;changed_length=0;
    unsigned expected_words=0,expected_lines=0,expected_sum=0,in_word=0;
    for(unsigned i=0;i<size;i++){
        unsigned c=(unsigned char)"one two\nthree\r\nfour\t"[i%19];input[i]=(unsigned char)c;
        unsigned space=c==' '||(c>=9&&c<=13);expected_words+=!space&&!in_word;
        in_word=!space;expected_lines+=c=='\n';expected_sum+=c;
    }
    if(size&&input[size-1]!='\n')expected_lines++;
    assert(!docstats_main());
    assert(bytes==size&&words==expected_words&&lines==expected_lines&&checksum==expected_sum);
    assert(syncs==1&&strstr(saved,name)&&strstr(output,"Saved and synchronized:"));
    assert(config_reads==(arg[0]?0u:1u));
    unsigned chunks=(size+4095)/4096;
    assert(reads==chunks+1&&yields==chunks/16-chunks/64&&presents==chunks/64+1);
    char count[50];snprintf(count,sizeof count,"Bytes: %u\n",size);assert(strstr(saved,count));
}
int main(void){
    check(56812,"/Documents/a sample.txt","invalid unused setting","/Documents/a sample.txt");
    check(0,"","/Documents/empty.txt\r\n","/Documents/empty.txt");
    check(100,"",NULL,"/Documents/stats-sample.txt");
    check(2*1024*1024,"/Documents/large.txt",NULL,"/Documents/large.txt");
    check(16*1024*1024,"/Documents/large.txt",NULL,"/Documents/large.txt");
    /* A length change remains a visible retryable error, never a saved result. */
    reads=0;changed_length=1;keys="sq";saved[0]=output[0]=0;syncs=0;
    assert(!docstats_main());assert(!ready&&!saved[0]&&!syncs&&strstr(output,"Input length changed"));
    free(input);
    puts("DocStats: startup path wins, config/default/empty fallback, complete 2/16 MiB counts, bounded batching and changed-length detection passed");
}
