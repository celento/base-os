/* Only normal supported geometry and lifecycle operations. */
#define TERM_TASK_FIXTURE_ONLY
#include "term_task_info_host.c"
int main(void){
    reset();executable("canvas.bex");
    memset(terminal_arena+0xC0000,0x5a,0x40000);
    assert(sizeof(Terminal)==91468 && sizeof(Terminal)*8==731744);
    for(int owner=0;owner<8;owner++){
        term_select(owner);term_reset();
        assert(!term_canvas()&&term_canvas_width()==160&&term_canvas_height()==100);
        command("start /canvas.bex");
        assert(callbacks[owner].resize&&callbacks[owner].resize(320,200)==0);
        assert(term_canvas()&&term_canvas_width()==320&&term_canvas_height()==200);
        for(int y=0;y<200;y++)for(int x=0;x<320;x++)
            callbacks[owner].plot(x,y,owner+1);
        callbacks[owner].plot(-1,0,99);callbacks[owner].plot(320,199,99);
        callbacks[owner].plot(0,-1,99);callbacks[owner].plot(319,200,99);
    }
    for(int owner=0;owner<8;owner++){
        term_select(owner);
        for(int i=0;i<64000;i++)assert(term_canvas()[i]==owner+1);
        command("start /canvas.bex"); /* A busy start must preserve this canvas. */
        assert(term_canvas_width()==320&&term_canvas()[63999]==owner+1);
    }
    term_select(3);assert(callbacks[3].resize(320,200)==0);
    for(int i=0;i<64000;i++)assert(term_canvas()[i]==0);
    plot(319,199,9);
    assert(callbacks[3].resize(160,100)==0);
    assert(term_canvas_width()==160&&term_canvas_height()==100);
    for(int i=0;i<64000;i++)assert(terms[3].canvas[i]==0);
    plot(159,99,45);assert(term_canvas()[15999]==45);
    assert(callbacks[3].resize(320,200)==0);
    for(int i=0;i<64000;i++)assert(term_canvas()[i]==0);
    plot(319,199,46);assert(term_canvas()[63999]==46);
    term_task_stop(3);command("exec /canvas.bex");
    assert(!term_canvas()&&term_canvas_width()==160&&term_canvas_height()==100);
    assert(canvas_resize(320,200)==0);plot(319,199,2);
    command("basic /canvas.bex");
    assert(!term_canvas()&&term_canvas_width()==160&&term_canvas_height()==100);
    canvas_resize(320,200);command("clear");
    assert(!term_canvas()&&term_canvas_width()==160&&term_canvas_height()==100);
    canvas_resize(320,200);term_reset();
    assert(!term_canvas()&&term_canvas_width()==160&&term_canvas_height()==100);
    for(int i=0xC0000;i<0x100000;i++)assert(terminal_arena[i]==0x5a);
    puts("native canvas: 731744-byte eight-terminal footprint, independent full canvases, clipping, clear and legacy lifecycle reset passed");
}
