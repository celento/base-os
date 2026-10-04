#include "workspace_common.h"
/* Seed a full committed workspace from versioned document chunks, then verify
 * each word and calculate a deterministic unsigned checksum while yielding. */
int main(void) {
    if(!workspace_ready())return 1;
    BosFileInfo input;
    if(open_input(&input)!=BOS_OK){bos_print("Cannot open versioned input.\n");return 2;}
    unsigned char chunk[BOS_FILE_CHUNK_MAX];unsigned bytes=0,seed=0;
    for(;;){
        int got=bos_file_read_at(input.handle,chunk,sizeof chunk,bytes);
        if(got<0){bos_file_close(input.handle);bos_print("Input changed or read failed.\n");return 3;}
        if(!got)break;
        for(int i=0;i<got;i++)seed=seed*33u+chunk[i];
        bytes+=(unsigned)got;bos_yield();
    }
    if(bos_file_close(input.handle)!=BOS_OK)return 4;
    unsigned *values=bos_workspace();unsigned count=bos_workspace_bytes()/sizeof *values;
    for(unsigned i=0;i<count;i++){
        if(values[i]){bos_print("Workspace was not initially zero.\n");return 5;}
        values[i]=(i*1664525u+1013904223u)^seed;
        if(!(i%4096u))bos_yield();
    }
    unsigned checksum=0;
    for(unsigned i=0;i<count;i++){
        unsigned expected=(i*1664525u+1013904223u)^seed;
        if(values[i]!=expected){bos_print("Workspace verification failed.\n");return 6;}
        checksum+=values[i];if(!(i%4096u))bos_yield();
    }
    char report[192];unsigned n=append_text(report,0,"BEX2 workspace array\nInput bytes: ");
    n=append_number(report,n,bytes);n=append_text(report,n,"\nWords: ");
    n=append_number(report,n,count);n=append_text(report,n,"\nSeed: ");
    n=append_number(report,n,seed);n=append_text(report,n,"\nChecksum: ");
    n=append_number(report,n,checksum);n=append_text(report,n,"\n");
    bos_write(report,n);
    if(save_report("workspace-array",report,n)!=BOS_OK){bos_print("Report was not confirmed durable.\n");return 7;}
    return 0;
}
