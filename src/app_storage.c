#include "app_storage.h"
#include "fs.h"
/* One-time initialization cannot reset an established view generation. */
void app_storage_init(void){
    static int ready;
    if(!ready){kmemset((void *)TERM_MEMORY,0,sizeof(AppStorage));ready=1;}
}
