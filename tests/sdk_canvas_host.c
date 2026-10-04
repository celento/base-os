/* Only normal supported geometry and lifecycle operations. */
#define TERM_TASK_FIXTURE_ONLY
#include "term_task_info_host.c"
int main(void){
    reset();executable("canvas.bex");
    memset(terminal_arena+0xC0000,0x5a,0x40000);
    /* The same container now separates terminal text and explicit app views. */
    assert(sizeof(TerminalText)==27432 && sizeof(AppView)==64104);
    assert(sizeof(AppStorage)==732288 && sizeof(AppStorage)<=APP_STORAGE_CAPACITY);
    for(int owner=0;owner<8;owner++){
        term_select(owner);term_reset();
        assert(!term_canvas()&&term_canvas_width()==160&&term_canvas_height()==100);
        command("start /canvas.bex");
        assert(callbacks[owner].resize&&callbacks[owner].resize(&callbacks[owner].binding,320,200)==0);
        assert(!term_canvas()&&term_canvas_width()==160&&term_canvas_height()==100);
        for(int y=0;y<200;y++)for(int x=0;x<320;x++)
            callbacks[owner].plot(&callbacks[owner].binding,x,y,owner+1);
        callbacks[owner].plot(&callbacks[owner].binding,-1,0,99);callbacks[owner].plot(&callbacks[owner].binding,320,199,99);
        callbacks[owner].plot(&callbacks[owner].binding,0,-1,99);callbacks[owner].plot(&callbacks[owner].binding,319,200,99);
        callbacks[owner].present(&callbacks[owner].binding);
        assert(term_canvas()&&term_canvas_width()==320&&term_canvas_height()==200);
    }
    for(int owner=0;owner<8;owner++){
        term_select(owner);
        for(int i=0;i<64000;i++)assert(term_canvas()[i]==owner+1);
        command("start /canvas.bex"); /* A busy start must preserve this canvas. */
        assert(term_canvas_width()==320&&term_canvas()[63999]==owner+1);
    }
    term_select(3);assert(callbacks[3].resize(&callbacks[3].binding,320,200)==0);callbacks[3].present(&callbacks[3].binding);
    for(int i=0;i<64000;i++)assert(term_canvas()[i]==0);
    plot(319,199,9);
    assert(callbacks[3].resize(&callbacks[3].binding,160,100)==0);callbacks[3].present(&callbacks[3].binding);
    assert(term_canvas_width()==160&&term_canvas_height()==100);
    for(int i=0;i<64000;i++)assert(views[3].canvas.pixels[i]==0);
    plot(159,99,45);callbacks[3].present(&callbacks[3].binding);assert(term_canvas()[15999]==45);
    assert(callbacks[3].resize(&callbacks[3].binding,320,200)==0);callbacks[3].present(&callbacks[3].binding);
    for(int i=0;i<64000;i++)assert(term_canvas()[i]==0);
    plot(319,199,46);callbacks[3].present(&callbacks[3].binding);assert(term_canvas()[63999]==46);
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
    puts("native canvas: 732288-byte shared text/view footprint, independent full canvases, clipping, clear and legacy lifecycle reset passed");
}
