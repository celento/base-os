#include "baseos.h"
/* Read-only ordinary reboot verifier. No transaction/sync mutation, kernel
 * diagnostics, retained handle assumption or large application buffer. */
#ifndef DOCUMENT_SEED
#define DOCUMENT_SEED 1u
#endif
#ifndef DOCUMENT_BYTES
#define DOCUMENT_BYTES 262144u
#endif
_Static_assert(DOCUMENT_BYTES<=262144u,"bounded verifier document size");
static unsigned char chunk[4096];
static char path[129];
static void number(unsigned value){char text[11],reverse[10];unsigned n=0;
    do{reverse[n++]=(char)('0'+value%10);value/=10;}while(value);
    for(unsigned i=0;i<n;++i)text[i]=reverse[n-i-1];
    text[n]=0;bos_print(text);
}
int main(void){
    if(bos_argument(path,sizeof path)<=0){const char *p="/Documents/staged.txt";unsigned i=0;do{path[i]=p[i];}while(p[i++]);}
    BosFileInfo file;int result=bos_file_open(path,BOS_FILE_OPEN_READ,&file);
    if(result!=BOS_OK){bos_print("STAGED_REOPEN OPEN_FAILED\n");return 1;}
    if(file.size!=DOCUMENT_BYTES){bos_file_close(file.handle);bos_print("STAGED_REOPEN SIZE_FAILED\n");return 2;}
    unsigned hash=2166136261u;
    for(unsigned offset=0;offset<DOCUMENT_BYTES;){
        unsigned count=DOCUMENT_BYTES-offset;if(count>sizeof chunk)count=sizeof chunk;
        result=bos_file_read_at(file.handle,chunk,count,offset);
        if(result!=(int)count){bos_file_close(file.handle);bos_print("STAGED_REOPEN READ_CHANGED\n");return 3;}
        for(unsigned i=0;i<count;++i){unsigned at=offset+i;
            unsigned char expected=at%80u==79u?'\n':(unsigned char)('A'+(at/80u+at%80u+DOCUMENT_SEED)%26u);
            if(chunk[i]!=expected){bos_file_close(file.handle);bos_print("STAGED_REOPEN BYTE_FAILED\n");return 4;}
            hash=(hash^chunk[i])*16777619u;
        }
        offset+=count;bos_yield();
    }
    BosFileInfo current;result=bos_file_info(file.handle,&current);
    if(result!=BOS_OK||current.revision!=file.revision){bos_file_close(file.handle);bos_print("STAGED_REOPEN VERSION_CHANGED\n");return 5;}
    if(bos_file_close(file.handle)!=BOS_OK)return 6;
    bos_print("STAGED_REOPEN PASS bytes=");number(DOCUMENT_BYTES);bos_print(" seed=");number(DOCUMENT_SEED);
    bos_print(" fnv1a=");number(hash);bos_print(" path=");bos_print(path);bos_print("\n");return 0;
}
