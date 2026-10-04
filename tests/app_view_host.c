/* Ordinary explicit-owner operations after the Terminal ownership extraction. */
#define TERM_TASK_FIXTURE_ONLY
#include "term_task_info_host.c"
static TerminalText selected_text;
static AppView stopped_view;
static TerminalText stopped_text;
static unsigned char stopped_frame[APP_CANVAS_PIXELS];
int main(void){
    reset();int app=executable("view.bex");
    /* The view service can initialize before any Terminal reset. Its private
     * no-output policy is useful here without advertising a new launch mode. */
    assert(!app_view_start_file(0,app,fs_identity(app),0,0,APP_VIEW_OUTPUT_NONE));
    assert(app_view_running(0)&&views[0].binding.generation==1);
    callbacks[0].plot(&callbacks[0].binding,0,0,42);callbacks[0].present(&callbacks[0].binding);
    term_select(1);term_reset();assert(app_view_running(0)&&app_view_frame(0).pixels[0]==42);
    assert(!terms[0].count);callbacks[0].print(&callbacks[0].binding,"Discard this private no-output diagnostic");
    assert(!terms[0].count);exit_next[0]=1;
    AppViewUpdate initial=app_view_poll_update();assert(initial.slot==0&&!terms[0].count);
    assert(!views[0].binding.process&&views[0].binding.generation==1&&app_view_frame(0).pixels[0]==42);
    term_select(2);term_reset();command("echo owner history");
    int folder=fs_mkdir(fs_root(),"owner folder");assert(folder>=0);term_set_cwd(folder);
    term_char('d');term_char('r');term_input_lost(2);
    int hcount=T.hcount,hpos=T.hpos;unsigned cwd_identity=T.cwd_identity;
    term_select(7);term_reset();term_char('x');term_input_lost(7);selected_text=T;
    char argument[]="/Documents/copied.txt";
    assert(!app_view_start_file(2,app,fs_identity(app),argument,sizeof argument-1,APP_VIEW_OUTPUT_TERMINAL));
    assert(selected==7&&!memcmp(&T,&selected_text,sizeof T));
    TerminalText *text=&terms[2];
    assert(text->cwd==folder&&text->cwd_identity==cwd_identity&&text->hcount==hcount&&text->hpos==hpos);
    assert(!strcmp(text->input,"dr")&&text->len==2&&text->input_invalid);
    memset(argument,'X',sizeof argument);assert(!fs_rename(app,"renamed.bex"));
    AppViewInfo info;assert(app_view_info(2,&info));
    assert(!strcmp(info.name,"view.bex")&&!strcmp(info.document,"copied.txt"));
    assert(views[2].binding.slot==2&&views[2].binding.process==info.instance);
    ProcessIO io=callbacks[2];
    char line[]="Copied explicit owner output";io.print(&io.binding,line);memset(line,'Y',sizeof line);
    assert(!strcmp(text->lines[(text->head+text->count-1)%TERM_LINES],"Copied explicit owner output"));
    assert(!memcmp(&T,&selected_text,sizeof T));
    io.plot(&io.binding,1,2,11);AppCanvasFrame frame=app_view_frame(2);assert(!frame.pixels);
    io.present(&io.binding);frame=app_view_frame(2);assert(frame.pixels&&frame.width==160&&frame.pixels[321]==11);
    io.plot(&io.binding,1,2,12);assert(frame.pixels[321]==11);
    /* A retained DONE incarnation is identity-matching but cannot emit I/O. */
    assert(process_request_stop(info.instance));
    assert(app_view_binding_matches(&io.binding)&&!process_binding_live(&io.binding));
    stopped_view=views[2];stopped_text=*text;memcpy(stopped_frame,frame.pixels,sizeof stopped_frame);
    io.print(&io.binding,"Late output");io.plot(&io.binding,1,2,99);io.rect(&io.binding,0,0,160,100,99);
    assert(io.resize(&io.binding,320,200)<0);io.present(&io.binding);
    assert(!memcmp(&views[2],&stopped_view,sizeof stopped_view));
    assert(!memcmp(text,&stopped_text,sizeof stopped_text));assert(!memcmp(frame.pixels,stopped_frame,sizeof stopped_frame));
    AppViewUpdate result=app_view_poll_update();assert(result.slot==2&&(result.flags&APP_VIEW_LIFECYCLE));
    assert(!views[2].binding.process&&!views[2].canvas.pending&&app_view_frame(2).pixels[321]==11);
    assert(!memcmp(&T,&selected_text,sizeof T));
    /* One common consumer is also behind each unchanged legacy polling API. */
    assert(!term_task_start_file(2,app,fs_identity(app)));int before=steps[2];
    result=app_view_poll_update();assert(result.slot==2&&steps[2]==before+1);
    result=term_task_poll_update();assert(result.slot==2&&steps[2]==before+2);
    assert(term_task_poll()&&steps[2]==before+3);
    /* An active deferred stop revokes I/O immediately, before record cleanup. */
    stop_deferred[2]=1;assert(!app_view_close(2));io=callbacks[2];
    assert(app_view_binding_matches(&io.binding)&&!process_binding_live(&io.binding));
    stopped_view=views[2];stopped_text=terms[2];
    io.print(&io.binding,"Stopping output");io.plot(&io.binding,0,0,99);io.present(&io.binding);
    assert(!memcmp(&views[2],&stopped_view,sizeof stopped_view));
    assert(!memcmp(&terms[2],&stopped_text,sizeof stopped_text));
    stop_deferred[2]=0;assert(app_view_close(2));unsigned generation=views[2].binding.generation;
    /* Clearing/resetting Terminal command state cannot restart view identity. */
    term_select(2);term_reset();assert(views[2].binding.generation==generation);
    assert(!term_task_start_file(2,app,fs_identity(app))&&views[2].binding.generation==generation+1);
    term_task_stop(2);
    assert(!app_view_canvas_size(-1,&hcount,&hpos)&&!hcount&&!hpos);
    puts("App views: explicit copied sinks/metadata, preserved terminal command state, published snapshot, retained-DONE I/O refusal, shared scheduler and persistent generation passed.");
}
