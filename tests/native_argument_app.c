#include "baseos.h"
/* Valid ring-3 argument reads across yields and an intentionally small, in-range
 * output buffer. No illegal instructions, faulting pointers or malformed images. */
int main(void){
    char argument[BOS_ARGUMENT_MAX+2],again[BOS_ARGUMENT_MAX+2];
    int length=bos_argument(0,0);unsigned owner=bos_task_id();
    if(length<0||length>(int)BOS_ARGUMENT_MAX)return 1;
    for(unsigned i=0;i<sizeof argument;i++)argument[i]=again[i]='#';
    if(length&&bos_argument(argument,(unsigned)length)!=-1)return 2;
    for(unsigned i=0;i<sizeof argument;i++)if(argument[i]!='#')return 3;
    if(bos_argument(argument,(unsigned)length+1)!=length||argument[length]||argument[length+1]!='#')return 4;
    if(!owner)return length?5:0;
    bos_yield();bos_sleep(30);
    if(bos_argument(again,sizeof again)!=length)return 6;
    for(int i=0;i<=length;i++)if(argument[i]!=again[i])return 7;
    char path[]="/Documents/argument-1.txt";path[20]=(char)('0'+owner);
    const char *data=length?argument:"none";unsigned size=length?(unsigned)length:4u;
    return bos_write_file(path,data,size)==(int)size?0:8;
}
