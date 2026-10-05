#include "baseos.h"
/* Affirmative public absence result, while another owner stages an unpublished
 * create. No stale Files view, private state, mutation or decoder failure. */
int main(void){
    BosFileInfo file;
    int result=bos_file_open("/Documents/created.txt",BOS_FILE_OPEN_READ,&file);
    if(result==BOS_E_NOT_FOUND){bos_print("STAGED_ABSENCE PASS NOT_FOUND /Documents/created.txt\n");return 0;}
    if(result==BOS_OK)bos_file_close(file.handle);
    bos_print("STAGED_ABSENCE FAILED /Documents/created.txt\n");return 1;
}
