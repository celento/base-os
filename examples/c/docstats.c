#include "baseos.h"
/* A streaming document tool. A startup path takes priority over stats-path.txt;
 * R rereads that input. Each terminal saves its own report. */
static unsigned char chunk[BOS_FILE_CHUNK_MAX];
static unsigned histogram[256],bytes,words,lines,checksum;
static char path[129];
static char startup_path[BOS_ARGUMENT_MAX+1];
static unsigned owner,ready;
static const unsigned char glyphs[36][5]={
    {7,5,5,5,7},{2,6,2,2,7},{7,1,7,4,7},{7,1,7,1,7},{5,5,7,1,1},
    {7,4,7,1,7},{7,4,7,5,7},{7,1,1,1,1},{7,5,7,5,7},{7,5,7,1,7},
    {2,5,7,5,5},{6,5,6,5,6},{3,4,4,4,3},{6,5,5,5,6},{7,4,6,4,7},
    {7,4,6,4,4},{3,4,5,5,3},{5,5,7,5,5},{7,2,2,2,7},{1,1,1,5,2},
    {5,5,6,5,5},{4,4,4,4,7},{5,7,7,5,5},{5,7,7,7,5},{2,5,5,5,2},
    {6,5,6,4,4},{2,5,5,3,1},{6,5,6,5,5},{3,4,2,1,6},{7,2,2,2,2},
    {5,5,5,5,7},{5,5,5,5,2},{5,5,7,7,5},{5,5,2,5,5},{5,5,2,2,2},
    {7,1,2,4,7}
};
static void text(int x,int y,const char *s,unsigned color){
    for(;*s;s++,x+=8){
        int g=*s>='0'&&*s<='9'?*s-'0':*s>='A'&&*s<='Z'?*s-'A'+10:-1;
        if(g<0)continue;
        for(unsigned row=0;row<5;row++)for(unsigned col=0;col<3;col++)
            if(glyphs[g][row]&(4u>>col))bos_rect(x+(int)col*2,y+(int)row*2,2,2,color);
    }
}
static unsigned number(char *out,unsigned n){
    char reverse[10];unsigned count=0;
    do{reverse[count++]=(char)('0'+n%10);n/=10;}while(n);
    for(unsigned i=0;i<count;i++)out[i]=reverse[count-i-1];
    out[count]=0;return count;
}
static void metric(int x,int y,const char *label,unsigned value){
    char n[11];text(x,y,label,7);number(n,value);text(x+48,y,n,15);
}
static void draw(unsigned expected){
    bos_rect(0,0,320,200,0);
    text(12,8,ready?"DOCUMENT STATS":"READING DOCUMENT",15);
    metric(12,28,"BYTES",bytes);metric(172,28,"WORDS",words);
    metric(12,45,"LINES",lines);text(172,45,"BYTE FREQUENCY",7);
    bos_rect(30,65,258,1,8);bos_rect(30,177,258,1,8);
    unsigned max=1;
    for(unsigned i=0;i<256;i++)if(histogram[i]>max)max=histogram[i];
    for(unsigned i=0;i<256;i++){
        unsigned height=histogram[i]*110u/max;
        if(height)bos_rect(31+(int)i,177-(int)height,1,height,i<32?6:i<127?7:8);
    }
    if(ready)text(12,187,"R RELOAD   S SAVE   Q EXIT",7);
    else{
        /* Divide first: bytes*296 would overflow for a supported 16 MiB file. */
        unsigned progress=expected?bytes/((expected+295u)/296u):0;
        if(progress>296)progress=296;
        bos_rect(12,187,progress,5,7);
    }
    bos_present();
}
static int input_path(void){
    static const char config[]="/Documents/stats-path.txt";
    static const char fallback[]="/Documents/stats-sample.txt";
    if(startup_path[0]){
        unsigned length=bos_strlen(startup_path);
        for(unsigned i=0;i<=length;i++)path[i]=startup_path[i];
        return 0;
    }
    int size=bos_file_size(config);
    if(size<0){for(unsigned i=0;i<sizeof fallback;i++)path[i]=fallback[i];return 0;}
    if(size<1||size>130)return -1;
    char setting[131];int got=bos_read_file(config,setting,sizeof setting-1);
    if(got!=size)return -1;
    while(got&&(setting[got-1]=='\r'||setting[got-1]=='\n'))got--;
    if(!got||got>128||setting[0]!='/')return -1;
    for(int i=0;i<got;i++){
        if(setting[i]<32||setting[i]>126)return -1;
        path[i]=setting[i];
    }
    path[got]=0;return 0;
}
static int analyze(void){
    ready=0;bytes=words=lines=checksum=0;
    for(unsigned i=0;i<256;i++)histogram[i]=0;
    if(input_path()){bos_print("Use one absolute path, at most 128 bytes, in stats-path.txt.\n");return -1;}
    int expected=bos_file_size(path);
    if(expected<0){bos_print("Input file is missing: ");bos_print(path);bos_print("\n");return -1;}
    bos_print("Reading: ");bos_print(path);bos_print("\n");
    unsigned in_word=0,last=0,chunks=0;
    for(;;){
        int got=bos_read_file_at(path,chunk,sizeof chunk,bytes);
        if(got<0){bos_print("Read failed; the document may have changed.\n");return -1;}
        if(!got)break;
        for(int i=0;i<got;i++){
            unsigned c=chunk[i],space=c==' '||(c>=9&&c<=13);
            histogram[c]++;checksum+=c;
            if(!space&&!in_word)words++;
            in_word=!space;if(c=='\n')lines++;last=c;
        }
        bytes+=(unsigned)got;
        ++chunks;
        /* Each read stays 4 KiB. Redraw per 256 KiB and yield per 64 KiB, rather
         * than scheduling a full desktop turn for every small read. PIT still
         * preempts user execution, including processing any individual chunk. */
        if(!(chunks%64u))draw((unsigned)expected);
        else if(owner&&!(chunks%16u))bos_yield();
    }
    /* Stat/read are independent calls; reject a changed-length source. Same-size
     * concurrent edits cannot be detected without a filesystem snapshot API. */
    if(bytes!=(unsigned)expected||bos_file_size(path)!=expected){
        bos_print("Input length changed. Press R to read it again.\n");return -1;
    }
    if(bytes&&last!='\n')lines++;
    ready=1;draw(bytes);bos_print("Ready. R reloads, S saves a report, Q exits.\n");return 0;
}
static unsigned append(char *out,unsigned at,const char *s){while(*s)out[at++]=*s++;return at;}
static void save_report(void){
    if(!ready){bos_print("Reload a document before saving a report.\n");return;}
    char report[320],n[11],destination[]="/Documents/stats-1.txt";
    destination[17]=(char)('0'+owner);
    unsigned used=append(report,0,"Document: ");used=append(report,used,path);
    used=append(report,used,"\nBytes: ");number(n,bytes);used=append(report,used,n);
    used=append(report,used,"\nWords: ");number(n,words);used=append(report,used,n);
    used=append(report,used,"\nLines: ");number(n,lines);used=append(report,used,n);
    used=append(report,used,"\nByte sum: ");number(n,checksum);used=append(report,used,n);
    used=append(report,used,"\n");
    if(bos_replace_file(destination,report,used)!=(int)used){bos_print("Report write failed; check disk capacity.\n");return;}
    if(bos_sync()){bos_print("Report is in RAM, but disk sync failed. Press S to retry.\n");return;}
    bos_print("Saved and synchronized: ");bos_print(destination);bos_print("\n");
}
int main(void){
    owner=bos_task_id();
    if(bos_argument(startup_path,sizeof startup_path)<0){
        bos_print("This app needs the native startup-argument API.\n");return 1;
    }
    if(bos_canvas_size(320,200)){bos_print("This app needs the 320x200 native canvas API.\n");return 1;}
    bos_print("Document Stats streams files in 4096-byte chunks.\n");
    bos_print(startup_path[0]?"Using the startup document; R rereads it.\n":
              "Choose an input in /Documents/stats-path.txt; R reloads.\n");
    analyze();
    if(!owner)return ready?0:1;
    for(;;){
        int key=bos_key();
        if(key=='q'||key=='Q'||key==27)return 0;
        if(key=='r'||key=='R')analyze();
        if(key=='s'||key=='S')save_report();
        bos_sleep(30);
    }
}
