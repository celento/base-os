/* Ordinary SDK BEX2 client. All workspace accesses remain within its declared
 * 3 MiB; the report is a bounded file replacement and owned sync receipt. */
#include "baseos_app2.h"
#ifndef LARGER_MEMORY_VARIANT
#define LARGER_MEMORY_VARIANT 1
#endif
#if LARGER_MEMORY_VARIANT != 1 && LARGER_MEMORY_VARIANT != 2
#error "Use one of the two documented valid patterns"
#endif
#define WORKSPACE_BYTES (3u*1024u*1024u)
static unsigned text(char *out,unsigned n,const char *s){while(*s)out[n++]=*s++;return n;}
static unsigned number(char *out,unsigned n,unsigned v){
    char reversed[10];unsigned count=0;
    do{reversed[count++]=(char)('0'+v%10u);v/=10u;}while(v);
    while(count)out[n++]=reversed[--count];
    return n;
}
static unsigned field(char *out,unsigned n,const char *label,unsigned v){
    n=text(out,n,label);n=number(out,n,v);out[n++]='\n';return n;
}
static unsigned pattern(unsigned i){
    unsigned within=i&4095u;
    /* Unique ordinary four-byte page headers prevent a repeated MiB from
     * masking page aliasing/reordering; variants have disjoint namespaces. */
    if(within<4u){
        unsigned tag=(i>>12)^(LARGER_MEMORY_VARIANT*0x9e3779b9u);
        return (tag>>(within*8u))&255u;
    }
    return (i*37u+(i>>12)*17u+LARGER_MEMORY_VARIANT*53u)&255u;
}
static unsigned identity(char *out,unsigned n,unsigned generation,unsigned task){
    n=text(out,n,"variant ");n=number(out,n,LARGER_MEMORY_VARIANT);
    n=text(out,n," generation ");n=number(out,n,generation);
    n=text(out,n," task ");return number(out,n,task);
}
static int save(const char *path,const char *data,unsigned bytes){
    BosFileInfo f;int result;
    for(unsigned attempt=0;;attempt++){
        result=bos_file_open(path,BOS_FILE_OPEN_READ|BOS_FILE_OPEN_WRITE,&f);
        if(result==BOS_E_NOT_FOUND)result=bos_file_open(path,
            BOS_FILE_OPEN_READ|BOS_FILE_OPEN_WRITE|BOS_FILE_OPEN_CREATE,&f);
        if(result!=BOS_E_BUSY||attempt==5999)break;
        if(bos_sleep(10)<0)return BOS_E_BUSY;
    }
    if(result!=BOS_OK)return result;
    for(unsigned attempt=0;;attempt++){
        result=bos_file_replace(f.handle,data,bytes,&f);
        if(result!=BOS_E_BUSY||attempt==5999)break;
        if(bos_sleep(10)<0)break;
    }
    int closed=bos_file_close(f.handle);
    if(result!=BOS_OK)return result;
    if(closed!=BOS_OK)return closed;
    BosHandle operation;result=bos_sync_begin(&operation);
    if(result!=BOS_OK)return result;
    do{result=bos_sync_wait(operation,60000);}while(result==BOS_E_TIMEOUT);
    int released=bos_sync_release(operation);
    return result!=BOS_OK?result:released;
}
int main(void){
    unsigned task=bos_task_id(),bytes=bos_workspace_bytes(),generation=1;
    unsigned char *work=bos_workspace();char argument[129];
    int length=bos_argument(argument,sizeof argument);
    if(!task||bytes!=WORKSPACE_BYTES||length<0)return 31;
    if(length){
        static const char expected[]="/Documents/relaunch.txt";
        if((unsigned)length!=sizeof expected-1)return 32;
        for(unsigned i=0;i<sizeof expected;i++)if(argument[i]!=expected[i])return 32;
        generation=2;
    }
    BosMemoryInfo info;
    if(bos_memory_info(&info,sizeof info)!=BOS_OK||info.format!=2||info.table_pages!=2||
       info.page_bytes!=4096||info.owned_pages!=info.mapped_pages+info.table_pages)return 33;
    /* Check every byte before writing any byte. Yield at page boundaries. */
    for(unsigned i=0;i<bytes;i++){
        if(work[i])return 34;
        if((i&16383u)==16383u&&bos_yield()<0)return 35;
    }
    for(unsigned i=0;i<bytes;i++){
        work[i]=(unsigned char)pattern(i);
        if((i&16383u)==16383u&&bos_yield()<0)return 35;
    }
    char out[512];unsigned n=text(out,0,"Held ");n=identity(out,n,generation,task);out[n++]='\n';
    n=field(out,n,"Zero bytes: ",bytes);bos_write(out,n);
    /* Neither client verifies before the controller has observed both complete
     * fills. The ordinary key protocol also proves foreground input ownership. */
    unsigned round=0,token=0;
    for(;;){
        int key;
        while((key=bos_key())){
            if(key>='0'&&key<='9'){token=(token*10u+(unsigned)(key-'0'))%1000000u;continue;}
            if(key=='\r'||key=='\n'){
                n=text(out,0,"Ack ");n=identity(out,n,generation,task);
                n=text(out,n," token ");n=number(out,n,token);out[n++]='\n';bos_write(out,n);token=0;
            }
            if(key=='q')return 0;
            if(key!='v')continue;
            unsigned checksum=2166136261u;
            for(unsigned i=0;i<bytes;i++){
                if(work[i]!=(unsigned char)pattern(i))return 36;
                checksum=(checksum^work[i])*16777619u;
                if((i&16383u)==16383u&&bos_yield()<0)return 35;
            }
            if(bos_memory_info(&info,sizeof info)!=BOS_OK)return 37;
            ++round;n=text(out,0,"BEX2 larger memory\n");
            n=field(out,n,"Variant: ",LARGER_MEMORY_VARIANT);n=field(out,n,"Generation: ",generation);
            n=field(out,n,"Task: ",task);n=field(out,n,"Round: ",round);
            n=field(out,n,"Workspace bytes: ",bytes);n=field(out,n,"Zero bytes: ",bytes);
            n=field(out,n,"Checksum: ",checksum);n=field(out,n,"Mapped pages: ",info.mapped_pages);
            n=field(out,n,"Owned pages: ",info.owned_pages);n=field(out,n,"Table pages: ",info.table_pages);
            n=field(out,n,"Policy pages: ",info.policy_pages);n=field(out,n,"Pool total: ",info.pool_total_pages);
            n=field(out,n,"Pool free: ",info.pool_free_pages);n=field(out,n,"Regions: ",info.region_count);
            /* Clear old visible report text using ordinary newline output. */
            bos_print("\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n"
                      "\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n");
            bos_write(out,n);
            char path[96];unsigned p=text(path,0,"/Documents/mem-v");p=number(path,p,LARGER_MEMORY_VARIANT);
            p=text(path,p,"-g");p=number(path,p,generation);p=text(path,p,"-t");p=number(path,p,task);
            p=text(path,p,"-r");p=number(path,p,round);p=text(path,p,".txt");path[p]=0;
            if(save(path,out,n)!=BOS_OK)return 38;
            bos_print("Durable ");bos_print(path);bos_print("\n");
        }
        if(bos_sleep(25)<0)return 39;
    }
}
