#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "media_mp3.h"
#include "media.h"
#include "layout.h"
int main(int argc,char **argv) {
    assert(argc==3);
    FILE *input=fopen(argv[1],"rb");assert(input);
    assert(!fseek(input,0,SEEK_END));long bytes=ftell(input);assert(bytes>0&&(unsigned long)bytes<=AUDIO_WORK_CAPACITY);
    rewind(input);uint8_t *data=malloc((size_t)bytes);assert(data);
    assert(fread(data,1,(size_t)bytes,input)==(size_t)bytes);fclose(input);
    MediaMp3 mp3;assert(media_mp3_open(&mp3,data,(uint32_t)bytes)==0);
    FILE *output=fopen(argv[2],"wb");assert(output);
    int16_t pcm[4096];unsigned frames=0,reads=0;
    while(!media_mp3_finished(&mp3)) {
        /* Legal small reads exercise partial decoded-frame buffering. */
        unsigned budget=++reads%3==0?127:sizeof(pcm)/(2*mp3.channels);
        int count=media_mp3_read(&mp3,pcm,budget);assert(count>=0);
        assert(fwrite(pcm,mp3.channels*2,(unsigned)count,output)==(unsigned)count);
        frames+=(unsigned)count;
        assert(reads<=mp3.frames/64u+4096u);
    }
    fclose(output);free(data);
    assert(frames>0&&frames<=mp3.frames);
    printf("MP3 decoded: %u frames, %u Hz, %u channels, %u bounded reads\n",frames,mp3.sample_rate,mp3.channels,reads);
    return 0;
}
