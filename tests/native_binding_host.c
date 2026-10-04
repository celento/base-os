/* Ordinary copied attachment delivery, restart, reset and identity exhaustion. */
#define TERM_TASK_FIXTURE_ONLY
#include "term_task_info_host.c"
static Terminal saved_terms[PROCESS_TASKS];
static unsigned char saved_pixels[PROCESS_TASKS][CANVAS_PIXELS];
static void snapshot(void){
    memcpy(saved_terms,terms,sizeof saved_terms);
    memcpy(saved_pixels,published_canvases,sizeof saved_pixels);
}
static void others_unchanged(int owner,int selection){
    assert(selected==selection);
    for(int slot=0;slot<PROCESS_TASKS;slot++)if(slot!=owner){
        assert(!memcmp(&terms[slot],&saved_terms[slot],sizeof terms[slot]));
        assert(!memcmp(published_canvases[slot],saved_pixels[slot],CANVAS_PIXELS));
    }
}
static void all_unchanged(int selection){
    assert(selected==selection);
    assert(!memcmp(terms,saved_terms,sizeof saved_terms));
    assert(!memcmp(published_canvases,saved_pixels,sizeof saved_pixels));
}
static void stale_delivery(const ProcessIO *io,int selection){
    snapshot();
    io->print(&io->binding,"Stale output must be ignored.");all_unchanged(selection);
    io->plot(&io->binding,3,4,99);all_unchanged(selection);
    assert(io->resize(&io->binding,320,200)<0);all_unchanged(selection);
    io->rect(&io->binding,0,0,160,100,99);all_unchanged(selection);
    io->present(&io->binding);all_unchanged(selection);
}
int main(void){
    reset();int app=executable("binding.bex");
    term_select(5);term_reset();command("start /binding.bex");
    TermTaskInfo info;assert(term_task_info(5,&info));
    ProcessIO original=callbacks[5];
    assert(info.instance==terms[5].process&&info.instance==original.binding.process);
    assert((info.instance&BOS_HANDLE_TYPE_PROCESS)&&info.instance!=5);
    assert(original.binding.slot==5&&original.binding.generation==terms[5].binding_generation);
    term_select(2);term_reset();term_char('x');terms[5].task_dirty=0;
    snapshot();
    original.print(&original.binding,"Context routes by binding.");others_unchanged(5,2);
    Terminal *owner=&terms[5];
    assert(!strcmp(owner->lines[(owner->head+owner->count-1)%TERM_LINES],"Context routes by binding."));
    assert(owner->task_dirty==TERM_TASK_TEXT);
    assert(!original.resize(&original.binding,320,200));others_unchanged(5,2);
    assert(owner->canvas_width==320&&owner->canvas_height==200&&owner->canvas_pending);
    original.plot(&original.binding,3,4,17);others_unchanged(5,2);
    assert(owner->canvas[4*320+3]==17);
    original.rect(&original.binding,10,11,2,3,42);others_unchanged(5,2);
    for(int y=11;y<14;y++)for(int x=10;x<12;x++)assert(owner->canvas[y*320+x]==42);
    int width=-1,height=-1;assert(!term_canvas_size(5,&width,&height)&&!width&&!height);
    original.present(&original.binding);others_unchanged(5,2);
    assert(term_canvas_size(5,&width,&height)&&width==320&&height==200);
    assert(published_canvases[5][4*320+3]==17&&published_canvases[5][11*320+10]==42);
    assert(owner->task_dirty==(TERM_TASK_TEXT|TERM_TASK_LAYOUT)&&!strcmp(term_input(),"x"));

    term_task_stop(5);assert(!term_task_running(5)&&selected==2);
    assert(!term_task_start_file(5,app,fs_identity(app))&&selected==2);
    assert(term_task_info(5,&info)&&info.instance==terms[5].process&&info.instance!=original.binding.process);
    assert(terms[5].binding_generation==original.binding.generation+1);
    stale_delivery(&original,2);

    ProcessIO before_reset=callbacks[5];unsigned generation=terms[5].binding_generation;
    term_select(5);term_reset();assert(!terms[5].process&&terms[5].binding_generation==generation);
    command("start /binding.bex");
    assert(term_task_info(5,&info)&&info.instance==terms[5].process);
    assert(info.instance!=before_reset.binding.process&&terms[5].binding_generation==generation+1);
    term_select(2);stale_delivery(&before_reset,2);

    /* An active slice can defer stop; close/reset cannot detach its storage. */
    stop_deferred[5]=1;snapshot();
    assert(!term_task_close(5));all_unchanged(2);
    term_select(5);snapshot();term_reset();all_unchanged(5);
    assert(term_task_running(5)&&terms[5].process==info.instance);
    stop_deferred[5]=0;term_select(2);assert(term_task_close(5));

    /* Consume an externally completed peer before a ready task can run again. */
    assert(!term_task_start_file(5,app,fs_identity(app)));
    assert(!term_task_start_file(1,app,fs_identity(app)));
    ProcessHandle completed=terms[5].process;int peer_steps=steps[1];
    assert(process_request_stop(completed)&&process_status(completed)==PROCESS_TASK_DONE);
    TermTaskUpdate update=term_task_poll_update();
    assert(update.slot==5&&(update.flags&TERM_TASK_LIFECYCLE));
    assert(!terms[5].process&&process_status(completed)==PROCESS_TASK_EMPTY);
    assert(term_task_running(1)&&steps[1]==peer_steps&&selected==2);
    update=term_task_poll_update();assert(update.slot==1&&steps[1]==peer_steps+1&&selected==2);
    assert(term_task_close(1));

    /* The last representable generation is valid; a later launch cannot wrap. */
    assert(term_task_close(5));terms[5].binding_generation=UINT32_MAX-1;
    assert(!term_task_start_file(5,app,fs_identity(app))&&terms[5].binding_generation==UINT32_MAX);
    assert(term_task_close(5));ProcessCounts before,after;process_counts(&before);
    assert(term_task_start_file(5,app,fs_identity(app))<0&&!terms[5].process);
    process_counts(&after);assert(!memcmp(&before,&after,sizeof before));
    assert(terms[5].binding_generation==UINT32_MAX&&selected==2);
    term_select(5);term_reset();assert(terms[5].binding_generation==UINT32_MAX);
    assert(term_task_start_file(5,app,fs_identity(app))<0&&!terms[5].process);
    puts("Native bindings: copied callback context, selection independence, stale delivery refusal, exact handles, reset generations, deferred close/reset, peer completion and non-wrapping exhaustion passed.");
}
