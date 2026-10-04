#include "baseos.h"
static char previous[128];
int main(void) {
    const char *path="/Documents/sdk-note.txt";
    const char *message="This note was saved by a protected C application.\n";
    int n=bos_read_file(path,previous,sizeof(previous)-1);
    if(n>=0) { previous[n]=0; bos_print("Previous note:\n"); bos_print(previous); }
    else bos_print("Creating a new note.\n");
    int written=bos_write_file(path,message,bos_strlen(message));
    if(written==BOS_ERR_BUSY) {
        bos_print("Disk is saving; retry shortly. Run Notebook again to retry.\n");
        return 1;
    }
    if(written<0) {
        bos_print("Could not save. Check the Documents folder and free space.\n");
        return 1;
    }
    if(bos_sync()<0) {
        bos_print("Note is in RAM only; disk sync failed. Run Notebook again to retry.\n");
        return 1;
    }
    bos_print("Saved /Documents/sdk-note.txt\n");
    return 0;
}
