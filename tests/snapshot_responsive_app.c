/* Ordinary protected native client for production QMP verification.
 * No privileged calls, fs_sync_step, bos_sync, kernel pointers or test backdoors. */
#include "baseos.h"
#define REPORT_MAGIC 0x31505253u
#define MAX_KEYS 64u
#define REPORT_WORDS 256u
static unsigned phase, keys, busy, iterations, max_gap, max_busy_gap;
static unsigned start_tick, first_busy, last_busy, release_tick, previous;
static unsigned key_ticks[MAX_KEYS], key_busy[MAX_KEYS], errors, report_pending;
static unsigned report[REPORT_WORDS], manifest[16], readbuf[1024];
static unsigned verify_file, verify_offset, verify_hash=2166136261u, verified, verify_total;
static unsigned persisted, trigger_value, last_draw;
static const char trigger[]="/Documents/trigger.bin";
static const char probe[]="/Documents/probe.bin";
static const char report_path[]="/Documents/report.bin";
static char payload_path[]="/Documents/payload0.bin";
static void row(unsigned y,unsigned value){
    for(unsigned bit=0;bit<32;bit++)bos_rect((int)(bit*5),(int)y,4,5,(value&(1u<<bit))?15:0);
}
static void draw(unsigned now){
    bos_rect(0,0,160,100,0);
    bos_rect(0,0,8,8,4);bos_rect(8,0,8,8,6);
    bos_rect(16,0,8,8,8);bos_rect(24,0,8,8,10);
    row(12,phase);row(22,keys);row(32,busy);row(42,now);
    row(52,iterations);row(62,max_busy_gap);row(72,persisted);row(82,verified);
    bos_rect(0,94,160,6,errors?4:phase==1?6:7);
    bos_present();
}
static void fill_report(void){
    for(unsigned i=0;i<REPORT_WORDS;i++)report[i]=0;
    report[0]=REPORT_MAGIC;report[1]=1;report[2]=BOS_TICKS_PER_SECOND;
    report[3]=start_tick;report[4]=first_busy;report[5]=last_busy;report[6]=release_tick;
    report[7]=busy;report[8]=keys;report[9]=max_gap;report[10]=max_busy_gap;
    report[11]=iterations;report[12]=errors;report[13]=trigger_value;
    for(unsigned i=0;i<keys&&i<MAX_KEYS;i++){report[16+i*2]=key_ticks[i];report[17+i*2]=key_busy[i];}
}
int main(void){
    if(!bos_task_id())return 2;
    if(bos_file_size(trigger)!=1||bos_file_size(probe)!=1||bos_file_size(report_path)!=(int)sizeof report)return 3;
    unsigned char byte=0;
    if(bos_read_file(trigger,&byte,1)!=1)return 4;
    trigger_value=byte;
    if(bos_read_file(report_path,report,sizeof report)!=(int)sizeof report)return 5;
    if(report[0]==REPORT_MAGIC&&byte=='B')persisted=REPORT_MAGIC;
    if(bos_read_file("/Documents/manifest.bin",manifest,sizeof manifest)!=(int)sizeof manifest||
       manifest[0]!=0x31464e4du||manifest[1]>4)return 6;
    bos_print("Snapshot response probe: T starts, A echoes, R reports, V verifies.\n");
    bos_print("Rows: phase, keys, busy count, PIT, iterations, busy gap, report, verified.\n");
    previous=bos_ticks();
    for(;;){
        unsigned now=bos_ticks(),gap=now-previous;previous=now;iterations++;
        if(gap>max_gap)max_gap=gap;
        if(phase==1&&gap>max_busy_gap)max_busy_gap=gap;
        int key;unsigned redraw=0;
        while((key=bos_key())){
            redraw=1;
            if(key=='q'||key==27)return errors?1:0;
            if(key=='a'){
                if(keys<MAX_KEYS){key_ticks[keys]=bos_ticks();key_busy[keys]=phase==1&&busy>0;}
                keys++;
            }
            if(key=='t'&&phase==0){
                byte='B';int result=bos_write_file(trigger,&byte,1);
                if(result==1){phase=1;trigger_value=byte;start_tick=bos_ticks();max_gap=max_busy_gap=0;previous=start_tick;}
                else if(result!=BOS_ERR_BUSY)errors++;
            }
            if(key=='r'&&phase==2)report_pending=1;
            if(key=='v'&&phase!=1){phase=4;verified=0;verify_file=verify_offset=verify_total=0;verify_hash=2166136261u;}
        }
        if(phase==1){
            byte='P';int result=bos_write_file(probe,&byte,1);
            if(result==BOS_ERR_BUSY){if(!busy)first_busy=bos_ticks();last_busy=bos_ticks();busy++;}
            else if(result==1&&busy){release_tick=bos_ticks();phase=2;report_pending=1;redraw=1;}
            else if(result!=1){errors++;phase=7;redraw=1;}
        }
        if(report_pending){
            fill_report();int result=bos_write_file(report_path,report,sizeof report);
            if(result==(int)sizeof report){report_pending=0;phase=3;persisted=REPORT_MAGIC;redraw=1;}
            else if(result!=BOS_ERR_BUSY){errors++;report_pending=0;phase=7;redraw=1;}
        }
        if(phase==4){
            payload_path[18]=(char)('0'+verify_file);
            unsigned length=manifest[2+verify_file*2];
            int got=bos_read_file_at(payload_path,readbuf,sizeof readbuf,verify_offset);
            if(got<=0||(unsigned)got>length-verify_offset){errors++;phase=7;redraw=1;}
            else {
                const unsigned char *bytes=(const unsigned char *)readbuf;
                for(int i=0;i<got;i++)verify_hash=(verify_hash^bytes[i])*16777619u;
                verify_offset+=(unsigned)got;verify_total+=(unsigned)got;
                if(verify_offset==length){
                    if(verify_hash!=manifest[3+verify_file*2]){errors++;phase=7;redraw=1;}
                    else {verified++;verify_file++;verify_offset=0;verify_hash=2166136261u;
                        if(verify_file==manifest[1]){phase=5;redraw=1;}}
                }
            }
        }
        now=bos_ticks();
        if(redraw||now-last_draw>=3){last_draw=now;draw(now);}
        else if(bos_sleep(0)<0)return 7;
    }
}
