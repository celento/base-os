/* Inspect only. Loading this tool never executes input instructions. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "executable.h"
static unsigned char image[BOS_BEX2_FILE_MAX+1];
int main(int argc,char **argv) {
    if(argc!=2)return 2;
    FILE *f=fopen(argv[1],"rb");if(!f)return 2;
    size_t size=fread(image,1,sizeof image,f);int more=fgetc(f);fclose(f);
    if(more!=EOF)return 2;
    ExecutablePlan p;ExecutablePolicy policy={1,1,BOS_BEX2_FILE_MAX,1024};
    int r=executable_plan_bex2(image,(uint32_t)size,&policy,&p);
    if(r){fprintf(stderr,"BEX2 plan result %d\n",r);return 1;}
    printf("{\"entry\":%u,\"text\":[%u,%u,%u],\"data\":[%u,%u,%u],"
           "\"workspace\":[%u,%u,%u],\"stack\":[%u,%u,%u],"
           "\"owned_pages\":%u,\"mapped_pages\":%u,\"table_pages\":%u}\n",
           p.entry_offset,p.text.offset,p.text.bytes,p.text.pages,
           p.data.offset,p.data.bytes,p.data.pages,p.workspace.offset,p.workspace.bytes,p.workspace.pages,
           p.stack.offset,p.stack.bytes,p.stack.pages,p.owned_pages,p.mapped_pages,p.table_pages);
    return 0;
}
