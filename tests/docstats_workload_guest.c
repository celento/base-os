/* Normal complete DocStats scans with another app, redraw and real SB16 PCM. */
#define FEATURE_TEST
#include "../src/kernel.c"
static void check(int ok,const char *why){if(!ok)panic(why);}
static int file(const char *path){return fs_resolve(fs_root(),path);}
static void command(const char *s){while(*s)term_char(*s++);term_enter();}
static int has_line(int owner,const char *text){
    int previous=context_slot;term_select(owner);int found=0;
    for(int i=0;i<term_count();i++)if(!kstrcmp(term_get(i),text)){found=1;break;}
    term_select(previous);return found;
}
static void number(unsigned n){char out[12];unsigned at=0;do{out[at++]=(char)('0'+n%10);n/=10;}while(n);while(at){char c[2]={out[--at],0};platform_log(c);}}
static unsigned pcm_position;
static int pcm(void *context,int16_t *out,unsigned capacity){
    (void)context;
    unsigned total=22050u*150u,left=total-pcm_position;if(capacity>left)capacity=left;
    for(unsigned i=0;i<capacity;i++)out[i]=(int16_t)(((pcm_position+i)%100)*200-10000);
    pcm_position+=capacity;return (int)capacity;
}
static void repaint(void){cursor_restore();draw_desktop();draw_ui();flip_vga();cursor_on=0;}
static void tick(void){poll_time();platform_poll();term_task_poll();__asm__ volatile("hlt");}
static void run_stats(const char *app,const char *path,const char *label){
    context_set(0);term_task_close(0);term_reset();
    char command_line[81];kstrcpy(command_line,"start /Programs/");kstrcpy(command_line+kstrlen(command_line),app);
    kstrcpy(command_line+kstrlen(command_line)," ");kstrcpy(command_line+kstrlen(command_line),path);
    command(command_line);check(term_task_running(0),"DocStats workload launch");
    TermTaskInfo info;check(term_task_info(0,&info)&&info.document[0],"DocStats document metadata");
    pcm_position=0;audio_set_volume(100);
    check(!audio_play_pcm_stream(22050,1,22050u*150u,pcm,0),"DocStats workload audio start");
    while(audio_status()->state==AUDIO_LOADING){platform_poll();__asm__ volatile("hlt");}
    check(audio_status()->state==AUDIO_PLAYING,"DocStats workload audio prefill");
    context_set(2);term_char('x');
    unsigned began=timer_ticks(),last_draw=began,draws=0,turns=0,max_turn=0,last_input=began,inputs=0;
    while(!has_line(0,"Ready. R reloads, S saves a report, Q exits.")){
        unsigned before=timer_ticks();
        tick();turns++;
        unsigned duration=timer_ticks()-before;if(duration>max_turn)max_turn=duration;
        check(term_task_running(0)&&term_task_running(1),"DocStats or peer task exited early");
        check(!kstrcmp(term_input(),"x"),"native scan changed idle Terminal input");
        check(audio_status()->state==AUDIO_PLAYING&&!audio_status()->underruns,"DocStats scan stalled SB16 audio");
        unsigned now=timer_ticks();
        if(now-last_input>=7){
            term_backspace();term_char('x');term_task_key(1,'+');inputs++;last_input=now;
        }
        if(now-last_draw>=7){repaint();draws++;last_draw=timer_ticks();}
        check(timer_ticks()-began<120*TIMER_HZ,"DocStats scan exceeded bounded workload deadline");
    }
    unsigned elapsed=timer_ticks()-began,played=audio_status()->played_frames;
    check(played>0&&draws>0&&inputs>0&&!audio_status()->underruns,"DocStats workload lacked actual concurrent progress");
    audio_stop();term_backspace();
    platform_log("DOCSTATS-METRIC ");platform_log(label);
    platform_log(" ticks=");number(elapsed);platform_log(" turns=");number(turns);
    platform_log(" max_turn_ticks=");number(max_turn);platform_log(" draws=");number(draws);
    platform_log(" input_rounds=");number(inputs);platform_log(" audio_frames=");number(played);platform_log(" underruns=0\n");
    /* Save outside the scan timing: sync is a whole-volume operation. */
    int old=file("/Documents/stats-1.txt");if(old>=0)check(!fs_delete(old),"remove disposable prior report");
    term_task_key(0,'s');unsigned saved=timer_ticks();
    while((file("/Documents/stats-1.txt")<0||fs_needs_sync())&&timer_ticks()-saved<30*TIMER_HZ)tick();
    check(file("/Documents/stats-1.txt")>=0&&!fs_needs_sync(),"DocStats workload report did not synchronize");
    int reports=file("/Documents"),copy=fs_create(reports,label),report=file("/Documents/stats-1.txt");
    check(copy>=0&&fs_write(copy,fs_data(report),fs_size(report))==fs_size(report),"DocStats workload report copy");
    term_task_key(0,'q');saved=timer_ticks();
    while(term_task_running(0)&&timer_ticks()-saved<5*TIMER_HZ)tick();
    check(!term_task_running(0)&&has_line(0,"Native task finished."),"DocStats workload exit");
}
void feature_test(void){
    check(fs_file_limit()==16*1024*1024,"DocStats workload needs opt-in large profile");
    if(file("/Documents/workload-done.txt")>=0){
        check(file("/Documents/old-2m")>=0&&file("/Documents/new-2m")>=0&&file("/Documents/old-16m")>=0&&file("/Documents/new-16m")>=0,"DocStats report reboot persistence");
        platform_log("DOCSTATS-WORKLOAD-REBOOT-PASS\n");return;
    }
    check(audio_status()->available,"DocStats workload needs real SB16");
    for(int i=0;i<3;i++){open_term();check(win_front()==i,"DocStats workload window slots");}
    wins[0].x=18;wins[0].y=72;wins[0].w=700;wins[0].h=550;
    wins[1].x=738;wins[1].y=72;wins[1].w=510;wins[1].h=285;
    wins[2].x=738;wins[2].y=377;wins[2].w=510;wins[2].h=245;
    context_set(1);command("start /Programs/counter.bex");check(term_task_running(1),"DocStats peer Counter start");
    run_stats("unbatched.bex","/Documents/input-2m.txt","old-2m");
    run_stats("docstats.bex","/Documents/input-2m.txt","new-2m");
    run_stats("unbatched.bex","/Documents/input-16m.txt","old-16m");
    run_stats("docstats.bex","/Documents/input-16m.txt","new-16m");
    term_task_key(1,'s');unsigned began=timer_ticks();
    while((file("/Documents/counter-2.txt")<0||fs_needs_sync())&&timer_ticks()-began<30*TIMER_HZ)tick();
    int counter=file("/Documents/counter-2.txt");check(counter>=0&&fs_size(counter)>2,"peer Counter input/progress/save");
    term_task_stop(1);int done=fs_create(file("/Documents"),"workload-done.txt");
    check(done>=0&&fs_write(done,"done",4)==4&&!fs_sync(),"DocStats workload final sync");
    repaint();platform_log("DOCSTATS-WORKLOAD-PASS\n");
    for(;;)__asm__ volatile("hlt");
}
