#include "baseos.h"
/* A compact document model generates 256 KiB through one reusable 4 KiB buffer.
 * Explicit controls demonstrate unpublished create, captured replacement,
 * conflicts, Save As and content durability without growing a built-in editor.
 * Compile unchanged as hosted BEX1/BEX2 or an owned-window BEX2 fixture. */
static unsigned char chunk[4096];
static char primary[129],destination[129];
static BosFileTransactionStatusV1 stage;
static BosFileInfo file;
static BosHandle receipt;
static unsigned model_seed=1,stage_seed,accepted_seed,accepted_revision,sync_revision;
static unsigned upload_active,verify_active,verify_offset,copy_number,durable;
static int last_result;
static const char *state="MODEL READY";
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

static char *append(char *out,const char *source){while(*source)*out++=*source++;*out=0;return out;}
static void log_state(const char *tag,unsigned value){
    char line[96],*out=append(line,"STAGED_DOCUMENT ");out=append(out,tag);out=append(out," ");
    number(out,value);out+=bos_strlen(out);append(out,"\n");bos_print(line);
}
static void draw(void){
    bos_rect(0,0,320,200,0);
    text(8,8,"STAGED DOCUMENT",15);text(8,27,state,11);
    char value[11];number(value,model_seed);text(8,46,"MODEL",7);text(72,46,value,15);
    if(!accepted_revision||model_seed!=accepted_seed)text(176,46,"UNSAVED",14);
    text(8,63,"BYTES 262144",7);
    if(stage.handle){
        number(value,stage.received_bytes);text(8,80,"STAGED",7);text(72,80,value,15);
        bos_rect(8,96,stage.received_bytes/864u,4,3);
    }
    if(accepted_revision){text(8,109,durable?(model_seed==accepted_seed?"CONTENT DURABLE":"OLDER CONTENT DURABLE"):"RAM ACCEPTED",durable?10:14);}
    if(last_result<0){number(value,(unsigned)-last_result);text(8,125,"ERROR",12);text(64,125,value,12);}
    text(8,144,"C CREATE   R REPLACE   A ACCEPT",7);
    text(8,160,"S SAVE AS  D SYNC  V VERIFY",7);
    text(8,177,"M EDIT MODEL   Q EXIT",7);bos_present();
}
static unsigned char document_byte(unsigned offset,unsigned seed){
    return offset%80u==79u?'\n':(unsigned char)('A'+(offset/80u+offset%80u+seed)%26u);
}
static void abort_stage(void){
    if(stage.handle)bos_file_transaction_abort(stage.handle);
    stage.handle=0;upload_active=0;
}
static void close_file(void){if(file.handle)bos_file_close(file.handle);file.handle=0;}
static void error(const char *label,int result){state=label;last_result=result;upload_active=verify_active=0;log_state(label,(unsigned)-result);}
static void begin(unsigned creating,unsigned save_as){
    if(receipt){state="WAIT FOR SYNC";return;}
    /* This is an explicit user-selected restart/recovery. The compact model
     * remains unchanged; no old stage is rebound to another target/version. */
    abort_stage();close_file();verify_active=0;accepted_revision=durable=0;last_result=0;
    if(save_as){
        char *out=append(destination,"/Documents/staged-copy-");char n[11];
        number(n,bos_task_id());out=append(out,n);out=append(out,"-");
        number(n,++copy_number);out=append(out,n);append(out,".txt");
    }else append(destination,creating?"/Documents/created.txt":primary);
    int result;
    if(creating)result=bos_file_transaction_begin_create(destination,262144,&stage);
    else{
        result=bos_file_open(destination,BOS_FILE_OPEN_READ|BOS_FILE_OPEN_WRITE,&file);
        if(result==BOS_OK)result=bos_file_transaction_begin_replace(file.handle,file.revision,262144,&stage);
    }
    if(result!=BOS_OK){stage.handle=0;error(result==BOS_E_CHANGED?"CONFLICT MODEL KEPT":"BEGIN REFUSED",result);return;}
    stage_seed=model_seed;upload_active=1;state="UPLOADING PRIVATE";
    bos_print("Staging: ");bos_print(destination);bos_print("\n");
}
static void upload_one(void){
    unsigned offset=stage.received_bytes,count=262144-offset;if(count>sizeof chunk)count=sizeof chunk;
    for(unsigned i=0;i<count;++i)chunk[i]=document_byte(offset+i,stage_seed);
    int result=bos_file_transaction_append(stage.handle,chunk,count,offset);
    if(result!=(int)count){error("UPLOAD MODEL KEPT",result);return;}
    stage.received_bytes+=count;
    if(stage.received_bytes==262144){stage.state=BOS_FILE_TRANSACTION_COMPLETE;upload_active=0;state="STAGED PRESS A";log_state("STAGED",stage.received_bytes);}
}
static void accept(void){
    if(!stage.handle||upload_active){state="FINISH UPLOAD FIRST";return;}
    int result=bos_file_transaction_accept_ram(stage.handle,&file);
    if(result!=BOS_OK){error(result==BOS_E_CHANGED?"CONFLICT MODEL KEPT":result==BOS_E_BUSY?"SNAPSHOT BUSY RETRY A":"ACCEPT MODEL KEPT",result);return;}
    stage.handle=0;accepted_seed=stage_seed;accepted_revision=file.revision;durable=0;last_result=0;
    state="RAM ACCEPTED PRESS D";log_state("RAM_ACCEPTED",accepted_revision);
}
static void sync_begin(void){
    if(!accepted_revision||!file.handle){state="ACCEPT CONTENT FIRST";return;}
    if(receipt)return;
    int result=bos_sync_begin(&receipt);
    if(result!=BOS_OK){receipt=0;error("SYNC REFUSED RAM KEPT",result);return;}
    sync_revision=accepted_revision;durable=0;last_result=0;state="DURABILITY PENDING";
}
static void sync_poll(void){
    int result=bos_sync_poll(receipt);
    if(result==BOS_PENDING)return;
    int released=bos_sync_release(receipt);receipt=0;
    if(result!=BOS_OK){error("SYNC FAILED RAM KEPT",result);return;}
    if(released!=BOS_OK){error("SYNC RECEIPT ERROR",released);return;}
    BosFileInfo current;result=bos_file_info(file.handle,&current);
    if(result!=BOS_OK||current.revision!=sync_revision){error("CONTENT NOT CONFIRMED",result==BOS_OK?BOS_E_CHANGED:result);return;}
    durable=1;state=model_seed==accepted_seed?"CONTENT DURABLE":"OLDER CONTENT DURABLE";log_state("DURABLE_CONFIRMED",sync_revision);
}
static void verify_one(void){
    unsigned count=file.size-verify_offset;if(count>sizeof chunk)count=sizeof chunk;
    int result=bos_file_read_at(file.handle,chunk,count,verify_offset);
    if(result!=(int)count){error("VERIFY VERSION CHANGED",result);return;}
    for(unsigned i=0;i<count;++i)if(chunk[i]!=document_byte(verify_offset+i,accepted_seed)){error("VERIFY BYTE DIFFERENCE",BOS_E_CHANGED);return;}
    verify_offset+=count;
    if(verify_offset==file.size){verify_active=0;state=model_seed==accepted_seed?"EXACT CONTENT VERIFIED":"OLDER CONTENT VERIFIED";log_state("VERIFY_EXACT",file.size);}
}
int main(void){
    BosFileTransactionInfoV1 info;int result=bos_file_transaction_query(&info,sizeof info);
    if(result!=BOS_OK||info.chunk_bytes<sizeof chunk||info.total_bytes<262144||
       (info.capabilities&3u)!=3u){bos_print("Staged document needs desktop IDE file transactions; no fallback write.\n");return 1;}
    if(bos_canvas_size(320,200)!=0)return 1;
    if(bos_argument(primary,sizeof primary)<=0)append(primary,"/Documents/staged.txt");
    bos_print("C creates /Documents/created.txt; R stages the current input version.\n");
    bos_print("A accepts RAM; D confirms that content on disk; V verifies every byte.\n");
    bos_print("S explicitly saves the preserved model under a new name after conflict.\n");
    for(;;){
        int key;while((key=bos_key())!=0){
            if(key=='q'||key=='Q'||key==27){abort_stage();if(receipt)bos_sync_release(receipt);close_file();return 0;}
            if(key=='c'||key=='C')begin(1,0);
            if(key=='r'||key=='R')begin(0,0);
            if(key=='s'||key=='S')begin(1,1);
            if(key=='a'||key=='A')accept();
            if(key=='d'||key=='D')sync_begin();
            if(key=='m'||key=='M'){abort_stage();verify_active=0;if(model_seed!=~0u)model_seed++;state="MODEL CHANGED UNSAVED";last_result=0;}
            if((key=='v'||key=='V')&&accepted_revision&&file.handle){verify_offset=0;verify_active=1;state="VERIFYING CONTENT";last_result=0;}
        }
        if(upload_active)upload_one();
        if(verify_active)verify_one();
        if(receipt)sync_poll();
        draw();bos_sleep(20);
    }
}
