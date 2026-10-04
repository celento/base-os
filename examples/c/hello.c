#include "baseos.h"
int main(void) {
    bos_print("Hello from a C app running in protected user mode!\n");
    for (unsigned y=0;y<100;y+=10)
        for (unsigned x=0;x<160;x+=10)
            bos_rect((int)x,(int)y,10,10,128+(x/10+y/10*5)%125);
    bos_present();
    return 0;
}
