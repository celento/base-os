#define FEATURE_TEST
#include "../src/kernel.c"
static void sdk_check(int ok,const char *why){if(!ok)panic(why);}
static void sdk_command(const char *text){while(*text)term_char(*text++);term_enter();}
void feature_test(void){
    int documents=fs_find_child(fs_root(),"Documents");
    int prior=fs_find_child(documents,"sdk-note.txt");
    open_term();
    sdk_command("exec /Programs/hello-c.bex");
    sdk_check(!kstrcmp(term_get(term_count()-1),"Program finished."),"C graphics app failed");
    const unsigned char *canvas=term_canvas();
    sdk_check(canvas&&canvas[0]==128&&canvas[15*160+15]==134,"C rectangle syscall output");
    sdk_command("exec /Programs/notebook.bex");
    sdk_check(!kstrcmp(term_get(term_count()-1),"Program finished."),"C notebook app failed");
    int id=fs_find_child(documents,"sdk-note.txt");
    sdk_check(id>=0&&!kstrcmp(fs_data(id),"This note was saved by a protected C application.\n"),"C document write/read");
    sdk_check(fs_sync()==0,"C notebook persistence");
    platform_log(prior<0?"SDK-C-APPS-PASS\n":"SDK-REBOOT-PASS\n");
}
