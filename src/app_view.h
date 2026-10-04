#ifndef APP_VIEW_H
#define APP_VIEW_H
#include "app_canvas.h"
/* Bounded internal views, one per desktop slot. This is not a public window
 * API and introduces no executable mode, capability or handle domain. */
#define APP_VIEW_NAME_LEN 24
#define APP_VIEW_TITLE_LEN (APP_VIEW_NAME_LEN*2+3)
typedef struct {
    char name[APP_VIEW_NAME_LEN]; /* Copied basename, not a mutable filesystem pointer. */
    char document[APP_VIEW_NAME_LEN]; /* Copied startup basename, or empty. */
    int owner;                   /* Zero-based terminal/window slot. */
    int state;                   /* PROCESS_TASK_READY or PROCESS_TASK_SLEEPING. */
    unsigned started_ticks;      /* PIT time at the accepted start. */
    unsigned elapsed_sec;        /* Wall-clock lifetime, including sleep. */
    unsigned instance;           /* Full opaque process handle; recheck before UI actions. */
} AppViewInfo;
enum {
    APP_VIEW_CANVAS=1, APP_VIEW_TEXT=2, APP_VIEW_LAYOUT=4, APP_VIEW_LIFECYCLE=8
};
typedef struct { int slot; unsigned flags; } AppViewUpdate;
typedef enum { APP_VIEW_OUTPUT_NONE, APP_VIEW_OUTPUT_TERMINAL } AppViewOutput;
int app_view_reset(int slot);
void app_view_mark(int slot,unsigned flags);
AppCanvasFrame app_view_frame(int slot);
int app_view_canvas_size(int slot,int *width,int *height);
void app_view_canvas_reset(int slot);
void app_view_canvas_unbuffered(int slot);
void app_view_plot(int slot,int x,int y,int color);
void app_view_rect(int slot,int x,int y,int width,int height,int color);
int app_view_resize(int slot,int width,int height);
int app_view_start_file(int slot,int file,unsigned identity,const char *argument,
                        unsigned argument_length,AppViewOutput output);
int app_view_info(int slot,AppViewInfo *out);
int app_view_title(int slot,char *out,int capacity);
int app_view_running(int slot);
/* Identity-only compatibility check; live I/O also checks the process owner. */
int app_view_binding_matches(const ProcessBinding *binding);
int app_view_key(int slot,int key);
void app_view_stop(int slot);
int app_view_close(int slot);
/* Sole common scheduler/completion consumer. At most one slice per call,
 * with retained DONE drained before scheduling any ready peer. */
AppViewUpdate app_view_poll_update(void);
int app_view_poll(void);
#endif
