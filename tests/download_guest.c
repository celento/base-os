/* Normal desktop/file-download flows using real RTL8139, SB16 and data disk.
 * This fixture only runs with disposable images from download_test.py. */
#define FEATURE_TEST
#include "../src/kernel.c"
#include "../src/download.h"
#ifndef HTTP_PORT
#define HTTP_PORT "18080"
#endif
#define DL_ORIGIN "http://10.0.2.2:" HTTP_PORT
#define DL_PAGE "<!doctype html><title>Saved HTTP page</title><h1>Offline page</h1><p>Original bytes &amp; markup.</p><script>not displayed</script>\n"
static unsigned draws, last_draw;
static int native_slot=-1;
static void dl_check(int okay,const char *why){if(!okay){platform_log("DOWNLOAD-FAIL ");platform_log(why);platform_log(" | ");platform_log(download_status()->message);platform_log("\n");panic(why);}}
static void dl_command(const char *text){while(*text)term_char(*text++);term_enter();}
static int dl_terminal_has(const char *text){for(int i=0;i<term_count();i++)if(!kstrcmp(term_get(i),text))return 1;return 0;}
static void dl_draw(void){render_scene();flip_vga();draws++;last_draw=timer_ticks();}
static void dl_turn(void){
    poll_time();platform_poll();net_poll();download_tick();browser_tick();term_task_poll();audio_poll();
    if(timer_ticks()-last_draw>=7)dl_draw();
}
static void dl_wait(int expected){
    unsigned start=timer_ticks();while(download_active()&&timer_ticks()-start<18*TIMER_HZ){dl_turn();__asm__ volatile("hlt");}
    dl_check(download_status()->state==expected,"download terminal state");
}
static void dl_begin(const char *suffix,const char *path){char url[NET_URL_MAX];kstrcpy(url,DL_ORIGIN);kstrcpy(url+kstrlen(url),suffix);dl_check(download_start(0,url,path)==0,"download starts");}
static void dl_verify(const char *path,unsigned length){
    int id=fs_resolve(0,path);dl_check(id>0&&fs_size(id)==(int)length,"binary file length");
    const unsigned char *data=(const unsigned char *)fs_data(id);
    for(unsigned i=0;i<length;i++)dl_check(data[i]==(unsigned char)(i*37+(i>>16)+91),"exact binary file data");
}
static void dl_absent(const char *path){dl_check(fs_resolve(0,path)<0,"failed request left a destination file");}
static void dl_wait_browser(void){unsigned start=timer_ticks();while(browser_loading()&&timer_ticks()-start<18*TIMER_HZ){dl_turn();__asm__ volatile("hlt");}dl_check(!browser_loading()&&browser_can_save(),"complete browser page");}
static void dl_verify_page(const char *path){int id=fs_resolve(0,path);dl_check(id>0&&fs_size(id)==sizeof DL_PAGE-1&&!kstrcmp(fs_data(id),DL_PAGE),"saved original page bytes");}
void feature_test(void){
    dl_check(fs_file_limit()==FS_FILE_MAX&&!kstrcmp(fs_storage_name(),"IDE data disk"),"data volume required");
    if(fs_resolve(0,"/download-pass")>=0){
        dl_verify("/binary.bin",FS_FILE_MAX);dl_verify("/medium.bin",20037);
        dl_verify_page("/Downloads/page.html");dl_verify_page("/Downloads/page-2.html");
        const char *absent[]={"/cancel.bin","/too-large.bin","/incomplete.bin","/redirect.bin","/missing.bin","/changed/file.bin"};
        for(unsigned i=0;i<sizeof absent/sizeof *absent;i++)dl_absent(absent[i]);
        dl_check(!kstrcmp(fs_data(fs_resolve(0,"/keep.bin")),"Keep this exact file"),"existing file persisted unchanged");
        platform_log("DOWNLOAD-REBOOT-PASS\n");for(;;)__asm__ volatile("hlt");
    }
    dl_check(net_status()->available&&audio_status()->available,"RTL8139 and SB16 required");
    open_term();int transfer_slot=win_front();wins[transfer_slot].x=18;wins[transfer_slot].y=75;wins[transfer_slot].w=600;wins[transfer_slot].h=270;
    dl_command("download " DL_ORIGIN "/medium /medium.bin");dl_check(download_active(),"Terminal download returns immediately");
    dl_command("echo Terminal remains usable");dl_check(dl_terminal_has("Terminal remains usable"),"Terminal responsive during transfer");
    dl_wait(DOWNLOAD_DONE);dl_verify("/medium.bin",20037);platform_log("DOWNLOAD-20KB-ASYNC-PASS\n");
    int keep=fs_create(0,"keep.bin");dl_check(keep>0&&fs_write(keep,"Keep this exact file",20)==20,"prepare existing file");
    dl_check(download_start(0,DL_ORIGIN "/medium","/keep.bin")<0,"existing file start rejected");
    dl_begin("/slow","/cancel.bin");unsigned start=timer_ticks();
    while(download_status()->received<16384&&timer_ticks()-start<4*TIMER_HZ){dl_turn();__asm__ volatile("hlt");}
    dl_check(download_active()&&download_status()->received>=16384,"download progress before cancel");
    dl_check(download_cancel()&&!net_busy(),"owned cancellation");dl_absent("/cancel.bin");platform_log("DOWNLOAD-CANCEL-PASS\n");
    dl_begin("/oversized","/too-large.bin");dl_wait(DOWNLOAD_ERROR);dl_absent("/too-large.bin");platform_log("DOWNLOAD-OVERSIZE-PASS\n");
    dl_begin("/broken","/incomplete.bin");dl_wait(DOWNLOAD_ERROR);dl_absent("/incomplete.bin");
    dl_begin("/redirect","/redirect.bin");dl_wait(DOWNLOAD_ERROR);dl_absent("/redirect.bin");
    dl_begin("/missing","/missing.bin");dl_wait(DOWNLOAD_ERROR);dl_absent("/missing.bin");
    dl_check(download_start(0,"https://example.com/file","/secure.bin")<0&&!net_busy(),"HTTPS rejected without downgrade");
    int folder=fs_mkdir(0,"changed");dl_check(folder>0,"test destination folder");
    dl_begin("/medium","/changed/file.bin");unsigned identity=fs_identity(folder);dl_check(!fs_delete(folder),"delete destination folder");
    dl_check(fs_mkdir(0,"changed")==folder&&fs_identity(folder)!=identity,"reuse folder slot");dl_wait(DOWNLOAD_ERROR);dl_absent("/changed/file.bin");
    platform_log("DOWNLOAD-FAILURES-PRESERVE-PASS\n");
    open_term();native_slot=win_front();wins[native_slot].x=18;wins[native_slot].y=365;wins[native_slot].w=600;wins[native_slot].h=275;
    dl_command("start /Programs/counter.bex");dl_check(term_task_running(native_slot),"native counter starts");
    open_edit();int edit_slot=win_front();wins[edit_slot].x=638;wins[edit_slot].y=75;wins[edit_slot].w=610;wins[edit_slot].h=240;
    const char *text="Editing while 2 MiB downloads in the background.\n";while(*text)edit_insert(*text++);
    dl_check(edit_write_named("download-notes.txt"),"save first Editor content");
    open_browser(-1);int browser_slot=win_front();wins[browser_slot].x=638;wins[browser_slot].y=335;wins[browser_slot].w=610;wins[browser_slot].h=340;
    browser_open("about:home");
    dl_check(audio_play(audio_example,sizeof audio_example)==0,"start audio during transfer");
    dl_begin("/binary","/binary.bin");
    browser_open(DL_ORIGIN "/page");dl_check(download_active()&&!browser_loading()&&!kstrcmp(browser_url(),"about:home"),"Browser busy request preserves downloader");
    browser_key(0x26,0,BROWSER_MOD_CTRL);browser_key(0,'x',0);dl_draw();
    start=timer_ticks();unsigned previous_frames=0,audio_progress=0,changed=0,clips=0,draws_before=draws;
    while(download_active()&&timer_ticks()-start<18*TIMER_HZ){
        dl_turn();
        const AudioStatus *a=audio_status();dl_check(a->state!=AUDIO_ERROR&&!a->underruns,"audio continuity during transfer");
        if(a->played_frames>previous_frames)audio_progress++;
        previous_frames=a->played_frames;
        if(a->state==AUDIO_FINISHED){clips++;dl_check(audio_play(audio_example,sizeof audio_example)==0,"restart audio clip");previous_frames=0;}
        if(!changed&&download_status()->received>=32768){
            context_set(edit_slot);const char *more="A second edit was saved during real HTTP reception.\n";while(*more)edit_insert(*more++);
            dl_check(edit_save(),"concurrent Editor save");term_task_key(native_slot,'+');term_task_key(native_slot,'s');changed=1;
        }
        __asm__ volatile("hlt");
    }
    dl_check(download_status()->state==DOWNLOAD_DONE,"2 MiB download completed");dl_verify("/binary.bin",FS_FILE_MAX);
    char metric[24];fmt_uint(metric,changed);platform_log("DL concurrent changed=");platform_log(metric);fmt_uint(metric,audio_progress);platform_log(" audio_updates=");platform_log(metric);fmt_uint(metric,draws-draws_before);platform_log(" draws=");platform_log(metric);platform_log("\n");
    dl_check(changed&&audio_progress>3&&draws>draws_before+2,"Editor, audio and redraw progressed alongside download");
    int note=fs_resolve(0,"/download-notes.txt");dl_check(note>=0&&fs_size(note)>70,"Editor content survived download");
    char counter_path[]="/Documents/counter-1.txt";counter_path[19]=(char)('1'+native_slot);
    dl_check(fs_resolve(0,counter_path)>=0&&term_task_running(native_slot),"native counter progressed and saved");
    term_task_stop(native_slot);audio_stop();platform_log("DOWNLOAD-2MIB-DESKTOP-AUDIO-NATIVE-PASS\n");
    char number[24];fmt_uint(number,(timer_ticks()-start)*1000/TIMER_HZ);platform_log("DOWNLOAD-2MIB-MS ");platform_log(number);platform_log("\n");
    (void)clips;browser_open(DL_ORIGIN "/page");dl_wait_browser();
    browser_key(0x1f,0,BROWSER_MOD_CTRL);dl_verify_page("/Downloads/page.html");
    browser_key(0x1f,0,BROWSER_MOD_CTRL);dl_verify_page("/Downloads/page-2.html");
    browser_open_file(fs_resolve(0,"/Downloads/page.html"));dl_check(!kstrcmp(browser_title(),"Saved HTTP page"),"offline saved page opens");
    platform_log("DOWNLOAD-BROWSER-OFFLINE-SAVE-PASS\n");
    context_set(transfer_slot);dl_command("downloads");dl_command("echo Binary and offline pages saved successfully.");
    int marker=fs_create(0,"download-pass");dl_check(marker>0&&fs_write(marker,"PASS",4)==4&&!fs_sync(),"durable download flush");
    dl_draw();platform_log("DOWNLOAD-QEMU-PASS\n");for(;;)__asm__ volatile("hlt");
}
