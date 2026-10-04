/* Normal Terminal document operands and immutable copied launch metadata. */
#define TERM_TASK_FIXTURE_ONLY
#include "term_task_info_host.c"
static int ordinary(int parent,const char *name){
    int id=fs_create(parent,name);assert(id>=0);
    assert(fs_write(id,"one two\n",8)==8);return id;
}
static void idle(void){term_task_close(selected);}
static void no_start(const char *text){command(text);assert(!term_task_running(selected));}
int main(void){
    reset();int app=executable("native stats.bex"),documents=fs_find_child(fs_root(),"Documents");
    int document=ordinary(documents,"a sample.txt");
    term_select(0);term_reset();
    command("start \"/native stats.bex\" \"/Documents/a sample.txt\"");
    assert(term_task_running(0)&&!strcmp(arguments[0],"/Documents/a sample.txt"));
    TermTaskInfo before,after;char title[TERM_TASK_TITLE_LEN],small[5];
    assert(term_task_info(0,&before)&&!strcmp(before.name,"native stats.bex")&&
           !strcmp(before.document,"a sample.txt"));
    assert(term_task_title(0,title,sizeof title)&&!strcmp(title,"native stats.bex - a sample.txt"));
    assert(term_task_title(0,small,sizeof small)&&!strcmp(small,"nati"));
    term_select(1);term_reset();term_char('x');
    assert(term_task_title(0,title,sizeof title)&&selected==1&&!strcmp(term_input(),"x"));
    assert(!fs_rename(document,"renamed.txt")&&!fs_rename(app,"renamed.bex"));
    assert(!fs_delete(document));ordinary(documents,"replacement.txt");
    assert(term_task_info(0,&after)&&!memcmp(&before,&after,sizeof before));
    assert(!strcmp(arguments[0],"/Documents/a sample.txt"));
    assert(!strcmp(title,"native stats.bex - a sample.txt"));
    term_select(0);canvas_resize(320,200);plot(0,0,9);
    command("start /renamed.bex /Documents/replacement.txt");
    assert(term_task_info(0,&after)&&after.instance==before.instance&&term_canvas()[0]==9);
    assert(!strcmp(arguments[0],"/Documents/a sample.txt"));idle();
    assert(!term_task_title(0,title,sizeof title)&&!title[0]);
    command("start /renamed.bex");assert(!arguments[0][0]);
    assert(term_task_info(0,&after)&&!after.document[0]&&after.instance!=before.instance);idle();
    no_start("start /renamed.bex \"\"");
    no_start("start /renamed.bex /Documents/replacement.txt extra");
    no_start("start /renamed.bex \"/Documents/replacement.txt");
    no_start("start \"/renamed.bex\"/Documents/replacement.txt");
    no_start("start /renamed.bex /Documents");
    no_start("start /renamed.bex /Documents/missing.txt");
    term_set_cwd(documents);command("start ../renamed.bex replacement.txt");
    assert(!strcmp(arguments[0],"/Documents/replacement.txt"));idle();
    command("start \"../renamed.bex\" \"replacement.txt\"   ");
    assert(!strcmp(arguments[0],"/Documents/replacement.txt"));idle();
    /* The interactive line stays 80 bytes, but resolving a short relative operand
     * can produce the full supported 128-byte startup path. */
    int folder=fs_root();
    for(int i=0;i<5;i++){folder=fs_mkdir(folder,"12345678901234567890123");assert(folder>=0);}
    ordinary(folder,"1234567");ordinary(folder,"12345678");term_set_cwd(folder);
    command("start /renamed.bex 1234567");
    assert(strlen(arguments[0])==PROCESS_ARGUMENT_MAX);idle();
    no_start("start /renamed.bex 12345678");
    /* Direct file launch retains the original no-argument route and owner guard. */
    assert(!term_task_start_file(0,app,fs_identity(app))&&!arguments[0][0]);
    assert(term_task_info(0,&after)&&!after.document[0]);idle();
    assert(term_task_start_file(0,app,fs_identity(app)+1)<0&&!term_task_running(0));
    puts("native arguments: quoted and relative paths, exact 128-byte bound, empty/extra operands, copied labels, busy owners and legacy no-argument launch passed");
}
