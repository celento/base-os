/* Explicit owner metadata uses complete published frames, without selection. */
#define TERM_TASK_FIXTURE_ONLY
#include "term_task_info_host.c"
static void size(int owner,int on,int width,int height){
    int before=selected,w=-1,h=-1;
    assert(term_canvas_size(owner,&w,&h)==on);
    assert(w==width&&h==height&&selected==before);
}
int main(void){
    reset();executable("canvas.bex");
    for(int owner=0;owner<PROCESS_TASKS;owner++){
        term_select(owner);term_reset();command("start /canvas.bex");
        size(owner,0,0,0);
        int w=owner%2?320:160,h=owner%2?200:100;
        assert(!canvas_resize(w,h));plot(w-1,h-1,owner+1);
        size(owner,0,0,0);callbacks[owner].present();
    }
    term_select(7);term_char('q');
    for(int owner=0;owner<PROCESS_TASKS;owner++)size(owner,1,owner%2?320:160,owner%2?200:100);
    assert(!strcmp(term_input(),"q"));
    size(-1,0,0,0);size(PROCESS_TASKS,0,0,0);
    int n=99;assert(!term_canvas_size(0,0,&n)&&n==0);
    n=99;assert(!term_canvas_size(0,&n,0)&&n==0);
    assert(!term_canvas_size(0,0,0)&&selected==7);
    term_select(0);assert(!canvas_resize(320,200));plot(319,199,9);
    term_select(7);size(0,1,160,100); /* Incomplete working resize stays hidden. */
    term_select(0);callbacks[0].present();term_select(7);size(0,1,320,200);
    term_task_stop(0);size(0,1,320,200);
    term_select(0);command("start /canvas.bex");term_select(7);size(0,0,0,0);
    term_select(0);plot(0,0,9);callbacks[0].present();term_select(7);size(0,1,160,100);
    term_task_close(0);size(0,1,160,100); /* Close retains its last complete view. */
    term_select(0);term_reset();command("exec /canvas.bex");plot(2,3,9);
    term_select(7);size(0,1,160,100); /* Direct synchronous canvas compatibility. */
    term_select(0);command("clear");term_select(7);size(0,0,0,0);
    for(int owner=1;owner<PROCESS_TASKS;owner++)size(owner,1,owner%2?320:160,owner%2?200:100);
    puts("Canvas owner size: complete-frame geometry, eight owners, selection, resize, stop, close/reuse, clear and synchronous compatibility passed.");
}
