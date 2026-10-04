/* Ordinary BEX1 application for public-platform contract validation.
 * Every observation is published through the supported native canvas. The app
 * uses no kernel addresses, test hooks, injected calls, or privileged services.
 * Handles entered with H are copied from another ordinary client's display to
 * check the documented owner rejection; their encoding is never interpreted. */
#include "baseos.h"
#define MAGIC 0x504c4154u
#define WORDS 18u
#define SHARED_BYTES 8192u
static BosAbiInfo abi;
static BosFileInfo file, verify_file;
static unsigned rows[WORDS], page, keys, loops, last_draw, errors, last_gap, max_gap;
static unsigned operation, pending, foreign, entering, entered, digits;
static int file_result, sync_result=BOS_E_STALE, foreign_result=BOS_E_STALE;
static unsigned read_hash, verify_result, verified, verify_index, verify_offset;
static unsigned verify_hash, manifest[12], verify_running, max_call_ticks;
static unsigned char bytes[SHARED_BYTES], chunk[BOS_FILE_CHUNK_MAX];
static char payload_path[]="/Documents/payload0.bin";
static const char shared[]="/Documents/platform.bin";
static unsigned hash(const unsigned char *p,unsigned n,unsigned h){
    for(unsigned i=0;i<n;i++)h=(h^p[i])*16777619u;
    return h;
}
static void draw(void){
    rows[0]=MAGIC;rows[1]=abi.process;rows[2]=bos_task_id();rows[3]=page;
    rows[4]=keys;rows[5]=loops;rows[6]=bos_ticks();
    if(page==1){
        rows[7]=abi.struct_size;rows[8]=abi.abi_major;rows[9]=abi.abi_minor;
        rows[10]=abi.features;rows[11]=abi.context;rows[12]=abi.user_bytes;
        rows[13]=abi.image_bytes;rows[14]=abi.stack_reserved_bytes;
        rows[15]=abi.file_chunk_bytes;rows[16]=abi.replace_bytes;rows[17]=abi.file_bytes;
    }else if(page==2){
        rows[7]=abi.files_per_process;rows[8]=abi.files_total;
        rows[9]=abi.operations_per_process;rows[10]=abi.operations_total;
        rows[11]=abi.wait_milliseconds;rows[12]=abi.ticks_per_second;
        rows[13]=abi.processes_total;rows[14]=abi.path_bytes;
        rows[15]=max_gap;rows[16]=max_call_ticks;rows[17]=errors;
    }else{
        rows[7]=(unsigned)file_result;rows[8]=file.handle;rows[9]=file.revision;
        rows[10]=file.size;rows[11]=read_hash;rows[12]=(unsigned)sync_result;
        rows[13]=operation;rows[14]=pending;rows[15]=(unsigned)foreign_result;
        rows[16]=verify_result;rows[17]=errors;
    }
    bos_rect(0,0,320,200,0);
    bos_rect(0,0,8,8,10);bos_rect(8,0,8,8,4);
    bos_rect(16,0,8,8,6);bos_rect(24,0,8,8,8);
    for(unsigned row=0;row<WORDS;row++)for(unsigned bit=0;bit<32;bit++)
        bos_rect((int)(bit*5),(int)(12+row*10),4,5,(rows[row]&(1u<<bit))?15:0);
    bos_rect(168,12,144,8,errors?4:sync_result==BOS_PENDING?6:7);
    if(entering)bos_rect(168,28,digits*12+4,8,8);
    bos_present();
}
static void open_shared(void){
    if(file.handle){bos_file_close(file.handle);file.handle=0;}
    file_result=bos_file_open(shared,BOS_FILE_OPEN_READ|BOS_FILE_OPEN_WRITE,&file);
    if(file_result==BOS_OK){
        file_result=bos_file_read_at(file.handle,chunk,sizeof chunk,0);
        if(file_result>0)read_hash=hash(chunk,(unsigned)file_result,2166136261u);
    }
}
static void begin(void){
    if(operation){sync_result=BOS_E_BUSY;return;}
    pending=0;sync_result=bos_sync_begin(&operation);
    if(sync_result==BOS_OK)sync_result=bos_sync_poll(operation);
}
static void verify_start(void){
    verify_result=0;verified=verify_index=verify_offset=0;verify_hash=2166136261u;
    if(bos_read_file("/Documents/platform-manifest.bin",manifest,sizeof manifest)!=(int)sizeof manifest||
       manifest[0]!=MAGIC||manifest[1]>4){verify_result=0xffffffffu;errors++;return;}
    int result=bos_file_open(shared,BOS_FILE_OPEN_READ,&verify_file);
    if(result!=BOS_OK){verify_result=(unsigned)result;errors++;return;}
    result=bos_file_read_at(verify_file.handle,bytes,sizeof chunk,0);
    int second=bos_file_read_at(verify_file.handle,bytes+sizeof chunk,sizeof chunk,sizeof chunk);
    bos_file_close(verify_file.handle);verify_file.handle=0;
    if(result!=(int)sizeof chunk||second!=(int)sizeof chunk){verify_result=0xfffffffeu;errors++;return;}
    for(unsigned i=0;i<sizeof bytes;i++)if(bytes[i]!='B'){verify_result=0xfffffffdu;errors++;return;}
    verify_running=1;
}
static void verify_step(void){
    for(unsigned step=0;step<16&&verify_running;step++){
        if(!verify_file.handle){
            payload_path[18]=(char)('0'+verify_index);
            int result=bos_file_open(payload_path,BOS_FILE_OPEN_READ,&verify_file);
            if(result!=BOS_OK){verify_result=(unsigned)result;verify_running=0;errors++;return;}
        }
        int got=bos_file_read_at(verify_file.handle,chunk,sizeof chunk,verify_offset);
        unsigned expected=manifest[2+verify_index*2];
        if(got<=0||(unsigned)got>expected-verify_offset){verify_result=0xfffffffcu;verify_running=0;errors++;return;}
        verify_hash=hash(chunk,(unsigned)got,verify_hash);verify_offset+=(unsigned)got;
        if(verify_offset==expected){
            bos_file_close(verify_file.handle);verify_file.handle=0;
            if(verify_hash!=manifest[3+verify_index*2]){verify_result=0xfffffffbu;verify_running=0;errors++;return;}
            verified++;verify_index++;verify_offset=0;verify_hash=2166136261u;
            if(verify_index==manifest[1]){verify_result=MAGIC;verify_running=0;}
        }
    }
}
int main(void){
    int result=bos_abi_query(&abi,sizeof abi);
    if(result<0){
        file_result=result;
        if(!bos_canvas_size(320,200))draw();
        bos_print("Platform query unavailable: documented BEX1 fallback.\n");return 0;
    }
    if(bos_canvas_size(320,200)){return 2;}
    bos_print("Platform client: O open, R read, W replace B, X replace C.\n");
    bos_print("S sync, L release, T timed wait, H handle, V verify, 0/1/2 pages.\n");
    bos_print("A echoes input. Q exits normally.\n");
    unsigned previous=bos_ticks();last_draw=previous;
    if(!bos_task_id())page=1;
    draw();
    if(!bos_task_id())return 0;
    for(;;){
        unsigned now=bos_ticks();last_gap=now-previous;previous=now;
        if(last_gap>max_gap)max_gap=last_gap;
        loops++;unsigned redraw=0;int key;
        while((key=bos_key())){
            keys++;redraw=1;unsigned started=bos_ticks();
            if(entering){
                if(key=='\n'||key=='\r'){
                    foreign=entered;foreign_result=bos_sync_poll(foreign);entering=0;
                }else if(digits<8){
                    unsigned nibble=key>='0'&&key<='9'?(unsigned)(key-'0'):
                        key>='a'&&key<='f'?(unsigned)(key-'a'+10):16u;
                    if(nibble<16){entered=(entered<<4)|nibble;digits++;}
                }
            }else if(key=='q'||key==27)return errors?1:0;
            else if(key>='0'&&key<='2')page=(unsigned)(key-'0');
            else if(key=='o')open_shared();
            else if(key=='r'){
                file_result=bos_file_read_at(file.handle,chunk,sizeof chunk,sizeof chunk);
                if(file_result>0)read_hash=hash(chunk,(unsigned)file_result,2166136261u);
            }else if(key=='w'||key=='x'){
                for(unsigned i=0;i<sizeof bytes;i++)bytes[i]=key=='w'?'B':'C';
                file_result=bos_file_replace(file.handle,bytes,sizeof bytes,&file);
            }else if(key=='i')file_result=bos_file_info(file.handle,&file);
            else if(key=='s')begin();
            else if(key=='l'){sync_result=bos_sync_release(operation);if(sync_result==BOS_OK)operation=0;}
            else if(key=='t')sync_result=bos_sync_wait(operation,20);
            else if(key=='h'){entering=1;entered=digits=0;}
            else if(key=='v')verify_start();
            unsigned elapsed=bos_ticks()-started;if(elapsed>max_call_ticks)max_call_ticks=elapsed;
        }
        if(operation){
            sync_result=bos_sync_poll(operation);
            if(sync_result==BOS_PENDING)pending++;
        }
        if(verify_running)verify_step();
        now=bos_ticks();
        if(redraw||now-last_draw>=7){last_draw=now;draw();}
        else bos_sleep(0);
    }
}
