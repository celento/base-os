/* Ordinary bounded BEX2 client for visible capacity and survivor checks. */
#include "baseos_app2.h"
static unsigned number(char *out,unsigned at,unsigned value){
    char reverse[10];unsigned n=0;
    do{reverse[n++]=(char)('0'+value%10);value/=10;}while(value);
    while(n)out[at++]=reverse[--n];
    return at;
}
static unsigned text(char *out,unsigned at,const char *word){while(*word)out[at++]=*word++;return at;}
static unsigned pattern(unsigned i,unsigned id){return (i*37u+(i>>12)*17u+id*13u)&255u;}
int main(void){
    unsigned id=bos_task_id(),bytes=bos_workspace_bytes();unsigned char *work=bos_workspace();
    BosMemoryInfo info;
    if(!id||bos_memory_info(&info,sizeof info)!=BOS_OK||info.format!=2||info.table_pages!=2)return 31;
    for(unsigned i=0;i<bytes;i++)if(work[i])return 32;
    for(unsigned i=0;i<bytes;i++)work[i]=(unsigned char)pattern(i,id);
    char line[80];unsigned n=text(line,0,"Ready workspace ");n=number(line,n,bytes);
    n=text(line,n," task ");n=number(line,n,id);n=text(line,n," owned ");n=number(line,n,info.owned_pages);
    line[n++]='\n';bos_write(line,n);
    for(;;){
        int key;while((key=bos_key())){
            if(key=='q')return 0;
            if(key=='v'){
                for(unsigned i=0;i<bytes;i++)if(work[i]!=pattern(i,id))return 33;
                n=text(line,0,"Verified survivor task ");n=number(line,n,id);line[n++]='\n';bos_write(line,n);
            }
        }
        if(bos_sleep(25))return 34;
    }
}
