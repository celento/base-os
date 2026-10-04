#include "workspace_example_shim.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned char *workspace,*input;
static unsigned workspace_size,input_size,input_live,output_live,yields,reads,replaces,waits,creates;
static unsigned saved_size;
static char saved[256];
void *bos_workspace(void){return workspace;}
unsigned bos_workspace_bytes(void){return workspace_size;}
unsigned bos_task_id(void){return 3;}
int bos_argument(char *out,unsigned capacity){assert(capacity==129);out[0]=0;return 0;}
int bos_file_open(const char *path,unsigned flags,BosFileInfo *out){
    if(flags==BOS_FILE_OPEN_READ){
        assert(!strcmp(path,"/Documents/stats-sample.txt")&&!input_live);
        *out=(BosFileInfo){.struct_size=sizeof *out,.handle=1,.size=input_size,.revision=17};
        input_live=1;return BOS_OK;
    }
    assert(!input_live&&!output_live&&strstr(path,"/Documents/workspace-")==path);
    assert(strstr(path,"-3.txt"));
    if(flags==(BOS_FILE_OPEN_READ|BOS_FILE_OPEN_WRITE))return BOS_E_NOT_FOUND;
    assert(flags==(BOS_FILE_OPEN_READ|BOS_FILE_OPEN_WRITE|BOS_FILE_OPEN_CREATE));
    if(!creates++)return BOS_E_BUSY;
    *out=(BosFileInfo){.struct_size=sizeof *out,.handle=2,.revision=19};output_live=1;return BOS_OK;
}
int bos_file_read_at(BosHandle file,void *out,unsigned capacity,unsigned offset){
    assert(file==1&&input_live&&capacity==4096&&offset<=input_size);reads++;
    unsigned bytes=input_size-offset;if(bytes>capacity)bytes=capacity;
    memcpy(out,input+offset,bytes);return (int)bytes;
}
int bos_file_replace(BosHandle file,const void *data,unsigned bytes,BosFileInfo *out){
    assert(file==2&&output_live&&out->handle==2&&bytes<sizeof saved);
    if(!replaces++)return BOS_E_BUSY;
    memcpy(saved,data,bytes);saved_size=bytes;out->size=bytes;out->revision=20;return BOS_OK;
}
int bos_file_close(BosHandle file){
    if(file==1){assert(input_live);input_live=0;}else{assert(file==2&&output_live);output_live=0;}
    return BOS_OK;
}
int bos_sync_begin(BosHandle *out){assert(saved_size&&!input_live&&!output_live);*out=3;return BOS_OK;}
int bos_sync_wait(BosHandle operation,unsigned milliseconds){
    assert(operation==3&&milliseconds==60000);return waits++?BOS_OK:BOS_E_TIMEOUT;
}
int bos_sync_release(BosHandle operation){assert(operation==3&&waits==2);return BOS_OK;}
int bos_yield(void){yields++;return 0;}
int bos_sleep(unsigned milliseconds){assert(milliseconds==10);return 0;}
int bos_print(const char *text){return (int)strlen(text);}
int bos_write(const char *text,unsigned bytes){assert(text&&bytes);return (int)bytes;}
extern int workspace_example_main(void);
int main(int argc,char **argv){
    assert(argc==4);workspace_size=(unsigned)strtoul(argv[3],0,10);
    assert(workspace_size==1048576||workspace_size==3145728);
    workspace=calloc(1,workspace_size);assert(workspace);
    FILE *f=fopen(argv[1],"rb");assert(f);assert(!fseek(f,0,SEEK_END));
    long size=ftell(f);assert(size>=0&&size<16777216);rewind(f);input_size=(unsigned)size;
    input=malloc(input_size+1);assert(input);assert(fread(input,1,input_size,f)==input_size);fclose(f);
    assert(workspace_example_main()==0);
    assert(!input_live&&!output_live&&yields>=workspace_size/16384u*2u&&reads>=2&&replaces==2&&waits==2&&creates==2);
    f=fopen(argv[2],"rb");assert(f);char expected[256];size_t bytes=fread(expected,1,sizeof expected,f);fclose(f);
    assert(bytes==saved_size&&!memcmp(expected,saved,bytes));
    fwrite(saved,1,saved_size,stdout);free(workspace);free(input);
    puts("Workspace example: real algorithm and ordinary bounded service sequence passed");
}
