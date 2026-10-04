/* Ordinary owned-source/profile boundaries; no guest memory fault injection. */
#define main audio_default_test_main
#include "audio_host.c"
#undef main
#include <stdlib.h>

static unsigned copy_polls, scan_polls;
static void check_copy_stopped(void) {
    assert(status.state == AUDIO_STOPPED && !hardware_running);
    ++copy_polls;
}
static void check_scan_or_copy(void) {
    if (status.state == AUDIO_PLAYING) {
        assert(hardware_running && mp3.data == source_buffer);
        ++scan_polls;
    } else check_copy_stopped();
}
static uint8_t *make_large_wave(unsigned bytes) {
    assert(bytes >= 44 && !(bytes & 1));
    (void)make_wave(1);
    u32(4,bytes-8);u32(40,bytes-44);
    uint8_t *data=malloc(bytes);assert(data);
    memcpy(data,file,44);
    for(unsigned i=44;i<bytes;i++)data[i]=(uint8_t)(i*37+11);
    return data;
}
static void test_eligibility(void) {
    assert(audio_capacity_bytes()==AUDIO_WORK_CAPACITY);
    for(unsigned active=0;active<2;active++)for(unsigned available=0;available<2;available++) {
        assert(audio_configure_source_workspace(active,available));
        assert(audio_capacity_bytes()==(active&&available?AUDIO_LARGE_WORK_CAPACITY:AUDIO_WORK_CAPACITY));
        assert(source_buffer==(active&&available?audio_test_large_source:audio_test_source));
    }
    assert(audio_configure_source_workspace(0,1)); /* Extra RAM alone owns nothing. */
    memset(audio_test_large_source,0x53,sizeof audio_test_large_source);
    unsigned bytes=make_wave(80000);
    assert(!audio_play(file,bytes));
    assert(!audio_configure_source_workspace(1,1)&&source_buffer==audio_test_source);
    audio_pause(1);assert(!audio_configure_source_workspace(1,1));
    audio_pause(0);for(unsigned i=0;i<8;i++)audio_poll();
    assert(status.state==AUDIO_PLAYING&&!audio_configure_source_workspace(1,1));
    audio_pause(1);assert(!audio_configure_source_workspace(1,1));audio_pause(0);
    assert(!memcmp(audio_test_source,file,bytes));
    for(unsigned i=0;i<sizeof audio_test_large_source;i++)assert(audio_test_large_source[i]==0x53);
    assert(audio_play(file,AUDIO_WORK_CAPACITY+1)==MEDIA_TOO_LARGE);
    assert(status.state==AUDIO_PLAYING&&!memcmp(audio_test_source,file,bytes));
    audio_stop();
}
static void test_owned_wave_boundaries(int large) {
    assert(audio_configure_source_workspace(large,1));
    unsigned capacity=audio_capacity_bytes();
    for(unsigned trim=0;trim<=2;trim+=2) {
        unsigned bytes=capacity-trim;
        uint8_t *data=make_large_wave(bytes);
        copy_polls=0;background_check=check_copy_stopped;
        assert(!audio_play_wav(data,bytes));background_check=0;
        assert(copy_polls==(bytes+SOURCE_COPY_BYTES_PER_POLL-1)/SOURCE_COPY_BYTES_PER_POLL);
        assert(!memcmp(source_buffer,data,bytes));
        assert(wave.data==source_buffer+44&&wave.data_bytes==bytes-44);
        memset(data,0,bytes);free(data); /* Source deletion cannot invalidate playback. */
        assert(status.total_frames==(bytes-44)/2&&status.state==AUDIO_LOADING);
        for(unsigned i=0;i<8;i++)audio_poll();
        assert(status.state==AUDIO_PLAYING);
        for(unsigned i=0;i<RING_BYTES;i++)assert(audio_test_dma[i]==(uint8_t)((i+44)*37+11));
        assert(audio_play_wav(source_buffer,capacity+1)==MEDIA_TOO_LARGE);
        assert(status.state==AUDIO_PLAYING&&status.error==MEDIA_TOO_LARGE);
        unsigned saved_cursor=wave.frame_cursor;
        unsigned small=make_wave(1000);u32(24,4999); /* Explicit unsupported rate. */
        assert(audio_play_wav(file,small)==MEDIA_UNSUPPORTED);
        assert(status.state==AUDIO_PLAYING&&wave.frame_cursor==saved_cursor);
        assert(source_buffer[bytes-1]==(uint8_t)((bytes-1)*37+11));
        audio_clear_error();audio_set_volume(37);audio_pause(1);
        now+=1000;audio_poll();assert(status.state==AUDIO_PAUSED);
        audio_pause(0);assert(status.state==AUDIO_PLAYING&&status.volume==37);
        assert(!audio_configure_source_workspace(!large,1));
        audio_stop();
    }
}
static void test_large_mp3(const char *path) {
    FILE *input=fopen(path,"rb");assert(input);
    assert(!fseek(input,0,SEEK_END));long length=ftell(input);
    assert(length>AUDIO_WORK_CAPACITY&&length<AUDIO_LARGE_WORK_CAPACITY);
    unsigned bytes=(unsigned)length;uint8_t *data=malloc(bytes);assert(data);
    rewind(input);assert(fread(data,1,bytes,input)==bytes);fclose(input);
    assert(audio_configure_source_workspace(0,1));
    assert(audio_play(data,bytes)==MEDIA_TOO_LARGE&&status.state==AUDIO_STOPPED);
    assert(audio_configure_source_workspace(1,1));
    assert(!audio_play(data,bytes));
    assert(!memcmp(source_buffer,data,bytes)&&mp3.data==source_buffer);
    for(unsigned i=0;status.state==AUDIO_LOADING;i++){assert(i<100);audio_poll();}
    assert(status.state==AUDIO_PLAYING);
    scan_polls=copy_polls=0;background_check=check_scan_or_copy;
    assert(!audio_play(data,bytes));background_check=0;
    assert(scan_polls>100&&copy_polls==(bytes+16383)/16384);
    assert(mp3.data==source_buffer&&!memcmp(source_buffer,data,bytes));
    memset(data,0,bytes);free(data);
    /* Consume and compare every sample with a separately advanced decoder. */
    MediaMp3 reference;assert(!media_mp3_open(&reference,source_buffer,bytes));
    static int16_t pcm[1152*2];unsigned available=0,index=0,consumed=0;
    position=0;for(unsigned i=0;status.state==AUDIO_LOADING;i++){assert(i<100);audio_poll();}
    assert(status.state==AUDIO_PLAYING&&status.sample_rate==44100&&status.channels==2);
    assert(audio_duration_ms()>=180000&&audio_duration_ms()<181000);
    audio_pause(1);now+=1000;audio_poll();assert(status.state==AUDIO_PAUSED);
    audio_pause(0);audio_set_volume(100);
    while(status.state==AUDIO_PLAYING) {
        for(unsigned i=0;i<512;i++) {
            while(index==available&&!media_mp3_finished(&reference)) {
                int count=media_mp3_read(&reference,pcm,1152);assert(count>=0);
                available=(unsigned)count;index=0;
            }
            for(unsigned c=0;c<2;c++) {
                int16_t want=index<available?pcm[index*2+c]:0;
                unsigned sample=(position/2+i*2+c)%(RING_BYTES/2);
                assert(((int16_t *)audio_test_dma)[sample]==want);
            }
            if(index<available)index++;
        }
        consumed+=512;assert(consumed<44100*182);
        position=(position+512*4)%RING_BYTES;now++;
        unsigned decoded=mp3.decoded_frames;audio_poll();
        assert(mp3.decoded_frames-decoded<=1152);
    }
    assert(status.state==AUDIO_FINISHED&&!status.error&&!status.underruns);
    assert(status.total_frames==reference.decoded_frames&&status.played_frames==status.total_frames);
    assert(audio_position_ms()==audio_duration_ms());
    assert(!audio_configure_source_workspace(0,1));audio_stop();
    /* Add a legal ID3v2 tag to reach the exact 16 MiB file boundary. */
    unsigned padding=AUDIO_LARGE_WORK_CAPACITY-bytes;
    assert(padding>=10&&padding-10<0x10000000);
    uint8_t *full=calloc(1,AUDIO_LARGE_WORK_CAPACITY);assert(full);
    memcpy(full,"ID3\4\0\0",6);unsigned tag=padding-10;
    full[6]=(tag>>21)&127;full[7]=(tag>>14)&127;full[8]=(tag>>7)&127;full[9]=tag&127;
    memcpy(full+padding,source_buffer,bytes);
    /* Encoded fixture has no ID3 tag; nested ID3 tags are not MP3 frames. */
    assert(memcmp(full+padding,"ID3",3));
    assert(!audio_play(full,AUDIO_LARGE_WORK_CAPACITY));
    assert(!memcmp(source_buffer,full,AUDIO_LARGE_WORK_CAPACITY));free(full);
    assert(mp3.cursor==padding&&mp3.data==source_buffer);
    assert(audio_play(source_buffer,AUDIO_LARGE_WORK_CAPACITY+1)==MEDIA_TOO_LARGE);
    assert(status.state==AUDIO_LOADING);audio_stop();
    assert(audio_configure_source_workspace(0,1));
    assert(audio_capacity_bytes()==AUDIO_WORK_CAPACITY&&source_buffer==audio_test_source);
}
int main(int argc,char **argv) {
    assert(argc==2);assert(!audio_init());
    test_eligibility();test_owned_wave_boundaries(0);test_owned_wave_boundaries(1);
    test_large_mp3(argv[1]);
    puts("Large owned audio eligibility, exact boundaries, independent sources, polled replacement, complete 3-minute MP3 and bounded decode passed.");
}
