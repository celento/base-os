/* Ordinary finite ingress overflow must not execute a partial shell draft. */
#define TERM_TASK_FIXTURE_ONLY
#include "term_task_info_host.c"
#include "input_ingress.h"
static InputIngress ingress;
static void type(const char *text){while(*text)term_char(*text++);}
int main(void){
    reset();term_select(0);term_reset();command("echo complete history");
    type("touch /retained.txt");char draft[81];strcpy(draft,term_input());
    input_init(&ingress,800,600,0,0);
    for(unsigned i=0;i<=INPUT_CAPACITY;++i)input_keyboard_byte(&ingress,0x1e,i);
    InputSample sample;assert(input_pop(&ingress,&sample)&&sample.kind==INPUT_RESET);
    term_input_lost(0);int history_count=T.hcount;
    term_enter();term_enter();
    assert(fs_resolve(0,"/retained.txt")<0 && !strcmp(draft,term_input()) && T.hcount==history_count);
    assert(strstr(term_get(term_count()-1),"command not run"));
    term_backspace();assert(T.input_invalid);
    strcpy(draft,term_input());term_history(-1);assert(!T.input_invalid);
    term_history(1);assert(T.input_invalid && !strcmp(draft,term_input()));
    term_enter();assert(fs_resolve(0,"/retained.tx")<0 && !strcmp(draft,term_input()));
    while(*term_input())term_backspace();
    assert(!T.input_invalid);
    command("touch /complete.txt");assert(fs_resolve(0,"/complete.txt")>=0);
    /* An empty affected line also needs an explicit reset. Other owners keep
     * their bytes; the desktop chooses which open slots to mark on loss. */
    term_input_lost(0);type("touch /blocked.txt");term_enter();assert(fs_resolve(0,"/blocked.txt")<0);
    term_select(1);term_reset();command("touch /peer.txt");assert(fs_resolve(0,"/peer.txt")>=0);
    term_select(0);assert(!strcmp(term_input(),"touch /blocked.txt") && T.input_invalid);
    puts("Terminal input loss: partial drafts retained, Enter refused, history ownership and complete-line recovery passed.");
}
