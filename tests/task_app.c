#include "baseos.h"
typedef struct { unsigned owner, value, steps, keys, last_key, checksum; } Record;
static Record state;
int main(void){
    state.owner=bos_task_id();state.value=state.owner*100;
    char path[]="/Documents/task-1.dat";path[16]=(char)('0'+state.owner);
    for(;;){
        int key;while((key=bos_key())!=0){
            state.keys++;state.last_key=(unsigned)key;state.checksum+=(unsigned)key;
            if(key=='q'){
                if(bos_write_file(path,&state,sizeof state)!=(int)sizeof state)return 13;
                return 0;
            }
            state.value+=(unsigned)key;
        }
        state.steps++;
        bos_plot(0,0,state.value&255);
        if(bos_write_file(path,&state,sizeof state)!=(int)sizeof state)return 11;
        if(bos_yield())return 12;
        if(bos_sleep(25))return 14;
    }
}
