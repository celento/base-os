#define FEATURE_TEST
#include "../src/kernel.c"
static void check(int ok,const char *why){if(!ok)panic(why);}
static void command(const char *s){while(*s)term_char(*s++);term_enter();}
static int file(const char *path){return fs_resolve(fs_root(),path);}
static int has_line(const char *s){
    for(int i=0;i<term_count();i++)if(!kstrcmp(term_get(i),s))return 1;
    return 0;
}
static void tick(void){term_task_poll();poll_time();platform_poll();__asm__ volatile("hlt");}
static void until_done(int owner){
    unsigned began=timer_ticks();
    while(term_task_running(owner)&&timer_ticks()-began<5*TIMER_HZ)tick();
    check(!term_task_running(owner),"SDK app did not exit");
}
static void verify_record(unsigned owner){
    char path[]="/Documents/native-1.bin";path[18]=(char)('0'+owner);
    int id=file(path);check(id>=0&&fs_size(id)==32768,"SDK 32 KiB report missing");
    const unsigned *r=(const unsigned *)fs_data(id);
    check(r[0]==owner&&r[1]==2097152&&r[2]==267386880&&r[3]==512,"SDK streaming record mismatch");
    const unsigned char *bytes=(const unsigned char *)fs_data(id);
    for(unsigned i=16;i<32768;i++)check(bytes[i]==(unsigned char)(i*13u+owner),"SDK replacement truncated");
}
static void report_ready(int owner,const char *expected){
    term_select(owner);unsigned began=timer_ticks();
    while(!has_line("Ready. R reloads, S saves a report, Q exits.")&&timer_ticks()-began<20*TIMER_HZ){
        check(term_task_running(owner),"DocStats quit before ready");tick();
    }
    check(has_line("Ready. R reloads, S saves a report, Q exits."),"DocStats read deadline");
    check(term_canvas_width()==320&&term_canvas_height()==200&&term_canvas(),"DocStats canvas mode");
    term_task_key(owner,'s');began=timer_ticks();
    char path[]="/Documents/stats-1.txt";path[17]=(char)('1'+owner);
    int id;
    while(((id=file(path))<0||fs_needs_sync())&&timer_ticks()-began<20*TIMER_HZ)tick();
    check(id>=0&&!kstrcmp(fs_data(id),expected)&&!fs_needs_sync(),"DocStats saved report mismatch");
    term_task_key(owner,'q');until_done(owner);
}
void feature_test(void){
    check(fs_file_limit()==FS_FILE_MAX,"SDK requires disposable IDE data disk");
    int done=file("/Documents/sdk-done.txt");
    if(done>=0){
        verify_record(1);verify_record(2);
        check(fs_used_bytes()==fs_capacity(),"SDK reboot full volume mismatch");
        term_select(0);term_reset();command("exec /Programs/capacity.bex");
        check(has_line("Program finished."),"SDK full-volume reboot regression");
        platform_log("NATIVE-SDK-EXPANDED-REBOOT-PASS\n");return;
    }
    check(fs_size(file("/Programs/stream.bex"))>32768,"SDK large C image absent");
    check(fs_size(file("/Programs/maximage.bex"))==PROCESS_IMAGE_LIMIT,"SDK exact-cap image absent");
    term_select(0);term_reset();command("exec /Programs/maximage.bex");
    check(has_line("Program finished."),"SDK 48 KiB executable failed");
    command("start /Programs/stream.bex");
    term_select(1);term_reset();command("start /Programs/stream.bex");
    term_select(2);term_reset();term_char('x');
    unsigned began=timer_ticks();
    while((file("/Documents/native-1.bin")<0||file("/Documents/native-2.bin")<0||fs_needs_sync())&&timer_ticks()-began<60*TIMER_HZ){
        check(term_task_running(0)&&term_task_running(1),"SDK streaming task failed");tick();
    }
    check(!kstrcmp(term_input(),"x"),"SDK polling changed selected terminal");
    verify_record(1);verify_record(2);
    for(int owner=0;owner<2;owner++){
        term_select(owner);
        check(term_canvas_width()==320&&term_canvas_height()==200,"SDK concurrent geometry lost");
        const unsigned char *pixels=term_canvas();
        check(pixels&&pixels[0]==45&&pixels[199*320+319]==owner+21,"SDK concurrent canvas lost");
    }
    platform_log("NATIVE-SDK-STREAM-2M-CONCURRENT-PASS\n");
    term_task_key(0,'q');term_task_key(1,'q');until_done(0);until_done(1);
    term_select(0);command("exec /Programs/hello-c.bex");
    check(has_line("Program finished.")&&term_canvas_width()==160&&term_canvas_height()==100,"SDK legacy canvas reset");
    const unsigned char *pixels=term_canvas();
    check(pixels&&pixels[0]==128&&pixels[15*160+15]==134,"SDK legacy C app changed");
    command("exec /Programs/notebook.bex");
    check(has_line("Program finished.")&&file("/Documents/sdk-note.txt")>=0,"SDK legacy notebook failed");
    command("basic /Programs/demo.bas");
    check(term_canvas_width()==160&&term_canvas_height()==100,"SDK BASIC geometry reset");
    platform_log("NATIVE-SDK-LEGACY-PASS\n");
    term_reset();command("start /Programs/docstats.bex");
    report_ready(0,"Document: /Documents/stats-sample.txt\nBytes: 56812\nWords: 9021\nLines: 904\nByte sum: 4874156\n");
    int docs=file("/Documents"),config=fs_create(docs,"stats-path.txt");
    int empty=fs_create(docs,"empty.txt");check(config>=0&&empty>=0,"DocStats config fixture");
    const char *empty_path="/Documents/empty.txt\n";
    check(fs_write(config,empty_path,kstrlen(empty_path))==kstrlen(empty_path),"DocStats configure path");
    term_select(1);term_reset();command("start /Programs/docstats.bex");
    report_ready(1,"Document: /Documents/empty.txt\nBytes: 0\nWords: 0\nLines: 0\nByte sum: 0\n");
    platform_log("NATIVE-SDK-DOCSTATS-PASS\n");
    /* Valid full-volume replacement failures retain old data and node count. */
    int capacity=fs_create(docs,"capacity.txt");check(capacity>=0&&fs_write(capacity,"keep",4)==4,"SDK capacity fixture");
    done=fs_create(docs,"sdk-done.txt");check(done>=0&&fs_write(done,"done",4)==4,"SDK completion fixture");
    int fillers[4];for(int i=0;i<4;i++){
        char name[]="sdk-fill-0.bin";name[9]+=(char)i;
        fillers[i]=fs_create(docs,name);check(fillers[i]>=0,"SDK fill create");
    }
    for(int i=0;i<4;i++){
        unsigned remaining=fs_capacity()-fs_used_bytes();if(remaining>FS_FILE_MAX)remaining=FS_FILE_MAX;
        check(fs_write(fillers[i],fs_data(file("/Documents/native-input.bin")),remaining)==(int)remaining,"SDK fill capacity");
    }
    check(fs_used_bytes()==fs_capacity(),"SDK full capacity mismatch");
    int nodes=fs_node_count();term_select(0);term_reset();command("exec /Programs/capacity.bex");
    check(has_line("Program finished.")&&fs_node_count()==nodes&&!kstrcmp(fs_data(capacity),"keep"),"SDK failed write changed original");
    check(fs_sync()==0,"SDK final durable snapshot");
    platform_log("NATIVE-SDK-CAPACITY-PASS\nNATIVE-SDK-EXPANDED-PASS\n");
}
