/* Normal SDK-only staged drawing for production screenshot verification. */
#include "baseos.h"
static unsigned width=160,height=100,phase;
static volatile unsigned work;
static void complete(void){
    bos_rect(0,0,width,height,phase?8:4);
    bos_rect(0,0,width,8,10);
    bos_rect(0,height-8,width,8,6);
    for(unsigned bit=0;bit<8;bit++)bos_rect((int)(bit*8),16,8,8,(phase&(1u<<bit))?15:0);
}
static void compute(unsigned ticks){
    unsigned began=bos_ticks();
    do {for(unsigned i=0;i<100000;i++)work=(work*1664525u)+1013904223u;}
    while((unsigned)(bos_ticks()-began)<ticks);
}
int main(void){
    complete();bos_present();
    bos_print("Frame publication: N present, Y yield, S sleep, Z zero, R resize, E exit, T stop.\n");
    for(;;){
        int key=bos_key();
        if(!key){bos_sleep(10);continue;}
        if(key!='n'&&key!='y'&&key!='s'&&key!='z'&&key!='r'&&key!='d'&&key!='e'&&key!='t')continue;
        if(key=='r'){width=320;height=200;bos_canvas_size(width,height);}
        if(key=='d'){width=160;height=100;bos_canvas_size(width,height);}
        bos_rect(0,0,width,height,0);
        bos_print("Computing the next complete frame across ordinary PIT slices.\n");
        compute((key=='t'?20:2)*BOS_TICKS_PER_SECOND);
        phase++;complete();
        if(key=='y')bos_yield();
        else if(key=='s')bos_sleep(100);
        else if(key=='z')bos_sleep(0);
        else if(key=='e')return 42;
        else bos_present();
    }
}
