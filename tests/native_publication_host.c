/* Supported drawing/lifecycle operations against the real Terminal and renderer. */
static void task_test_step(int owner);
#define TERM_TASK_STEP_HOOK task_test_step
#define TERM_TASK_FIXTURE_ONLY
#include "term_task_info_host.c"
#include "canvas_view.h"
enum { IDLE, CLEAR, FOOTER, PUBLISH, SMALL, LARGE, CLEAR_TEXT };
static int operation;
static void task_test_step(int owner) {
    const ProcessIO *io=&callbacks[owner];
#ifdef NATIVE_RECTANGLE_CALLBACK
    assert(io->rect);
    if(operation==CLEAR||operation==CLEAR_TEXT)
        io->rect(&io->binding,0,0,terms[owner].canvas_width,terms[owner].canvas_height,0);
    if(operation==FOOTER)
        io->rect(&io->binding,0,terms[owner].canvas_height-1,terms[owner].canvas_width,1,42);
#else
    if(operation==CLEAR||operation==CLEAR_TEXT)
        for(int y=0;y<terms[owner].canvas_height;y++)
            for(int x=0;x<terms[owner].canvas_width;x++)io->plot(&io->binding,x,y,0);
    if(operation==FOOTER)
        for(int x=0;x<terms[owner].canvas_width;x++)io->plot(&io->binding,x,terms[owner].canvas_height-1,42);
#endif
    if(operation==SMALL)assert(!io->resize(&io->binding,160,100));
    if(operation==LARGE)assert(!io->resize(&io->binding,320,200));
    if(operation==CLEAR_TEXT)io->print(&io->binding,"Ordinary text can force a full redraw.");
    if(operation==PUBLISH)io->present(&io->binding);
}
#define TITLE_H 32
#define TERM_PAD 12
#define EDIT_LINE_H 14
static unsigned char rendered[800*600],reference[sizeof rendered],frame[64000];
static int polls;
static void put_pixel(int x,int y,int color){assert(x>=0&&x<800&&y>=0&&y<600);rendered[y*800+x]=(unsigned char)color;}
void platform_poll(void){polls++;}
#include "native_publication_render.inc"
static TermTaskUpdate step(int op,unsigned flags){
    operation=op;TermTaskUpdate update=term_task_poll_update();
    assert(update.slot==0&&update.flags==flags);return update;
}
static void render(void){memset(rendered,19,sizeof rendered);draw_term_canvas(25,40,700,550);}
static void unchanged(int width,int height){
    assert(term_canvas_width()==width&&term_canvas_height()==height);
    assert(!memcmp(term_canvas(),frame,(size_t)width*height));
    render();assert(!memcmp(rendered,reference,sizeof rendered));
}
static void begin_frame(int width,int height){
    assert(!callbacks[0].resize(&callbacks[0].binding,width,height));
    for(int y=0;y<height;y++)for(int x=0;x<width;x++)plot(x,y,(x+y*13)%251+1);
    int before=polls;callbacks[0].present(&callbacks[0].binding);assert(polls==before);
    memcpy(frame,term_canvas(),(size_t)width*height);render();memcpy(reference,rendered,sizeof reference);
    terms[0].task_dirty=0;
}
static unsigned program_input_calls;
static void program_input(int active) {
    assert(active == !(program_input_calls & 1));
    ++program_input_calls;
}
int main(void){
    term_set_program_input(program_input);
    reset();executable("canvas.bex");term_select(0);term_reset();command("start /canvas.bex");
    assert(callbacks[0].present&&!term_canvas());
    step(IDLE,TERM_TASK_TEXT|TERM_TASK_LAYOUT|TERM_TASK_LIFECYCLE);
    step(LARGE,0);step(FOOTER,0);
    assert(!term_canvas()&&term_canvas_width()==160&&term_canvas_height()==100);
    step(PUBLISH,TERM_TASK_LAYOUT);
    assert(term_canvas_width()==320&&term_canvas_height()==200&&term_canvas()[63999]==42);
    for(int size=0;size<2;size++){
        int width=size?320:160,height=size?200:100;
        begin_frame(width,height);
        /* Several completed native slices and a full redraw still see frame A. */
        step(CLEAR,0);unchanged(width,height);
        step(IDLE,0);unchanged(width,height);
        step(CLEAR_TEXT,TERM_TASK_TEXT);unchanged(width,height);
        step(FOOTER,0);unchanged(width,height);
        step(PUBLISH,TERM_TASK_CANVAS);
        for(int i=0;i<width*(height-1);i++)assert(term_canvas()[i]==0);
        for(int i=width*(height-1);i<width*height;i++)assert(term_canvas()[i]==42);
        step(PUBLISH,0);
        /* Geometry and stride cannot change before publication. */
        begin_frame(width,height);
        step(size?SMALL:LARGE,0);unchanged(width,height);
        step(PUBLISH,TERM_TASK_LAYOUT);
        assert(term_canvas_width()==(size?160:320));
        for(int i=0;i<term_canvas_width()*term_canvas_height();i++)assert(term_canvas()[i]==0);
    }
    begin_frame(160,100);
    step(LARGE,0);step(SMALL,0);unchanged(160,100);
    step(PUBLISH,TERM_TASK_CANVAS); /* Net geometry unchanged; clear still visible. */
    for(int i=0;i<16000;i++)assert(!term_canvas()[i]);
    begin_frame(320,200);step(CLEAR,0);term_task_stop(0);unchanged(320,200);
    assert(!terms[0].canvas_pending&&!term_task_running(0));
    command("start /canvas.bex");assert(!term_canvas()&&term_canvas_width()==160);
    terms[0].task_dirty=0;step(FOOTER,0);assert(!term_canvas());
    term_task_close(0);assert(!term_canvas()&&!terms[0].canvas_pending);
    command("start /canvas.bex");begin_frame(160,100);
    command("clear");assert(!term_canvas()&&terms[0].canvas_buffered);
    terms[0].task_dirty=0;step(FOOTER,0);assert(!term_canvas());step(PUBLISH,TERM_TASK_LAYOUT);
    term_task_stop(0);
    /* Every slot owns its pixels; resetting one cannot reinterpret another. */
    for(int owner=0;owner<8;owner++){
        term_select(owner);term_reset();command("start /canvas.bex");
        assert(!canvas_resize(320,200));plot(319,199,owner+1);callbacks[owner].present(&callbacks[owner].binding);
    }
    for(int owner=0;owner<8;owner++){
        term_select(owner);assert(term_canvas()[63999]==owner+1);
        plot(319,199,99);term_task_stop(owner);assert(term_canvas()[63999]==owner+1);
    }
    term_select(3);term_reset();assert(!term_canvas());
    command("start /canvas.bex");plot(0,0,7);callbacks[3].present(&callbacks[3].binding);
    assert(term_canvas_width()==160&&term_canvas()[0]==7&&term_canvas()[15999]==0);
    for(int owner=0;owner<8;owner++)if(owner!=3){
        term_select(owner);assert(term_canvas_width()==320&&term_canvas()[63999]==owner+1);
    }
    assert(!program_input_calls);
    term_select(3);term_task_stop(3);command("exec /canvas.bex");
    assert(program_input_calls==2);plot(2,3,9);
    assert(term_canvas()==terms[3].canvas&&term_canvas()[3*160+2]==9);
    command("basic /canvas.bex");
    assert(program_input_calls==4);plot(4,5,10);
    assert(term_canvas()==terms[3].canvas&&term_canvas()[5*160+4]==10);
    puts("Native publication: preempted working bytes/metadata stay hidden from actual renderer; publication, resize, stop/close, clear, reuse, eight slots and synchronous compatibility passed.");
}
