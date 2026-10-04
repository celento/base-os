/* Finite supported rectangle cases: exact scalar pixels and Terminal state. */
#include <limits.h>
#define TERM_TASK_FIXTURE_ONLY
#include "term_task_info_host.c"

typedef struct { int x,y,width,height,color; } Rectangle;
static Terminal before,scalar;
static unsigned char published_before[CANVAS_PIXELS];

/* Match the old syscall loop, including unsigned register-coordinate addition. */
static void scalar_rect(Rectangle rect){
    for(unsigned y=0;y<(unsigned)rect.height;y++)
        for(unsigned x=0;x<(unsigned)rect.width;x++)
            plot((int)((unsigned)rect.x+x),(int)((unsigned)rect.y+y),rect.color);
}
static void equivalent(Rectangle rect,int buffered,int visible,int pending){
    T.canvas_buffered=buffered;T.canvas_on=visible;T.canvas_pending=pending;
    T.task_dirty=TERM_TASK_TEXT;
    T.published_on=1;T.published_width=160;T.published_height=100;
    for(unsigned i=0;i<CANVAS_PIXELS;i++)T.canvas[i]=(unsigned char)(i*13+7);
    memset(published_canvases[selected],53,CANVAS_PIXELS);
    memcpy(published_before,published_canvases[selected],CANVAS_PIXELS);
    before=T;scalar_rect(rect);scalar=T;T=before;
    canvas_rect(rect.x,rect.y,rect.width,rect.height,rect.color);
    /* Full structure compares exact working pixels, activation and dirty flags. */
    assert(!memcmp(&T,&scalar,sizeof T));
    assert(!memcmp(published_before,published_canvases[selected],CANVAS_PIXELS));
    const unsigned char *pixels=term_canvas();
    assert(pixels==(buffered?published_canvases[selected]:(T.canvas_on?T.canvas:0)));
}
static void pixel_equivalence(void){
    term_select(3);term_reset();
    for(int mode=0;mode<2;mode++){
        int width=mode?320:160,height=mode?200:100;
        assert(!canvas_resize(width,height));
        const Rectangle cases[]={
            {0,0,width,height,0},{0,0,width,height,255},
            {0,0,1,1,17},{width-1,height-1,1,1,33},
            {1,2,1,3,45},{1,2,2,3,45},{1,2,3,3,45},
            {1,2,4,3,45},{1,2,5,3,45},{1,2,7,3,45},
            {1,2,8,3,45},{1,2,9,3,45},
            {-3,-2,8,7,256},{width-3,height-2,8,7,-1},
            {-1,0,width,height,511},{0,-1,width,height,-257},
            {-width+1,0,width,height,19},{0,-height+1,width,height,19},
            {width-1,0,width,height,21},{0,height-1,width,height,21},
            {-width,0,width,height,31},{0,-height,width,height,31},
            {-width-1,0,width,height,31},{0,-height-1,width,height,31},
            {width,0,width,height,31},{0,height,width,height,31},
            {width+1,height+1,width,height,31},
            {0,0,0,height,99},{0,0,width,0,99},{0,0,0,0,99},
            {-1,-1,1,1,99},{INT_MIN,0,width,height,1},
            {0,INT_MIN,width,height,1},{INT_MAX,0,width,height,1},
            {0,INT_MAX,width,height,1},{INT_MAX,INT_MAX,width,height,1}
        };
        for(unsigned i=0;i<sizeof cases/sizeof *cases;i++)
            for(int buffered=0;buffered<2;buffered++)
                for(int visible=0;visible<2;visible++)
                    for(int pending=0;pending<2;pending++)
                        equivalent(cases[i],buffered,visible,pending);
    }
}
static void owner_lifecycle(void){
    executable("rect.bex");
    for(int owner=0;owner<PROCESS_TASKS;owner++){
        term_select(owner);term_reset();command("start /rect.bex");
        const ProgramIO *io=&callbacks[owner];assert(io->rect);
        assert(!io->resize(320,200));io->rect(0,0,320,200,owner+1);
        assert(!term_canvas());io->present();
        for(int i=0;i<64000;i++)assert(term_canvas()[i]==owner+1);
        io->rect(1,1,318,198,99); /* Keep an incomplete next frame private. */
    }
    for(int owner=0;owner<PROCESS_TASKS;owner++){
        term_select(owner);
        for(int i=0;i<64000;i++)assert(term_canvas()[i]==owner+1);
        assert(T.canvas[321]==99&&T.canvas_pending);
        term_task_stop(owner);
        for(int i=0;i<64000;i++)assert(term_canvas()[i]==owner+1);
        assert(!T.canvas_pending);
    }
    term_select(3);command("start /rect.bex");
    const ProgramIO *io=&callbacks[3];T.task_dirty=0;
    io->rect(-5,0,5,100,2);io->rect(160,0,160,100,2);
    io->rect(0,0,0,100,2);io->present();
    assert(!term_canvas()&&!T.canvas_on&&!T.canvas_pending&&!T.task_dirty);
    io->rect(-2,-3,5,7,301);
    assert(!term_canvas()&&T.canvas_pending&&!T.task_dirty);
    io->present();assert(T.task_dirty==TERM_TASK_LAYOUT);
    assert(term_canvas_width()==160&&term_canvas_height()==100);
    for(int y=0;y<100;y++)for(int x=0;x<160;x++)
        assert(term_canvas()[y*160+x]==(x<3&&y<4?45:0));
    T.task_dirty=0;io->rect(1,1,1,1,45);io->present();
    assert(T.task_dirty==TERM_TASK_CANVAS); /* Same color still counts as a draw. */
    T.task_dirty=0;io->present();assert(!T.task_dirty);
    assert(!io->resize(320,200));io->rect(319,199,1,1,6);
    assert(term_canvas_width()==160);io->present();
    assert(term_canvas_width()==320&&term_canvas()[63999]==6);
    assert(!io->resize(320,200));io->present();
    for(int i=0;i<64000;i++)assert(!term_canvas()[i]);
    command("clear");T.task_dirty=0;io->rect(159,99,1,1,7);
    assert(!term_canvas());io->present();
    assert(term_canvas_width()==160&&term_canvas()[15999]==7);
    io->rect(159,99,1,1,99);term_task_close(3);
    assert(term_canvas()[15999]==7&&!T.canvas_pending);
    command("exec /rect.bex");canvas_rect(1,2,3,4,9);
    assert(term_canvas()==T.canvas&&term_canvas()[321]==9);
    command("basic /rect.bex");canvas_rect(2,3,4,5,10);
    assert(term_canvas()==T.canvas&&term_canvas()[482]==10);
    term_reset();assert(!term_canvas()&&term_canvas_width()==160);
    for(int owner=0;owner<PROCESS_TASKS;owner++)if(owner!=3){
        term_select(owner);
        for(int i=0;i<64000;i++)assert(term_canvas()[i]==owner+1);
    }
}
int main(void){
    reset();pixel_equivalence();owner_lifecycle();
    puts("Native rectangles: exact scalar pixels/state at both sizes, clipping/no-ops, color truncation, working-frame privacy, publication/resize/reset and eight-owner isolation passed.");
}
