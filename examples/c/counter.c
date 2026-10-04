#include "baseos.h"
/* Two terminals can run this unchanged executable with independent state. */
static unsigned value, paused;
static const unsigned char digits[10][5]={
    {7,5,5,5,7},{2,6,2,2,7},{7,1,7,4,7},{7,1,7,1,7},{5,5,7,1,1},
    {7,4,7,1,7},{7,4,7,5,7},{7,1,1,1,1},{7,5,7,5,7},{7,5,7,1,7}
};
static unsigned number(char *out,unsigned n){
    char backwards[12];unsigned count=0;
    do{backwards[count++]=(char)('0'+n%10);n/=10;}while(n);
    for(unsigned i=0;i<count;i++)out[i]=backwards[count-i-1];
    out[count]=0;return count;
}
static void draw(unsigned id){
    bos_rect(0,0,160,100,128);
    bos_rect(4,4,152,4,paused?173:134);
    unsigned n=value%10000;
    for(int i=3;i>=0;i--){
        unsigned digit=n%10;n/=10;
        for(unsigned y=0;y<5;y++)for(unsigned x=0;x<3;x++)
            if(digits[digit][y]&(1u<<(2-x)))bos_rect(12+i*37+x*8,22+y*10,7,9,134+id%6);
    }
    bos_rect(4,86,(value%38)*4+4,6,paused?173:145);
    bos_present();
}
int main(void){
    unsigned id=bos_task_id();
    if(!id){bos_print("Use start /Programs/counter.bex for a desktop task.\n");return 0;}
    char path[]="/Documents/counter-1.txt";path[19]=(char)('0'+id);
    char saved[20];int size=bos_read_file(path,saved,sizeof saved-1);
    if(size>0){saved[size]=0;for(int i=0;i<size&&saved[i]>='0'&&saved[i]<='9';i++)value=value*10+(unsigned)(saved[i]-'0');}
    bos_print("Counter: +/- changes by 10; Space pauses; S saves.\n");
    bos_print("Q or Escape exits. Ctrl+C stops. Each window saves separately.\n");
    unsigned next=bos_ticks()+BOS_TICKS_PER_SECOND;
    for(;;){
        int key;while((key=bos_key())!=0){
            if(key=='q'||key=='Q'||key==27)return 0;
            if(key=='+'||key=='=')value+=10;
            if(key=='-'&&value>=10)value-=10;
            if(key==' ')paused=!paused;
            if(key=='s'||key=='S'){
                unsigned n=number(saved,value);saved[n++]='\n';
                if(bos_write_file(path,saved,n)>=0){bos_print("Saved ");bos_print(path);bos_print("\n");}
                else bos_print("Save failed.\n");
            }
        }
        unsigned now=bos_ticks();
        if((int)(now-next)>=0){if(!paused)value++;next=now+BOS_TICKS_PER_SECOND;}
        draw(id);
        if(bos_sleep(50)<0)return 1;
    }
}
