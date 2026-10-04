/* Supported task slices: classify changes without losing another slot's state. */
static void task_test_step(int owner);
#define TERM_TASK_STEP_HOOK task_test_step
#define TERM_TASK_FIXTURE_ONLY
#include "term_task_info_host.c"
enum { QUIET, PIXELS, TEXT, BOTH, RESIZE, OUTSIDE };
static int operation;
static void task_test_step(int owner) {
    if(operation==PIXELS||operation==BOTH)callbacks[owner].plot(&callbacks[owner].binding,3,4,17);
    if(operation==TEXT||operation==BOTH)callbacks[owner].print(&callbacks[owner].binding,"ordinary output");
    if(operation==RESIZE)assert(!callbacks[owner].resize(&callbacks[owner].binding,320,200));
    if(operation==OUTSIDE)callbacks[owner].plot(&callbacks[owner].binding,-1,4,17);
    if(operation!=QUIET)callbacks[owner].present(&callbacks[owner].binding);
}
static TermTaskUpdate update(int op,int owner,unsigned flags) {
    operation=op;
    int previous=selected;
    TermTaskUpdate result=term_task_poll_update();
    assert(selected==previous&&result.slot==owner&&result.flags==flags);
    if(owner>=0)assert(!terms[owner].task_dirty);
    return result;
}
int main(void) {
    reset();executable("canvas.bex");
    term_select(2);term_reset();command("start /canvas.bex");
    term_select(7);term_reset();term_char('x');
    update(QUIET,2,TERM_TASK_TEXT|TERM_TASK_LAYOUT|TERM_TASK_LIFECYCLE);
    update(PIXELS,2,TERM_TASK_LAYOUT);
    update(PIXELS,2,TERM_TASK_CANVAS);
    update(BOTH,2,TERM_TASK_CANVAS|TERM_TASK_TEXT);
    update(RESIZE,2,TERM_TASK_LAYOUT);
    assert(terms[2].canvas_width==320&&terms[2].canvas_height==200);
    update(PIXELS,2,TERM_TASK_CANVAS);
    update(OUTSIDE,2,0);
    update(QUIET,2,0);
    assert(!strcmp(term_input(),"x"));
    states[2]=PROCESS_TASK_SLEEPING;
    update(QUIET,-1,0);
    term_select(6);term_reset();command("start /canvas.bex");term_select(7);
    update(PIXELS,6,TERM_TASK_TEXT|TERM_TASK_LAYOUT|TERM_TASK_LIFECYCLE);
    states[2]=PROCESS_TASK_READY;
    update(PIXELS,2,TERM_TASK_CANVAS);
    update(PIXELS,6,TERM_TASK_CANVAS);
    exit_next[2]=1;
    update(QUIET,2,TERM_TASK_TEXT|TERM_TASK_LIFECYCLE);
    assert(!term_task_running(2)&&!terms[2].process);
    operation=TEXT;assert(term_task_poll());
    operation=QUIET;assert(!term_task_poll());
    term_task_stop(6);
    assert(terms[6].task_dirty==(TERM_TASK_TEXT|TERM_TASK_LIFECYCLE));
    update(QUIET,-1,0);
    term_select(2);command("start /canvas.bex");term_select(7);
    update(PIXELS,2,TERM_TASK_TEXT|TERM_TASK_LAYOUT|TERM_TASK_LIFECYCLE);
    assert(!strcmp(term_input(),"x"));
    puts("Native update flags: start/activation/pixels/text/resize/exit, selection, idle, sleeping and slot fairness passed.");
}
