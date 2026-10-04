/* Host-only SDK model for the unchanged application algorithms. The two real
 * clients run concurrently against distinct ordinary host allocations. */
#include "larger_memory_app_shim.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define BYTES (3u*1024u*1024u)
typedef struct {
    unsigned char *workspace;
    unsigned variant,generation,yields,held,keys,round,opened,replaces,waits,live,receipt;
    const char *directory;
} Client;
static _Thread_local Client *client;
static pthread_barrier_t filled;
static unsigned page_tag(const unsigned char *workspace,unsigned page){
    const unsigned char *at=workspace+page*4096u;
    return (unsigned)at[0]|((unsigned)at[1]<<8)|((unsigned)at[2]<<16)|((unsigned)at[3]<<24);
}
extern int larger_memory_1_main(void);
extern int larger_memory_2_main(void);
void *bos_workspace(void){return client->workspace;}
unsigned bos_workspace_bytes(void){return BYTES;}
unsigned bos_task_id(void){return client->variant+1;}
int bos_argument(char *out,unsigned capacity){
    const char *s=client->generation==1?"":"/Documents/relaunch.txt";
    assert(capacity>strlen(s));strcpy(out,s);return (int)strlen(s);
}
/* Synthetic host pool, deliberately unrelated to an E820/QEMU expectation. */
int bos_memory_info(BosMemoryInfo *out,unsigned capacity){
    assert(capacity==sizeof *out);
    *out=(BosMemoryInfo){.struct_size=sizeof *out,.version=1,.format=2,.page_bytes=4096,
        .virtual_bytes=4u*1024u*1024u,.mapped_pages=785,.owned_pages=787,.table_pages=2,
        .policy_pages=1024,.pool_total_pages=4096,.pool_free_pages=2522,.region_count=3};
    return BOS_OK;
}
int bos_yield(void){client->yields++;sched_yield();return 0;}
int bos_sleep(unsigned milliseconds){assert(milliseconds==10||milliseconds==25);sched_yield();return 0;}
int bos_key(void){
    if(!client->keys){
        assert(client->held&&client->yields==BYTES/16384u*2u);
        int result=pthread_barrier_wait(&filled);
        assert(result==0||result==PTHREAD_BARRIER_SERIAL_THREAD);
    }
    static const char keys[]="42\nvvq";
    assert(client->keys<sizeof keys-1);
    return keys[client->keys++];
}
int bos_write(const char *text,unsigned bytes){
    assert(bytes&&bytes<=512);
    if(bytes>=5&&!memcmp(text,"Held ",5)){
        assert(!client->held);
        for(unsigned i=0;i<BYTES;i++){
            /* Independent widened arithmetic, not the app helper. */
            unsigned expected;
            if(i%4096<4){
                unsigned tag=(i/4096)^((unsigned long long)client->variant*2654435769ull&0xffffffffu);
                expected=(tag/(1u<<((i%4096)*8u)))%256;
            }else expected=(unsigned)(((unsigned long long)i*37+(i/4096)*17+client->variant*53)%256);
            assert(client->workspace[i]==expected);
        }
        for(unsigned page=0;page<BYTES/4096u;page++)
            for(unsigned previous=0;previous<page;previous++)
                assert(page_tag(client->workspace,page)!=page_tag(client->workspace,previous));
        client->held=1;
    }
    return (int)bytes;
}
int bos_print(const char *text){assert(text);return (int)strlen(text);}
int bos_file_open(const char *path,unsigned flags,BosFileInfo *out){
    char expected[96];snprintf(expected,sizeof expected,"/Documents/mem-v%u-g%u-t%u-r%u.txt",
        client->variant,client->generation,client->variant+1,client->round+1);
    assert(!strcmp(path,expected)&&!client->live&&!client->receipt);
    if(flags==(BOS_FILE_OPEN_READ|BOS_FILE_OPEN_WRITE)){
        return client->opened++%2?BOS_E_NOT_FOUND:BOS_E_BUSY;
    }
    assert(flags==(BOS_FILE_OPEN_READ|BOS_FILE_OPEN_WRITE|BOS_FILE_OPEN_CREATE));
    *out=(BosFileInfo){.struct_size=sizeof *out,.handle=9,.revision=1};client->live=1;return BOS_OK;
}
int bos_file_replace(BosHandle file,const void *data,unsigned bytes,BosFileInfo *out){
    assert(file==9&&client->live&&bytes<512&&out->handle==9);
    if(client->replaces++%2==0)return BOS_E_BUSY;
    char path[512];snprintf(path,sizeof path,"%s/v%u-g%u-r%u.txt",client->directory,
        client->variant,client->generation,client->round+1);
    FILE *f=fopen(path,"wb");assert(f);assert(fwrite(data,1,bytes,f)==bytes);assert(!fclose(f));
    out->size=bytes;out->revision=2;return BOS_OK;
}
int bos_file_close(BosHandle file){assert(file==9&&client->live);client->live=0;return BOS_OK;}
int bos_sync_begin(BosHandle *operation){assert(!client->live&&!client->receipt);client->receipt=1;*operation=11;return BOS_OK;}
int bos_sync_wait(BosHandle operation,unsigned milliseconds){
    assert(operation==11&&milliseconds==60000&&client->receipt);
    return client->waits++%2?BOS_OK:BOS_E_TIMEOUT;
}
int bos_sync_release(BosHandle operation){assert(operation==11&&client->receipt);client->receipt=0;client->round++;return BOS_OK;}
static void *run(void *data){
    client=data;
    int result=client->variant==1?larger_memory_1_main():larger_memory_2_main();
    assert(result==0&&client->round==2&&!client->live&&!client->receipt);
    assert(client->yields==4u*BYTES/16384u&&client->replaces==4&&client->waits==4);
    return NULL;
}
int main(int argc,char **argv){
    assert(argc==2);
    Client clients[2]={{.variant=1,.generation=1,.directory=argv[1]},
                       {.variant=2,.generation=1,.directory=argv[1]}};
    assert(!pthread_barrier_init(&filled,NULL,2));pthread_t threads[2];
    for(unsigned i=0;i<2;i++){
        clients[i].workspace=calloc(1,BYTES);assert(clients[i].workspace);
        assert(!pthread_create(&threads[i],NULL,run,&clients[i]));
    }
    for(unsigned i=0;i<2;i++)assert(!pthread_join(threads[i],NULL));
    for(unsigned first=0;first<BYTES/4096u;first++)
        for(unsigned second=0;second<BYTES/4096u;second++)
            assert(page_tag(clients[0].workspace,first)!=page_tag(clients[1].workspace,second));
    for(unsigned i=0;i<2;i++)free(clients[i].workspace);
    assert(!pthread_barrier_destroy(&filled));
    assert(!pthread_barrier_init(&filled,NULL,1));
    Client replacement={.variant=2,.generation=2,.directory=argv[1]};
    replacement.workspace=calloc(1,BYTES);assert(replacement.workspace);run(&replacement);free(replacement.workspace);
    assert(!pthread_barrier_destroy(&filled));
    puts("Two real 3 MiB algorithms, fill barrier, repeated full verification, bounded save, and fresh zero reuse passed.");
    return 0;
}
