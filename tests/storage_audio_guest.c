#define FEATURE_TEST
#include "../src/kernel.c"
static unsigned char *const sample=(unsigned char *)VIDEO_BASE;
static void put16(unsigned offset,unsigned v){sample[offset]=v;sample[offset+1]=v>>8;}
static void put32(unsigned offset,unsigned v){put16(offset,v);put16(offset+2,v>>16);}
static void verify(int ok,const char *why){if(!ok)panic(why);}
void feature_test(void){
    verify(fs_file_limit()==2*1024*1024,"audio storage test needs data disk");
    verify(audio_status()->available,"audio storage test needs SB16");audio_set_volume(100);
    unsigned frames=22050*15,size=frames*2;
    kmemcpy(sample,"RIFF",4);put32(4,size+36);kmemcpy(sample+8,"WAVEfmt ",8);
    put32(16,16);put16(20,1);put16(22,1);put32(24,22050);put32(28,44100);
    put16(32,2);put16(34,16);kmemcpy(sample+36,"data",4);put32(40,size);
    for(unsigned i=0;i<frames;i++)put16(44+i*2,(unsigned)((int)(i%100)*240-12000));
    verify(audio_play(sample,size+44)==0,"long PCM open");
    while(audio_status()->state==AUDIO_LOADING){audio_poll();__asm__ volatile("hlt");}
    verify(audio_status()->state==AUDIO_PLAYING,"long PCM start");
    for(unsigned i=0;i<1700000;i++)sample[i]=(unsigned char)(i*37u);
    int ids[4];
    for(int n=0;n<4;n++){char name[]="bulk-0.dat";name[5]+=(char)n;ids[n]=fs_create(fs_root(),name);verify(ids[n]>=0,"bulk file create");verify(fs_write(ids[n],(const char *)sample,1700000)==1700000,"bulk file write");}
    platform_log("STORAGE-AUDIO-SYNC\n");verify(fs_sync()==0,"large snapshot while audio plays");
    verify(audio_status()->state==AUDIO_PLAYING&&audio_status()->underruns==0,"audio interrupted by snapshot");
    verify(fs_delete(ids[0])==0,"large compaction");verify(fs_sync()==0,"second snapshot while audio plays");
    unsigned began=timer_ticks();
    while(audio_status()->state==AUDIO_PLAYING){audio_poll();drain_8042();verify(timer_ticks()-began<20*TIMER_HZ,"audio storage completion");__asm__ volatile("hlt");}
    verify(audio_status()->state==AUDIO_FINISHED&&audio_status()->underruns==0,"audio storage final state");
    verify(audio_status()->played_frames==frames,"audio storage complete frames");
    platform_log("STORAGE-AUDIO-PASS\n");
}
