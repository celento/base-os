#include "baseos.h"
/* This ordinary initialized application data deliberately exceeds 16 KiB on
 * disk. The same 32 KiB buffer then streams a full file and saves a document. */
static unsigned char data[BOS_DOCUMENT_MAX]={17,[BOS_DOCUMENT_MAX-1]=99};
static unsigned fresh_bss;
static const char input[]="/Documents/native-input.bin";
static unsigned char pattern(unsigned i){return (unsigned char)(i*37u+(i>>16)+11u);}
static void require(int ok,int code){if(!ok)bos_exit(code);}
int main(void){
    unsigned owner=bos_task_id(),total=0,sum=0,chunks=0;
    require(owner&&data[0]==17&&data[BOS_DOCUMENT_MAX-1]==99&&!fresh_bss,1);
    require(bos_file_size(input)==2097152,2);
    require(!bos_canvas_size(320,200),3);
    bos_rect(0,0,320,200,owner+10);
    bos_rect(-3,-3,4,4,45);bos_plot(319,199,owner+20);bos_present();
    require(bos_read_file(input,data,4096)==4096,4);
    for(unsigned i=0;i<4096;i++)require(data[i]==pattern(i),5);
    data[0]=77;require(!bos_read_file_at(input,data,0,7)&&data[0]==77,6);
    require(!bos_read_file_at(input,data,4096,2097152)&&data[0]==77,7);
    require(!bos_read_file_at(input,data,4096,0xffffffffu)&&data[0]==77,8);
    for(unsigned i=0;i<16;i++)data[i]=88;
    require(bos_read_file_at(input,data,16,2097145)==7&&data[7]==88,9);
    for(unsigned i=0;i<7;i++)require(data[i]==pattern(2097145+i),10);
    while(total<2097152){
        int size=bos_read_file_at(input,data,4096,total);require(size==4096,11);
        for(int i=0;i<size;i++){require(data[i]==pattern(total+(unsigned)i),12);sum+=data[i];}
        total+=(unsigned)size;chunks++;
        bos_rect(10,10,300,8,owner+10);bos_rect(10,10,total*300u/2097152u,8,7);
        require(!bos_yield(),13);
    }
    for(unsigned i=0;i<sizeof data;i++)data[i]=(unsigned char)(i*13u+owner);
    unsigned *record=(unsigned *)data;
    record[0]=owner;record[1]=total;record[2]=sum;record[3]=chunks;
    char output[]="/Documents/native-1.bin";output[18]=(char)('0'+owner);
    require(bos_replace_file(output,data,sizeof data)==sizeof data,14);
    require(!bos_sync(),15);
    /* Stay live with independent canvases until the guest asks us to finish. */
    while(bos_key()!='q')bos_sleep(20);
    return 0;
}
