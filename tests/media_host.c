#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "media.h"

static uint8_t file[4096];
static void u16(unsigned p,unsigned v){file[p]=v;file[p+1]=v>>8;}
static void u32(unsigned p,unsigned v){u16(p,v);u16(p+2,v>>16);}
static unsigned make_wave(unsigned bits,unsigned channels,unsigned frames) {
    memset(file,0,sizeof file);
    unsigned size=frames*channels*(bits/8);
    memcpy(file,"RIFF",4);u32(4,36+size+(size&1));memcpy(file+8,"WAVEfmt ",8);
    u32(16,16);u16(20,1);u16(22,channels);u32(24,22050);
    u32(28,22050*channels*(bits/8));u16(32,channels*(bits/8));u16(34,bits);
    memcpy(file+36,"data",4);u32(40,size);
    for(unsigned i=0;i<size;i++)file[44+i]=(uint8_t)(i*37);
    return 44+size+(size&1);
}
int main(void) {
    MediaWave wave;int16_t pcm[512];
    for(unsigned bits=8;bits<=16;bits+=8)for(unsigned channels=1;channels<=2;channels++) {
        unsigned n=make_wave(bits,channels,65);
        assert(!media_wave_open(&wave,file,n));
        assert(wave.frames==65&&wave.channels==channels&&wave.bits_per_sample==bits);
        assert(media_wave_read(&wave,pcm,13)==13);
        for(unsigned i=0;i<13*channels;i++) {
            int expected=bits==8?((int)file[44+i]-128)*256:(int16_t)(file[44+i*2]|file[45+i*2]<<8);
            assert(pcm[i]==expected);
        }
        assert(media_wave_read(&wave,pcm,100)==52);
        assert(!media_wave_read(&wave,pcm,1));
        /* Every truncation, including the odd data-chunk pad, is rejected. */
        for(unsigned i=0;i<n;i++)assert(media_wave_open(&wave,file,i));
    }
    unsigned n=make_wave(16,2,24);
    /* Odd unknown chunks are legal, as is data preceding fmt. */
    memmove(file+24,file+12,n-12);memcpy(file+12,"JUNK",4);u32(16,3);
    file[20]=1;file[21]=2;file[22]=3;file[23]=0;u32(4,n+12-8);n+=12;
    assert(!media_wave_open(&wave,file,n));
    uint8_t saved[4096];memcpy(saved,file,sizeof file);
    for(unsigned i=0;i<n;i++) {
        for(unsigned v=0;v<256;v+=17) {
            file[i]=(uint8_t)v;
            if(!media_wave_open(&wave,file,n))media_wave_read(&wave,pcm,256/wave.channels);
        }
        file[i]=saved[i];
    }
    n=make_wave(16,2,24);u16(20,3);assert(media_wave_open(&wave,file,n)==MEDIA_UNSUPPORTED);
    n=make_wave(16,2,24);u16(32,1);assert(media_wave_open(&wave,file,n)==MEDIA_BAD_FILE);
    n=make_wave(16,2,24);u32(28,1);assert(media_wave_open(&wave,file,n)==MEDIA_BAD_FILE);
    n=make_wave(16,2,24);u32(40,0xffffffff);assert(media_wave_open(&wave,file,n)==MEDIA_BAD_FILE);
    n=make_wave(16,2,24);u32(4,0xffffffff);assert(media_wave_open(&wave,file,n)==MEDIA_BAD_FILE);
    n=make_wave(16,2,24);u32(24,192000);assert(media_wave_open(&wave,file,n)==MEDIA_UNSUPPORTED);
    assert(media_wave_open(&wave,0,1024)==MEDIA_BAD_FILE);
    assert(media_wave_open(0,file,n)==MEDIA_BAD_FILE);
    assert(!media_wave_read(0,pcm,4));
    puts("WAV parser, conversion, mutations, and truncation tests passed.");
}
