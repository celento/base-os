/* Valid, linked ordinary app used only as loader input by host tests. */
#include "baseos_app2.h"
static volatile unsigned initialized[8]={1,2,3,4,5,6,7,8};
static volatile unsigned bss[1300];
int main(void){
    bss[1299]=initialized[7];
    unsigned *workspace=(unsigned *)bos_workspace();
    workspace[0]=bss[1299];
    bos_yield();
    return workspace[0]==8?0:1;
}
