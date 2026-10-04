#include "workspace_common.h"
/* Build a line-offset index in the committed workspace. Offsets reference the
 * opened document revision; they are ordinary numbers, never saved pointers. */
int main(void) {
    if(!workspace_ready())return 1;
    unsigned *offsets=bos_workspace();unsigned capacity=bos_workspace_bytes()/sizeof *offsets;
    for(unsigned i=0;i<capacity;i++){
        if(offsets[i]){bos_print("Workspace was not initially zero.\n");return 2;}
        offsets[i]=0xffffffffu;if(!(i%4096u))bos_yield();
    }
    BosFileInfo input;
    if(open_input(&input)!=BOS_OK){bos_print("Cannot open versioned input.\n");return 3;}
    unsigned char chunk[BOS_FILE_CHUNK_MAX];unsigned bytes=0,lines=0,new_line=1;
    for(;;){
        int got=bos_file_read_at(input.handle,chunk,sizeof chunk,bytes);
        if(got<0){bos_file_close(input.handle);bos_print("Input changed or read failed.\n");return 4;}
        if(!got)break;
        for(int i=0;i<got;i++){
            if(new_line){
                if(lines==capacity){bos_file_close(input.handle);bos_print("Line index is full.\n");return 5;}
                offsets[lines++]=bytes+(unsigned)i;
            }
            new_line=chunk[i]=='\n';
        }
        bytes+=(unsigned)got;bos_yield();
    }
    if(bos_file_close(input.handle)!=BOS_OK)return 6;
    unsigned checksum=0;
    for(unsigned i=0;i<capacity;i++){
        if(i<lines)checksum+=offsets[i];
        else if(offsets[i]!=0xffffffffu){bos_print("Index tail verification failed.\n");return 7;}
        if(!(i%4096u))bos_yield();
    }
    char report[192];unsigned n=append_text(report,0,"BEX2 workspace line index\nInput bytes: ");
    n=append_number(report,n,bytes);n=append_text(report,n,"\nIndex capacity: ");
    n=append_number(report,n,capacity);n=append_text(report,n,"\nLines: ");
    n=append_number(report,n,lines);n=append_text(report,n,"\nOffset checksum: ");
    n=append_number(report,n,checksum);n=append_text(report,n,"\n");
    bos_write(report,n);
    if(save_report("workspace-index",report,n)!=BOS_OK){bos_print("Report was not confirmed durable.\n");return 8;}
    return 0;
}
