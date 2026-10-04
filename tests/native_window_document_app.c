/* Ordinary GUI qualification app. All observation uses public SDK calls,
 * copied Output rows, and a source-decodable visible canvas. No guest hooks. */
#include "baseos_app2.h"
#define DOCUMENT_BYTES 16384u
static const char path[]="/Documents/window-save.txt";
static unsigned char document[DOCUMENT_BYTES],readback[4096];
static BosUiTargetInfoV1 target;
static BosMemoryInfo memory;
static BosHandle operation;
static unsigned state=1,saves,verified,phase,sleeps;
static int result;
static const unsigned char glyphs[36][5]={
    {7,5,5,5,7},{2,6,2,2,7},{7,1,7,4,7},{7,1,7,1,7},{5,5,7,1,1},
    {7,4,7,1,7},{7,4,7,5,7},{7,1,1,1,1},{7,5,7,5,7},{7,5,7,1,7},
    {2,5,7,5,5},{6,5,6,5,6},{3,4,4,4,3},{6,5,5,5,6},{7,4,6,4,7},
    {7,4,6,4,4},{3,4,5,5,3},{5,5,7,5,5},{7,2,2,2,7},{1,1,1,5,2},
    {5,5,6,5,5},{4,4,4,4,7},{5,7,7,5,5},{5,7,7,7,5},{2,5,5,5,2},
    {6,5,6,4,4},{2,5,5,3,1},{6,5,6,5,5},{3,4,2,1,6},{7,2,2,2,2},
    {5,5,5,5,7},{5,5,5,5,2},{5,5,7,7,5},{5,5,2,5,5},{5,5,2,2,2},{7,1,2,4,7}
};
static char *text(char *out,const char *s){while(*s)*out++=*s++;*out=0;return out;}
static char *number(char *out,unsigned n){
    char digits[10];unsigned count=0;do{digits[count++]=(char)('0'+n%10);n/=10;}while(n);
    while(count)*out++=digits[--count];
    *out=0;return out;
}
static char *signed_number(char *out,int n){
    unsigned value=(unsigned)n;if(n<0){*out++='-';value=0u-value;}return number(out,value);
}
static char *field(char *out,const char *name,unsigned n){return number(text(out,name),n);}
static void label(int y,const char *s,unsigned color){
    for(int x=2;*s&&x<155;s++,x+=4)for(unsigned row=0;row<5;row++){
        unsigned c=(unsigned char)*s,bits=0;
        if(c>='0'&&c<='9')bits=glyphs[c-'0'][row];
        else if(c>='A'&&c<='Z')bits=glyphs[c-'A'+10][row];
        else if(c=='-')bits=row==2?7:0;
        else if(c=='=')bits=row==1||row==3?7:0;
        for(unsigned col=0;col<3;col++)if(bits&(1u<<(2-col)))bos_plot(x+(int)col,y+(int)row,color);
    }
}
static void draw(void){
    char line[81],*p;bos_rect(0,0,160,100,0);label(2,"WINDOW DOC V1",8);
    p=field(line,"STATE=",state);(void)p;label(9,line,7);
    p=field(line,"SAVE=",saves);field(p," VERIFY=",verified);label(16,line,7);
    p=field(line,"OWN=",memory.owned_pages);field(p," FREE=",memory.pool_free_pages);label(23,line,7);
    p=field(line,"SLOT=",bos_task_id());field(p," PHASE=",phase);label(30,line,6);
    p=field(line,"SLEEP=",sleeps);signed_number(text(p," RESULT="),result);label(37,line,8);
    bos_rect(8,52,144,40,phase&1?6:8);bos_present();
}
static void report(const char *tag){
    char line[256],*p=text(line,"WINDOW DOC ");p=text(p,tag);
    p=field(p," state=",state);p=field(p," saved=",saves);p=field(p," verified=",verified);
    p=field(p," owned_pages=",memory.owned_pages);p=field(p," free_pages=",memory.pool_free_pages);
    p=field(p," total_pages=",memory.pool_total_pages);p=signed_number(text(p," result="),result);
    text(p,"\n");bos_print(line);
}
static void memory_refresh(void){
    int status=bos_memory_info(&memory,sizeof memory);
    if(status!=BOS_OK){result=status;state=6;}
}
static void save_begin(void){
    if(operation)return;
    BosFileInfo file;
    result=bos_file_open(path,BOS_FILE_OPEN_READ|BOS_FILE_OPEN_WRITE,&file);
    if(result==BOS_E_NOT_FOUND)result=bos_file_open(path,BOS_FILE_OPEN_READ|BOS_FILE_OPEN_WRITE|BOS_FILE_OPEN_CREATE,&file);
    if(result!=BOS_OK){state=6;report("OPEN ERROR");draw();return;}
    result=bos_file_replace(file.handle,document,sizeof document,&file);
    int closed=bos_file_close(file.handle);if(result==BOS_OK)result=closed;
    if(result==BOS_OK)result=bos_sync_begin(&operation);
    state=result==BOS_OK?2:6;report("SAVE BEGIN");draw();
}
static void save_poll(void){
    if(!operation)return;
    result=bos_sync_wait(operation,10);
    if(result==BOS_PENDING||result==BOS_E_TIMEOUT)return;
    int released=bos_sync_release(operation);operation=0;
    if(result==BOS_OK)result=released;
    if(result==BOS_OK){saves++;state=3;}else state=6;
    memory_refresh();report("SAVE COMPLETE");draw();
}
static void verify(void){
    BosFileInfo file;verified=0;
    result=bos_file_open(path,BOS_FILE_OPEN_READ,&file);
    if(result==BOS_OK){
        if(file.size!=sizeof document)result=BOS_E_CHANGED;
        for(unsigned offset=0;result==BOS_OK&&offset<sizeof document;offset+=sizeof readback){
            int count=bos_file_read_at(file.handle,readback,sizeof readback,offset);
            if(count!=(int)sizeof readback){result=count<0?count:BOS_E_CHANGED;break;}
            for(unsigned i=0;i<sizeof readback;i++)if(readback[i]!=document[offset+i]){result=BOS_E_CHANGED;break;}
        }
        int closed=bos_file_close(file.handle);if(result==BOS_OK)result=closed;
    }
    verified=result==BOS_OK;state=verified?5:6;report("VERIFY");draw();
}
int main(void){
    BosAbiInfo abi;result=bos_abi_query(&abi,sizeof abi);
#ifdef BOS_APP_NATIVE_WINDOW_V1
    unsigned required=BOS_FEATURE_OWNED_NATIVE_WINDOW|BOS_FEATURE_OWNED_SYNC|BOS_FEATURE_MEMORY_INFO;
#else
    /* Ordinary hosted build used only as an old-kernel public page observer. */
    unsigned required=BOS_FEATURE_HOSTED_UI|BOS_FEATURE_OWNED_SYNC|BOS_FEATURE_MEMORY_INFO;
#endif
    if(result!=BOS_OK||(abi.features&required)!=required){bos_print("WINDOW DOC unsupported ABI\n");return 7;}
#ifdef BOS_APP_NATIVE_WINDOW_V1
    result=bos_ui_window_adopt(BOS_UI_SUB_POINTER,&target);
    unsigned expected_kind=BOS_UI_KIND_OWNED_WINDOW;
#else
    result=bos_ui_host_open(BOS_UI_SUB_POINTER,&target);
    unsigned expected_kind=BOS_UI_KIND_HOSTED_CANVAS;
#endif
    if(result!=BOS_OK||target.kind!=expected_kind){bos_print("WINDOW DOC endpoint failed\n");return 7;}
    for(unsigned i=0;i<sizeof document;i++)document[i]=i%80==79?'\n':(unsigned char)(32+(i*17+31)%95);
    memory_refresh();report("READY");draw();
    for(;;){
        BosUiEventV1 event;while(bos_ui_read(target.target,&event)==BOS_OK){}
        int key;while((key=bos_key())>0){
            if(key=='q'||key=='0'||key==27){report("EXIT ZERO");bos_ui_release(target.target);return 0;}
            if(key=='7'){result=7;report("EXIT SEVEN");bos_ui_release(target.target);return 7;}
            if(key=='p'){phase++;draw();}
            if(key=='m'){memory_refresh();report("MEMORY");draw();}
            if(key=='v')verify();
            if(key=='s')save_begin();
            if(key=='h'||key=='a'){
                sleeps++;state=4;report("SLEEP");draw();bos_sleep(2000);state=1;
                if(key=='a')save_begin();else {report("AWAKE");draw();}
            }
            if(key=='l')for(unsigned i=0;i<64;i++){char line[40];text(number(text(line,"WINDOW LOG ROW "),i),"\n");bos_print(line);}
        }
        save_poll();
        bos_ui_wait(target.target,BOS_UI_WAIT_QUEUE|BOS_UI_WAIT_LEGACY_KEY,operation?10:100);
    }
}
