/* Separate ordinary GUI capacity fixture. Build as BEX2 native-v1 with exactly
 * 1 MiB workspace and 16 KiB stack. Existing Pointer/document files are never
 * rebuilt or changed by its collector. VERIFY=256 visibly reports that this
 * app wrote and read back both ends of all256 declared workspace pages. */
#define main page_budget_document_main
#include "native_window_document_app.c"
#undef main
int main(void){
    unsigned bytes=bos_workspace_bytes();
    if(bytes!=1048576u){bos_print("PAGE BUDGET requires1MiB workspace\n");return 7;}
    volatile unsigned char *workspace=(volatile unsigned char *)bos_workspace();
    for(unsigned page=0;page<bytes/4096u;page++){
        unsigned offset=page*4096u;
        workspace[offset]=(unsigned char)(page*17u+31u);
        workspace[offset+4095u]=(unsigned char)(page*29u+7u);
    }
    for(unsigned page=0;page<bytes/4096u;page++){
        unsigned offset=page*4096u;
        if(workspace[offset]!=(unsigned char)(page*17u+31u)||
           workspace[offset+4095u]!=(unsigned char)(page*29u+7u)){
            bos_print("PAGE BUDGET ordinary workspace verification failed\n");return 7;
        }
    }
    verified=bytes/4096u;
    bos_print("PAGE BUDGET verified256 workspace pages\n");
    return page_budget_document_main();
}
